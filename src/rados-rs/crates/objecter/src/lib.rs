//! Objecter - Map notification and coordination
//!
//! This crate provides shared coordination mechanisms for Ceph clients,
//! inspired by Ceph's C++ Objecter.
//!
//! Currently provides:
//! - MapNotifier: Pub/sub mechanism for map updates (OSDMap, MDSMap, MonMap, etc.)

pub mod map_notifier;

pub use map_notifier::MapNotifier;
