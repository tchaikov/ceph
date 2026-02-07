//! Map notification and subscription mechanism
//!
//! This module provides a focused notification system for map updates (OSDMap, MDSMap, MonMap, etc.),
//! replacing the general-purpose MessageBus for this specific use case.
//!
//! # Design
//!
//! The MapNotifier maintains the current map and allows:
//! - **Posting**: Clients can post new maps
//! - **Subscribing**: Any component can subscribe to receive updates
//! - **Waiting**: Components can wait for a map if none is available
//! - **Getting**: Components can retrieve the latest map non-blocking
//!
//! # Example
//!
//! ```rust,ignore
//! // Create notifier (shared between clients)
//! let notifier = Arc::new(MapNotifier::<OSDMap>::new());
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
//! // Anyone can get the latest map
//! if let Some(map) = notifier.get_latest().await {
//!     // Use current map
//! }
//!
//! // Or wait for the first map if none available yet
//! let map = notifier.wait_for_map().await;
//! ```

use std::sync::Arc;
use tokio::sync::{mpsc, RwLock};
use tracing::{debug, trace};

/// Trait that map types must implement for notifier compatibility
pub trait MapLike: Send + Sync + 'static {
    /// Get the epoch of this map
    fn epoch(&self) -> u32;
}

/// Map notifier for subscription-based updates
///
/// This is a focused replacement for MessageBus specifically for map updates (OSDMap, MDSMap, MonMap, etc.).
/// It maintains the current map and notifies all subscribers when a new map is posted.
///
/// Generic over T which must implement MapLike trait.
pub struct MapNotifier<T: MapLike> {
    /// Current map (highest epoch seen)
    current: Arc<RwLock<Option<Arc<T>>>>,
    
    /// List of active subscribers
    /// When a new map is posted, it's sent to all active subscribers
    subscribers: Arc<RwLock<Vec<mpsc::UnboundedSender<Arc<T>>>>>,
    
    /// Notify waiting tasks when a new map arrives
    notify: Arc<tokio::sync::Notify>,
}

impl<T: MapLike> MapNotifier<T> {
    /// Create a new map notifier
    pub fn new() -> Self {
        Self {
            current: Arc::new(RwLock::new(None)),
            subscribers: Arc::new(RwLock::new(Vec::new())),
            notify: Arc::new(tokio::sync::Notify::new()),
        }
    }

    /// Post a new map
    ///
    /// This updates the current map (if the epoch is higher) and notifies all
    /// subscribers. The map is only stored if its epoch is greater than the
    /// current epoch.
    ///
    /// # Arguments
    ///
    /// * `map` - The new map to post
    ///
    /// # Returns
    ///
    /// * `true` if the map was posted (higher epoch)
    /// * `false` if the map was ignored (lower or equal epoch)
    pub async fn post(&self, map: Arc<T>) -> bool {
        let new_epoch = map.epoch();
        
        // Check and update current map
        let mut current = self.current.write().await;
        let current_epoch = current.as_ref().map(|m| m.epoch()).unwrap_or(0);
        
        if new_epoch <= current_epoch {
            debug!(
                "Ignoring map epoch {} (current epoch is {})",
                new_epoch, current_epoch
            );
            return false;
        }
        
        debug!("Posting new map epoch {} (was {})", new_epoch, current_epoch);
        *current = Some(Arc::clone(&map));
        drop(current); // Release lock before notifying
        
        // Notify all subscribers
        let mut subscribers = self.subscribers.write().await;
        
        // Remove closed channels and notify active ones
        subscribers.retain(|tx| {
            match tx.send(Arc::clone(&map)) {
                Ok(()) => {
                    trace!("Notified subscriber of map epoch {}", new_epoch);
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

    /// Subscribe to map updates
    ///
    /// Returns a channel receiver that will receive all future map updates.
    /// The receiver will get a copy of every new map posted after subscription.
    ///
    /// # Returns
    ///
    /// An unbounded receiver that will receive `Arc<T>` for each update
    pub async fn subscribe(&self) -> mpsc::UnboundedReceiver<Arc<T>> {
        let (tx, rx) = mpsc::unbounded_channel();
        
        // Add to subscribers list
        let mut subs = self.subscribers.write().await;
        subs.push(tx);
        debug!("New subscriber added (total: {})", subs.len());
        
        rx
    }

    /// Get the current map without blocking
    ///
    /// Returns the latest map if one has been posted, or None if no map
    /// is available yet.
    ///
    /// # Returns
    ///
    /// * `Some(Arc<T>)` if a map is available
    /// * `None` if no map has been posted yet
    pub async fn get_latest(&self) -> Option<Arc<T>> {
        let current = self.current.read().await;
        current.as_ref().map(Arc::clone)
    }

    /// Get the current epoch without blocking
    ///
    /// Returns the epoch of the current map, or 0 if no map is available.
    ///
    /// # Returns
    ///
    /// The current epoch, or 0 if no map has been posted
    pub async fn get_epoch(&self) -> u32 {
        let current = self.current.read().await;
        current.as_ref().map(|m| m.epoch()).unwrap_or(0)
    }

    /// Wait for a map to be available
    ///
    /// This blocks until at least one map has been posted. If a map is
    /// already available, it returns immediately with the current map.
    ///
    /// # Returns
    ///
    /// The current (or newly posted) map
    pub async fn wait_for_map(&self) -> Arc<T> {
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

impl<T: MapLike> Default for MapNotifier<T> {
    fn default() -> Self {
        Self::new()
    }
}
