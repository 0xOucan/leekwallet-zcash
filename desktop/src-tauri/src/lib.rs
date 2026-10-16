//! LeekWallet Zcash desktop: the Tauri layer.
//!
//! The wallet itself is `zecz` (host/zecz-core); [`backend`] turns its
//! typed API into blocking calls with serializable results, and this file
//! only exposes them as commands. No protocol or key logic lives in the
//! webview.
//!
//! Every command that touches the wallet or the network runs on its own
//! worker thread, so the window never waits on SQLite or lightwalletd.

pub mod backend;

use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;

use tauri::{AppHandle, Emitter, Manager, State};
use zecz::{Network, Profile};

/// The event `sync` emits for each step, carrying a [`backend::SyncProgress`].
pub const SYNC_EVENT: &str = "sync://progress";

/// The app's one profile, fixed at startup.
pub struct AppState {
    profile: Profile,
    /// A sync is running; a second one, or an import, is refused meanwhile.
    busy: Arc<AtomicBool>,
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
    fn take(flag: &Arc<AtomicBool>) -> Result<Self, String> {
        if flag.swap(true, Ordering::SeqCst) {
            return Err("a sync is already running".into());
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
    let guard = BusyGuard::take(&state.busy)?;
    let p = state.profile.clone();
    off_thread(move || {
        let _guard = guard;
        backend::init_watch_only(&p, &ufvk, birthday)
    })
    .await
}

#[tauri::command]
async fn sync(app: AppHandle, state: State<'_, AppState>) -> Result<backend::SyncDone, String> {
    let guard = BusyGuard::take(&state.busy)?;
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

#[tauri::command]
async fn tip(state: State<'_, AppState>) -> Result<backend::TipDto, String> {
    let server = state.profile.server.clone();
    off_thread(move || backend::tip(&server)).await
}

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    tauri::Builder::default()
        .setup(|app| {
            let network = network_from_env();
            let server = std::env::var("LEEKZ_SERVER").ok().filter(|s| !s.is_empty());
            let profile = Profile::new(network, data_dir(app.handle(), network)?, server);
            app.manage(AppState { profile, busy: Arc::new(AtomicBool::new(false)) });
            Ok(())
        })
        .invoke_handler(tauri::generate_handler![profile_status, init_watch_only, sync, balances, addresses, tip])
        .run(tauri::generate_context!())
        .expect("error while running LeekWallet Zcash");
}
