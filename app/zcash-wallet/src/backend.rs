//! The wallet itself: blocking functions over `zecz::wallet` that take a
//! profile (a wallet directory, its network and server) and return plain,
//! serializable data.
//!
//! Each call opens the wallet, does its work and closes it again. The
//! wallet database is SQLite and the handle is not `Sync`, so holding one
//! open across commands would need a lock around every call for no gain;
//! opening is cheap next to anything that touches lightwalletd.
//!
//! The functions block (network calls run on a private current-thread
//! runtime), so the app's commands run them on a worker thread, and the
//! integration tests call them directly.

use std::future::Future;

use anyhow::{bail, Context, Result};
use serde::Serialize;
use zecz::wallet::{self, AccountSpec, InitOutcome, PoolBalance, SyncEvent};
use zecz::{Network, Profile, Wallet};

/// The account name a watch-only import gets in the wallet database.
pub const WATCH_ONLY_NAME: &str = "watch-only";

/// Blocks scanned per lightwalletd request batch.
pub const SYNC_BATCH: u32 = 1000;

/// Runs a future to completion on a fresh current-thread runtime.
pub(crate) fn block_on<F: Future>(f: F) -> Result<F::Output> {
    let rt = tokio::runtime::Builder::new_current_thread().enable_all().build().context("tokio runtime")?;
    Ok(rt.block_on(f))
}

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct ProfileStatus {
    /// "test" or "main".
    pub network: String,
    pub server: String,
    pub data_dir: String,
    /// The wallet tracks an account.
    pub has_account: bool,
    pub birthday: Option<u32>,
    pub fully_scanned: Option<u32>,
    pub chain_tip: Option<u32>,
    /// The account was imported from a LeekWallet and can be signed for.
    pub hardware: bool,
    /// Its short seed fingerprint ("ZFP"), for a hardware account.
    pub zfp: Option<String>,
}

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct AddressesDto {
    pub unified: String,
    pub transparent: Option<String>,
    pub orchard_only: String,
}

impl From<wallet::Addresses> for AddressesDto {
    fn from(a: wallet::Addresses) -> Self {
        AddressesDto { unified: a.unified, transparent: a.transparent, orchard_only: a.orchard_only }
    }
}

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Imported {
    /// The UFVK was already in the wallet; nothing changed.
    pub already_imported: bool,
    pub birthday: u32,
    pub addresses: AddressesDto,
}

/// Progress of a sync, as the frontend draws it.
#[derive(Clone, Debug, PartialEq, Serialize)]
#[serde(rename_all = "camelCase", tag = "kind")]
pub enum SyncProgress {
    /// Connected to lightwalletd; scanning runs from `from` to `tip`.
    Started { server: String, tip: u64, from: u32 },
    /// Blocks `start..end` have been scanned. `fraction` is the share of
    /// the blocks between `from` and the tip scanned so far (0..=1); the
    /// scanner does not go in order, so this counts blocks, not heights.
    Scanned { start: u32, end: u32, tip: u32, fraction: f64 },
}

#[derive(Clone, Copy, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct SyncDone {
    pub chain_tip: u32,
    pub fully_scanned: u32,
    /// Scan progress from the wallet database, 0..=1.
    pub progress: f64,
}

#[derive(Clone, Copy, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct PoolDto {
    pub total: u64,
    pub spendable: u64,
    pub pending: u64,
}

impl From<PoolBalance> for PoolDto {
    fn from(p: PoolBalance) -> Self {
        PoolDto { total: p.total, spendable: p.spendable, pending: p.pending }
    }
}

#[derive(Clone, Copy, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct BalancesDto {
    pub chain_tip: u32,
    pub fully_scanned: u32,
    pub synced: bool,
    pub orchard: PoolDto,
    pub ironwood: PoolDto,
    pub transparent: PoolDto,
    pub total: u64,
    pub spendable: u64,
}

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct TipDto {
    pub server: String,
    pub chain_name: String,
    pub height: u64,
    pub vendor: String,
    pub version: String,
}

/// Where the profile stands. Never fails on a missing wallet: an empty
/// data directory is a normal first-run state.
pub fn profile_status(profile: &Profile) -> Result<ProfileStatus> {
    let mut s = ProfileStatus {
        network: profile.net_name().to_string(),
        server: profile.server.clone(),
        data_dir: profile.data_dir.display().to_string(),
        has_account: false,
        birthday: None,
        fully_scanned: None,
        chain_tip: None,
        hardware: false,
        zfp: None,
    };
    if !profile.exists() {
        return Ok(s);
    }
    let w = Wallet::open(profile.clone())?;
    if w.has_account()? {
        s.has_account = true;
        s.birthday = Some(w.birthday()?);
        if let Some(o) = w.key_origin()? {
            s.hardware = true;
            s.zfp = Some(o.short_fingerprint());
        }
        if let Some(st) = w.sync_status()? {
            s.fully_scanned = Some(st.fully_scanned);
            s.chain_tip = Some(st.chain_tip);
        }
    }
    Ok(s)
}

/// Imports the board's account (as `zGetAccount` gave it to the webview)
/// as this profile's wallet, born at `birthday`.
///
/// The board derives its address itself; its viewing key must lead to the
/// same one, or the key is not what the board showed. A wallet that
/// already tracks the account as watch-only is refused: it could never
/// put the ZIP 32 origin on a spend, so the board could never sign one.
pub fn import_board(p: &Profile, ufvk: &str, seed_fingerprint: &str, account: u32, ua: &str, birthday: u32) -> Result<Imported> {
    let fp: [u8; 32] = hex::decode(seed_fingerprint.trim())
        .ok()
        .and_then(|v| v.try_into().ok())
        .context("the seed fingerprint is not 32 bytes of hex")?;
    let derived = wallet::addresses_for_ufvk(p.network, ufvk.trim()).context("the board's viewing key")?;
    if derived.unified != ua.trim() {
        bail!("the board's address does not match its own viewing key; refusing to import");
    }
    let sapling = sapling_activation(p.network);
    if birthday < sapling {
        bail!("birthday {birthday} is before Sapling activation ({sapling})");
    }
    let spec = AccountSpec::device(ufvk.trim().to_string(), fp, account, crate::hardware::DEVICE_NAME);
    let (w, outcome) = block_on(Wallet::init(p.clone(), &spec, birthday, |_| {}))??;
    if w.key_origin()? != spec.origin {
        bail!(
            "This network's wallet already holds another account, or this one as watch-only. Forget the wallet in Settings and import again."
        );
    }
    Ok(match outcome {
        InitOutcome::AlreadyImported { addresses } => Imported { already_imported: true, birthday: w.birthday()?, addresses: addresses.into() },
        InitOutcome::Imported(a) => Imported { already_imported: false, birthday: a.birthday, addresses: a.addresses.into() },
    })
}

/// Imports a bare UFVK as a view-only account born at `birthday`.
pub fn init_watch_only(profile: &Profile, ufvk: &str, birthday: u32) -> Result<Imported> {
    let ufvk = ufvk.trim();
    // Checked before anything touches the disk or the network.
    wallet::parse_ufvk(profile.network, ufvk)?;
    let sapling = sapling_activation(profile.network);
    if birthday < sapling {
        bail!("birthday {birthday} is before Sapling activation ({sapling})");
    }
    let spec = AccountSpec::view_only(ufvk.to_string(), WATCH_ONLY_NAME);
    let (w, outcome) = block_on(Wallet::init(profile.clone(), &spec, birthday, |_| {}))??;
    Ok(match outcome {
        InitOutcome::AlreadyImported { addresses } => {
            Imported { already_imported: true, birthday: w.birthday()?, addresses: addresses.into() }
        }
        InitOutcome::Imported(a) => Imported { already_imported: false, birthday: a.birthday, addresses: a.addresses.into() },
    })
}

pub(crate) fn sapling_activation(network: Network) -> u32 {
    use zecz::wallet::Network as N;
    match network {
        N::MainNetwork => 419_200,
        N::TestNetwork => 280_000,
    }
}

/// Scans the chain until the wallet is up to date, reporting progress.
pub fn sync(profile: &Profile, on_progress: impl Fn(SyncProgress) + Send + Sync) -> Result<SyncDone> {
    let mut w = Wallet::open(profile.clone())?;
    let from = match w.sync_status()? {
        Some(s) => s.fully_scanned.max(w.birthday()?),
        None => w.birthday()?,
    };
    let state = std::sync::Mutex::new((0u64, 0u32)); // (blocks scanned, tip)
    let status = block_on(w.sync(SYNC_BATCH, |e| match e {
        SyncEvent::Started { server, tip } => {
            state.lock().unwrap().1 = tip as u32;
            on_progress(SyncProgress::Started { server, tip, from });
        }
        SyncEvent::Scanned(r) => {
            let mut st = state.lock().unwrap();
            st.0 += u64::from(r.end.saturating_sub(r.start));
            let tip = r.tip.unwrap_or(st.1);
            let span = u64::from(tip.saturating_sub(from)).max(1);
            let fraction = (st.0 as f64 / span as f64).min(1.0);
            on_progress(SyncProgress::Scanned { start: r.start, end: r.end, tip, fraction });
        }
        // Full transactions fetched after the scan: nothing to show beyond
        // the balances and history they correct.
        SyncEvent::Transactions(_) => {}
    }))??;
    let s = status.context("the wallet has no summary after sync")?;
    let progress = if s.progress.1 == 0 { 1.0 } else { s.progress.0 as f64 / s.progress.1 as f64 };
    Ok(SyncDone { chain_tip: s.chain_tip, fully_scanned: s.fully_scanned, progress })
}

/// The account's balances per pool; zero before the first sync.
pub fn balances(profile: &Profile) -> Result<Option<BalancesDto>> {
    let w = Wallet::open(profile.clone())?;
    if w.sync_status()?.is_none() {
        return Ok(None);
    }
    let b = w.balances()?;
    let a = b.account.unwrap_or_default();
    Ok(Some(BalancesDto {
        chain_tip: b.chain_tip,
        fully_scanned: b.fully_scanned,
        synced: b.synced,
        orchard: a.orchard.into(),
        ironwood: a.ironwood.into(),
        transparent: a.transparent.into(),
        total: a.total,
        spendable: a.spendable,
    }))
}

pub fn addresses(profile: &Profile) -> Result<AddressesDto> {
    Ok(Wallet::open(profile.clone())?.addresses()?.into())
}

pub fn tip(server: &str) -> Result<TipDto> {
    let t = block_on(wallet::tip(server))??;
    Ok(TipDto { server: t.server, chain_name: t.chain_name, height: t.height, vendor: t.vendor, version: t.version })
}

// ----------------------------------------------------------------- history

/// How many transactions the history shows.
pub const HISTORY_LIMIT: u32 = 200;

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct RecipientDto {
    pub address: String,
    pub value_zat: u64,
}

/// One row of the history.
#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct TxDto {
    pub txid: String,
    /// None while unmined.
    pub mined_height: Option<u32>,
    /// Unix seconds of its block, when the wallet has it.
    pub block_time: Option<i64>,
    pub expiry_height: Option<u32>,
    pub expired: bool,
    /// "received", "sent", "self" (sent to the account itself: the amount
    /// is the fee it cost), "shielded" or "internal".
    pub kind: &'static str,
    pub amount_zat: u64,
    /// Change of the balance, negative when value left.
    pub delta_zat: i64,
    /// Only when this wallet paid it.
    pub fee_zat: Option<u64>,
    pub memos: Vec<String>,
    pub recipients: Vec<RecipientDto>,
}

/// The account's transactions, newest first (unmined on top). Empty for
/// an empty profile.
pub fn history(profile: &Profile) -> Result<Vec<TxDto>> {
    if !profile.exists() {
        return Ok(Vec::new());
    }
    let w = Wallet::open(profile.clone())?;
    if !w.has_account()? {
        return Ok(Vec::new());
    }
    Ok(w.history(HISTORY_LIMIT)?.into_iter().map(tx_dto).collect())
}

fn tx_dto(t: wallet::TxRecord) -> TxDto {
    use wallet::TxKind;
    let kind = match t.kind {
        TxKind::Received => "received",
        TxKind::Sent => "sent",
        TxKind::SentToSelf => "self",
        TxKind::Shielded => "shielded",
        TxKind::Internal => "internal",
    };
    TxDto {
        txid: t.txid,
        mined_height: t.mined_height,
        block_time: t.block_time,
        expiry_height: t.expiry_height,
        expired: t.expired_unmined,
        kind,
        amount_zat: t.amount_zat,
        delta_zat: t.delta_zat,
        // A received transaction's fee was the payer's to pay.
        fee_zat: if t.kind == TxKind::Received { None } else { t.fee_zat },
        memos: t.memos,
        recipients: t.recipients.into_iter().map(|r| RecipientDto { address: r.address, value_zat: r.value_zat }).collect(),
    }
}

// ----------------------------------------------------------------- receive

/// A fresh diversified address from [`next_address`].
#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct NewAddressDto {
    pub unified: String,
    /// Decimal: a diversifier index can exceed what a JSON number holds.
    pub diversifier_index: String,
    /// Its transparent receiver; None for an Orchard-only address.
    pub transparent: Option<String>,
}

/// Rotates to a new diversified address of the account. The wallet
/// records it, so what is paid to it is found when syncing.
pub fn next_address(profile: &Profile) -> Result<NewAddressDto> {
    let a = Wallet::open(profile.clone())?.next_address()?;
    Ok(NewAddressDto { unified: a.unified, diversifier_index: a.diversifier_index.to_string(), transparent: a.transparent })
}

/// The wallet's address for a swap (NEAR Intents refunds and payouts):
/// with `fresh`, a new Orchard-only address recorded in the wallet, so the
/// swap service sees no transparent receiver and cannot link one swap to
/// the next; otherwise the default Orchard-only one (for a price preview).
pub fn swap_address(profile: &Profile, fresh: bool) -> Result<String> {
    let mut w = Wallet::open(profile.clone())?;
    Ok(if fresh { w.next_shielded_address()?.unified } else { w.addresses()?.orchard_only })
}
