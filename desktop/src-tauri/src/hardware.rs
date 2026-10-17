//! Hardware accounts: the LeekWallet holds the seed, this app holds the
//! watch-only wallet for the account it exports.
//!
//! The same flows as `zecz device-init`, `device-show-address`,
//! `send --compact`, `sign-device` and `finish` (host/zecz-cli), as
//! blocking functions over a paired [`DeviceSession`]. The session is kept
//! between calls, so the user compares a passkey once per connection, not
//! once per action. The Tauri layer runs these on worker threads, one
//! device call at a time; the tests drive them against `zdev`.

use anyhow::{anyhow, bail, Context, Result};
use serde::Serialize;
use zecz::device::{self, Device, DeviceError, Value};
use zecz::wallet::{self, AccountSpec, FinishEvent, InitOutcome, KeyOrigin, SendRequest};
use zecz::zsig::Zsig;
use zecz::{Profile, Wallet};

use crate::backend::{block_on, sapling_activation, AddressesDto};

/// The account name a device import gets in the wallet database.
pub const DEVICE_NAME: &str = "leekwallet";

/// One step of a device flow, for the status line under the button.
#[derive(Clone, Debug, PartialEq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct DeviceStatus {
    /// "pair", "unlock", "account", "import", "birthday", "verify",
    /// "upload", "review", "signed", "prove", "broadcast".
    pub step: &'static str,
    pub message: String,
    /// Bytes uploaded so far, for "upload".
    pub done: Option<u64>,
    pub total: Option<u64>,
}

impl DeviceStatus {
    pub fn new(step: &'static str, message: impl Into<String>) -> Self {
        DeviceStatus { step, message: message.into(), done: None, total: None }
    }

    fn upload(done: u64, total: u64) -> Self {
        DeviceStatus { step: "upload", message: "Sending the transaction to the device…".into(), done: Some(done), total: Some(total) }
    }
}

/// What the device said about one of its accounts (zGetAccount).
#[derive(Clone, Debug)]
pub struct DeviceAccount {
    pub index: u32,
    pub ufvk: String,
    pub ua: String,
    pub t_address: String,
    pub seed_fingerprint: [u8; 32],
    /// Present when a companion recorded it, or the seed came from a .zkv card.
    pub birthday: Option<u32>,
}

/// A paired, unlocked device and the account it exported.
pub struct DeviceSession {
    dev: Device,
    /// "usb" or "ble".
    pub transport: String,
    /// The serial port or the Bluetooth id.
    pub port: String,
    pub account: DeviceAccount,
    /// The link failed or the device dropped the session: connect again.
    pub lost: bool,
}

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct PairedDto {
    pub transport: String,
    pub port: String,
    pub account: u32,
    /// The short seed fingerprint the device shows ("ZFP").
    pub zfp: String,
    pub ua: String,
    pub t_address: String,
    pub birthday: Option<u32>,
    /// Whether this seed is the wallet's device account; None when the
    /// wallet has no device account yet.
    pub matches_wallet: Option<bool>,
}

impl DeviceSession {
    pub fn describe(&self, profile: &Profile) -> Result<PairedDto> {
        let a = &self.account;
        Ok(PairedDto {
            transport: self.transport.clone(),
            port: self.port.clone(),
            account: a.index,
            zfp: wallet::short_fingerprint(&a.seed_fingerprint),
            ua: a.ua.clone(),
            t_address: a.t_address.clone(),
            birthday: a.birthday,
            matches_wallet: wallet_origin(profile)?.map(|o| o.seed_fingerprint == a.seed_fingerprint),
        })
    }
}

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct DeviceImported {
    pub already_imported: bool,
    pub birthday: u32,
    /// "user", "device", or "wallet" (already imported): where the
    /// birthday came from.
    pub birthday_from: &'static str,
    /// Whether the birthday was recorded on the device now: Some(true) in
    /// flash, Some(false) until the device locks (passphrase, temporary or
    /// SD seed), None when the device already had it.
    pub birthday_stored: Option<bool>,
    pub zfp: String,
    pub addresses: AddressesDto,
}

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct PaymentDto {
    pub recipient: String,
    pub amount_zat: u64,
    pub pool: String,
    pub has_memo: bool,
}

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct ChangeDto {
    pub value_zat: u64,
    pub pool: String,
}

/// A built, unsigned send, as the review screen shows it.
#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct ProposalDto {
    /// SHA-256 of the signer view, hex: what `sign_and_send` takes.
    pub id: String,
    pub target_height: u32,
    pub payments: Vec<PaymentDto>,
    pub change: Vec<ChangeDto>,
    pub fee_zat: u64,
    /// Payments plus fee: what leaves the wallet.
    pub total_zat: u64,
    /// Size of the PCZT the device receives.
    pub signer_view_bytes: usize,
    pub compact: bool,
}

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct SentDto {
    pub txid: String,
    pub signatures: usize,
    pub size: usize,
    pub broadcast: bool,
}

fn device_code(e: &anyhow::Error) -> Option<u64> {
    e.chain().find_map(|c| c.downcast_ref::<DeviceError>()).map(|d| d.code)
}

/// Marks the session lost when a device call failed below the protocol (a
/// frame that did not go through) or the device dropped the session or
/// locked, so the next call asks for a new connection instead of failing
/// the same way.
fn track<T>(s: &mut DeviceSession, r: Result<T>) -> Result<T> {
    if let Err(e) = &r {
        match device_code(e) {
            None => s.lost = true,
            Some(c) if c == device::ERR_SESSION || c == device::ERR_NOT_UNLOCKED => s.lost = true,
            Some(_) => {}
        }
    }
    r
}

/// An error as the user should read it: what happened in words, then the
/// device's own detail. Device strings are developer messages and never
/// the headline (PROTOCOL.md §6).
pub fn explain(e: &anyhow::Error) -> String {
    let full = format!("{e:#}");
    // Errors leek-device already put in words (zShowAddress) keep them.
    if e.downcast_ref::<DeviceError>().is_some() && e.chain().count() > 1 {
        return full;
    }
    let hint = match device_code(e) {
        Some(device::ERR_USER_REJECTED) => "Rejected on the device.".to_string(),
        Some(device::ERR_USER_TIMEOUT) => "The device stopped waiting for an answer. Try again and answer on the device.".to_string(),
        Some(device::ERR_NOT_UNLOCKED) => "The device is locked. Unlock it with its PIN, then connect again.".to_string(),
        Some(device::ERR_SESSION) => {
            "The device ended the session (it locked, restarted or paired with another app). Connect again.".to_string()
        }
        Some(device::ERR_BUSY) => "The device is busy with a question on its screen. Answer it, then try again.".to_string(),
        Some(c) if c & 0xFF00 == device::ERR_ZSIGN_BASE => format!(
            "The device refused to sign this transaction (reason 0x{:02X}). Nothing was signed.",
            c & 0xFF
        ),
        _ => return full,
    };
    format!("{hint} ({full})")
}

fn account_of(r: &Value, index: u32, net: &str) -> Result<DeviceAccount> {
    if device::get_text(r, "network")? != net {
        bail!("the device answered for a different network");
    }
    Ok(DeviceAccount {
        index,
        ufvk: device::get_text(r, "ufvk")?,
        ua: device::get_text(r, "ua")?,
        t_address: device::get_text(r, "tAddress")?,
        seed_fingerprint: device::get_bytes(r, "seedFingerprint")?
            .try_into()
            .map_err(|_| anyhow!("seedFingerprint is not 32 bytes"))?,
        birthday: device::get_uint(r, "birthday").ok().and_then(|b| u32::try_from(b).ok()),
    })
}

/// The wallet's device account, if it has one: its ZIP 32 origin.
pub fn wallet_origin(profile: &Profile) -> Result<Option<KeyOrigin>> {
    if !profile.exists() {
        return Ok(None);
    }
    let w = Wallet::open(profile.clone())?;
    if !w.has_account()? {
        return Ok(None);
    }
    w.key_origin()
}

/// Pairs with an opened device (the user compares `on_passkey`'s six
/// digits with the device's and presses ALLOW there), waits for its PIN if
/// it is locked, and reads account `account`.
pub fn connect(
    mut dev: Device,
    transport: &str,
    port: &str,
    account: u32,
    profile: &Profile,
    on_passkey: &dyn Fn(&str),
    on_status: &dyn Fn(DeviceStatus),
) -> Result<DeviceSession> {
    on_status(DeviceStatus::new("pair", "Pairing…"));
    dev.pair(|p| {
        on_passkey(p);
        on_status(DeviceStatus::new("pair", "Check that the device shows the same six digits, then press ALLOW on it."));
    })?;
    on_status(DeviceStatus::new("unlock", "Paired. If the device asks for its PIN, enter it there."));
    dev.ensure_unlocked()?;
    on_status(DeviceStatus::new("account", "Reading the account from the device…"));
    let net = profile.net_name();
    let account = account_of(&dev.z_get_account(account, net)?, account, net)?;
    Ok(DeviceSession { dev, transport: transport.to_string(), port: port.to_string(), account, lost: false })
}

/// Imports the device's account as the wallet's, fetching the tree state
/// before the birthday from lightwalletd.
pub fn device_import(
    profile: &Profile,
    s: &mut DeviceSession,
    birthday: Option<u32>,
    on_status: &dyn Fn(DeviceStatus),
) -> Result<DeviceImported> {
    device_import_with(profile, s, birthday, on_status, |p, spec, b| block_on(Wallet::init(p.clone(), spec, b, |_| {}))?)
}

/// [`device_import`] with the wallet creation supplied (the tests build
/// the birthday offline).
pub fn device_import_with(
    profile: &Profile,
    s: &mut DeviceSession,
    birthday: Option<u32>,
    on_status: &dyn Fn(DeviceStatus),
    init: impl FnOnce(&Profile, &AccountSpec, u32) -> Result<(Wallet, InitOutcome)>,
) -> Result<DeviceImported> {
    let a = s.account.clone();
    // The user's birthday wins (they may know better, e.g. an older seed);
    // otherwise the device's, as `zecz device-init` does.
    let (birthday, birthday_from) = match (birthday, a.birthday) {
        (Some(b), _) => (b, "user"),
        (None, Some(b)) => (b, "device"),
        (None, None) => bail!(
            "The device does not know this account's birthday. Enter the height of the first block that can hold its funds (for a new wallet, the chain tip)."
        ),
    };
    let sapling = sapling_activation(profile.network);
    if birthday < sapling {
        bail!("birthday {birthday} is before Sapling activation ({sapling})");
    }
    // The device derives its address itself; its UFVK must lead to the same one.
    let derived = wallet::addresses_for_ufvk(profile.network, &a.ufvk).context("device UFVK")?;
    if derived.unified != a.ua {
        bail!("the device's address does not match its own viewing key; refusing to import");
    }
    let spec = AccountSpec::device(a.ufvk.clone(), a.seed_fingerprint, a.index, DEVICE_NAME);
    on_status(DeviceStatus::new("import", format!("Importing the account, born at block {birthday}…")));
    let (w, outcome) = init(profile, &spec, birthday)?;
    if w.key_origin()? != spec.origin {
        bail!(
            "This wallet already holds the account as watch-only, so it can never ask the device to sign. Move the wallet folder ({}) away and import from the device again.",
            profile.data_dir.display()
        );
    }
    let (already_imported, wallet_birthday, addresses) = match outcome {
        InitOutcome::AlreadyImported { addresses } => (true, w.birthday()?, addresses),
        InitOutcome::Imported(i) => (false, i.birthday, i.addresses),
    };
    drop(w);
    // Recorded on the device for the next companion, as zecz does.
    let birthday_stored = if a.birthday != Some(wallet_birthday) {
        on_status(DeviceStatus::new("birthday", format!("Recording birthday {wallet_birthday} on the device…")));
        let r = s.dev.z_set_birthday(a.index, wallet_birthday);
        let stored = track(s, r)?;
        s.account.birthday = Some(wallet_birthday);
        Some(stored)
    } else {
        None
    };
    Ok(DeviceImported {
        already_imported,
        birthday: wallet_birthday,
        birthday_from: if already_imported { "wallet" } else { birthday_from },
        birthday_stored,
        zfp: wallet::short_fingerprint(&a.seed_fingerprint),
        addresses: addresses.into(),
    })
}

/// The wallet's device account, after checking that the connected device
/// holds its seed: signing or verifying with another seed would at best
/// fail on the device.
fn same_seed(profile: &Profile, s: &mut DeviceSession) -> Result<KeyOrigin> {
    let origin = wallet_origin(profile)?.ok_or_else(|| {
        anyhow!("This wallet has no LeekWallet account (it is empty or watch-only). Import the account from the device first.")
    })?;
    let fp = if origin.account_index == s.account.index {
        s.account.seed_fingerprint
    } else {
        let net = profile.net_name();
        let r = s.dev.z_get_account(origin.account_index, net);
        let r = track(s, r)?;
        account_of(&r, origin.account_index, net)?.seed_fingerprint
    };
    if fp != origin.seed_fingerprint {
        bail!(
            "This LeekWallet holds another seed (ZFP {}) than the wallet's account (ZFP {}). Connect the device that holds it, or open that seed on the device.",
            wallet::short_fingerprint(&fp),
            origin.short_fingerprint()
        );
    }
    Ok(origin)
}

/// Sends `ua` to the device, which shows it only if it is one of the
/// account's addresses; the user compares and answers there.
pub fn verify_address(profile: &Profile, s: &mut DeviceSession, ua: &str, on_status: &dyn Fn(DeviceStatus)) -> Result<()> {
    let ua = ua.trim();
    let origin = same_seed(profile, s)?;
    on_status(DeviceStatus::new("verify", "Compare the address on the device with the one here, then answer on the device."));
    let r = s.dev.z_show_address(origin.account_index, profile.net_name(), ua);
    track(s, r)
}

/// Proposes a payment and builds the compact signer view for the device.
/// The full PCZT stays in the wallet's `pending/` for [`finish`]. Returns
/// the summary and the signer view.
pub fn propose_send(profile: &Profile, to: &str, amount_zec: &str, memo: Option<&str>) -> Result<(ProposalDto, Vec<u8>)> {
    let to = to.trim();
    if to.is_empty() {
        bail!("enter the recipient's address");
    }
    let amount_zat = wallet::parse_zec(amount_zec.trim())?;
    let memo = memo.map(str::trim_end).filter(|m| !m.is_empty()).map(str::to_string);
    let mut w = Wallet::open(profile.clone())?;
    if w.key_origin()?.is_none() {
        bail!("This is a watch-only account; nothing could sign the send. Import the account from a LeekWallet to spend.");
    }
    let p = w.propose_send(&SendRequest { to: to.to_string(), amount_zat, memo, compact: true })?;
    let payments: Vec<PaymentDto> = p
        .payments
        .iter()
        .map(|x| PaymentDto { recipient: x.recipient.clone(), amount_zat: x.amount_zat, pool: x.pool.clone(), has_memo: x.has_memo })
        .collect();
    let total_zat = payments.iter().map(|x| x.amount_zat).sum::<u64>() + p.fee_zat;
    let dto = ProposalDto {
        id: proposal_id(&p.signer_view),
        target_height: p.target_height,
        payments,
        change: p.change.iter().map(|c| ChangeDto { value_zat: c.value_zat, pool: c.pool.clone() }).collect(),
        fee_zat: p.fee_zat,
        total_zat,
        signer_view_bytes: p.signer_view.len(),
        compact: p.compact,
    };
    Ok((dto, p.signer_view))
}

/// The id a proposal goes by: the hex SHA-256 of its signer view, the
/// name its full PCZT is kept under in `pending/`.
pub fn proposal_id(view: &[u8]) -> String {
    zecz::zsig::sha256(view).iter().map(|b| format!("{b:02x}")).collect()
}

/// Has the device check, show and sign `view`; the user reviews it there
/// and ACCEPTs on its last page. Returns the `.zsig` bytes.
pub fn device_sign(profile: &Profile, s: &mut DeviceSession, view: &[u8], on_status: &dyn Fn(DeviceStatus)) -> Result<Vec<u8>> {
    let origin = same_seed(profile, s)?;
    on_status(DeviceStatus::upload(0, view.len() as u64));
    let r = s.dev.z_sign(view, origin.account_index, profile.net_name(), |done, total| {
        on_status(DeviceStatus::upload(done as u64, total as u64));
        if done == total {
            on_status(DeviceStatus::new("review", "Review the transaction on the device. ACCEPT on its last page signs."));
        }
    });
    let (_sighash, entries) = track(s, r)?;
    if entries.is_empty() {
        bail!("the device returned no signatures");
    }
    let n = entries.len();
    on_status(DeviceStatus::new("signed", format!("Signed on the device ({n} spend{}).", if n == 1 { "" } else { "s" })));
    Ok(Zsig::new(view, entries).to_bytes()?)
}

/// Applies the device's signatures to the kept full PCZT (each checked
/// against its rk), proves, stores the transaction and, if `broadcast`,
/// sends it through lightwalletd.
pub fn finish(profile: &Profile, view: &[u8], zsig: &[u8], broadcast: bool, on_status: &dyn Fn(DeviceStatus)) -> Result<SentDto> {
    use zecz::authorize::ProveEvent;
    let mut w = Wallet::open(profile.clone())?;
    let f = block_on(w.finish(view, zsig, broadcast, |e| match e {
        FinishEvent::SignaturesApplied { count } => {
            on_status(DeviceStatus::new("prove", format!("{count} signature(s) applied. Proving…")))
        }
        FinishEvent::Proving(ProveEvent::LoadingKey { pool, .. }) => {
            on_status(DeviceStatus::new("prove", format!("Loading the {pool} proving key (slow the first time)…")))
        }
        FinishEvent::Proving(ProveEvent::Proving { pool }) => on_status(DeviceStatus::new("prove", format!("Proving the {pool} bundle…"))),
        FinishEvent::Stored { txid, .. } if broadcast => on_status(DeviceStatus::new("broadcast", format!("Broadcasting {txid}…"))),
        FinishEvent::Stored { .. } => {}
    }))??;
    Ok(SentDto { txid: f.txid, signatures: f.signatures, size: f.raw.len(), broadcast: f.broadcast })
}

/// The send after the user reviewed it here: sign on the device, then
/// [`finish`] and broadcast.
pub fn sign_and_send(profile: &Profile, s: &mut DeviceSession, view: &[u8], on_status: &dyn Fn(DeviceStatus)) -> Result<SentDto> {
    let zsig = device_sign(profile, s, view, on_status)?;
    finish(profile, view, &zsig, true, on_status)
}
