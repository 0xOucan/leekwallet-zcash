//! What the commands do, without Tauri: blocking functions over
//! `zecz::wallet` that take a profile and return plain, serializable data.
//!
//! Each call opens the wallet, does its work and closes it again. The
//! wallet database is SQLite and the handle is not `Sync`, so holding one
//! open across commands would need a lock around every call for no gain;
//! opening is cheap next to anything that touches lightwalletd.
//!
//! The functions block (network calls run on a private current-thread
//! runtime), so the Tauri layer runs them on a worker thread, and the
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
fn block_on<F: Future>(f: F) -> Result<F::Output> {
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
    };
    if !profile.exists() {
        return Ok(s);
    }
    let w = Wallet::open(profile.clone())?;
    if w.has_account()? {
        s.has_account = true;
        s.birthday = Some(w.birthday()?);
        if let Some(st) = w.sync_status()? {
            s.fully_scanned = Some(st.fully_scanned);
            s.chain_tip = Some(st.chain_tip);
        }
    }
    Ok(s)
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

fn sapling_activation(network: Network) -> u32 {
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
