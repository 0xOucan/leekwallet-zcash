//! Quick unlock: an app keeps its Storage Key wrapped by a platform key that
//! lives in secure hardware and is usable only after the user's
//! fingerprint, face or device PIN, so it opens without the board.
//!
//! The app unlocks once with the board ([`get_storage_key`](crate::get_storage_key)),
//! then [`QuickUnlock::create`] hands the key to the app's [`KeyWrapper`]
//! (Android Keystore, the Apple Secure Enclave, Windows Hello, WebAuthn
//! PRF) and returns a record the app stores; [`QuickUnlock::open`] gives the
//! key back after the platform's prompt. A stolen host is then protected by
//! the platform's secure hardware and its attempt limits, not by the board.
//!
//! There is deliberately no software wrapper: a key derived on the host from
//! a short PIN is brute-forced offline in seconds. [`QuickUnlock::create`]
//! refuses a wrapper that does not say it is hardware-backed and asks for
//! the user ([`LeekError::QuickUnlockNotSecure`]); an app on a platform
//! without one does not offer quick unlock. The board path keeps working
//! from the seed, on this host or a new one.

use hmac::Mac;
use zeroize::Zeroizing;

use crate::account::Network;
use crate::error::LeekError;
use crate::storage::{validate_app_id, StorageKey, LABEL_COMMIT};

/// What a [`KeyWrapper`] says about its key. Only the platform knows; the
/// app reports it honestly (Android `KeyInfo.securityLevel` /
/// `isUserAuthenticationRequired`, a Secure Enclave key with an access
/// control, a WebAuthn credential with user verification).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct WrapperInfo {
    /// The key is generated in and never leaves secure hardware (StrongBox,
    /// a TEE, the Secure Enclave, a TPM, a security key).
    pub hardware_backed: bool,
    /// The hardware uses the key only after the user's biometric or device
    /// PIN, with its own attempt limits.
    pub user_auth_required: bool,
    /// What the app shows ("Fingerprint (StrongBox)"); stored in the record,
    /// up to 64 bytes (longer is cut).
    pub label: String,
}

/// Why a [`KeyWrapper`] call failed.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum WrapFailure {
    /// The user cancelled the platform's prompt, or failed it.
    Cancelled,
    /// The platform key is gone or no longer usable (a new fingerprint was
    /// enrolled, the device PIN was removed, the app was reinstalled), or the
    /// blob does not decrypt under it.
    Invalidated,
    /// Anything else the platform reported.
    Failed,
}

impl From<WrapFailure> for LeekError {
    fn from(f: WrapFailure) -> Self {
        match f {
            WrapFailure::Cancelled => LeekError::QuickUnlockCancelled,
            WrapFailure::Invalidated => LeekError::QuickUnlockInvalidated,
            WrapFailure::Failed => LeekError::QuickUnlockFailed,
        }
    }
}

/// The app's platform key: encrypts and decrypts 32 bytes under a key held
/// in secure hardware. Called on the thread that called [`QuickUnlock`];
/// a wrapper that shows a prompt blocks it until the user answers, so call
/// from a background thread.
pub trait KeyWrapper {
    fn info(&self) -> WrapperInfo;
    /// Encrypts `key` (the Storage Key) under the platform key, creating
    /// that key if needed. The blob may hold a nonce or an ephemeral public
    /// key; it must authenticate what it encrypts.
    fn wrap(&self, key: &[u8]) -> Result<Vec<u8>, WrapFailure>;
    /// Decrypts what [`wrap`](Self::wrap) returned, after the user's
    /// biometric or device PIN.
    fn unwrap(&self, blob: &[u8]) -> Result<Zeroizing<Vec<u8>>, WrapFailure>;
    /// Deletes the platform key. Already gone is not an error.
    fn delete_key(&self) -> Result<(), WrapFailure>;
}

const MAGIC: &[u8; 4] = b"ZAQU";
const VERSION: u8 = 1;
const FLAG_HARDWARE: u8 = 0x01;
const FLAG_USER_AUTH: u8 = 0x02;
const SALT_LEN: usize = 32;
const CHECK_LEN: usize = 32;
const TAG_LEN: usize = 32;
const MAX_LABEL: usize = 64;
/// The largest wrapped blob a record holds (an Android AES-GCM blob is 60
/// bytes, a Secure Enclave ECIES one 113).
pub const MAX_WRAPPED_LEN: usize = 4096;
/// Prefix of the record tag's input, apart from the key check's (a bare
/// 32-byte salt) under the same `commit` subkey.
const TAG_DOMAIN: &[u8] = b"Zcash_AppStorage_QuickUnlock_v1";

/// A quick-unlock record: the Storage Key wrapped by the platform, bound to
/// its app, network and account. Not secret on its own (the platform key is
/// needed), but keep it in the app's private storage and delete it with
/// [`disable`](Self::disable).
///
/// ```text
/// "ZAQU" || 0x01 (version) || flags (0x03: hardware-backed, user auth)
///        || coin_type (LE32) || account (LE32) || [len] app_id || [len] label
///        || salt (32) || check (32) || wrapped_len (LE16) || wrapped || tag (32)
/// ```
///
/// `check = HMAC-SHA256(SubKey("commit"), salt)` is the draft's key check:
/// an unwrap that returns another key fails it. `tag = HMAC-SHA256(SubKey("commit"),
/// "Zcash_AppStorage_QuickUnlock_v1" || every byte before it)` binds the
/// header to the key, so a record moved to another app, account or network,
/// or changed in any byte, is refused.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct QuickUnlock {
    bytes: Vec<u8>,
    app_id: String,
    account: u32,
    network: Network,
    label: String,
    /// Offsets of the salt, the wrapped blob and the tag.
    salt: usize,
    wrapped: (usize, usize),
    tag: usize,
}

impl QuickUnlock {
    /// Wraps `key` with `wrapper` (on Android this already asks for the
    /// user's fingerprint or PIN). Refused with
    /// [`LeekError::QuickUnlockNotSecure`] unless the wrapper reports a
    /// hardware-backed key that requires the user, before `wrap` is called.
    pub fn create(key: &StorageKey, wrapper: &dyn KeyWrapper) -> Result<Self, LeekError> {
        let mut salt = [0u8; SALT_LEN];
        getrandom::fill(&mut salt).map_err(|_| LeekError::QuickUnlockFailed)?;
        Self::create_with(key, wrapper, &salt)
    }

    fn create_with(key: &StorageKey, wrapper: &dyn KeyWrapper, salt: &[u8; SALT_LEN]) -> Result<Self, LeekError> {
        key.live()?;
        let info = wrapper.info();
        secure(&info)?;
        let wrapped = wrapper.wrap(&key.to_bytes()[..])?;
        if wrapped.is_empty() || wrapped.len() > MAX_WRAPPED_LEN {
            return Err(LeekError::QuickUnlockFailed);
        }
        let mut label = info.label;
        if label.len() > MAX_LABEL {
            let mut end = MAX_LABEL;
            while !label.is_char_boundary(end) {
                end -= 1;
            }
            label.truncate(end);
        }
        let app_id = key.app_id();
        let mut b = Vec::with_capacity(16 + app_id.len() + label.len() + SALT_LEN + CHECK_LEN + 2 + wrapped.len() + TAG_LEN);
        b.extend_from_slice(MAGIC);
        b.push(VERSION);
        b.push(FLAG_HARDWARE | FLAG_USER_AUTH);
        b.extend_from_slice(&key.network().coin_type().to_le_bytes());
        b.extend_from_slice(&key.account().to_le_bytes());
        b.push(app_id.len() as u8);
        b.extend_from_slice(app_id.as_bytes());
        b.push(label.len() as u8);
        b.extend_from_slice(label.as_bytes());
        b.extend_from_slice(salt);
        b.extend_from_slice(&key.check_value(salt).finalize().into_bytes());
        b.extend_from_slice(&(wrapped.len() as u16).to_le_bytes());
        b.extend_from_slice(&wrapped);
        let tag = tag(key, &b).finalize().into_bytes();
        b.extend_from_slice(&tag);
        Self::from_bytes(&b)
    }

    /// Reads a stored record. [`LeekError::StorageCorrupted`] if it is not
    /// one (nothing is verified until [`open`](Self::open)).
    pub fn from_bytes(bytes: &[u8]) -> Result<Self, LeekError> {
        let mut r = Reader { b: bytes, at: 0 };
        if r.take(4)? != MAGIC || r.u8()? != VERSION || r.u8()? != FLAG_HARDWARE | FLAG_USER_AUTH {
            return Err(LeekError::StorageCorrupted);
        }
        let network = Network::from_coin_type(r.u32()?).ok_or(LeekError::StorageCorrupted)?;
        let account = r.u32()?;
        let n = r.u8()? as usize;
        let app_id = std::str::from_utf8(r.take(n)?).map_err(|_| LeekError::StorageCorrupted)?.to_string();
        let n = r.u8()? as usize;
        let label = std::str::from_utf8(r.take(n)?).map_err(|_| LeekError::StorageCorrupted)?.to_string();
        if account >= 1 << 31 || validate_app_id(&app_id).is_err() || label.len() > MAX_LABEL {
            return Err(LeekError::StorageCorrupted);
        }
        let salt = r.at;
        r.take(SALT_LEN + CHECK_LEN)?;
        let n = u16::from_le_bytes(r.take(2)?.try_into().expect("2 bytes")) as usize;
        if n == 0 || n > MAX_WRAPPED_LEN {
            return Err(LeekError::StorageCorrupted);
        }
        let wrapped = (r.at, r.at + n);
        r.take(n)?;
        let tag = r.at;
        r.take(TAG_LEN)?;
        if r.at != bytes.len() {
            return Err(LeekError::StorageCorrupted);
        }
        Ok(QuickUnlock { bytes: bytes.to_vec(), app_id, account, network, label, salt, wrapped, tag })
    }

    /// The record to store.
    pub fn to_bytes(&self) -> &[u8] {
        &self.bytes
    }

    pub fn app_id(&self) -> &str {
        &self.app_id
    }

    pub fn account(&self) -> u32 {
        self.account
    }

    pub fn network(&self) -> Network {
        self.network
    }

    /// The wrapper's label when the record was made, for the unlock screen.
    pub fn label(&self) -> &str {
        &self.label
    }

    /// The Storage Key for `app_id`, `account` and `network`, after the
    /// platform's prompt. In order: [`LeekError::StorageKeyMismatch`] if the
    /// record is for another app, account or network (no prompt is shown);
    /// [`LeekError::QuickUnlockNotSecure`] for a wrapper that is not
    /// hardware-backed and user-authenticated; the wrapper's failure
    /// ([`LeekError::QuickUnlockCancelled`], [`LeekError::QuickUnlockInvalidated`],
    /// [`LeekError::QuickUnlockFailed`]); `QuickUnlockInvalidated` if the
    /// unwrapped key fails the key check; [`LeekError::StorageCorrupted`] if
    /// the record was changed. On every failure but a cancel, offer the
    /// board and set quick unlock up again after it.
    pub fn open(&self, app_id: &str, account: u32, network: Network, wrapper: &dyn KeyWrapper) -> Result<StorageKey, LeekError> {
        if app_id != self.app_id || account != self.account || network != self.network {
            return Err(LeekError::StorageKeyMismatch);
        }
        secure(&wrapper.info())?;
        let raw = wrapper.unwrap(&self.bytes[self.wrapped.0..self.wrapped.1])?;
        let bytes: Zeroizing<[u8; 32]> =
            Zeroizing::new(raw[..].try_into().map_err(|_| LeekError::QuickUnlockInvalidated)?);
        drop(raw);
        let key = StorageKey::from_bytes(*bytes, app_id, account, network)?;
        let salt = &self.bytes[self.salt..self.salt + SALT_LEN];
        let check = &self.bytes[self.salt + SALT_LEN..self.salt + SALT_LEN + CHECK_LEN];
        // Constant time (hmac's verify).
        key.check_value(salt).verify_slice(check).map_err(|_| LeekError::QuickUnlockInvalidated)?;
        tag(&key, &self.bytes[..self.tag]).verify_slice(&self.bytes[self.tag..]).map_err(|_| LeekError::StorageCorrupted)?;
        Ok(key)
    }

    /// Turns quick unlock off: deletes the platform key. The app deletes the
    /// stored record too (this crate does no I/O). Works without the record,
    /// so a damaged one can still be cleaned up.
    pub fn disable(wrapper: &dyn KeyWrapper) -> Result<(), LeekError> {
        match wrapper.delete_key() {
            Ok(()) | Err(WrapFailure::Invalidated) => Ok(()),
            Err(e) => Err(e.into()),
        }
    }
}

fn secure(info: &WrapperInfo) -> Result<(), LeekError> {
    if info.hardware_backed && info.user_auth_required {
        Ok(())
    } else {
        Err(LeekError::QuickUnlockNotSecure)
    }
}

fn tag(key: &StorageKey, body: &[u8]) -> hmac::Hmac<sha2::Sha256> {
    let commit = key.subkey_unchecked(LABEL_COMMIT);
    let mut mac = <hmac::Hmac<sha2::Sha256> as Mac>::new_from_slice(&commit[..]).expect("HMAC takes any key length");
    mac.update(TAG_DOMAIN);
    mac.update(body);
    mac
}

struct Reader<'a> {
    b: &'a [u8],
    at: usize,
}

impl<'a> Reader<'a> {
    fn take(&mut self, n: usize) -> Result<&'a [u8], LeekError> {
        let s = self.b.get(self.at..self.at + n).ok_or(LeekError::StorageCorrupted)?;
        self.at += n;
        Ok(s)
    }

    fn u8(&mut self) -> Result<u8, LeekError> {
        Ok(self.take(1)?[0])
    }

    fn u32(&mut self) -> Result<u32, LeekError> {
        Ok(u32::from_le_bytes(self.take(4)?.try_into().expect("4 bytes")))
    }
}

#[cfg(test)]
mod tests {
    use std::cell::{Cell, RefCell};

    use chacha20poly1305::aead::{Aead, KeyInit};
    use chacha20poly1305::{XChaCha20Poly1305, XNonce};

    use super::*;

    #[derive(Clone, Copy, PartialEq)]
    enum Mode {
        Ok,
        Cancel,
        OtherKey,
        Short,
    }

    /// A stand-in for a platform key: XChaCha20-Poly1305 under a fixed key,
    /// claiming whatever `info` says. Counts the prompts it would show.
    struct Fake {
        info: WrapperInfo,
        key: [u8; 32],
        mode: Cell<Mode>,
        deleted: Cell<bool>,
        prompts: Cell<u32>,
        last: RefCell<Vec<u8>>,
    }

    impl Fake {
        fn new(hardware_backed: bool, user_auth_required: bool) -> Self {
            Fake {
                info: WrapperInfo { hardware_backed, user_auth_required, label: "Fingerprint (test)".into() },
                key: [0x5a; 32],
                mode: Cell::new(Mode::Ok),
                deleted: Cell::new(false),
                prompts: Cell::new(0),
                last: RefCell::new(Vec::new()),
            }
        }
    }

    impl KeyWrapper for Fake {
        fn info(&self) -> WrapperInfo {
            self.info.clone()
        }

        fn wrap(&self, key: &[u8]) -> Result<Vec<u8>, WrapFailure> {
            self.prompts.set(self.prompts.get() + 1);
            *self.last.borrow_mut() = key.to_vec();
            let mut nonce = [0u8; 24];
            getrandom::fill(&mut nonce).unwrap();
            let ct = XChaCha20Poly1305::new_from_slice(&self.key).unwrap().encrypt(XNonce::from_slice(&nonce), key).unwrap();
            Ok([&nonce[..], &ct].concat())
        }

        fn unwrap(&self, blob: &[u8]) -> Result<Zeroizing<Vec<u8>>, WrapFailure> {
            self.prompts.set(self.prompts.get() + 1);
            if self.deleted.get() {
                return Err(WrapFailure::Invalidated);
            }
            match self.mode.get() {
                Mode::Cancel => return Err(WrapFailure::Cancelled),
                Mode::OtherKey => return Ok(Zeroizing::new(vec![7; 32])),
                Mode::Short => return Ok(Zeroizing::new(vec![7; 31])),
                Mode::Ok => {}
            }
            if blob.len() < 24 {
                return Err(WrapFailure::Invalidated);
            }
            XChaCha20Poly1305::new_from_slice(&self.key)
                .unwrap()
                .decrypt(XNonce::from_slice(&blob[..24]), &blob[24..])
                .map(Zeroizing::new)
                .map_err(|_| WrapFailure::Invalidated)
        }

        fn delete_key(&self) -> Result<(), WrapFailure> {
            if self.deleted.replace(true) {
                Err(WrapFailure::Invalidated)
            } else {
                Ok(())
            }
        }
    }

    const APP: &str = "xyz.leekwallet.zcash";

    fn key() -> StorageKey {
        StorageKey::from_seed(&[1u8; 32], APP, 0, Network::Main).unwrap()
    }

    #[test]
    fn round_trip() {
        let k = key();
        let sealed = k.seal_db(b"wallet").unwrap();
        let w = Fake::new(true, true);
        let q = QuickUnlock::create(&k, &w).unwrap();
        assert_eq!(&w.last.borrow()[..], &k.to_bytes()[..]);
        assert_eq!((q.app_id(), q.account(), q.network(), q.label()), (APP, 0, Network::Main, "Fingerprint (test)"));
        assert_eq!(&q.to_bytes()[..6], b"ZAQU\x01\x03");
        // Stored and read back, it opens to the same key.
        let q = QuickUnlock::from_bytes(q.to_bytes()).unwrap();
        let back = q.open(APP, 0, Network::Main, &w).unwrap();
        assert_eq!(back.to_bytes(), k.to_bytes());
        assert_eq!(&back.open_db(&sealed).unwrap()[..], b"wallet");
        assert_eq!(w.prompts.get(), 2);
        // Two records of the same key differ (fresh salt, fresh wrap).
        assert_ne!(QuickUnlock::create(&k, &w).unwrap().to_bytes(), q.to_bytes());
        // A long label is cut at a character boundary.
        let mut long = Fake::new(true, true);
        long.info.label = "é".repeat(40);
        let q = QuickUnlock::create(&k, &long).unwrap();
        assert_eq!(q.label(), "é".repeat(32));
        assert!(q.open(APP, 0, Network::Main, &long).is_ok());
    }

    #[test]
    fn refuses_wrappers_that_are_not_hardware_and_user_auth() {
        let k = key();
        for (hw, ua) in [(false, true), (true, false), (false, false)] {
            let w = Fake::new(hw, ua);
            assert_eq!(QuickUnlock::create(&k, &w).unwrap_err(), LeekError::QuickUnlockNotSecure);
            assert_eq!(w.prompts.get(), 0, "wrap is not called");
        }
        // Nor does such a wrapper open a record made with a real one.
        let q = QuickUnlock::create(&k, &Fake::new(true, true)).unwrap();
        let soft = Fake::new(false, true);
        assert_eq!(q.open(APP, 0, Network::Main, &soft).unwrap_err(), LeekError::QuickUnlockNotSecure);
        assert_eq!(soft.prompts.get(), 0);
    }

    #[test]
    fn bound_to_app_account_and_network() {
        let k = key();
        let w = Fake::new(true, true);
        let q = QuickUnlock::create(&k, &w).unwrap();
        w.prompts.set(0);
        for (app, account, net) in [("com.example.wallet", 0, Network::Main), (APP, 1, Network::Main), (APP, 0, Network::Test)] {
            assert_eq!(q.open(app, account, net, &w).unwrap_err(), LeekError::StorageKeyMismatch);
        }
        assert_eq!(w.prompts.get(), 0, "no prompt for another app's record");
        // The header rewritten for another account, app or network, the rest
        // kept: the key check passes (same key), the tag does not.
        let b = q.to_bytes();
        let mut acct = b.to_vec();
        acct[10] = 1;
        let mut app = b.to_vec();
        app[15..15 + 5].copy_from_slice(b"abc.d");
        let mut net = b.to_vec();
        net[6..10].copy_from_slice(&1u32.to_le_bytes());
        for (moved, app, account, n) in [
            (acct, APP.to_string(), 1, Network::Main),
            (app, format!("abc.d{}", &APP[5..]), 0, Network::Main),
            (net, APP.to_string(), 0, Network::Test),
        ] {
            let m = QuickUnlock::from_bytes(&moved).unwrap();
            assert_eq!(m.open(&app, account, n, &w).unwrap_err(), LeekError::StorageCorrupted, "{app} {account}");
        }
    }

    #[test]
    fn tampering() {
        let k = key();
        let w = Fake::new(true, true);
        let q = QuickUnlock::create(&k, &w).unwrap();
        let b = q.to_bytes();
        // Every single-bit flip is refused, by the parser, the binding, the
        // platform's own authentication, the key check or the tag.
        for i in 0..b.len() {
            let mut t = b.to_vec();
            t[i] ^= 0x01;
            let r = QuickUnlock::from_bytes(&t).and_then(|m| m.open(APP, 0, Network::Main, &w));
            assert!(r.is_err(), "byte {i}");
        }
        // The label and the tag are covered by the tag alone.
        let label = 15 + APP.len() + 1;
        for i in [label, b.len() - 1] {
            let mut t = b.to_vec();
            t[i] ^= 0x01;
            let m = QuickUnlock::from_bytes(&t).unwrap();
            assert_eq!(m.open(APP, 0, Network::Main, &w).unwrap_err(), LeekError::StorageCorrupted);
        }
        for bad in [&b[..b.len() - 1], &[b, &[0][..]].concat()[..], &b[..20], &[]] {
            assert_eq!(QuickUnlock::from_bytes(bad).unwrap_err(), LeekError::StorageCorrupted);
        }
    }

    #[test]
    fn wrong_key_cancel_and_disable() {
        let k = key();
        let w = Fake::new(true, true);
        let q = QuickUnlock::create(&k, &w).unwrap();
        for (mode, want) in [
            (Mode::OtherKey, LeekError::QuickUnlockInvalidated),
            (Mode::Short, LeekError::QuickUnlockInvalidated),
            (Mode::Cancel, LeekError::QuickUnlockCancelled),
        ] {
            w.mode.set(mode);
            assert_eq!(q.open(APP, 0, Network::Main, &w).unwrap_err(), want);
        }
        assert_eq!(LeekError::QuickUnlockCancelled.recovery(), crate::Recovery::RetrySameLink);
        assert_eq!(LeekError::QuickUnlockInvalidated.recovery(), crate::Recovery::DoNotRetry);
        w.mode.set(Mode::Ok);
        // Another platform key (reinstalled app, new enrolment): its AEAD fails.
        let mut other = Fake::new(true, true);
        other.key = [0x11; 32];
        assert_eq!(q.open(APP, 0, Network::Main, &other).unwrap_err(), LeekError::QuickUnlockInvalidated);
        // Disabled: the platform key is gone, the record opens no more.
        QuickUnlock::disable(&w).unwrap();
        QuickUnlock::disable(&w).unwrap();
        assert_eq!(q.open(APP, 0, Network::Main, &w).unwrap_err(), LeekError::QuickUnlockInvalidated);
        // A forgotten key is not wrapped.
        let mut f = key();
        f.forget();
        assert_eq!(QuickUnlock::create(&f, &Fake::new(true, true)).unwrap_err(), LeekError::StorageKeyForgotten);
    }

    /// The layout is pinned: a change here strands every stored record.
    #[test]
    fn record_format_is_stable() {
        let k = key();
        let w = Fake::new(true, true);
        let salt = [0x40u8; SALT_LEN];
        let q = QuickUnlock::create_with(&k, &w, &salt).unwrap();
        let b = q.to_bytes();
        let label = b"Fingerprint (test)";
        let mut want = b"ZAQU\x01\x03".to_vec();
        want.extend_from_slice(&133u32.to_le_bytes());
        want.extend_from_slice(&0u32.to_le_bytes());
        want.push(APP.len() as u8);
        want.extend_from_slice(APP.as_bytes());
        want.push(label.len() as u8);
        want.extend_from_slice(label);
        want.extend_from_slice(&salt);
        want.extend_from_slice(&k.check_value(&salt).finalize().into_bytes());
        let wrapped = &b[want.len() + 2..b.len() - TAG_LEN];
        assert_eq!(wrapped.len(), 24 + 32 + 16);
        want.extend_from_slice(&(wrapped.len() as u16).to_le_bytes());
        want.extend_from_slice(wrapped);
        let mut mac = <hmac::Hmac<sha2::Sha256> as Mac>::new_from_slice(&k.subkey("commit").unwrap()[..]).unwrap();
        mac.update(b"Zcash_AppStorage_QuickUnlock_v1");
        mac.update(&want);
        want.extend_from_slice(&mac.finalize().into_bytes());
        assert_eq!(b, &want[..]);
    }
}
