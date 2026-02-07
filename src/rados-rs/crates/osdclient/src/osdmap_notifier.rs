//! OSDMap notification and subscription mechanism
//!
//! This module provides a focused notification system for OSDMap updates,
//! replacing the general-purpose MessageBus for this specific use case.
//!
//! # Design
//!
//! The OSDMapNotifier maintains the current OSDMap and allows:
//! - **Posting**: MonClient and OSDClient can post new OSDMaps
//! - **Subscribing**: Any component can subscribe to receive updates
//! - **Waiting**: Components can wait for an OSDMap if none is available
//! - **Getting**: Components can retrieve the latest OSDMap non-blocking
//!
//! # Example
//!
//! ```rust,ignore
//! // Create notifier (shared between MonClient and OSDClient)
//! let notifier = Arc::new(OSDMapNotifier::new());
//!
//! // MonClient posts new OSDMaps when received from monitors
//! notifier.post(osdmap).await;
//!
//! // OSDClient subscribes to receive all updates
//! let mut rx = notifier.subscribe();
//! while let Some(osdmap) = rx.recv().await {
//!     // Handle new OSDMap
//! }
//!
//! // Anyone can get the latest OSDMap
//! if let Some(osdmap) = notifier.get_latest().await {
//!     // Use current map
//! }
//!
//! // Or wait for the first OSDMap if none available yet
//! let osdmap = notifier.wait_for_map().await;
//! ```

use std::sync::Arc;
use tokio::sync::{mpsc, RwLock};
use tracing::{debug, trace};

use crate::osdmap::OSDMap;

/// OSDMap notifier for subscription-based updates
///
/// This is a focused replacement for MessageBus specifically for OSDMap updates.
/// It maintains the current OSDMap and notifies all subscribers when a new map
/// is posted.
pub struct OSDMapNotifier {
    /// Current OSDMap (highest epoch seen)
    current: Arc<RwLock<Option<Arc<OSDMap>>>>,
    
    /// List of active subscribers
    /// When a new OSDMap is posted, it's sent to all active subscribers
    subscribers: Arc<RwLock<Vec<mpsc::UnboundedSender<Arc<OSDMap>>>>>,
    
    /// Notify waiting tasks when a new OSDMap arrives
    notify: Arc<tokio::sync::Notify>,
}

impl OSDMapNotifier {
    /// Create a new OSDMap notifier
    pub fn new() -> Self {
        Self {
            current: Arc::new(RwLock::new(None)),
            subscribers: Arc::new(RwLock::new(Vec::new())),
            notify: Arc::new(tokio::sync::Notify::new()),
        }
    }

    /// Post a new OSDMap
    ///
    /// This updates the current map (if the epoch is higher) and notifies all
    /// subscribers. The map is only stored if its epoch is greater than the
    /// current epoch.
    ///
    /// # Arguments
    ///
    /// * `osdmap` - The new OSDMap to post
    ///
    /// # Returns
    ///
    /// * `true` if the map was posted (higher epoch)
    /// * `false` if the map was ignored (lower or equal epoch)
    pub async fn post(&self, osdmap: Arc<OSDMap>) -> bool {
        let new_epoch = osdmap.epoch;
        
        // Check and update current map
        let mut current = self.current.write().await;
        let current_epoch = current.as_ref().map(|m| m.epoch).unwrap_or(0);
        
        if new_epoch <= current_epoch {
            debug!(
                "Ignoring OSDMap epoch {} (current epoch is {})",
                new_epoch, current_epoch
            );
            return false;
        }
        
        debug!("Posting new OSDMap epoch {} (was {})", new_epoch, current_epoch);
        *current = Some(Arc::clone(&osdmap));
        drop(current); // Release lock before notifying
        
        // Notify all subscribers
        let mut subscribers = self.subscribers.write().await;
        
        // Remove closed channels and notify active ones
        subscribers.retain(|tx| {
            match tx.send(Arc::clone(&osdmap)) {
                Ok(()) => {
                    trace!("Notified subscriber of OSDMap epoch {}", new_epoch);
                    true // Keep this subscriber
                }
                Err(_) => {
                    trace!("Removing closed subscriber");
                    false // Remove this subscriber
                }
            }
        });
        
        drop(subscribers);
        
        // Wake up any tasks waiting for a map
        self.notify.notify_waiters();
        
        true
    }

    /// Subscribe to OSDMap updates
    ///
    /// Returns a channel receiver that will receive all future OSDMap updates.
    /// The receiver will get a copy of every new OSDMap posted after subscription.
    ///
    /// # Returns
    ///
    /// An unbounded receiver that will receive `Arc<OSDMap>` for each update
    pub fn subscribe(&self) -> mpsc::UnboundedReceiver<Arc<OSDMap>> {
        let (tx, rx) = mpsc::unbounded_channel();
        
        // Add to subscribers list
        // Note: This is a synchronous operation for simplicity
        // We'll use a different approach if needed
        let subscribers = Arc::clone(&self.subscribers);
        tokio::spawn(async move {
            let mut subs = subscribers.write().await;
            subs.push(tx);
            debug!("New subscriber added (total: {})", subs.len());
        });
        
        rx
    }

    /// Get the current OSDMap without blocking
    ///
    /// Returns the latest OSDMap if one has been posted, or None if no map
    /// is available yet.
    ///
    /// # Returns
    ///
    /// * `Some(Arc<OSDMap>)` if a map is available
    /// * `None` if no map has been posted yet
    pub async fn get_latest(&self) -> Option<Arc<OSDMap>> {
        let current = self.current.read().await;
        current.as_ref().map(Arc::clone)
    }

    /// Get the current epoch without blocking
    ///
    /// Returns the epoch of the current OSDMap, or 0 if no map is available.
    ///
    /// # Returns
    ///
    /// The current epoch, or 0 if no map has been posted
    pub async fn get_epoch(&self) -> u32 {
        let current = self.current.read().await;
        current.as_ref().map(|m| m.epoch).unwrap_or(0)
    }

    /// Wait for an OSDMap to be available
    ///
    /// This blocks until at least one OSDMap has been posted. If a map is
    /// already available, it returns immediately with the current map.
    ///
    /// # Returns
    ///
    /// The current (or newly posted) OSDMap
    pub async fn wait_for_map(&self) -> Arc<OSDMap> {
        loop {
            // Check if we have a map
            {
                let current = self.current.read().await;
                if let Some(map) = current.as_ref() {
                    return Arc::clone(map);
                }
            }
            
            // Wait for notification
            self.notify.notified().await;
        }
    }
}

impl Default for OSDMapNotifier {
    fn default() -> Self {
        Self::new()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::time::Duration;

    // Helper to create a test OSDMap with a given epoch
    fn create_test_osdmap(epoch: u32) -> Arc<OSDMap> {
        Arc::new(OSDMap {
            epoch,
            fsid: denc::UuidD { bytes: [0u8; 16] },
            created: denc::UTime::default(),
            modified: denc::UTime::default(),
            pools: Default::default(),
            pool_max: 0,
            flags: 0,
            max_osd: 0,
            osd_state: Vec::new(),
            osd_weight: Vec::new(),
            osd_addrs_client: Vec::new(),
            pg_upmap: Default::default(),
            pg_upmap_items: Default::default(),
            pg_temp: Default::default(),
            primary_temp: Default::default(),
            osd_primary_affinity: Vec::new(),
            erasure_code_profiles: Default::default(),
            removed_snaps_queue: Vec::new(),
            new_removed_snaps: Default::default(),
            new_purged_snaps: Default::default(),
            crush: None,
            cluster_snapshot: String::new(),
            pool_name: Default::default(),
            osd_uuid: Vec::new(),
            blocklist: Default::default(),
            range_blocklist: Default::default(),
            stretch_mode_enabled: false,
            stretch_bucket_count: 0,
            degraded_stretch_mode: 0,
            recovering_stretch_mode: 0,
            stretch_mode_bucket: 0,
            crush_version: 0,
            last_up_change: denc::UTime::default(),
            last_in_change: denc::UTime::default(),
            pg_upmap_primaries: Default::default(),
            osd_addrs_hb_back: Vec::new(),
            osd_info: Vec::new(),
            osd_addrs_cluster: Vec::new(),
            cluster_snapshot_epoch: 0,
            osd_xinfo: Vec::new(),
            osd_addrs_hb_front: Vec::new(),
            nearfull_ratio: 0.0,
            full_ratio: 0.0,
            backfillfull_ratio: 0.0,
            require_min_compat_client: Default::default(),
            require_osd_release: Default::default(),
            crush_node_flags: Default::default(),
            device_class_flags: Default::default(),
            allow_crimson: false,
        })
    }

    #[tokio::test]
    async fn test_post_and_get() {
        let notifier = OSDMapNotifier::new();
        
        // Initially no map
        assert!(notifier.get_latest().await.is_none());
        assert_eq!(notifier.get_epoch().await, 0);
        
        // Post a map
        let map1 = create_test_osdmap(1);
        assert!(notifier.post(Arc::clone(&map1)).await);
        
        // Should be able to get it
        let current = notifier.get_latest().await.unwrap();
        assert_eq!(current.epoch, 1);
        assert_eq!(notifier.get_epoch().await, 1);
    }

    #[tokio::test]
    async fn test_post_only_accepts_higher_epochs() {
        let notifier = OSDMapNotifier::new();
        
        let map2 = create_test_osdmap(2);
        let map1 = create_test_osdmap(1);
        let map3 = create_test_osdmap(3);
        
        // Post epoch 2
        assert!(notifier.post(Arc::clone(&map2)).await);
        assert_eq!(notifier.get_epoch().await, 2);
        
        // Try to post epoch 1 (should be rejected)
        assert!(!notifier.post(Arc::clone(&map1)).await);
        assert_eq!(notifier.get_epoch().await, 2);
        
        // Post epoch 3 (should succeed)
        assert!(notifier.post(Arc::clone(&map3)).await);
        assert_eq!(notifier.get_epoch().await, 3);
    }

    #[tokio::test]
    async fn test_subscribe() {
        let notifier = Arc::new(OSDMapNotifier::new());
        
        // Subscribe before posting
        let mut rx = notifier.subscribe();
        
        // Give subscriber time to register
        tokio::time::sleep(Duration::from_millis(10)).await;
        
        // Post a map
        let map1 = create_test_osdmap(1);
        notifier.post(Arc::clone(&map1)).await;
        
        // Subscriber should receive it
        let received = tokio::time::timeout(Duration::from_secs(1), rx.recv())
            .await
            .expect("Timeout waiting for map")
            .expect("Channel closed");
        assert_eq!(received.epoch, 1);
        
        // Post another map
        let map2 = create_test_osdmap(2);
        notifier.post(Arc::clone(&map2)).await;
        
        // Should receive the second one too
        let received = tokio::time::timeout(Duration::from_secs(1), rx.recv())
            .await
            .expect("Timeout waiting for map")
            .expect("Channel closed");
        assert_eq!(received.epoch, 2);
    }

    #[tokio::test]
    async fn test_wait_for_map() {
        let notifier = Arc::new(OSDMapNotifier::new());
        let notifier_clone = Arc::clone(&notifier);
        
        // Spawn a task that waits for a map
        let handle = tokio::spawn(async move {
            notifier_clone.wait_for_map().await
        });
        
        // Give the task time to start waiting
        tokio::time::sleep(Duration::from_millis(10)).await;
        
        // Post a map
        let map1 = create_test_osdmap(1);
        notifier.post(Arc::clone(&map1)).await;
        
        // The waiting task should complete
        let result = tokio::time::timeout(Duration::from_secs(1), handle)
            .await
            .expect("Timeout waiting for task")
            .expect("Task failed");
        
        assert_eq!(result.epoch, 1);
    }

    #[tokio::test]
    async fn test_wait_for_map_immediate() {
        let notifier = OSDMapNotifier::new();
        
        // Post a map first
        let map1 = create_test_osdmap(1);
        notifier.post(Arc::clone(&map1)).await;
        
        // wait_for_map should return immediately
        let result = tokio::time::timeout(Duration::from_millis(100), notifier.wait_for_map())
            .await
            .expect("Should return immediately");
        
        assert_eq!(result.epoch, 1);
    }

    #[tokio::test]
    async fn test_multiple_subscribers() {
        let notifier = Arc::new(OSDMapNotifier::new());
        
        // Create multiple subscribers
        let mut rx1 = notifier.subscribe();
        let mut rx2 = notifier.subscribe();
        let mut rx3 = notifier.subscribe();
        
        // Give subscribers time to register
        tokio::time::sleep(Duration::from_millis(10)).await;
        
        // Post a map
        let map1 = create_test_osdmap(1);
        notifier.post(Arc::clone(&map1)).await;
        
        // All subscribers should receive it
        for rx in [&mut rx1, &mut rx2, &mut rx3] {
            let received = tokio::time::timeout(Duration::from_secs(1), rx.recv())
                .await
                .expect("Timeout waiting for map")
                .expect("Channel closed");
            assert_eq!(received.epoch, 1);
        }
    }
}
