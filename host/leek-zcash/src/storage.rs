//! App Storage Keys (docs/zcash/zip-draft-app-storage-keys.md): a key per
//! app, network and account that the board derives from its seed and
//! releases only over the paired session after a press, and the sealed
//! storage an app keeps under it.
//!
//! An app that offers "unlock with the board" stores only what
//! [`StorageKey::seal`] returns (its database, its UFVK), asks the board for
//! the key each time it opens ([`get_storage_key`]), and drops the key when it
//! closes. A stolen host or a backup of it then reads nothing without the
//! board and the user's press. It does not protect against the app itself or
//! malware on the host while the app is open.
//!
//! The derivation is ZIP 32 Registered key derivation (the `zip32` crate);
//! subkeys are HKDF-SHA256; sealing is XChaCha20-Poly1305 with a key check
//! value, as the draft recommends. The sealed format is this crate's, see
//! [`StorageKey::seal`].

use std::fmt;

use chacha20poly1305::aead::{Aead, KeyInit, Payload};
use chacha20poly1305::{XChaCha20Poly1305, XNonce};
use hkdf::Hkdf;
use hmac::{Hmac, Mac};
use leek_device::device::Device;
use sha2::Sha256;
use zeroize::{Zeroize, Zeroizing};
use zip32::registered::{cryptovalue_from_subpath, PathElement};
use zip32::ChildIndex;

use crate::account::{AccountProgress, Network};
use crate::error::{from_device, Ctx, LeekError};

/// The ZIP number the draft would be assigned. A placeholder: it is an input
/// to the derivation, so every Storage Key changes when the real one is
/// known. The board reports the one it uses (`Features::storage_key`).
pub const STORAGE_ZIP_NUMBER: u16 = 65535;

/// ZIP 32 Registered key derivation context string.
const CONTEXT: &[u8] = b"AppStorageKeys";
/// HKDF info prefix for Storage Subkeys.
const SUBKEY_INFO: &[u8] = b"Zcash_AppStorage_v1";

/// The draft's subkey labels.
pub const LABEL_DB: &str = "db";
pub const LABEL_UFVK: &str = "ufvk";
const LABEL_COMMIT: &str = "commit";

/// The sealed format: magic, version, algorithm.
const MAGIC: &[u8; 4] = b"ZASK";
const VERSION: u8 = 1;
const ALG_XCHACHA20POLY1305: u8 = 1;
const SALT_LEN: usize = 32;
const CHECK_LEN: usize = 32;
const NONCE_LEN: usize = 24;
const TAG_LEN: usize = 16;
/// magic || version || alg || salt || check || nonce
pub const SEALED_HEADER_LEN: usize = 4 + 1 + 1 + SALT_LEN + CHECK_LEN + NONCE_LEN;

/// Whether `app_id` is a valid App identifier: lower-case ASCII reverse-DNS,
/// labels of `[a-z0-9]` then `[a-z0-9_-]`, at least two, 3 to 64 bytes in
/// all. Nothing is normalised; the board refuses what this refuses.
pub fn validate_app_id(app_id: &str) -> Result<(), LeekError> {
    let b = app_id.as_bytes();
    if !(3..=64).contains(&b.len()) {
        return Err(LeekError::InvalidAppId);
    }
    let mut labels = 0;
    for label in b.split(|&c| c == b'.') {
        let lead = |c: &u8| c.is_ascii_lowercase() || c.is_ascii_digit();
        match label.split_first() {
            Some((first, rest))
                if lead(first) && rest.len() <= 62 && rest.iter().all(|c| lead(c) || *c == b'-' || *c == b'_') => {}
            _ => return Err(LeekError::InvalidAppId),
        }
        labels += 1;
    }
    if labels < 2 {
        return Err(LeekError::InvalidAppId);
    }
    Ok(())
}

/// A subkey label: the draft's `db` and `ufvk`, or a private `x-...`; 1 to
/// 64 bytes of printable ASCII. `commit` is the key check's own.
fn check_label(label: &str) -> Result<(), LeekError> {
    let ok = (label == LABEL_DB || label == LABEL_UFVK || label.starts_with("x-"))
        && label.len() <= 64
        && label.bytes().all(|c| c.is_ascii_graphic());
    if ok {
        Ok(())
    } else {
        Err(LeekError::InvalidStorageLabel)
    }
}

/// An app's Storage Key, with the app, account and network it was derived
/// for. Wiped when dropped (or by [`forget`](Self::forget)); `Debug` does
/// not print it.
pub struct StorageKey {
    key: Zeroizing<[u8; 32]>,
    app_id: String,
    account: u32,
    network: Network,
}

impl fmt::Debug for StorageKey {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("StorageKey")
            .field("app_id", &self.app_id)
            .field("account", &self.account)
            .field("network", &self.network)
            .finish_non_exhaustive()
    }
}

impl StorageKey {
    /// A key from its 32 bytes: one the board released earlier and the app
    /// kept in the platform's secure storage ("remember on this device").
    pub fn from_bytes(key: [u8; 32], app_id: &str, account: u32, network: Network) -> Result<Self, LeekError> {
        let key = Zeroizing::new(key);
        validate_app_id(app_id)?;
        if account >= 1 << 31 {
            return Err(LeekError::WrongAccount);
        }
        Ok(StorageKey { key, app_id: app_id.into(), account, network })
    }

    /// The key computed from a seed, as the board does. For tests and for
    /// the draft's emergency recovery only: a seed on a general-purpose
    /// computer is a seed no longer secret.
    pub fn from_seed(seed: &[u8], app_id: &str, account: u32, network: Network) -> Result<Self, LeekError> {
        validate_app_id(app_id)?;
        if account >= 1 << 31 {
            return Err(LeekError::WrongAccount);
        }
        let mut tag = Vec::with_capacity(1 + app_id.len());
        tag.push(app_id.len() as u8);
        tag.extend_from_slice(app_id.as_bytes());
        let path = [
            PathElement::new(ChildIndex::hardened(network.coin_type()), &[]),
            PathElement::new(ChildIndex::hardened(account), &[]),
            PathElement::new(ChildIndex::hardened(0), &tag),
        ];
        let mut k = cryptovalue_from_subpath(CONTEXT, seed, STORAGE_ZIP_NUMBER, &path)
            .map_err(|_| LeekError::InvalidSeed)?;
        let mut key = Zeroizing::new([0u8; 32]);
        key.copy_from_slice(&k[..32]);
        k.zeroize();
        Ok(StorageKey { key, app_id: app_id.into(), account, network })
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

    /// The key itself, for an app that keeps it in the platform's secure
    /// storage. Never write it anywhere else, never send it to a server.
    pub fn to_bytes(&self) -> Zeroizing<[u8; 32]> {
        self.key.clone()
    }

    /// Wipes the key now rather than when dropped. Every later call fails
    /// with [`LeekError::StorageKeyForgotten`].
    pub fn forget(&mut self) {
        self.key.zeroize();
        self.app_id.clear();
    }

    fn live(&self) -> Result<(), LeekError> {
        if self.app_id.is_empty() {
            Err(LeekError::StorageKeyForgotten)
        } else {
            Ok(())
        }
    }

    fn subkey_unchecked(&self, label: &str) -> Zeroizing<[u8; 32]> {
        let hk = Hkdf::<Sha256>::new(Some(&[]), &self.key[..]);
        let mut out = Zeroizing::new([0u8; 32]);
        hk.expand_multi_info(&[SUBKEY_INFO, &[label.len() as u8], label.as_bytes()], &mut out[..])
            .expect("32 bytes is a valid HKDF-SHA256 length");
        out
    }

    /// The Storage Subkey for `label`: HKDF-SHA256 with an empty salt and
    /// info `"Zcash_AppStorage_v1" || [len(label)] || label`. `db` keys a
    /// page-level database cipher such as SQLCipher.
    pub fn subkey(&self, label: &str) -> Result<Zeroizing<[u8; 32]>, LeekError> {
        self.live()?;
        if label != LABEL_COMMIT {
            check_label(label)?;
        }
        Ok(self.subkey_unchecked(label))
    }

    /// The context bound to every sealed object as associated data, after
    /// its header: the app, coin type, account and label.
    fn context(&self, label: &str) -> Vec<u8> {
        let mut c = Vec::with_capacity(SUBKEY_INFO.len() + 75 + label.len());
        c.extend_from_slice(SUBKEY_INFO);
        c.push(self.app_id.len() as u8);
        c.extend_from_slice(self.app_id.as_bytes());
        c.extend_from_slice(&self.network.coin_type().to_le_bytes());
        c.extend_from_slice(&self.account.to_le_bytes());
        c.push(label.len() as u8);
        c.extend_from_slice(label.as_bytes());
        c
    }

    fn check_value(&self, salt: &[u8]) -> Hmac<Sha256> {
        let commit = self.subkey_unchecked(LABEL_COMMIT);
        let mut mac = <Hmac<Sha256> as Mac>::new_from_slice(&commit[..]).expect("HMAC takes any key length");
        mac.update(salt);
        mac
    }

    /// Encrypts `plaintext` for `label` (`db`, `ufvk` or `x-...`):
    ///
    /// `"ZASK" || 0x01 || 0x01 || salt(32) || check(32) || nonce(24) || ciphertext || tag(16)`
    ///
    /// with a random salt and nonce per call, `check = HMAC-SHA256(SubKey("commit"), salt)`,
    /// and XChaCha20-Poly1305 under `SubKey(label)` with associated data the
    /// header followed by the context (`"Zcash_AppStorage_v1" || [len] || app_id
    /// || coin_type (LE32) || account (LE32) || [len] || label`).
    pub fn seal(&self, label: &str, plaintext: &[u8]) -> Result<Vec<u8>, LeekError> {
        let mut rnd = [0u8; SALT_LEN + NONCE_LEN];
        getrandom::fill(&mut rnd).map_err(|_| LeekError::DeviceInternal)?;
        self.seal_with(label, plaintext, &rnd)
    }

    fn seal_with(&self, label: &str, plaintext: &[u8], rnd: &[u8; SALT_LEN + NONCE_LEN]) -> Result<Vec<u8>, LeekError> {
        self.live()?;
        check_label(label)?;
        let (salt, nonce) = rnd.split_at(SALT_LEN);
        let mut out = Vec::with_capacity(SEALED_HEADER_LEN + plaintext.len() + TAG_LEN);
        out.extend_from_slice(MAGIC);
        out.push(VERSION);
        out.push(ALG_XCHACHA20POLY1305);
        out.extend_from_slice(salt);
        out.extend_from_slice(&self.check_value(salt).finalize().into_bytes());
        out.extend_from_slice(nonce);
        let mut aad = out.clone();
        aad.extend_from_slice(&self.context(label));
        let key = self.subkey_unchecked(label);
        let cipher = XChaCha20Poly1305::new_from_slice(&key[..]).expect("32-byte key");
        let ct = cipher
            .encrypt(XNonce::from_slice(nonce), Payload { msg: plaintext, aad: &aad })
            .map_err(|_| LeekError::DeviceInternal)?;
        out.extend_from_slice(&ct);
        Ok(out)
    }

    /// Decrypts what [`seal`](Self::seal) made for the same app, account,
    /// network and label. [`LeekError::StorageKeyMismatch`] if the key check
    /// differs (another seed or passphrase, app, account or network), before
    /// anything is decrypted; [`LeekError::StorageCorrupted`] if the object
    /// is malformed or was changed (or sealed for another label).
    pub fn open(&self, label: &str, sealed: &[u8]) -> Result<Zeroizing<Vec<u8>>, LeekError> {
        self.live()?;
        check_label(label)?;
        if sealed.len() < SEALED_HEADER_LEN + TAG_LEN || &sealed[..4] != MAGIC {
            return Err(LeekError::StorageCorrupted);
        }
        if sealed[4] != VERSION || sealed[5] != ALG_XCHACHA20POLY1305 {
            return Err(LeekError::StorageCorrupted);
        }
        let salt = &sealed[6..6 + SALT_LEN];
        let check = &sealed[6 + SALT_LEN..6 + SALT_LEN + CHECK_LEN];
        let nonce = &sealed[6 + SALT_LEN + CHECK_LEN..SEALED_HEADER_LEN];
        // Constant time (hmac's verify).
        self.check_value(salt).verify_slice(check).map_err(|_| LeekError::StorageKeyMismatch)?;
        let mut aad = sealed[..SEALED_HEADER_LEN].to_vec();
        aad.extend_from_slice(&self.context(label));
        let key = self.subkey_unchecked(label);
        let cipher = XChaCha20Poly1305::new_from_slice(&key[..]).expect("32-byte key");
        cipher
            .decrypt(XNonce::from_slice(nonce), Payload { msg: &sealed[SEALED_HEADER_LEN..], aad: &aad })
            .map(Zeroizing::new)
            .map_err(|_| LeekError::StorageCorrupted)
    }

    /// [`seal`](Self::seal) under `db`, for a database kept as one blob.
    pub fn seal_db(&self, db: &[u8]) -> Result<Vec<u8>, LeekError> {
        self.seal(LABEL_DB, db)
    }

    pub fn open_db(&self, sealed: &[u8]) -> Result<Zeroizing<Vec<u8>>, LeekError> {
        self.open(LABEL_DB, sealed)
    }

    /// [`seal`](Self::seal) under `ufvk`.
    pub fn seal_ufvk(&self, ufvk: &str) -> Result<Vec<u8>, LeekError> {
        self.seal(LABEL_UFVK, ufvk.as_bytes())
    }

    pub fn open_ufvk(&self, sealed: &[u8]) -> Result<Zeroizing<String>, LeekError> {
        let mut pt = self.open(LABEL_UFVK, sealed)?;
        match String::from_utf8(std::mem::take(&mut *pt)) {
            Ok(s) => Ok(Zeroizing::new(s)),
            Err(e) => {
                e.into_bytes().zeroize();
                Err(LeekError::StorageCorrupted)
            }
        }
    }
}

/// `zGetStorageKey`: the board shows the app identifier, account, network
/// and pairing code, and releases the app's Storage Key only after OK there,
/// every time (see [`get_storage_key_with`]).
pub fn get_storage_key(device: &mut Device, app_id: &str, account: u32, network: Network) -> Result<StorageKey, LeekError> {
    get_storage_key_with(device, app_id, account, network, &|_| {})
}

/// [`get_storage_key`], telling `progress` when the board is asking the
/// user ("Unlock app data?"; it always asks). A no on the board is
/// [`LeekError::UserRejected`]; no answer in its 120 s window,
/// [`LeekError::UserTimeout`]. An invalid `app_id` is refused here,
/// [`LeekError::InvalidAppId`], before anything is sent.
pub fn get_storage_key_with(
    device: &mut Device,
    app_id: &str,
    account: u32,
    network: Network,
    progress: &dyn Fn(AccountProgress),
) -> Result<StorageKey, LeekError> {
    validate_app_id(app_id)?;
    if account >= 1 << 31 {
        return Err(LeekError::WrongAccount);
    }
    progress(AccountProgress::ConfirmOnDevice);
    let key = device
        .z_get_storage_key(app_id, account, network.as_str())
        .map_err(|e| from_device(&e, Ctx::Other))?;
    StorageKey::from_bytes(key, app_id, account, network)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn h(s: &str) -> Vec<u8> {
        hex::decode(s.replace(char::is_whitespace, "")).unwrap()
    }

    const SEED: [u8; 32] = {
        let mut s = [0u8; 32];
        let mut i = 0;
        while i < 32 {
            s[i] = i as u8;
            i += 1;
        }
        s
    };

    /// The draft's provisional vectors (ZIP_NUMBER 65535): key and the three
    /// subkeys.
    #[test]
    fn draft_vectors() {
        let vectors = [
            (Network::Main, 0, "xyz.leekwallet.zcash",
             "f43c678ee392cc4552daa3bde21c3cb15b416811dfdacf64e7aceca6114bd0d0",
             "abd9811ecdfd468b538e7f11714bdacca99e354ea622d31f0ee3b2cee3f85973",
             "8946f802ebbfdef22df61d9ce06f675bc04e7d2a4f5ceae1bd3d56be5646cc3f",
             "7af26c9ac8d79de53e6e565ca1eb26e249ff35685f4d2758aebfc97b1cac51dd"),
            (Network::Test, 0, "xyz.leekwallet.zcash",
             "854cfb1ecebae4b636bc37c1043370b16e658a79fd5402098ffd7d7ddea277fd",
             "b676000d114d159c7ab7b1a235987a06f467b9b8285833dab1e316eb41b7ff1a",
             "c48632e7b9153c11e0ff7a7336747b96594998e94cb91ff9fc58951e304c2e1c",
             "18bd65966b6b16adff3c4222ab7a22ba12b6c6fb3041f41ff0001de003801bdc"),
            (Network::Main, 1, "xyz.leekwallet.zcash",
             "26efbcd704c2229a500441fafc913394bd8b576a96593b77096aaf6fa2c894a7",
             "19bf70dc5659a1b0d6bc4be96662a5505ac6fabd13ce57c9c168a38efa376f59",
             "a248cfd3c6541d3f54abc7661fa2cb5f96ad793cc8e070a7e3cdaee70f8ee302",
             "c62953d52efc2e8bf1bc6427d28d0ff6206e5f9a99409cbd85a0493daa4c80b2"),
            (Network::Main, 0, "com.example.wallet",
             "3977f965125080aed7b50e09ebbfc3b1760732e0b24ac8bb039514c36587d944",
             "80eb7f02d41fc666d0dc279d44f7d73539b4af0c8e9883e084bdd11f595913fe",
             "3f6e8cd26fe5549a7c67bda380679357f8dd37a096febe67d9f6288c8f290c36",
             "398a63cebbe64f2a782cf3d339b73c5b56351d974add6fa9da135f0b3f0e4a61"),
        ];
        for (net, account, app, key, db, ufvk, commit) in vectors {
            let k = StorageKey::from_seed(&SEED, app, account, net).unwrap();
            assert_eq!(k.to_bytes().to_vec(), h(key), "{app} {account} {net:?}");
            assert_eq!(k.subkey("db").unwrap().to_vec(), h(db));
            assert_eq!(k.subkey("ufvk").unwrap().to_vec(), h(ufvk));
            assert_eq!(k.subkey("commit").unwrap().to_vec(), h(commit));
        }
    }

    /// The full 64-byte cryptovalue of vector 1, against the draft's
    /// expansion of the ZIP 32 formulas written out by hand.
    #[test]
    fn draft_expansion() {
        use blake2b_simd::Params;
        let prf = |k: &[u8], t: &[&[u8]]| {
            let mut s = Params::new().hash_length(64).personal(b"Zcash_ExpandSeed").to_state();
            s.update(k);
            for p in t {
                s.update(p);
            }
            s.finalize().as_bytes().to_vec()
        };
        let mut i = Params::new()
            .hash_length(64)
            .personal(b"ZIPRegistered_KD")
            .to_state()
            .update(&[14])
            .update(b"AppStorageKeys")
            .update(&[32])
            .update(&SEED)
            .finalize()
            .as_bytes()
            .to_vec();
        for idx in [65535u32, 133, 0] {
            let (sk, c) = i.split_at(32);
            i = prf(c, &[&[0xAC], sk, &(idx | 1 << 31).to_le_bytes()]);
        }
        let (sk, c) = i.split_at(32);
        let tag = h("1478797a2e6c65656b77616c6c65742e7a63617368");
        let k = prf(c, &[&[0xAC], sk, &(1u32 << 31).to_le_bytes(), &[1], &tag]);
        assert_eq!(k, h("f43c678ee392cc4552daa3bde21c3cb15b416811dfdacf64e7aceca6114bd0d0
                         65a7e0e28f8c0f992b29877519e7c03565f49f0a87818969a8dd95a8dcb384de"));
    }

    #[test]
    fn app_ids() {
        for ok in ["a.b", "xyz.leekwallet.zcash", "com.example.wallet", "0.9", "a-.b_", "io.x1.y2"] {
            assert_eq!(validate_app_id(ok), Ok(()), "{ok}");
        }
        let long_ok = format!("{}.b", "a".repeat(62));
        assert_eq!(validate_app_id(&long_ok), Ok(()));
        let too_long = format!("{}.b", "a".repeat(63));
        for bad in [
            "", "a", "ab", "abc", "a.", ".a", "a..b", "Com.example", "com.Example", "com.example.", "-a.b", "a.-b",
            "_a.b", "a.b c", "a/b.c", "a.b\n", "com.exámple", "a.b\0c", too_long.as_str(),
        ] {
            assert_eq!(validate_app_id(bad), Err(LeekError::InvalidAppId), "{bad:?}");
        }
        assert_eq!(StorageKey::from_seed(&SEED, "Bad.id", 0, Network::Main).unwrap_err(), LeekError::InvalidAppId);
    }

    #[test]
    fn seal_open_round_trip() {
        let k = StorageKey::from_seed(&SEED, "xyz.leekwallet.zcash", 0, Network::Test).unwrap();
        let db = b"SQLite format 3\0 and the rest of the wallet".to_vec();
        let sealed = k.seal_db(&db).unwrap();
        assert_eq!(sealed.len(), SEALED_HEADER_LEN + db.len() + TAG_LEN);
        assert_eq!(&sealed[..6], b"ZASK\x01\x01");
        assert_eq!(&k.open_db(&sealed).unwrap()[..], &db[..]);
        let ufvk = "uviewtest1example";
        assert_eq!(&*k.open_ufvk(&k.seal_ufvk(ufvk).unwrap()).unwrap(), ufvk);
        assert_eq!(&k.open("x-contacts", &k.seal("x-contacts", b"").unwrap()).unwrap()[..], b"");
        // Fresh salt and nonce each time: two seals of the same data differ.
        let again = k.seal_db(&db).unwrap();
        assert_ne!(sealed[6..38], again[6..38]);
        assert_ne!(sealed[SEALED_HEADER_LEN..], again[SEALED_HEADER_LEN..]);
        // The key from its bytes (remembered mode) opens the same data.
        let k2 = StorageKey::from_bytes(*k.to_bytes(), "xyz.leekwallet.zcash", 0, Network::Test).unwrap();
        assert_eq!(&k2.open_db(&sealed).unwrap()[..], &db[..]);
    }

    /// Deterministic: the same randomness gives the same bytes, so the format
    /// is pinned (a change here breaks every stored file).
    #[test]
    fn sealed_format_is_stable() {
        let k = StorageKey::from_seed(&SEED, "xyz.leekwallet.zcash", 0, Network::Main).unwrap();
        let mut rnd = [0u8; SALT_LEN + NONCE_LEN];
        for (i, b) in rnd.iter_mut().enumerate() {
            *b = 0x40 + i as u8;
        }
        let sealed = k.seal_with("ufvk", b"uview1", &rnd).unwrap();
        assert_eq!(&sealed[6..38], &rnd[..32]);
        assert_eq!(&sealed[70..94], &rnd[32..]);
        let mut mac = <Hmac<Sha256> as Mac>::new_from_slice(&k.subkey("commit").unwrap()[..]).unwrap();
        mac.update(&rnd[..32]);
        assert_eq!(&sealed[38..70], &mac.finalize().into_bytes()[..]);
        assert_eq!(&*k.open_ufvk(&sealed).unwrap(), "uview1");
        assert_eq!(sealed, k.seal_with("ufvk", b"uview1", &rnd).unwrap());
    }

    #[test]
    fn tampering_and_wrong_keys() {
        let k = StorageKey::from_seed(&SEED, "xyz.leekwallet.zcash", 0, Network::Main).unwrap();
        let sealed = k.seal_db(b"wallet").unwrap();
        // Every single-bit flip is refused, header or body.
        for i in 0..sealed.len() {
            let mut t = sealed.clone();
            t[i] ^= 0x01;
            let e = k.open_db(&t).unwrap_err();
            let want = if (38..70).contains(&i) || (6..38).contains(&i) {
                LeekError::StorageKeyMismatch
            } else {
                LeekError::StorageCorrupted
            };
            assert_eq!(e, want, "byte {i}");
        }
        assert_eq!(k.open_db(&sealed[..sealed.len() - 1]).unwrap_err(), LeekError::StorageCorrupted);
        assert_eq!(k.open_db(&sealed[..SEALED_HEADER_LEN]).unwrap_err(), LeekError::StorageCorrupted);
        assert_eq!(k.open_db(&[]).unwrap_err(), LeekError::StorageCorrupted);
        // Another label: the key check passes (one key per object), the AEAD does not.
        assert_eq!(k.open(LABEL_UFVK, &sealed).unwrap_err(), LeekError::StorageCorrupted);
        // Another app, account, network or seed: refused by the key check.
        for other in [
            StorageKey::from_seed(&SEED, "com.example.wallet", 0, Network::Main).unwrap(),
            StorageKey::from_seed(&SEED, "xyz.leekwallet.zcash", 1, Network::Main).unwrap(),
            StorageKey::from_seed(&SEED, "xyz.leekwallet.zcash", 0, Network::Test).unwrap(),
            StorageKey::from_seed(&[7u8; 64], "xyz.leekwallet.zcash", 0, Network::Main).unwrap(),
        ] {
            assert_eq!(other.open_db(&sealed).unwrap_err(), LeekError::StorageKeyMismatch);
        }
        // The same key bytes under another context: the check passes, the
        // associated data does not.
        let moved = StorageKey::from_bytes(*k.to_bytes(), "xyz.leekwallet.zcash", 1, Network::Main).unwrap();
        assert_eq!(moved.open_db(&sealed).unwrap_err(), LeekError::StorageCorrupted);
    }

    #[test]
    fn labels_and_forget() {
        let mut k = StorageKey::from_seed(&SEED, "xyz.leekwallet.zcash", 0, Network::Main).unwrap();
        assert_eq!(k.seal("commit", b"x").unwrap_err(), LeekError::InvalidStorageLabel);
        assert_eq!(k.seal("cache", b"x").unwrap_err(), LeekError::InvalidStorageLabel);
        assert_eq!(k.seal("x-", b"x").map(|_| ()), Ok(()));
        assert_eq!(k.seal("x-a b", b"x").unwrap_err(), LeekError::InvalidStorageLabel);
        assert_eq!(k.subkey(&format!("x-{}", "a".repeat(63))).unwrap_err(), LeekError::InvalidStorageLabel);
        assert!(!format!("{k:?}").contains("f43c"));
        let sealed = k.seal_db(b"x").unwrap();
        k.forget();
        assert_eq!(*k.to_bytes(), [0u8; 32]);
        assert_eq!(k.open_db(&sealed).unwrap_err(), LeekError::StorageKeyForgotten);
        assert_eq!(k.subkey("db").unwrap_err(), LeekError::StorageKeyForgotten);
    }
}
