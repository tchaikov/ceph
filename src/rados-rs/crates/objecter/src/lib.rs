//! Objecter - Map notification and coordination
//!
//! This crate provides shared coordination mechanisms for Ceph clients,
//! inspired by Ceph's C++ Objecter.
//!
//! Currently provides:
//! - MapNotifier: Pub/sub mechanism for map updates (OSDMap, MDSMap, MonMap, etc.)
//! - OSDMapReceiver: Trait for handling raw OSDMap messages

pub mod map_notifier;
pub mod osdmap_receiver;

pub use map_notifier::MapNotifier;
pub use osdmap_receiver::OSDMapReceiver;
