//! LeekWallet Zcash desktop: the Tauri layer.
//!
//! The wallet itself is `zecz` (host/zecz-core); [`backend`] turns its
//! typed API into blocking calls with serializable results, and this file
//! only exposes them as commands. No protocol or key logic lives in the
//! webview.
//!
//! Every command that touches the wallet, the network or the device runs
//! on its own worker thread, so the window never waits on SQLite,
//! lightwalletd or a person reading the device's screen.

pub mod backend;
pub mod contacts;
pub mod hardware;
pub mod link;
pub mod near;
pub mod settings;

use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};

use tauri::{AppHandle, Emitter, Manager, State};
use zecz::{Network, Profile};

use hardware::{DeviceSession, DeviceStatus};

/// The event `sync` emits for each step, carrying a [`backend::SyncProgress`].
pub const SYNC_EVENT: &str = "sync://progress";
/// The six-digit pairing passkey, for the user to compare with the device.
pub const PASSKEY_EVENT: &str = "device://passkey";
/// Each step of a device flow, carrying a [`hardware::DeviceStatus`].
pub const DEVICE_STATUS_EVENT: &str = "device://status";

const WALLET_BUSY: &str = "the wallet is busy (a sync, an import or a send is running)";
const DEVICE_BUSY: &str = "the LeekWallet is busy with another request; finish that one first";

/// The selected network's wallet, the saved settings, and the device session.
pub struct AppState {
    /// `<app data dir>`: settings.json, wallets/<test|main>, contacts-<net>.json.
    app_data: PathBuf,
    /// The wallet the commands act on: the selected network's directory
    /// and server. Replaced only by `set_network` / `set_server`, which hold
    /// both busy flags (or the wallet's) while they do.
    profile: Arc<Mutex<Profile>>,
    /// LEEKZ_NETWORK or LEEKZ_SERVER chose this run's network or server.
    from_environment: Arc<AtomicBool>,
    /// A sync, import or send is running; another is refused meanwhile.
    busy: Arc<AtomicBool>,
    /// A device call is running. The device answers one request at a time
    /// and its frames carry no request ids, so a second is refused.
    device_busy: Arc<AtomicBool>,
    /// The paired device, kept between commands.
    device: Arc<Mutex<Option<DeviceSession>>>,
    /// The last proposed send: its id and the signer view for the device.
    proposal: Arc<Mutex<Option<(String, Vec<u8>)>>>,
    /// Held while the swaps file is read and rewritten.
    swaps_lock: Arc<Mutex<()>>,
    /// 1Click's token list and when it was fetched.
    swap_tokens: Arc<Mutex<Option<(std::time::Instant, Vec<near::Token>)>>>,
}

impl AppState {
    fn profile(&self) -> Profile {
        self.profile.lock().unwrap_or_else(|p| p.into_inner()).clone()
    }

    /// The address book of the selected network.
    fn contacts_path(&self) -> PathBuf {
        contacts::path_in(&self.app_data, self.profile().network)
    }

    /// The swaps of the selected network.
    fn swaps_path(&self) -> PathBuf {
        near::path_in(&self.app_data, self.profile().network)
    }

    /// The 1Click client, with the API key if one is set.
    fn one_click(&self) -> near::OneClick<near::Http> {
        let saved = settings::load(&settings::path_in(&self.app_data)).unwrap_or_default();
        near::OneClick::new(near::Http::default(), near::api_key(saved.one_click_api_key.as_deref()))
    }

    fn network_dto(&self) -> settings::NetworkDto {
        let saved = settings::load(&settings::path_in(&self.app_data)).unwrap_or_default();
        settings::NetworkDto::of(&self.profile(), &saved, self.from_environment.load(Ordering::SeqCst))
    }
}

/// The profile this run starts with: the saved network and server
/// (testnet and its default server the first time), unless LEEKZ_NETWORK
/// (test or main) or LEEKZ_SERVER says otherwise for this run. The
/// environment is never saved. Returns whether it decided anything.
fn startup_profile(app_data: &Path) -> (Profile, bool) {
    let path = settings::path_in(app_data);
    let saved = settings::load(&path).unwrap_or_else(|e| {
        eprintln!("LeekWallet Zcash: {e:#}; using the defaults (testnet)");
        settings::Settings::default()
    });
    let env_net = std::env::var("LEEKZ_NETWORK").ok().and_then(|n| settings::parse_network(&n).ok());
    let env_server = std::env::var("LEEKZ_SERVER").ok().filter(|s| !s.trim().is_empty());
    let network = env_net.or_else(|| saved.network().ok()).unwrap_or(Network::TestNetwork);
    let server = env_server.clone().unwrap_or_else(|| saved.server_for(network));
    let p = Profile::new(network, settings::wallet_dir(app_data, network), Some(server));
    (p, env_net.is_some() || env_server.is_some())
}

/// Runs `f` on a fresh thread and waits for it without blocking the async
/// runtime. Errors become strings for the frontend.
async fn off_thread<T, F>(f: F) -> Result<T, String>
where
    T: Send + 'static,
    F: FnOnce() -> anyhow::Result<T> + Send + 'static,
{
    let (tx, rx) = tokio::sync::oneshot::channel();
    std::thread::Builder::new()
        .name("zecz-worker".into())
        .spawn(move || {
            let _ = tx.send(f());
        })
        .map_err(|e| format!("cannot start a worker thread: {e}"))?;
    match rx.await {
        Ok(r) => r.map_err(|e| format!("{e:#}")),
        Err(_) => Err("the worker thread panicked".into()),
    }
}

/// Clears the busy flag when a guarded command ends, however it ends.
struct BusyGuard(Arc<AtomicBool>);

impl BusyGuard {
    fn take(flag: &Arc<AtomicBool>, what: &str) -> Result<Self, String> {
        if flag.swap(true, Ordering::SeqCst) {
            return Err(what.into());
        }
        Ok(BusyGuard(flag.clone()))
    }
}

impl Drop for BusyGuard {
    fn drop(&mut self) {
        self.0.store(false, Ordering::SeqCst);
    }
}

#[tauri::command]
async fn profile_status(state: State<'_, AppState>) -> Result<backend::ProfileStatus, String> {
    let p = state.profile();
    off_thread(move || backend::profile_status(&p)).await
}

#[tauri::command]
async fn init_watch_only(state: State<'_, AppState>, ufvk: String, birthday: u32) -> Result<backend::Imported, String> {
    let guard = BusyGuard::take(&state.busy, WALLET_BUSY)?;
    let p = state.profile();
    off_thread(move || {
        let _guard = guard;
        backend::init_watch_only(&p, &ufvk, birthday)
    })
    .await
}

#[tauri::command]
async fn sync(app: AppHandle, state: State<'_, AppState>) -> Result<backend::SyncDone, String> {
    let guard = BusyGuard::take(&state.busy, WALLET_BUSY)?;
    let p = state.profile();
    off_thread(move || {
        let _guard = guard;
        backend::sync(&p, |e| {
            let _ = app.emit(SYNC_EVENT, e);
        })
    })
    .await
}

#[tauri::command]
async fn balances(state: State<'_, AppState>) -> Result<Option<backend::BalancesDto>, String> {
    let p = state.profile();
    off_thread(move || backend::balances(&p)).await
}

#[tauri::command]
async fn addresses(state: State<'_, AppState>) -> Result<backend::AddressesDto, String> {
    let p = state.profile();
    off_thread(move || backend::addresses(&p)).await
}

// ---------------------------------------------------------------- contacts

/// The address book for this network, sorted by name.
#[tauri::command]
async fn contacts_list(state: State<'_, AppState>) -> Result<Vec<contacts::Contact>, String> {
    let path = state.contacts_path();
    off_thread(move || contacts::list(&path)).await
}

/// Adds a contact, or changes the one called `replacing`. Returns the book.
#[tauri::command]
async fn contact_save(
    state: State<'_, AppState>,
    name: String,
    address: String,
    replacing: Option<String>,
) -> Result<Vec<contacts::Contact>, String> {
    let (path, network) = (state.contacts_path(), state.profile().network);
    off_thread(move || contacts::save(&path, network, &name, &address, replacing.as_deref())).await
}

/// Removes the contact called `name`. Returns the book.
#[tauri::command]
async fn contact_remove(state: State<'_, AppState>, name: String) -> Result<Vec<contacts::Contact>, String> {
    let path = state.contacts_path();
    off_thread(move || contacts::remove(&path, &name)).await
}

/// The account's transactions, newest first.
#[tauri::command]
async fn history(state: State<'_, AppState>) -> Result<Vec<backend::TxDto>, String> {
    let p = state.profile();
    off_thread(move || backend::history(&p)).await
}

/// Rotates the receive address to a fresh diversified one.
#[tauri::command]
async fn next_address(state: State<'_, AppState>) -> Result<backend::NewAddressDto, String> {
    let guard = BusyGuard::take(&state.busy, WALLET_BUSY)?;
    let p = state.profile();
    off_thread(move || {
        let _guard = guard;
        backend::next_address(&p)
    })
    .await
}

/// A QR code of `text` (an address), as SVG.
#[tauri::command]
fn qr_svg(text: String) -> Result<String, String> {
    backend::qr_svg(&text).map_err(|e| format!("{e:#}"))
}

#[tauri::command]
async fn tip(state: State<'_, AppState>) -> Result<backend::TipDto, String> {
    let server = state.profile().server;
    off_thread(move || backend::tip(&server)).await
}

// ------------------------------------------------------------------ device

/// Runs `f` on the paired device. A session that lost its link, or that
/// the device dropped, is closed, so the next call asks to connect again.
fn with_device<T>(
    slot: &Mutex<Option<DeviceSession>>,
    f: impl FnOnce(&mut DeviceSession) -> anyhow::Result<T>,
) -> anyhow::Result<T> {
    let mut slot = slot.lock().unwrap_or_else(|p| p.into_inner());
    let s = slot.as_mut().ok_or_else(|| anyhow::anyhow!("No LeekWallet is connected. Connect it first."))?;
    let r = f(s);
    let lost = s.lost;
    if lost {
        *slot = None;
    }
    r.map_err(|e| {
        let msg = hardware::explain(&e);
        if lost {
            anyhow::anyhow!("{msg} The connection was closed; connect the LeekWallet again.")
        } else {
            anyhow::anyhow!("{msg}")
        }
    })
}

fn status_emitter(app: &AppHandle) -> impl Fn(DeviceStatus) + '_ {
    move |st| {
        let _ = app.emit(DEVICE_STATUS_EVENT, st);
    }
}

/// USB serial ports, the board's (Espressif VID 303a) first.
#[tauri::command]
async fn list_ports() -> Result<Vec<link::PortDto>, String> {
    off_thread(link::list_ports).await
}

/// LeekWallets advertising over Bluetooth (a four-second scan).
#[tauri::command]
async fn ble_scan() -> Result<Vec<link::BleDeviceDto>, String> {
    off_thread(link::ble_scan).await
}

/// Opens the link ("usb" with a serial port, or "ble" with an id from
/// `ble_scan` or none for the first LeekWallet found), pairs (the passkey
/// goes out on `device://passkey`; the user compares it and presses ALLOW
/// on the device), waits for the PIN, and reads the account. Replaces any
/// earlier session.
#[tauri::command]
async fn device_pair(
    app: AppHandle,
    state: State<'_, AppState>,
    transport: String,
    port: Option<String>,
    account: Option<u32>,
) -> Result<hardware::PairedDto, String> {
    let guard = BusyGuard::take(&state.device_busy, DEVICE_BUSY)?;
    let (slot, profile) = (state.device.clone(), state.profile());
    off_thread(move || {
        let _guard = guard;
        // Closed first: a serial port opens once, and the device serves one
        // Bluetooth peer at a time.
        *slot.lock().unwrap_or_else(|p| p.into_inner()) = None;
        let port = port.filter(|p| !p.trim().is_empty());
        let status = status_emitter(&app);
        status(DeviceStatus::new("connect", "Connecting…"));
        let dev = match transport.as_str() {
            "usb" => link::open_usb(port.as_deref().unwrap_or(""))?,
            "ble" => link::open_ble(port.as_deref())?,
            t => anyhow::bail!("unknown transport {t:?} (usb or ble)"),
        };
        let passkey = |p: &str| {
            let _ = app.emit(PASSKEY_EVENT, p.to_string());
        };
        let label = port.clone().unwrap_or_else(|| "first found".into());
        let s = hardware::connect(dev, &transport, &label, account.unwrap_or(0), &profile, &passkey, &status)
            .map_err(|e| anyhow::anyhow!(hardware::explain(&e)))?;
        let dto = s.describe(&profile)?;
        *slot.lock().unwrap_or_else(|p| p.into_inner()) = Some(s);
        Ok(dto)
    })
    .await
}

/// The paired device, if any. Never waits on a running device call.
#[tauri::command]
async fn device_session(state: State<'_, AppState>) -> Result<Option<hardware::PairedDto>, String> {
    let (slot, profile) = (state.device.clone(), state.profile());
    off_thread(move || {
        let r = match slot.try_lock() {
            Ok(s) => s.as_ref().map(|s| s.describe(&profile)).transpose(),
            Err(_) => Err(anyhow::anyhow!(DEVICE_BUSY)),
        };
        r
    })
    .await
}

/// Closes the link.
#[tauri::command]
async fn device_disconnect(state: State<'_, AppState>) -> Result<(), String> {
    let guard = BusyGuard::take(&state.device_busy, DEVICE_BUSY)?;
    let slot = state.device.clone();
    off_thread(move || {
        let _guard = guard;
        *slot.lock().unwrap_or_else(|p| p.into_inner()) = None;
        Ok(())
    })
    .await
}

/// Imports the paired device's account. `birthday` overrides the one the
/// device knows; one of the two is needed.
#[tauri::command]
async fn device_import(
    app: AppHandle,
    state: State<'_, AppState>,
    birthday: Option<u32>,
) -> Result<hardware::DeviceImported, String> {
    let dguard = BusyGuard::take(&state.device_busy, DEVICE_BUSY)?;
    let wguard = BusyGuard::take(&state.busy, WALLET_BUSY)?;
    let (slot, profile) = (state.device.clone(), state.profile());
    off_thread(move || {
        let _guards = (dguard, wguard);
        let status = status_emitter(&app);
        with_device(&slot, |s| hardware::device_import(&profile, s, birthday, &status))
    })
    .await
}

/// Shows `ua`, the address on screen, on the device for the user to compare.
#[tauri::command]
async fn verify_address(app: AppHandle, state: State<'_, AppState>, ua: String) -> Result<(), String> {
    let guard = BusyGuard::take(&state.device_busy, DEVICE_BUSY)?;
    let (slot, profile) = (state.device.clone(), state.profile());
    off_thread(move || {
        let _guard = guard;
        let status = status_emitter(&app);
        with_device(&slot, |s| hardware::verify_address(&profile, s, &ua, &status))
    })
    .await
}

/// Builds a send with the compact signer view and returns its summary for
/// review. `amount` is in ZEC, as typed.
#[tauri::command]
async fn propose_send(
    state: State<'_, AppState>,
    to: String,
    amount: String,
    memo: Option<String>,
) -> Result<hardware::ProposalDto, String> {
    let guard = BusyGuard::take(&state.busy, WALLET_BUSY)?;
    let (profile, slot) = (state.profile(), state.proposal.clone());
    off_thread(move || {
        let _guard = guard;
        let (dto, view) = hardware::propose_send(&profile, &to, &amount, memo.as_deref())?;
        *slot.lock().unwrap_or_else(|p| p.into_inner()) = Some((dto.id.clone(), view));
        Ok(dto)
    })
    .await
}

/// Builds a shielding of all transparent funds (compact signer view) and
/// returns its summary for review; signed by `sign_and_send` like a send.
/// It replaces any send waiting for review.
#[tauri::command]
async fn propose_shield(state: State<'_, AppState>) -> Result<hardware::ProposalDto, String> {
    let guard = BusyGuard::take(&state.busy, WALLET_BUSY)?;
    let (profile, slot) = (state.profile(), state.proposal.clone());
    off_thread(move || {
        let _guard = guard;
        let (dto, view) = hardware::propose_shield(&profile)?;
        *slot.lock().unwrap_or_else(|p| p.into_inner()) = Some((dto.id.clone(), view));
        Ok(dto)
    })
    .await
}

/// Signs the reviewed proposal on the device, then proves, stores and
/// broadcasts it; progress on `device://status`. Returns the txid. On
/// mainnet, `confirmMainnet` must be true: the user ticked the real-funds
/// confirmation under the review.
#[tauri::command]
async fn sign_and_send(
    app: AppHandle,
    state: State<'_, AppState>,
    proposal_id: String,
    confirm_mainnet: Option<bool>,
) -> Result<hardware::SentDto, String> {
    let dguard = BusyGuard::take(&state.device_busy, DEVICE_BUSY)?;
    let wguard = BusyGuard::take(&state.busy, WALLET_BUSY)?;
    let (slot, profile, proposal) = (state.device.clone(), state.profile(), state.proposal.clone());
    let (swaps_path, swaps_lock) = (state.swaps_path(), state.swaps_lock.clone());
    off_thread(move || {
        let _guards = (dguard, wguard);
        hardware::check_mainnet_confirmed(&profile, confirm_mainnet.unwrap_or(false))?;
        let view = match &*proposal.lock().unwrap_or_else(|p| p.into_inner()) {
            Some((id, view)) if *id == proposal_id => view.clone(),
            _ => anyhow::bail!("this send is no longer pending; build it again"),
        };
        let status = status_emitter(&app);
        let zsig = with_device(&slot, |s| hardware::device_sign(&profile, s, &view, &status))?;
        let sent = hardware::finish(&profile, &view, &zsig, true, &status)?;
        *proposal.lock().unwrap_or_else(|p| p.into_inner()) = None;
        // The deposit of a swap: its txid goes with the swap, so polling
        // tells 1Click about it, even after a restart.
        let _lock = swaps_lock.lock().unwrap_or_else(|p| p.into_inner());
        if let Err(e) = near::record_deposit(&swaps_path, &proposal_id, &sent.txid) {
            eprintln!("LeekWallet Zcash: deposit {} not recorded with its swap: {e:#}", sent.txid);
        }
        Ok(sent)
    })
    .await
}

// ------------------------------------------------------------------- swaps

/// What the swap screen needs to know before anything else.
#[derive(Clone, Debug, serde::Serialize)]
#[serde(rename_all = "camelCase")]
struct SwapInfo {
    /// "test" or "main". Swaps are mainnet only; testnet previews prices.
    network: String,
    /// A 1Click API key is set: no extra platform fee, confidential quotes.
    authenticated: bool,
    api_key_env: &'static str,
    explorer: &'static str,
    default_slippage_bps: u32,
    max_slippage_bps: u32,
}

#[tauri::command]
async fn swap_info(state: State<'_, AppState>) -> Result<SwapInfo, String> {
    Ok(SwapInfo {
        network: state.profile().net_name().to_string(),
        authenticated: state.one_click().has_api_key(),
        api_key_env: near::API_KEY_ENV,
        explorer: near::EXPLORER,
        default_slippage_bps: near::DEFAULT_SLIPPAGE_BPS,
        max_slippage_bps: near::MAX_SLIPPAGE_BPS,
    })
}

/// How long the token list (with its USD prices) is reused.
const TOKENS_FRESH: std::time::Duration = std::time::Duration::from_secs(300);

type TokenCache = Mutex<Option<(std::time::Instant, Vec<near::Token>)>>;

fn tokens_cached(cache: &TokenCache, client: &near::OneClick<near::Http>) -> anyhow::Result<Vec<near::Token>> {
    if let Some((at, t)) = &*cache.lock().unwrap_or_else(|p| p.into_inner()) {
        if at.elapsed() < TOKENS_FRESH {
            return Ok(t.clone());
        }
    }
    let t = client.tokens().map_err(|e| anyhow::anyhow!(near::explain(&e, None)))?;
    *cache.lock().unwrap_or_else(|p| p.into_inner()) = Some((std::time::Instant::now(), t.clone()));
    Ok(t)
}

/// The assets 1Click swaps (cached for five minutes).
#[tauri::command]
async fn swap_tokens(state: State<'_, AppState>) -> Result<Vec<near::Token>, String> {
    let (cache, client) = (state.swap_tokens.clone(), state.one_click());
    off_thread(move || tokens_cached(&cache, &client)).await
}

/// A quote. A dry one only prices the swap (testnet: only this). A real
/// one, on mainnet, gets a deposit address and a fresh Orchard-only
/// address of this wallet for the refund (from ZEC) or the payout (into
/// ZEC), and is stored with the swaps.
#[tauri::command]
async fn swap_quote(state: State<'_, AppState>, params: near::QuoteParams) -> Result<near::SwapRecord, String> {
    // A real quote records a new address in the wallet.
    let guard = if params.dry { None } else { Some(BusyGuard::take(&state.busy, WALLET_BUSY)?) };
    let (profile, cache, client) = (state.profile(), state.swap_tokens.clone(), state.one_click());
    let (path, lock) = (state.swaps_path(), state.swaps_lock.clone());
    off_thread(move || {
        let _guard = guard;
        if profile.network == Network::MainNetwork && !params.dry {
            let s = backend::profile_status(&profile)?;
            if !s.has_account {
                anyhow::bail!("Import an account first: a swap pays to and from this wallet.");
            }
            if params.direction == near::Direction::FromZec && !s.hardware {
                anyhow::bail!(
                    "This is a watch-only account; nothing could sign the deposit. Import the account from a LeekWallet to swap from ZEC."
                );
            }
        }
        let tokens = tokens_cached(&cache, &client)?;
        let fresh = !params.dry;
        let own = || -> anyhow::Result<String> {
            match backend::swap_address(&profile, fresh) {
                Ok(a) => Ok(a),
                // A price preview needs no wallet.
                Err(_) if !fresh => Ok(near::PREVIEW_ADDRESS.to_string()),
                Err(e) => Err(e),
            }
        };
        let spendable = || -> anyhow::Result<Option<u64>> { Ok(backend::balances(&profile)?.map(|b| b.spendable)) };
        let ctx = near::QuoteContext {
            network: profile.network,
            own_address: &own,
            spendable: &spendable,
            now: time::OffsetDateTime::now_utc(),
        };
        let rec = near::request_quote(&client, &tokens, &ctx, &params)?;
        if !rec.dry {
            let _lock = lock.lock().unwrap_or_else(|p| p.into_inner());
            near::save(&path, &rec)?;
        }
        Ok(rec)
    })
    .await
}

/// Builds the deposit of a swap from ZEC: the quoted amount to its deposit
/// address, a send like any other (compact signer view), returned for the
/// review and signed by `sign_and_send`, which records the txid with the
/// swap. Refused off mainnet, for an expired quote, or without the funds.
#[tauri::command]
async fn swap_execute(state: State<'_, AppState>, id: String) -> Result<hardware::ProposalDto, String> {
    let guard = BusyGuard::take(&state.busy, WALLET_BUSY)?;
    let (profile, slot) = (state.profile(), state.proposal.clone());
    let (path, lock) = (state.swaps_path(), state.swaps_lock.clone());
    off_thread(move || {
        let _guard = guard;
        let _lock = lock.lock().unwrap_or_else(|p| p.into_inner());
        let mut rec = near::get(&path, &id)?;
        let zat = near::check_executable(&rec, profile.network, time::OffsetDateTime::now_utc())?;
        near::check_funds(backend::balances(&profile)?.map(|b| b.spendable), zat)?;
        let address = rec.deposit_address.clone().unwrap_or_default();
        let (dto, view) = hardware::propose_send(&profile, &address, &near::zat_to_zec(zat), None)?;
        // What the device will be asked to sign is exactly the deposit.
        let pays = dto.payments.len() == 1 && dto.payments[0].recipient == address && dto.payments[0].amount_zat == zat;
        if !pays {
            anyhow::bail!("the proposal does not pay exactly the quoted deposit; not signing it");
        }
        rec.proposal_id = Some(dto.id.clone());
        near::save(&path, &rec)?;
        *slot.lock().unwrap_or_else(|p| p.into_inner()) = Some((dto.id.clone(), view));
        Ok(dto)
    })
    .await
}

/// Asks 1Click where a swap stands (telling it the deposit txid first if
/// it has not heard it) and returns the updated swap.
#[tauri::command]
async fn swap_status(state: State<'_, AppState>, id: String) -> Result<near::SwapRecord, String> {
    let client = state.one_click();
    let (path, lock) = (state.swaps_path(), state.swaps_lock.clone());
    off_thread(move || {
        let mut rec = {
            let _lock = lock.lock().unwrap_or_else(|p| p.into_inner());
            near::get(&path, &id)?
        };
        // The network calls run without the lock.
        near::refresh_status(&client, &mut rec, time::OffsetDateTime::now_utc());
        let _lock = lock.lock().unwrap_or_else(|p| p.into_inner());
        let current = near::get(&path, &id)?;
        // A deposit recorded meanwhile wins; the next poll reports on it.
        if current.deposit_txid != rec.deposit_txid {
            return Ok(current);
        }
        near::save(&path, &rec)?;
        Ok(rec)
    })
    .await
}

/// The swaps of this network, newest first.
#[tauri::command]
async fn swap_list(state: State<'_, AppState>) -> Result<Vec<near::SwapRecord>, String> {
    let (path, lock) = (state.swaps_path(), state.swaps_lock.clone());
    off_thread(move || {
        let _lock = lock.lock().unwrap_or_else(|p| p.into_inner());
        near::list(&path)
    })
    .await
}

/// Removes a swap that can no longer move funds. Returns the rest.
#[tauri::command]
async fn swap_forget(state: State<'_, AppState>, id: String) -> Result<Vec<near::SwapRecord>, String> {
    let (path, lock) = (state.swaps_path(), state.swaps_lock.clone());
    off_thread(move || {
        let _lock = lock.lock().unwrap_or_else(|p| p.into_inner());
        near::forget(&path, &id)
    })
    .await
}

// ----------------------------------------------------------------- network

/// The network the app is on and its lightwalletd server.
#[tauri::command]
async fn network_settings(state: State<'_, AppState>) -> Result<settings::NetworkDto, String> {
    Ok(state.network_dto())
}

/// Switches to `network` ("test" or "main") and saves the choice. The other
/// network's wallet stays in its own directory, untouched. The device
/// session and any pending send are dropped: both belong to the old
/// network. Refused while a wallet or device call runs.
#[tauri::command]
async fn set_network(state: State<'_, AppState>, network: String) -> Result<settings::NetworkDto, String> {
    let dguard = BusyGuard::take(&state.device_busy, DEVICE_BUSY)?;
    let wguard = BusyGuard::take(&state.busy, WALLET_BUSY)?;
    let (app_data, slot, proposal) = (state.app_data.clone(), state.device.clone(), state.proposal.clone());
    let (current, from_env) = (state.profile.clone(), state.from_environment.clone());
    off_thread(move || {
        let _guards = (dguard, wguard);
        let network = settings::parse_network(&network)?;
        let (_, p) = settings::switch_network(&settings::path_in(&app_data), &app_data, network)?;
        // After the save succeeded, and before the guards let another
        // command in: no session or proposal of the old network survives.
        *slot.lock().unwrap_or_else(|p| p.into_inner()) = None;
        *proposal.lock().unwrap_or_else(|p| p.into_inner()) = None;
        *current.lock().unwrap_or_else(|p| p.into_inner()) = p;
        from_env.store(false, Ordering::SeqCst);
        Ok(())
    })
    .await?;
    Ok(state.network_dto())
}

/// Sets the lightwalletd server of the current network and saves it; an
/// empty one goes back to the default. Its chain is checked when it is
/// used (sync, import, broadcast).
#[tauri::command]
async fn set_server(state: State<'_, AppState>, server: Option<String>) -> Result<settings::NetworkDto, String> {
    let wguard = BusyGuard::take(&state.busy, WALLET_BUSY)?;
    let (app_data, current) = (state.app_data.clone(), state.profile.clone());
    off_thread(move || {
        let _guard = wguard;
        let network = current.lock().unwrap_or_else(|p| p.into_inner()).network;
        let saved = settings::save_server(&settings::path_in(&app_data), network, server.as_deref())?;
        current.lock().unwrap_or_else(|p| p.into_inner()).server = saved.server_for(network);
        Ok(())
    })
    .await?;
    Ok(state.network_dto())
}

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    tauri::Builder::default()
        .setup(|app| {
            let app_data = app.path().app_data_dir()?;
            let (profile, from_env) = startup_profile(&app_data);
            app.manage(AppState {
                app_data,
                profile: Arc::new(Mutex::new(profile)),
                from_environment: Arc::new(AtomicBool::new(from_env)),
                busy: Arc::new(AtomicBool::new(false)),
                device_busy: Arc::new(AtomicBool::new(false)),
                device: Arc::new(Mutex::new(None)),
                proposal: Arc::new(Mutex::new(None)),
                swaps_lock: Arc::new(Mutex::new(())),
                swap_tokens: Arc::new(Mutex::new(None)),
            });
            Ok(())
        })
        .invoke_handler(tauri::generate_handler![
            profile_status,
            init_watch_only,
            sync,
            balances,
            addresses,
            history,
            next_address,
            qr_svg,
            tip,
            list_ports,
            ble_scan,
            device_pair,
            device_session,
            device_disconnect,
            device_import,
            verify_address,
            propose_send,
            propose_shield,
            sign_and_send,
            contacts_list,
            contact_save,
            contact_remove,
            network_settings,
            set_network,
            set_server,
            swap_info,
            swap_tokens,
            swap_quote,
            swap_execute,
            swap_status,
            swap_list,
            swap_forget
        ])
        .run(tauri::generate_context!())
        .expect("error while running LeekWallet Zcash");
}
