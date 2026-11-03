//! # leek-zcash: the LeekWallet SDK's Zcash kit
//!
//! What a Zcash wallet needs to sign with a LeekWallet board through the
//! hardware-signer path it already has for Keystone or Ledger: a signed PCZT
//! comes out, which the wallet combines, proves and extracts as before.
//!
//! - Account: [`get_account`], [`show_address`] ([`AddressCheck`]),
//!   [`to_checkable_ua`], [`set_birthday`], [`get_features`].
//! - Signing: [`preflight`] (host-side checks), [`prepare_for_signer`]
//!   (derivations some wallets leave out), [`signer_view_bytes`] (what goes
//!   to the board), [`sign_pczt`] (all of it over a link), and
//!   [`apply_zsig`] (a `.zsig` from any transport, such as an SD card, back
//!   into the wallet's PCZT, every signature verified).
//! - Errors: one [`LeekError`] with stable codes and a [`Recovery`] hint;
//!   messages never contain amounts, addresses, keys or PCZT bytes.
//!
//! The crate does no I/O of its own. Device calls take a
//! [`leek_device::device::Device`], which runs over any
//! [`leek_device::FramePort`] (USB serial, Bluetooth, a Kotlin or JS
//! callback, the zdev simulator). Pairing and unlocking stay in
//! `leek-device` (`Device::pair`, `Device::ensure_unlocked`); map their
//! errors with [`classify`].

mod account;
mod apply;
mod error;
mod redact;
mod ua;

pub use account::{
    get_account, get_features, set_birthday, show_address, Account, AccountMeta, AddressCheck, Features, Network,
    KEY_SOURCE, KNOWN_BRANCH_IDS, MAX_PCZT_BYTES, MAX_TRANSPARENT_INPUTS,
};
pub use apply::{apply_zsig, Signature};
pub use error::{classify, LeekError, Recovery, Unsupported};
pub use redact::{signer_view_bytes, SignerView};
pub use ua::to_checkable_ua;

/// The crates this API is expressed in, re-exported so a wallet uses the
/// same versions.
pub use leek_device;
pub use pczt;
