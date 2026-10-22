//! The Zcash wallet's commands: sync, balances, history, addresses, send
//! and shield, contacts and servers, over the wallet layer in
//! app/zcash-wallet (`leek_zcash_wallet`, over zecz in host/zecz-core).
//!
//! The board never appears here. The webview holds the paired, encrypted
//! session (main.ts's Client), so a send is three calls with the webview
//! in the middle: propose here (the signer view goes back), zSign on the
//! board there, and finish here with the `.zsig` it returns. The view the
//! board signed is the one `finish` checks the `.zsig` against, by hash.
//!
//! Every command names its wallet: the network, and the board's seed
//! fingerprint and account (from `zGetAccount`), or `watch-<hash>` for a
//! viewing key imported by hand (`leek_zcash_wallet::keys`). A BIP39
//! passphrase on the board is another seed, so another wallet, with its
//! own directory and address book. The server setting is per network and
//! shared. Nothing here holds a spending key.

use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;

use anyhow::{Context, Result};
use leek_zcash_wallet::keys::{self, WalletKey};
use leek_zcash_wallet::{backend, contacts, hardware, settings};
use serde::Serialize;
use tauri::{AppHandle, Emitter, Manager, State};
use zecz::wallet;
use zecz::{Network, Profile};

/// The event `zw_sync` emits per step: `{ network, progress }`.
pub const SYNC_EVENT: &str = "zw://sync";
/// The event `zw_finish` emits per step: `{ network, status }`.
pub const FINISH_EVENT: &str = "zw://finish";

const BUSY: &str = "this network's wallet is busy (a sync, an import or a send is running)";

pub struct ZState {
    /// `<app data>/zcash`: wallets/, contacts-*.json, swaps-*.json,
    /// settings.json (leek_zcash_wallet::keys).
    pub(crate) root: PathBuf,
    /// One flag per network: a sync on testnet does not hold up mainnet.
    busy: [Arc<AtomicBool>; 2],
}

impl ZState {
    pub fn new(app: &AppHandle) -> Result<Self> {
        let app_data = app.path().app_data_dir().context("no app data directory")?;
        let root = app_data.join("zcash");
        Ok(ZState { root, busy: [Arc::default(), Arc::default()] })
    }

    pub(crate) fn server(&self, net: Network) -> String {
        settings::load(&settings::path_in(&self.root)).unwrap_or_default().server_for(net)
    }

    /// The profile of `key`'s wallet, settling the old layout's wallet of
    /// that network first (it moves to its own account's place, which may
    /// be this one).
    pub(crate) fn profile(&self, key: &WalletKey) -> Profile {
        let dir = key.wallet_dir(&self.root);
        if !dir.exists() {
            if let Err(e) = keys::settle_legacy(&self.root, key.net) {
                eprintln!("zcash: the old {} wallet was not moved: {e:#}", key.net_name());
            }
        }
        Profile::new(key.net, dir, Some(self.server(key.net)))
    }

    pub(crate) fn flag(&self, net: Network) -> &Arc<AtomicBool> {
        &self.busy[usize::from(net == Network::MainNetwork)]
    }
}

pub(crate) fn parse_net(network: &str) -> Result<Network, String> {
    match network {
        "test" => Ok(Network::TestNetwork),
        "main" => Ok(Network::MainNetwork),
        other => Err(format!("unknown network {other:?} (test or main)")),
    }
}

/// Runs `f` on a fresh thread and awaits it without blocking the runtime.
/// SQLite, lightwalletd, 1Click and Halo 2 proving all block, some for
/// minutes.
pub(crate) async fn off_thread<T, F>(f: F) -> Result<T, String>
where
    T: Send + 'static,
    F: FnOnce() -> Result<T> + Send + 'static,
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
        Err(_) => Err("the wallet worker stopped unexpectedly".into()),
    }
}

/// Clears a busy flag when the command ends, however it ends.
pub(crate) struct Busy(Arc<AtomicBool>);

impl Busy {
    pub(crate) fn take(flag: &Arc<AtomicBool>) -> Result<Self, String> {
        if flag.swap(true, Ordering::SeqCst) {
            return Err(BUSY.into());
        }
        Ok(Busy(flag.clone()))
    }
}

impl Drop for Busy {
    fn drop(&mut self) {
        self.0.store(false, Ordering::SeqCst);
    }
}

// ------------------------------------------------------------------ status

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct StatusDto {
    #[serde(flatten)]
    pub profile: backend::ProfileStatus,
    pub default_server: String,
    /// The full ZIP 32 seed fingerprint of the board account, hex, so the
    /// webview can check the board it is about to sign with holds it.
    pub seed_fingerprint: Option<String>,
    pub account_index: Option<u32>,
}

fn status(p: &Profile) -> Result<StatusDto> {
    let origin = hardware::wallet_origin(p)?;
    Ok(StatusDto {
        profile: backend::profile_status(p)?,
        default_server: wallet::default_server(p.network).to_string(),
        seed_fingerprint: origin.map(|o| hex::encode(o.seed_fingerprint)),
        account_index: origin.map(|o| o.account_index),
    })
}

/// Whether this build has the wallet. A build without the `zcash-wallet`
/// feature has no such command, and the webview reads the rejection.
#[tauri::command]
pub fn zw_available() -> bool {
    true
}

/// The network, seed fingerprint (or watch-only name) and account every
/// wallet command names.
pub(crate) fn key_of(network: &str, wallet: &str, account: u32) -> Result<WalletKey, String> {
    WalletKey::parse(parse_net(network)?, wallet, account).map_err(|e| format!("{e:#}"))
}

#[tauri::command]
pub async fn zw_status(state: State<'_, ZState>, network: String, wallet: String, account: u32) -> Result<StatusDto, String> {
    let key = key_of(&network, &wallet, account)?;
    let p = state.profile(&key);
    off_thread(move || status(&p)).await
}

/// The wallets of `network` on this device (board accounts and viewing
/// keys), for opening one without the board.
#[tauri::command]
pub async fn zw_wallets(state: State<'_, ZState>, network: String) -> Result<Vec<keys::WalletInfo>, String> {
    let net = parse_net(&network)?;
    let root = state.root.clone();
    off_thread(move || keys::list_wallets(&root, net)).await
}

// ------------------------------------------------------------------ import

/// The wallet is the one of the account's own seed fingerprint and
/// account: the board's answer decides which wallet is imported into.
#[tauri::command]
#[allow(clippy::too_many_arguments)]
pub async fn zw_import(
    state: State<'_, ZState>,
    network: String,
    ufvk: String,
    seed_fingerprint: String,
    account: u32,
    ua: String,
    birthday: u32,
) -> Result<backend::Imported, String> {
    let key = key_of(&network, &seed_fingerprint, account)?;
    let net = key.net;
    let guard = Busy::take(state.flag(net))?;
    let p = state.profile(&key);
    off_thread(move || {
        let _guard = guard;
        backend::import_board(&p, &ufvk, &seed_fingerprint, account, &ua, birthday)
    })
    .await
}

/// A viewing key imported by hand: watch-only, it sees the account's
/// funds and can never spend them. Returns the name the other commands
/// take as `wallet` (`watch-<hash>`), with the import.
#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct WatchImported {
    pub wallet: String,
    #[serde(flatten)]
    pub imported: backend::Imported,
}

#[tauri::command]
pub async fn zw_import_watch(state: State<'_, ZState>, network: String, ufvk: String, birthday: u32) -> Result<WatchImported, String> {
    let net = parse_net(&network)?;
    let key = WalletKey::watch(net, &ufvk);
    let guard = Busy::take(state.flag(net))?;
    let p = state.profile(&key);
    off_thread(move || {
        let _guard = guard;
        let imported = backend::init_watch_only(&p, &ufvk, birthday)?;
        Ok(WatchImported { wallet: key.wallet(), imported })
    })
    .await
}

/// Deletes this network's wallet directory: the database of what the
/// viewing key has seen, and nothing that could spend. A re-import scans
/// again from the birthday. Refused while the wallet is busy.
#[tauri::command]
pub async fn zw_forget(state: State<'_, ZState>, network: String, wallet: String, account: u32) -> Result<(), String> {
    let key = key_of(&network, &wallet, account)?;
    let guard = Busy::take(state.flag(key.net))?;
    let dir = key.wallet_dir(&state.root);
    off_thread(move || {
        let _guard = guard;
        match std::fs::remove_dir_all(&dir) {
            Ok(()) => Ok(()),
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(()),
            Err(e) => Err(e).with_context(|| format!("removing {}", dir.display())),
        }
    })
    .await
}

// -------------------------------------------------------------------- sync

#[derive(Clone, Serialize)]
pub(crate) struct Tagged<T: Serialize> {
    network: String,
    #[serde(flatten)]
    inner: T,
}

#[tauri::command]
pub async fn zw_sync(app: AppHandle, state: State<'_, ZState>, network: String, wallet: String, account: u32) -> Result<backend::SyncDone, String> {
    let key = key_of(&network, &wallet, account)?;
    let guard = Busy::take(state.flag(key.net))?;
    let p = state.profile(&key);
    off_thread(move || {
        let _guard = guard;
        backend::sync(&p, |e| {
            let _ = app.emit(SYNC_EVENT, Tagged { network: network.clone(), inner: e });
        })
    })
    .await
}

#[tauri::command]
pub async fn zw_balances(state: State<'_, ZState>, network: String, wallet: String, account: u32) -> Result<Option<backend::BalancesDto>, String> {
    let p = state.profile(&key_of(&network, &wallet, account)?);
    off_thread(move || backend::balances(&p)).await
}

#[tauri::command]
pub async fn zw_addresses(state: State<'_, ZState>, network: String, wallet: String, account: u32) -> Result<backend::AddressesDto, String> {
    let p = state.profile(&key_of(&network, &wallet, account)?);
    off_thread(move || backend::addresses(&p)).await
}

#[tauri::command]
pub async fn zw_next_address(state: State<'_, ZState>, network: String, wallet: String, account: u32) -> Result<backend::NewAddressDto, String> {
    let key = key_of(&network, &wallet, account)?;
    let guard = Busy::take(state.flag(key.net))?;
    let p = state.profile(&key);
    off_thread(move || {
        let _guard = guard;
        backend::next_address(&p)
    })
    .await
}

#[tauri::command]
pub async fn zw_history(state: State<'_, ZState>, network: String, wallet: String, account: u32) -> Result<Vec<backend::TxDto>, String> {
    let p = state.profile(&key_of(&network, &wallet, account)?);
    off_thread(move || backend::history(&p)).await
}

/// The server's chain tip: the birthday of a wallet made today.
#[tauri::command]
pub async fn zw_tip(state: State<'_, ZState>, network: String) -> Result<backend::TipDto, String> {
    let net = parse_net(&network)?;
    let server = state.server(net);
    off_thread(move || {
        let t = backend::tip(&server)?;
        wallet::check_chain(net, &t.chain_name)?;
        Ok(t)
    })
    .await
}

/// What kind of address `address` is on `network` ("unified", "transparent", ...),
/// or why it is not one this wallet can pay.
#[tauri::command]
pub fn zw_check_address(network: String, address: String) -> Result<String, String> {
    wallet::check_address(parse_net(&network)?, address.trim()).map(str::to_string).map_err(|e| format!("{e:#}"))
}

// -------------------------------------------------------------------- send

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Proposed {
    pub proposal: hardware::ProposalDto,
    /// The compact signer view for the board, exactly the bytes `zw_finish`
    /// expects back with the `.zsig`.
    pub view: Vec<u8>,
}

#[tauri::command]
#[allow(clippy::too_many_arguments)]
pub async fn zw_propose_send(
    state: State<'_, ZState>,
    network: String,
    wallet: String,
    account: u32,
    to: String,
    amount: String,
    memo: Option<String>,
) -> Result<Proposed, String> {
    let key = key_of(&network, &wallet, account)?;
    let guard = Busy::take(state.flag(key.net))?;
    let p = state.profile(&key);
    off_thread(move || {
        let _guard = guard;
        let (proposal, view) = hardware::propose_send(&p, &to, &amount, memo.as_deref())?;
        Ok(Proposed { proposal, view })
    })
    .await
}

#[tauri::command]
pub async fn zw_propose_shield(state: State<'_, ZState>, network: String, wallet: String, account: u32) -> Result<Proposed, String> {
    let key = key_of(&network, &wallet, account)?;
    let guard = Busy::take(state.flag(key.net))?;
    let p = state.profile(&key);
    off_thread(move || {
        let _guard = guard;
        let (proposal, view) = hardware::propose_shield(&p)?;
        Ok(Proposed { proposal, view })
    })
    .await
}

/// Applies the board's `.zsig` to the kept full PCZT (each signature is
/// checked against its key), proves, stores and broadcasts. A mainnet
/// transaction needs `mainnet_confirmed`, the confirmation the user ticked
/// under the review. When the send was a swap's deposit (src/swaps.rs),
/// its txid is recorded with the swap, so following it tells 1Click.
#[tauri::command]
#[allow(clippy::too_many_arguments)]
pub async fn zw_finish(
    app: AppHandle,
    state: State<'_, ZState>,
    network: String,
    wallet: String,
    account: u32,
    view: Vec<u8>,
    zsig: Vec<u8>,
    mainnet_confirmed: bool,
) -> Result<hardware::SentDto, String> {
    let key = key_of(&network, &wallet, account)?;
    let guard = Busy::take(state.flag(key.net))?;
    let p = state.profile(&key);
    let swaps = key.swaps_path(&state.root);
    off_thread(move || {
        let _guard = guard;
        hardware::check_mainnet_confirmed(&p, mainnet_confirmed)?;
        let sent = hardware::finish(&p, &view, &zsig, true, &|s| {
            let _ = app.emit(FINISH_EVENT, Tagged { network: network.clone(), inner: s });
        })?;
        crate::swaps::deposit_sent(&swaps, &hardware::proposal_id(&view), &sent.txid);
        Ok(sent)
    })
    .await
}

// ---------------------------------------------------------------- settings

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct ServerDto {
    pub server: String,
    pub default_server: String,
    pub custom_server: bool,
}

/// Sets this network's lightwalletd server; None or empty goes back to the
/// default. Checked by asking it for its tip and chain before it is saved.
#[tauri::command]
pub async fn zw_set_server(state: State<'_, ZState>, network: String, server: Option<String>) -> Result<ServerDto, String> {
    let net = parse_net(&network)?;
    let guard = Busy::take(state.flag(net))?;
    let root = state.root.clone();
    off_thread(move || {
        let _guard = guard;
        let server = server.map(|s| s.trim().to_string()).filter(|s| !s.is_empty());
        if let Some(s) = &server {
            let s = settings::check_server(s)?;
            let t = backend::tip(&s)?;
            wallet::check_chain(net, &t.chain_name)?;
        }
        let saved = settings::save_server(&settings::path_in(&root), net, server.as_deref())?;
        Ok(ServerDto {
            server: saved.server_for(net),
            default_server: wallet::default_server(net).to_string(),
            custom_server: !saved.is_default_server(net),
        })
    })
    .await
}

// ---------------------------------------------------------------- contacts

#[tauri::command]
pub async fn zw_contacts(state: State<'_, ZState>, network: String, wallet: String, account: u32) -> Result<Vec<contacts::Contact>, String> {
    let key = key_of(&network, &wallet, account)?;
    let path = key.contacts_path(&state.root);
    off_thread(move || contacts::list(&path)).await
}

#[tauri::command]
#[allow(clippy::too_many_arguments)]
pub async fn zw_contact_save(
    state: State<'_, ZState>,
    network: String,
    wallet: String,
    account: u32,
    name: String,
    address: String,
    replacing: Option<String>,
) -> Result<Vec<contacts::Contact>, String> {
    let key = key_of(&network, &wallet, account)?;
    let net = key.net;
    let path = key.contacts_path(&state.root);
    off_thread(move || contacts::save(&path, net, &name, &address, replacing.as_deref())).await
}

#[tauri::command]
pub async fn zw_contact_remove(state: State<'_, ZState>, network: String, wallet: String, account: u32, name: String) -> Result<Vec<contacts::Contact>, String> {
    let path = key_of(&network, &wallet, account)?.contacts_path(&state.root);
    off_thread(move || contacts::remove(&path, &name)).await
}

#[cfg(test)]
mod tests {
    use super::*;

    fn key(name: &str) -> serde_json::Value {
        let v: Vec<serde_json::Value> =
            serde_json::from_slice(&std::fs::read(concat!(env!("CARGO_MANIFEST_DIR"), "/../../vectors/keys.json")).unwrap()).unwrap();
        v.into_iter().find(|k| k["name"] == name).unwrap()
    }

    /// The commands' path against testnet lightwalletd, with the public
    /// abandon-art test account: import (tree state from the server), sync
    /// a few blocks, read balances, addresses and history, refuse a second
    /// account. Needs the network, so it only runs when asked:
    /// `cargo test --lib -- --ignored online`.
    #[test]
    #[ignore]
    fn online_import_sync_and_read_on_testnet() {
        let k = key("test/abandon-art/pp0/acct0");
        let dir = std::env::temp_dir().join(format!("leek-zw-online-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        let p = Profile::new(Network::TestNetwork, dir, None);
        let tip = backend::tip(&p.server).unwrap();
        wallet::check_chain(p.network, &tip.chain_name).unwrap();
        let birthday = tip.height as u32 - 50;
        let (u, fp, ua) = (k["ufvk"].as_str().unwrap(), k["seed_fingerprint"].as_str().unwrap(), k["ua"].as_str().unwrap());
        let r = backend::import_board(&p, u, fp, 0, ua, birthday).unwrap();
        assert!(!r.already_imported);
        assert_eq!(r.addresses.unified, ua);
        assert!(backend::import_board(&p, u, fp, 0, ua, birthday).unwrap().already_imported);
        let s = status(&p).unwrap();
        assert_eq!(s.seed_fingerprint.as_deref(), Some(fp));
        assert_eq!(s.account_index, Some(0));
        let done = backend::sync(&p, |_| {}).unwrap();
        assert!(done.fully_scanned >= birthday);
        assert!(backend::balances(&p).unwrap().is_some());
        backend::history(&p).unwrap();
        let other = key("test/abandon-art/pp0/acct1");
        assert!(backend::import_board(&p, other["ufvk"].as_str().unwrap(), fp, 1, other["ua"].as_str().unwrap(), birthday).is_err());
        let _ = std::fs::remove_dir_all(&p.data_dir);
    }

    /// What the Send form asks before filling a scanned address in: the
    /// address must be of the wallet's network, whatever its kind.
    #[test]
    fn an_address_of_the_other_network_is_refused() {
        for (net, other) in [("test", "main"), ("main", "test")] {
            let k = key(&format!("{net}/abandon-art/pp0/acct0"));
            for field in ["ua", "transparent_address", "ua_orchard_only"] {
                let a = k[field].as_str().unwrap().to_string();
                assert!(zw_check_address(net.into(), a.clone()).is_ok(), "{net} {field}");
                assert!(zw_check_address(other.into(), a).is_err(), "{net} {field} accepted on {other}");
            }
        }
        assert!(zw_check_address("test".into(), "zcash:nothing".into()).is_err());
    }

    #[test]
    fn networks_are_named_not_guessed() {
        assert!(parse_net("test").is_ok() && parse_net("main").is_ok());
        assert!(parse_net("mainnet").is_err() && parse_net("").is_err());
    }

    #[test]
    fn commands_name_board_and_watch_only_wallets() {
        let fp = "ab".repeat(32);
        assert!(key_of("test", &fp, 0).is_ok());
        assert!(key_of("test", "watch-0011223344556677", 0).is_ok());
        assert!(key_of("mainnet", &fp, 0).is_err());
        assert!(key_of("test", "abcd", 0).is_err());
    }
}
