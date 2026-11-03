//! # leek-zcash-ffi: UniFFI bindings for the LeekWallet Zcash kit
//!
//! The [`leek_zcash`] API for Kotlin and Swift, byte-oriented: PCZTs,
//! `.zsig` files, account records and signed results cross the boundary as
//! bytes, and every call returns one [`LeekError`] whose `code` is
//! `leek_zcash::LeekError::code` (stable across releases and bindings) and
//! whose `recovery` says what to do.
//!
//! The host app owns the link: it implements [`FrameTransport`] over USB OTG
//! or Bluetooth LE (as the Android app's `UsbIo` and `BleIo` do) and hands
//! it to [`LeekDevice::new`]; framing, pairing, the encrypted session and the
//! Zcash calls run here. Every device call blocks: call it off the main
//! thread. [`LeekDevice::cancel`] (from any thread) interrupts the running
//! call between transport reads.

use std::fmt;
use std::sync::{Arc, Mutex, MutexGuard};
use std::time::{Duration, Instant};

use leek_transport_serial::{encode_frame, FrameDecoder, TransportError};
use leek_zcash as lz;
use leek_zcash::leek_device::device::{Device, FramePort};
use leek_zcash::pczt::Pczt;

uniffi::setup_scaffolding!();

// ---------------------------------------------------------------- errors

/// What a wallet can do about an error (`leek_zcash::Recovery`).
#[derive(Clone, Copy, Debug, PartialEq, Eq, uniffi::Enum)]
pub enum Recovery {
    /// Ask again over the same link and session.
    RetrySameLink,
    /// Reconnect and pair again.
    Reconnect,
    /// Retrying the same request cannot succeed.
    DoNotRetry,
}

impl From<lz::Recovery> for Recovery {
    fn from(r: lz::Recovery) -> Self {
        match r {
            lz::Recovery::RetrySameLink => Recovery::RetrySameLink,
            lz::Recovery::Reconnect => Recovery::Reconnect,
            lz::Recovery::DoNotRetry => Recovery::DoNotRetry,
        }
    }
}

/// Every error, by the hundreds digit of its stable `code`.
///
/// `code` is `leek_zcash::LeekError::code` (the README lists them),
/// `recovery` its `Recovery`, `reason` a fixed sentence per code that never
/// carries an amount, an address, a key or PCZT bytes, so it can go to logs.
/// `device_code` is the firmware's code when the board refused with one this
/// version does not know (`code` 900).
#[derive(Clone, Debug, PartialEq, Eq, uniffi::Error)]
pub enum LeekError {
    /// 1xx: the link (not found, permission, dropped, timeout, cancelled).
    Link { code: u16, recovery: Recovery, reason: String },
    /// 2xx: the session and the board's state (pairing, locked, busy, ...).
    Session { code: u16, recovery: Recovery, reason: String },
    /// 3xx: the user said no, or did not answer, on the board.
    User { code: u16, recovery: Recovery, reason: String },
    /// 4xx: account and network.
    Account { code: u16, recovery: Recovery, reason: String },
    /// 5xx: the PCZT (unsupported, too large, does not parse, ...).
    Pczt { code: u16, recovery: Recovery, reason: String },
    /// 6xx: the signatures (`.zsig` for other bytes, a bad signature, ...).
    Signature { code: u16, recovery: Recovery, reason: String },
    /// 7xx: stored app data (an invalid app identifier, data sealed for
    /// another key, damaged data, a forgotten Storage Key).
    Storage { code: u16, recovery: Recovery, reason: String },
    /// 9xx: a firmware error code this version does not know.
    Device { code: u16, recovery: Recovery, reason: String, device_code: u16 },
}

impl LeekError {
    pub fn code(&self) -> u16 {
        match self {
            LeekError::Link { code, .. }
            | LeekError::Session { code, .. }
            | LeekError::User { code, .. }
            | LeekError::Account { code, .. }
            | LeekError::Pczt { code, .. }
            | LeekError::Signature { code, .. }
            | LeekError::Storage { code, .. }
            | LeekError::Device { code, .. } => *code,
        }
    }

    pub fn recovery(&self) -> Recovery {
        match self {
            LeekError::Link { recovery, .. }
            | LeekError::Session { recovery, .. }
            | LeekError::User { recovery, .. }
            | LeekError::Account { recovery, .. }
            | LeekError::Pczt { recovery, .. }
            | LeekError::Signature { recovery, .. }
            | LeekError::Storage { recovery, .. }
            | LeekError::Device { recovery, .. } => *recovery,
        }
    }

    fn reason(&self) -> &str {
        match self {
            LeekError::Link { reason, .. }
            | LeekError::Session { reason, .. }
            | LeekError::User { reason, .. }
            | LeekError::Account { reason, .. }
            | LeekError::Pczt { reason, .. }
            | LeekError::Signature { reason, .. }
            | LeekError::Storage { reason, .. }
            | LeekError::Device { reason, .. } => reason,
        }
    }
}

impl fmt::Display for LeekError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{} (LeekError {})", self.reason(), self.code())
    }
}

impl std::error::Error for LeekError {}

impl From<lz::LeekError> for LeekError {
    fn from(e: lz::LeekError) -> Self {
        let code = e.code();
        let recovery = e.recovery().into();
        let reason = e.to_string();
        match (code / 100, e) {
            (_, lz::LeekError::Device { code: device_code }) => {
                LeekError::Device { code, recovery, reason, device_code }
            }
            (1, _) => LeekError::Link { code, recovery, reason },
            (2, _) => LeekError::Session { code, recovery, reason },
            (3, _) => LeekError::User { code, recovery, reason },
            (4, _) => LeekError::Account { code, recovery, reason },
            (6, _) => LeekError::Signature { code, recovery, reason },
            (7, _) => LeekError::Storage { code, recovery, reason },
            _ => LeekError::Pczt { code, recovery, reason },
        }
    }
}

type Result<T, E = LeekError> = std::result::Result<T, E>;

fn parse_pczt(bytes: &[u8]) -> Result<Pczt> {
    Pczt::parse(bytes).map_err(|e| {
        match e {
            lz::pczt::ParseError::UnknownVersion(_) => lz::LeekError::PcztVersion,
            _ => lz::LeekError::Parse,
        }
        .into()
    })
}

fn serialize(pczt: Pczt) -> Result<Vec<u8>> {
    pczt.serialize().map_err(|_| lz::LeekError::Parse.into())
}

fn bytes32(v: &[u8]) -> Result<[u8; 32]> {
    v.try_into().map_err(|_| lz::LeekError::Parse.into())
}

// ---------------------------------------------------------------- plain types

/// A Zcash network the board serves.
#[derive(Clone, Copy, Debug, PartialEq, Eq, uniffi::Enum)]
pub enum Network {
    Main,
    Test,
}

impl From<Network> for lz::Network {
    fn from(n: Network) -> Self {
        match n {
            Network::Main => lz::Network::Main,
            Network::Test => lz::Network::Test,
        }
    }
}

impl From<lz::Network> for Network {
    fn from(n: lz::Network) -> Self {
        match n {
            lz::Network::Main => Network::Main,
            lz::Network::Test => Network::Test,
        }
    }
}

/// The signer view sent to the board: `Full` (every firmware) or `Compact`
/// (about half the bytes, worth it over Bluetooth).
#[derive(Clone, Copy, Debug, PartialEq, Eq, uniffi::Enum)]
pub enum SignerView {
    Full,
    Compact,
}

impl From<SignerView> for lz::SignerView {
    fn from(v: SignerView) -> Self {
        match v {
            SignerView::Full => lz::SignerView::Full,
            SignerView::Compact => lz::SignerView::Compact,
        }
    }
}

/// An account's identity, and its UFVK when the wallet has it (then
/// `preflight` also checks every spend derives from it).
#[derive(Clone, Debug, PartialEq, Eq, uniffi::Record)]
pub struct AccountMeta {
    /// 32 bytes: the ZIP 32 seed fingerprint.
    pub seed_fingerprint: Vec<u8>,
    pub index: u32,
    pub network: Network,
    pub ufvk: Option<String>,
}

impl TryFrom<AccountMeta> for lz::AccountMeta {
    type Error = LeekError;
    fn try_from(a: AccountMeta) -> Result<Self> {
        Ok(lz::AccountMeta {
            seed_fingerprint: bytes32(&a.seed_fingerprint)?,
            index: a.index,
            network: a.network.into(),
            ufvk: a.ufvk,
        })
    }
}

/// A board account, as `zGetAccount` exports it.
#[derive(Clone, Debug, PartialEq, Eq, uniffi::Record)]
pub struct Account {
    pub index: u32,
    pub network: Network,
    /// 32 bytes.
    pub seed_fingerprint: Vec<u8>,
    /// Orchard + P2PKH UFVK.
    pub ufvk: String,
    pub ua: String,
    pub t_address: String,
    pub birthday: Option<u32>,
    /// What to import it with: `KEY_SOURCE` ("leekwallet").
    pub key_source: String,
}

impl From<lz::Account> for Account {
    fn from(a: lz::Account) -> Self {
        Account {
            index: a.index,
            network: a.network.into(),
            seed_fingerprint: a.seed_fingerprint.to_vec(),
            ufvk: a.ufvk,
            ua: a.ua,
            t_address: a.t_address,
            birthday: a.birthday,
            key_source: lz::KEY_SOURCE.into(),
        }
    }
}

/// What the board's firmware and signer support.
#[derive(Clone, Debug, PartialEq, Eq, uniffi::Record)]
pub struct Features {
    pub protocol: u64,
    pub firmware: Option<String>,
    pub model: Option<String>,
    pub spend_pools: Vec<String>,
    pub branch_ids: Vec<u32>,
    pub tx_versions: Vec<u32>,
    pub max_pczt_bytes: u64,
    pub max_transparent_inputs: u64,
    pub show_address: bool,
    /// The ZIP number of the board's App Storage Keys, or `None` without
    /// `zGetStorageKey`; offer "unlock with the board" only when it equals
    /// `storage_zip_number()`.
    pub storage_key: Option<u16>,
}

impl From<lz::Features> for Features {
    fn from(f: lz::Features) -> Self {
        Features {
            protocol: f.protocol,
            firmware: f.firmware,
            model: f.model,
            spend_pools: f.spend_pools,
            branch_ids: f.branch_ids,
            tx_versions: f.tx_versions,
            max_pczt_bytes: f.max_pczt_bytes as u64,
            max_transparent_inputs: f.max_transparent_inputs as u64,
            show_address: f.show_address,
            storage_key: f.storage_key,
        }
    }
}

impl From<Features> for lz::Features {
    fn from(f: Features) -> Self {
        lz::Features {
            protocol: f.protocol,
            firmware: f.firmware,
            model: f.model,
            spend_pools: f.spend_pools,
            branch_ids: f.branch_ids,
            tx_versions: f.tx_versions,
            max_pczt_bytes: usize::try_from(f.max_pczt_bytes).unwrap_or(usize::MAX),
            max_transparent_inputs: usize::try_from(f.max_transparent_inputs).unwrap_or(usize::MAX),
            show_address: f.show_address,
            storage_key: f.storage_key,
        }
    }
}

/// The result of an on-board address check.
#[derive(Clone, Copy, Debug, PartialEq, Eq, uniffi::Enum)]
pub enum AddressCheck {
    Verified,
    Rejected,
    NotOurs,
    Unsupported,
}

impl From<lz::AddressCheck> for AddressCheck {
    fn from(c: lz::AddressCheck) -> Self {
        match c {
            lz::AddressCheck::Verified => AddressCheck::Verified,
            lz::AddressCheck::Rejected => AddressCheck::Rejected,
            lz::AddressCheck::NotOurs => AddressCheck::NotOurs,
            lz::AddressCheck::Unsupported => AddressCheck::Unsupported,
        }
    }
}

/// What the board signed.
///
/// Persist `record` (or `signed_pczt` and `txid`) before broadcasting; read
/// it back with [`sign_result_from_bytes`].
#[derive(Clone, Debug, PartialEq, Eq, uniffi::Record)]
pub struct SignResult {
    /// The wallet's PCZT with the board's signatures applied and verified:
    /// hand it to the wallet's Keystone path (prove, extract).
    pub signed_pczt: Vec<u8>,
    /// 32 bytes: the shielded sighash the user reviewed on the board.
    pub sighash: Vec<u8>,
    /// The signatures as a `.zsig` file.
    pub zsig: Vec<u8>,
    /// How many signatures the board returned.
    pub signature_count: u32,
    /// 32 bytes, internal order: final before proving (ZIP 244).
    pub txid: Vec<u8>,
    /// The txid as explorers and lightwalletd show it.
    pub txid_hex: String,
    /// All of the above as one record (`SignResult::to_bytes`).
    pub record: Vec<u8>,
}

impl TryFrom<lz::SignResult> for SignResult {
    type Error = LeekError;
    fn try_from(r: lz::SignResult) -> Result<Self> {
        Ok(SignResult {
            record: r.to_bytes()?,
            txid_hex: r.txid_hex(),
            txid: r.txid.to_vec(),
            signature_count: r.signatures.len() as u32,
            sighash: r.sighash.to_vec(),
            zsig: r.zsig,
            signed_pczt: serialize(r.signed_pczt)?,
        })
    }
}

/// **Experimental.** What to write to the SD card (`prepare_sd_sign`).
#[derive(Clone, Debug, PartialEq, Eq, uniffi::Record)]
pub struct SdRequest {
    /// Where to write `bytes`, from the card's root: "LEEKZ/unsigned.pczt".
    pub file_name: String,
    /// Where the board writes its answer: "LEEKZ/signed.zsig" (remove any
    /// old one first).
    pub zsig_file_name: String,
    /// The signer view: keep it until the `.zsig` comes back.
    pub bytes: Vec<u8>,
    /// 32 bytes: SHA-256 of `bytes`, the hash the `.zsig` must name.
    pub sent_hash: Vec<u8>,
    /// The account and network to select on the board before signing.
    pub account: u32,
    pub network: Network,
}

/// One account of an [`AccountRecord`].
#[derive(Clone, Debug, PartialEq, Eq, uniffi::Record)]
pub struct AccountEntry {
    pub ufvk: String,
    pub index: u32,
    pub name: Option<String>,
    pub birthday: Option<u32>,
}

/// One seed's accounts, as stored or passed on (`leek-zcash-accounts`).
#[derive(Clone, Debug, PartialEq, Eq, uniffi::Record)]
pub struct AccountRecord {
    /// 32 bytes.
    pub seed_fingerprint: Vec<u8>,
    pub accounts: Vec<AccountEntry>,
}

/// How a mined transaction compares with the one the wallet extracted.
#[derive(Clone, Copy, Debug, PartialEq, Eq, uniffi::Enum)]
pub enum MinedCheck {
    Identical,
    SameEffectsOtherAuth,
    Different,
}

// ---------------------------------------------------------------- byte API

/// Host-side checks before anything goes to the board. `features`: the
/// board's, or `None` for the current firmware's.
#[uniffi::export]
pub fn preflight(pczt: Vec<u8>, account: AccountMeta, features: Option<Features>) -> Result<()> {
    let f = features.map(Into::into).unwrap_or_default();
    Ok(lz::preflight(&parse_pczt(&pczt)?, &account.try_into()?, &f)?)
}

/// Fills in the ZIP 32 / BIP 32 derivations some wallets leave out.
#[uniffi::export]
pub fn prepare_for_signer(pczt: Vec<u8>, ufvk: String, seed_fingerprint: Vec<u8>, index: u32) -> Result<Vec<u8>> {
    let p = lz::prepare_for_signer(parse_pczt(&pczt)?, &ufvk, &bytes32(&seed_fingerprint)?, index)?;
    serialize(p)
}

/// What goes to the board: upstream's `redact_pczt_for_signer`.
#[uniffi::export]
pub fn signer_view_bytes(pczt: Vec<u8>, view: SignerView) -> Result<Vec<u8>> {
    Ok(lz::signer_view_bytes(&parse_pczt(&pczt)?, view.into())?)
}

/// A `.zsig` (from any transport) into the wallet's PCZT, every signature
/// verified: the signed PCZT.
#[uniffi::export]
pub fn apply_zsig(pczt: Vec<u8>, sent: Vec<u8>, zsig: Vec<u8>) -> Result<Vec<u8>> {
    serialize(lz::apply_zsig(&parse_pczt(&pczt)?, &sent, &zsig)?)
}

/// The same UA without the Sapling receiver the board cannot check.
#[uniffi::export]
pub fn to_checkable_ua(ua: String) -> Result<String> {
    Ok(lz::to_checkable_ua(&ua)?)
}

// ---------------------------------------------------------------- stored app data

/// An app's Storage Key (docs/zcash/zip-draft-app-storage-keys.md), from
/// [`LeekDevice::get_storage_key`] or, for an app that remembers it in the
/// platform's secure storage, [`StorageKey::from_bytes`].
///
/// Seal the wallet database and the UFVK with it, keep only the sealed
/// bytes, and call [`forget`](Self::forget) when the app closes or locks:
/// the JVM does not promise when an object is collected, and the key is
/// wiped only then or on `forget`.
#[derive(uniffi::Object)]
pub struct StorageKey {
    inner: Mutex<lz::StorageKey>,
}

impl StorageKey {
    fn wrap(k: lz::StorageKey) -> Arc<Self> {
        Arc::new(StorageKey { inner: Mutex::new(k) })
    }
}

#[uniffi::export]
impl StorageKey {
    /// A key the app kept from an earlier release (remembered mode).
    #[uniffi::constructor]
    pub fn from_bytes(key: Vec<u8>, app_id: String, account: u32, network: Network) -> Result<Arc<Self>> {
        let mut k = bytes32(&key)?;
        let r = lz::StorageKey::from_bytes(k, &app_id, account, network.into());
        k.fill(0);
        Ok(Self::wrap(r?))
    }

    pub fn app_id(&self) -> String {
        lock(&self.inner).app_id().to_string()
    }

    pub fn account(&self) -> u32 {
        lock(&self.inner).account()
    }

    pub fn network(&self) -> Network {
        lock(&self.inner).network().into()
    }

    /// The 32 key bytes, for the platform's secure storage only (Android
    /// Keystore-wrapped, Apple Keychain). Never anywhere else.
    pub fn key_bytes(&self) -> Vec<u8> {
        lock(&self.inner).to_bytes().to_vec()
    }

    /// The `db` subkey, for a page-level database cipher such as SQLCipher.
    pub fn db_key(&self) -> Result<Vec<u8>> {
        Ok(lock(&self.inner).subkey(lz::LABEL_DB)?.to_vec())
    }

    /// The subkey for `label` (`db`, `ufvk`, `commit` or `x-...`).
    pub fn subkey(&self, label: String) -> Result<Vec<u8>> {
        Ok(lock(&self.inner).subkey(&label)?.to_vec())
    }

    pub fn seal(&self, label: String, plaintext: Vec<u8>) -> Result<Vec<u8>> {
        Ok(lock(&self.inner).seal(&label, &plaintext)?)
    }

    pub fn open(&self, label: String, sealed: Vec<u8>) -> Result<Vec<u8>> {
        Ok(lock(&self.inner).open(&label, &sealed)?.to_vec())
    }

    pub fn seal_db(&self, db: Vec<u8>) -> Result<Vec<u8>> {
        Ok(lock(&self.inner).seal_db(&db)?)
    }

    pub fn open_db(&self, sealed: Vec<u8>) -> Result<Vec<u8>> {
        Ok(lock(&self.inner).open_db(&sealed)?.to_vec())
    }

    pub fn seal_ufvk(&self, ufvk: String) -> Result<Vec<u8>> {
        Ok(lock(&self.inner).seal_ufvk(&ufvk)?)
    }

    pub fn open_ufvk(&self, sealed: Vec<u8>) -> Result<String> {
        Ok(lock(&self.inner).open_ufvk(&sealed)?.to_string())
    }

    /// Wipes the key now. Every later call fails with code 704.
    pub fn forget(&self) {
        lock(&self.inner).forget();
    }
}

/// Whether `app_id` is an App identifier the board accepts.
#[uniffi::export]
pub fn validate_app_id(app_id: String) -> Result<()> {
    Ok(lz::validate_app_id(&app_id)?)
}

/// The ZIP number this SDK derives Storage Keys under (a placeholder until
/// the draft is assigned one); compare with `Features.storage_key`.
#[uniffi::export]
pub fn storage_zip_number() -> u16 {
    lz::STORAGE_ZIP_NUMBER
}

/// **Experimental.** The file the board signs from the SD card.
#[uniffi::export]
pub fn prepare_sd_sign(pczt: Vec<u8>, account: AccountMeta, view: SignerView) -> Result<SdRequest> {
    let r = lz::prepare_sd_sign(&parse_pczt(&pczt)?, &account.try_into()?, view.into())?;
    Ok(SdRequest {
        file_name: r.file_name.into(),
        zsig_file_name: lz::SD_SIGNED_ZSIG.into(),
        sent_hash: r.sent_hash.to_vec(),
        bytes: r.bytes,
        account: r.account,
        network: r.network.into(),
    })
}

/// **Experimental.** The board's `LEEKZ/signed.zsig` into a [`SignResult`].
/// `sent` is `SdRequest.bytes`.
#[uniffi::export]
pub fn finish_sd_sign(pczt: Vec<u8>, sent: Vec<u8>, zsig: Vec<u8>) -> Result<SignResult> {
    lz::finish_sd_sign(&parse_pczt(&pczt)?, &sent, &zsig)?.try_into()
}

/// Reads `SignResult.record` back (txid and sighash re-checked).
#[uniffi::export]
pub fn sign_result_from_bytes(record: Vec<u8>) -> Result<SignResult> {
    lz::SignResult::from_bytes(&record)?.try_into()
}

/// The extracted transaction against the one fetched after mining.
#[uniffi::export]
pub fn check_mined(signed_tx: Vec<u8>, mined_tx: Vec<u8>) -> Result<MinedCheck> {
    Ok(match lz::check_mined(&signed_tx, &mined_tx)? {
        lz::MinedCheck::Identical => MinedCheck::Identical,
        lz::MinedCheck::SameEffectsOtherAuth => MinedCheck::SameEffectsOtherAuth,
        lz::MinedCheck::Different => MinedCheck::Different,
    })
}

/// A serialized transaction's txid (32 bytes, internal order).
#[uniffi::export]
pub fn tx_txid(tx: Vec<u8>) -> Result<Vec<u8>> {
    Ok(lz::tx_txid(&tx)?.to_vec())
}

/// The account record's CBOR body.
#[uniffi::export]
pub fn encode_account_record(record: AccountRecord) -> Result<Vec<u8>> {
    let r = lz::AccountRecord {
        seed_fingerprint: bytes32(&record.seed_fingerprint)?,
        accounts: record
            .accounts
            .into_iter()
            .map(|a| lz::AccountEntry { ufvk: a.ufvk, index: a.index, name: a.name, birthday: a.birthday })
            .collect(),
    };
    Ok(r.encode()?)
}

#[uniffi::export]
pub fn decode_account_record(bytes: Vec<u8>) -> Result<AccountRecord> {
    let r = lz::AccountRecord::decode(&bytes)?;
    Ok(AccountRecord {
        seed_fingerprint: r.seed_fingerprint.to_vec(),
        accounts: r
            .accounts
            .into_iter()
            .map(|a| AccountEntry { ufvk: a.ufvk, index: a.index, name: a.name, birthday: a.birthday })
            .collect(),
    })
}

/// The type name of the account record: "leek-zcash-accounts".
#[uniffi::export]
pub fn account_record_type() -> String {
    lz::ACCOUNT_RECORD_UR_TYPE.into()
}

// ---------------------------------------------------------------- the link

/// Why the host's transport failed.
#[derive(Clone, Debug, PartialEq, Eq, uniffi::Error)]
pub enum TransportFailure {
    /// No board on this link (unplugged, out of range).
    NotFound,
    /// The platform refused access (USB permission, Bluetooth permission).
    PermissionDenied,
    /// The link dropped.
    Disconnected,
    /// The host's callback threw something else.
    Unexpected,
}

impl fmt::Display for TransportFailure {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        fmt::Debug::fmt(self, f)
    }
}

impl std::error::Error for TransportFailure {}

impl From<uniffi::UnexpectedUniFFICallbackError> for TransportFailure {
    fn from(_: uniffi::UnexpectedUniFFICallbackError) -> Self {
        TransportFailure::Unexpected
    }
}

/// How the bytes over a [`FrameTransport`] are delimited.
#[derive(Clone, Copy, Debug, PartialEq, Eq, uniffi::Enum)]
pub enum LinkKind {
    /// A byte stream (USB CDC): `send` writes raw bytes, `recv` returns
    /// whatever arrived; framing ("LK" sync, length, CRC) is done here.
    /// Like the Android app's `UsbIo`.
    Stream,
    /// Whole frames (Bluetooth LE, where GATT delimits them): each `send`
    /// and each `recv` is one frame, a big-endian u16 length (type byte
    /// included), the type, the payload. Like `BleIo`.
    Frames,
}

/// The link to the board, implemented by the host app (Kotlin, Swift).
///
/// Calls come from the thread running the device call.
#[uniffi::export(with_foreign)]
pub trait FrameTransport: Send + Sync {
    /// Writes `bytes` (raw stream bytes, or one whole frame).
    fn send(&self, bytes: Vec<u8>) -> Result<(), TransportFailure>;
    /// Waits up to `timeout_ms` for bytes (or one whole frame); an empty
    /// result means nothing arrived in time.
    fn recv(&self, timeout_ms: u32) -> Result<Vec<u8>, TransportFailure>;
}

/// Receives the six-digit pairing code to compare with the board's screen.
#[uniffi::export(with_foreign)]
pub trait PairingListener: Send + Sync {
    fn on_pairing_code(&self, code: String);
}

/// Where `sign_pczt` is.
#[derive(Clone, Copy, Debug, PartialEq, Eq, uniffi::Enum)]
pub enum SignProgress {
    Checking,
    Uploading { sent: u64, total: u64 },
    /// The user reviews the transaction on the board.
    ReviewOnDevice,
    Applying,
}

/// Receives [`SignProgress`] steps. It must return quickly and not throw.
#[uniffi::export(with_foreign)]
pub trait SignListener: Send + Sync {
    fn on_progress(&self, progress: SignProgress);
}

/// The longest a single transport read blocks, so a cancel is seen soon.
const SLICE: Duration = Duration::from_millis(100);

/// State shared by a device and its port: the current call's cancel token
/// and the transport's last failure, which is more precise than what
/// `leek-device` reports.
#[derive(Default)]
struct Shared {
    token: Mutex<lz::CancelToken>,
    failure: Mutex<Option<lz::LeekError>>,
}

fn lock<T>(m: &Mutex<T>) -> MutexGuard<'_, T> {
    m.lock().unwrap_or_else(|p| p.into_inner())
}

impl Shared {
    fn cancelled(&self) -> bool {
        lock(&self.token).is_cancelled()
    }

    fn fail(&self, e: lz::LeekError) -> TransportError {
        *lock(&self.failure) = Some(e);
        TransportError::Serial(e.to_string())
    }
}

struct ForeignPort {
    t: Arc<dyn FrameTransport>,
    link: LinkKind,
    decoder: FrameDecoder,
    shared: Arc<Shared>,
}

fn failure(f: TransportFailure) -> lz::LeekError {
    match f {
        TransportFailure::NotFound => lz::LeekError::NotFound,
        TransportFailure::PermissionDenied => lz::LeekError::PermissionDenied,
        TransportFailure::Disconnected | TransportFailure::Unexpected => lz::LeekError::Disconnected,
    }
}

impl FramePort for ForeignPort {
    fn send(&mut self, frame_type: u8, payload: &[u8]) -> Result<(), TransportError> {
        if self.shared.cancelled() {
            return Err(self.shared.fail(lz::LeekError::Cancelled));
        }
        let bytes = match self.link {
            LinkKind::Stream => encode_frame(frame_type, payload).map_err(TransportError::Wire)?,
            LinkKind::Frames => {
                let body = u16::try_from(payload.len() + 1)
                    .map_err(|_| TransportError::Serial("frame too long".into()))?;
                let mut f = Vec::with_capacity(payload.len() + 3);
                f.extend_from_slice(&body.to_be_bytes());
                f.push(frame_type);
                f.extend_from_slice(payload);
                f
            }
        };
        self.t.send(bytes).map_err(|e| self.shared.fail(failure(e)))
    }

    fn recv(&mut self, timeout: Duration) -> Result<(u8, Vec<u8>), TransportError> {
        let deadline = Instant::now() + timeout;
        loop {
            if self.link == LinkKind::Stream {
                if let Some(frame) = self.decoder.next_frame().map_err(TransportError::Wire)? {
                    return Ok(frame);
                }
            }
            if self.shared.cancelled() {
                return Err(self.shared.fail(lz::LeekError::Cancelled));
            }
            let left = deadline.saturating_duration_since(Instant::now());
            if left.is_zero() {
                return Err(TransportError::Timeout);
            }
            let ms = left.min(SLICE).as_millis().max(1) as u32;
            let bytes = self.t.recv(ms).map_err(|e| self.shared.fail(failure(e)))?;
            if bytes.is_empty() {
                continue;
            }
            match self.link {
                LinkKind::Stream => self.decoder.feed(&bytes),
                LinkKind::Frames => {
                    if bytes.len() < 3 {
                        return Err(TransportError::Serial("short frame".into()));
                    }
                    let body = u16::from_be_bytes([bytes[0], bytes[1]]) as usize;
                    if body == 0 || body + 2 != bytes.len() {
                        return Err(TransportError::Serial("malformed frame".into()));
                    }
                    return Ok((bytes[2], bytes[3..].to_vec()));
                }
            }
        }
    }
}

/// A LeekWallet board over the host's [`FrameTransport`].
///
/// Calls are serialized (one at a time); each blocks until the board
/// answers, so run them off the main thread. Pair, then unlock, then the
/// rest. After [`cancel`](Self::cancel) or a `Reconnect` error, make a new
/// device and pair again.
#[derive(uniffi::Object)]
pub struct LeekDevice {
    device: Mutex<Device>,
    shared: Arc<Shared>,
}

impl LeekDevice {
    /// Starts a call: a fresh cancel token, no recorded failure.
    fn begin(&self) -> (MutexGuard<'_, Device>, lz::CancelToken) {
        let token = lz::CancelToken::new();
        *lock(&self.shared.token) = token.clone();
        *lock(&self.shared.failure) = None;
        (lock(&self.device), token)
    }

    /// The transport's own failure, if it had one, over what the core made
    /// of the text `leek-device` reported.
    fn err(&self, e: lz::LeekError) -> LeekError {
        lock(&self.shared.failure).take().unwrap_or(e).into()
    }

    fn any(&self, e: &anyhow::Error) -> LeekError {
        self.err(lz::classify(e))
    }
}

#[uniffi::export]
impl LeekDevice {
    #[uniffi::constructor]
    pub fn new(transport: Arc<dyn FrameTransport>, link: LinkKind) -> Arc<Self> {
        let shared = Arc::new(Shared::default());
        let port = ForeignPort { t: transport, link, decoder: FrameDecoder::new(), shared: shared.clone() };
        Arc::new(LeekDevice { device: Mutex::new(Device::with_port(Box::new(port))), shared })
    }

    /// Interrupts the running call (from any thread). The session is then
    /// out of step: make a new device and pair again.
    pub fn cancel(&self) {
        lock(&self.shared.token).cancel();
    }

    /// The pairing handshake; `listener` gets the code to compare.
    pub fn pair(&self, listener: Arc<dyn PairingListener>) -> Result<()> {
        let (mut d, _) = self.begin();
        d.pair(|code| listener.on_pairing_code(code.to_string())).map_err(|e| self.any(&e))
    }

    /// Waits for the user to unlock the board, if it is locked.
    pub fn ensure_unlocked(&self) -> Result<()> {
        let (mut d, _) = self.begin();
        d.ensure_unlocked().map_err(|e| self.any(&e))
    }

    pub fn get_features(&self) -> Result<Features> {
        let (mut d, _) = self.begin();
        lz::get_features(&mut d).map(Into::into).map_err(|e| self.err(e))
    }

    /// The ZIP 32 seed fingerprint (32 bytes) of the seed open on the board,
    /// with no question on the board: how to recognise the board on every
    /// connect. A stable identifier of the seed that every paired host can
    /// read. `get_account` is for when the UFVK is needed.
    pub fn seed_fingerprint(&self, network: Option<Network>) -> Result<Vec<u8>> {
        let (mut d, _) = self.begin();
        lz::seed_fingerprint(&mut d, network.map(Into::into)).map(|fp| fp.to_vec()).map_err(|e| self.err(e))
    }

    pub fn get_account(&self, index: u32, network: Network) -> Result<Account> {
        let (mut d, _) = self.begin();
        lz::get_account(&mut d, index, network.into()).map(Into::into).map_err(|e| self.err(e))
    }

    pub fn show_address(&self, index: u32, network: Network, ua: String) -> Result<AddressCheck> {
        let (mut d, _) = self.begin();
        lz::show_address(&mut d, index, network.into(), &ua).map(Into::into).map_err(|e| self.err(e))
    }

    /// The app's Storage Key: the board shows `app_id`, the account, the
    /// network and the pairing code, and releases it only after OK there,
    /// every time. Tell the user to confirm on the board before calling.
    pub fn get_storage_key(&self, app_id: String, index: u32, network: Network) -> Result<Arc<StorageKey>> {
        let (mut d, _) = self.begin();
        lz::get_storage_key(&mut d, &app_id, index, network.into()).map(StorageKey::wrap).map_err(|e| self.err(e))
    }

    /// True if the board keeps it in flash.
    pub fn set_birthday(&self, index: u32, network: Network, height: u32) -> Result<bool> {
        let (mut d, _) = self.begin();
        lz::set_birthday(&mut d, index, network.into(), height).map_err(|e| self.err(e))
    }

    /// Preflight, upload, review on the board, verify and apply.
    pub fn sign_pczt(
        &self,
        pczt: Vec<u8>,
        account: AccountMeta,
        view: SignerView,
        verify_account: bool,
        listener: Option<Arc<dyn SignListener>>,
    ) -> Result<SignResult> {
        let pczt = parse_pczt(&pczt)?;
        let account: lz::AccountMeta = account.try_into()?;
        let (mut d, token) = self.begin();
        let progress = |p: lz::SignProgress| {
            if let Some(l) = &listener {
                l.on_progress(match p {
                    lz::SignProgress::Checking => SignProgress::Checking,
                    lz::SignProgress::Uploading { sent, total } => {
                        SignProgress::Uploading { sent: sent as u64, total: total as u64 }
                    }
                    lz::SignProgress::ReviewOnDevice => SignProgress::ReviewOnDevice,
                    lz::SignProgress::Applying => SignProgress::Applying,
                })
            }
        };
        let opts = lz::SignOptions {
            view: view.into(),
            progress: Some(&progress),
            cancel: Some(token),
            features: None,
            verify_account,
        };
        let r = lz::sign_pczt(&mut d, &pczt, &account, opts).map_err(|e| self.err(e))?;
        r.try_into()
    }
}
