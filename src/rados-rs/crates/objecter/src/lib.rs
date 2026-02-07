//! Objecter - OSDMap notification and coordination
//!
//! This crate provides shared coordination mechanisms for RADOS object operations,
//! inspired by Ceph's C++ Objecter.
//!
//! Currently provides:
//! - OSDMapNotifier: Pub/sub mechanism for OSDMap updates between MonClient and OSDClient

pub mod osdmap_notifier;

pub use osdmap_notifier::OSDMapNotifier;
