//! LeekWallet Zcash host companion library (`zecz`).
//!
//! The wallet logic behind the `zecz` CLI and the desktop app: a watch-only
//! wallet for the device account ([`wallet`]), PCZT signing helpers
//! ([`authorize`], [`soft_signer`]), lightwalletd access ([`lwd`]) and
//! `.zkv` seed cards ([`zkv`], [`seed_card`]). Nothing here prints; long
//! operations report progress through callbacks.

pub mod authorize;
pub mod keys;
pub mod lwd;
pub mod seed_card;
pub mod soft_signer;
pub mod wallet;
pub mod zkv;

// The device client and .zsig format live in leek-device, shared with the
// Android app; re-exported so `zecz::device` and `zecz::zsig` stay put.
pub use leek_device::{device, zsig};

pub use wallet::{Network, Profile, Wallet};
