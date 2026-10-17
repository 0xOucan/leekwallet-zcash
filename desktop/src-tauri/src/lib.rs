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
pub mod hardware;
pub mod link;

use std::path::PathBuf;
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

/// The app's one profile, fixed at startup, and the device session.
pub struct AppState {
    profile: Profile,
    /// A sync, import or send is running; another is refused meanwhile.
    busy: Arc<AtomicBool>,
    /// A device call is running. The device answers one request at a time
    /// and its frames carry no request ids, so a second is refused.
    device_busy: Arc<AtomicBool>,
    /// The paired device, kept between commands.
    device: Arc<Mutex<Option<DeviceSession>>>,
    /// The last proposed send: its id and the signer view for the device.
    proposal: Arc<Mutex<Option<(String, Vec<u8>)>>>,
}

/// The network: testnet unless `LEEKZ_NETWORK=main`. A switch in the UI
/// comes with software wallets (8.2); until then mainnet is opt-in from
/// the environment only.
fn network_from_env() -> Network {
    match std::env::var("LEEKZ_NETWORK").as_deref() {
        Ok("main") | Ok("mainnet") => Network::MainNetwork,
        _ => Network::TestNetwork,
    }
}

/// `<app data dir>/wallets/<test|main>`: one wallet directory per network,
/// since a zecz data directory is bound to one.
fn data_dir(app: &AppHandle, network: Network) -> tauri::Result<PathBuf> {
    Ok(app.path().app_data_dir()?.join("wallets").join(zecz::wallet::network_name(network)))
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
    let p = state.profile.clone();
    off_thread(move || backend::profile_status(&p)).await
}

#[tauri::command]
async fn init_watch_only(state: State<'_, AppState>, ufvk: String, birthday: u32) -> Result<backend::Imported, String> {
    let guard = BusyGuard::take(&state.busy, WALLET_BUSY)?;
    let p = state.profile.clone();
    off_thread(move || {
        let _guard = guard;
        backend::init_watch_only(&p, &ufvk, birthday)
    })
    .await
}

#[tauri::command]
async fn sync(app: AppHandle, state: State<'_, AppState>) -> Result<backend::SyncDone, String> {
    let guard = BusyGuard::take(&state.busy, WALLET_BUSY)?;
    let p = state.profile.clone();
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
    let p = state.profile.clone();
    off_thread(move || backend::balances(&p)).await
}

#[tauri::command]
async fn addresses(state: State<'_, AppState>) -> Result<backend::AddressesDto, String> {
    let p = state.profile.clone();
    off_thread(move || backend::addresses(&p)).await
}

/// Rotates the receive address to a fresh diversified one.
#[tauri::command]
async fn next_address(state: State<'_, AppState>) -> Result<backend::NewAddressDto, String> {
    let guard = BusyGuard::take(&state.busy, WALLET_BUSY)?;
    let p = state.profile.clone();
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
    let server = state.profile.server.clone();
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
    let (slot, profile) = (state.device.clone(), state.profile.clone());
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
    let (slot, profile) = (state.device.clone(), state.profile.clone());
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
    let (slot, profile) = (state.device.clone(), state.profile.clone());
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
    let (slot, profile) = (state.device.clone(), state.profile.clone());
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
    let (profile, slot) = (state.profile.clone(), state.proposal.clone());
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
    let (profile, slot) = (state.profile.clone(), state.proposal.clone());
    off_thread(move || {
        let _guard = guard;
        let (dto, view) = hardware::propose_shield(&profile)?;
        *slot.lock().unwrap_or_else(|p| p.into_inner()) = Some((dto.id.clone(), view));
        Ok(dto)
    })
    .await
}

/// Signs the reviewed proposal on the device, then proves, stores and
/// broadcasts it; progress on `device://status`. Returns the txid.
#[tauri::command]
async fn sign_and_send(
    app: AppHandle,
    state: State<'_, AppState>,
    proposal_id: String,
) -> Result<hardware::SentDto, String> {
    let dguard = BusyGuard::take(&state.device_busy, DEVICE_BUSY)?;
    let wguard = BusyGuard::take(&state.busy, WALLET_BUSY)?;
    let (slot, profile, proposal) = (state.device.clone(), state.profile.clone(), state.proposal.clone());
    off_thread(move || {
        let _guards = (dguard, wguard);
        let view = match &*proposal.lock().unwrap_or_else(|p| p.into_inner()) {
            Some((id, view)) if *id == proposal_id => view.clone(),
            _ => anyhow::bail!("this send is no longer pending; build it again"),
        };
        let status = status_emitter(&app);
        let zsig = with_device(&slot, |s| hardware::device_sign(&profile, s, &view, &status))?;
        let sent = hardware::finish(&profile, &view, &zsig, true, &status)?;
        *proposal.lock().unwrap_or_else(|p| p.into_inner()) = None;
        Ok(sent)
    })
    .await
}

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    tauri::Builder::default()
        .setup(|app| {
            let network = network_from_env();
            let server = std::env::var("LEEKZ_SERVER").ok().filter(|s| !s.is_empty());
            let profile = Profile::new(network, data_dir(app.handle(), network)?, server);
            app.manage(AppState {
                profile,
                busy: Arc::new(AtomicBool::new(false)),
                device_busy: Arc::new(AtomicBool::new(false)),
                device: Arc::new(Mutex::new(None)),
                proposal: Arc::new(Mutex::new(None)),
            });
            Ok(())
        })
        .invoke_handler(tauri::generate_handler![
            profile_status,
            init_watch_only,
            sync,
            balances,
            addresses,
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
            sign_and_send
        ])
        .run(tauri::generate_context!())
        .expect("error while running LeekWallet Zcash");
}
