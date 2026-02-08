//! OSDMap receiver trait for handling raw OSDMap messages
//!
//! This trait allows MonClient to forward raw OSDMap data without depending
//! on the osdclient crate (which would create a circular dependency).
//!
//! # Example
//!
//! ```rust,ignore
//! struct MyOSDMapHandler {
//!     notifier: Arc<MapNotifier<OSDMap>>,
//! }
//!
//! impl OSDMapReceiver for MyOSDMapHandler {
//!     fn handle_osdmap(&self, epoch: u32, data: bytes::Bytes) {
//!         // Decode OSDMap from bytes
//!         match OSDMap::decode_versioned(&mut data.as_ref(), 0) {
//!             Ok(osdmap) => {
//!                 // Post to notifier
//!                 let notifier = Arc::clone(&self.notifier);
//!                 tokio::spawn(async move {
//!                     notifier.post(Arc::new(osdmap)).await;
//!                 });
//!             }
//!             Err(e) => {
//!                 eprintln!("Failed to decode OSDMap: {}", e);
//!             }
//!         }
//!     }
//! }
//! ```

use bytes::Bytes;

/// Trait for handling raw OSDMap message data
///
/// Implementations receive raw OSDMap bytes from MonClient and can decode
/// and distribute them as needed (e.g., to a MapNotifier).
///
/// This trait allows MonClient to forward OSDMap data without depending on
/// the osdclient crate, avoiding circular dependencies.
pub trait OSDMapReceiver: Send + Sync {
    /// Handle a raw OSDMap message
    ///
    /// # Arguments
    ///
    /// * `epoch` - The epoch number of this OSDMap
    /// * `data` - Raw encoded OSDMap bytes
    fn handle_osdmap(&self, epoch: u32, data: Bytes);
}
