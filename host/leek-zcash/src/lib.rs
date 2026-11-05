//! # leek-zcash: the LeekWallet SDK's Zcash kit
//!
//! What a Zcash wallet needs to sign with a LeekWallet board through the
//! hardware-signer path it already has for Keystone or Ledger: a signed PCZT
//! comes out, which the wallet combines, proves and extracts as before.
//!
//! - Account: [`seed_fingerprint`] (which seed is on the board, no press),
//!   [`get_account`] (the UFVK only after a press on the board;
//!   [`get_account_with`] reports it), [`show_address`] ([`AddressCheck`]),
//!   [`to_checkable_ua`], [`set_birthday`], [`get_features`].
//! - Signing: [`preflight`] (host-side checks), [`prepare_for_signer`]
//!   (derivations some wallets leave out), [`signer_view_bytes`] (what goes
//!   to the board), [`sign_pczt`] (all of it over a link), and
//!   [`apply_zsig`] (a `.zsig` from any transport, such as an SD card, back
//!   into the wallet's PCZT, every signature verified).
//! - SD card (experimental): [`prepare_sd_sign`] (the file the board reads)
//!   and [`finish_sd_sign`] (its `.zsig` back into a [`SignResult`]).
//! - Stored app data: [`get_storage_key`] (the app's Storage Key, after a
//!   press on the board every time) and [`StorageKey`] (subkeys, and
//!   [`StorageKey::seal`] / [`StorageKey::open`] for the database and the
//!   UFVK), per docs/zcash/zip-draft-app-storage-keys.md; [`QuickUnlock`]
//!   (the key kept wrapped by the platform's secure hardware behind the
//!   user's biometric or device PIN, through the app's [`KeyWrapper`]).
//! - Records: [`AccountRecord`] (the import record as CBOR),
//!   [`SignResult::to_bytes`] (persist before broadcast),
//!   [`check_broadcast`] (can it still be mined?) and [`check_mined`]
//!   (compare after mining).
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
mod persist;
mod preflight;
mod prepare;
mod quick;
mod record;
mod redact;
mod sd;
mod sign;
mod storage;
mod ua;

pub use account::{
    BOARD_UPDATE_TEXT, MIN_FIRMWARE_COMPAT, get_account, get_account_with, get_features, seed_fingerprint, set_birthday, show_address, Account, AccountMeta,
    AccountProgress, AddressCheck, Features, Network,
    KEY_SOURCE, KNOWN_BRANCH_IDS, MAX_PCZT_BYTES, MAX_TRANSPARENT_INPUTS,
};
pub use apply::{apply_zsig, Signature};
pub use error::{classify, LeekError, Recovery, Unsupported};
pub use persist::{check_broadcast, check_mined, tx_txid, MinedCheck};
pub use preflight::preflight;
pub use prepare::{prepare_for_signer, TRANSPARENT_SEARCH_LIMIT};
pub use quick::{KeyWrapper, QuickUnlock, WrapFailure, WrapperInfo, MAX_WRAPPED_LEN};
pub use record::{AccountEntry, AccountRecord, ACCOUNT_RECORD_UR_TYPE};
pub use redact::{signer_view_bytes, SignerView};
pub use sd::{finish_sd_sign, prepare_sd_sign, SdRequest, SD_DIR, SD_SIGNED_ZSIG, SD_UNSIGNED_PCZT};
pub use sign::{sign_pczt, CancelToken, CancellablePort, SignOptions, SignProgress, SignResult};
pub use storage::{
    get_storage_key, get_storage_key_with, validate_app_id, StorageKey, LABEL_DB, LABEL_UFVK, SEALED_HEADER_LEN,
    STORAGE_ZIP_NUMBER,
};
pub use ua::to_checkable_ua;

/// The crates this API is expressed in, re-exported so a wallet uses the
/// same versions.
pub use leek_device;
pub use pczt;
