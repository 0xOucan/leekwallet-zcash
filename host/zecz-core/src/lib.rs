//! LeekWallet Zcash host companion (`zecz`).

pub mod authorize;
pub mod keys;
pub mod lwd;
pub mod soft_signer;
pub mod zkv;

// The device client and .zsig format live in leek-device, shared with the
// Android app; re-exported so `zecz::device` and `zecz::zsig` stay put.
pub use leek_device::{device, zsig};
