//! The Zcash wallet: sync, balances, history, addresses, send and shield,
//! over `zecz` (host/zecz-core), the engine behind the `zecz` CLI and the
//! desktop app (desktop/).
//!
//! Most of what the commands do is the desktop app's own code, compiled in
//! here from desktop/src-tauri/src rather than copied (`backend`,
//! `hardware`, `contacts`, `settings`; see lib.rs): one implementation of
//! importing, syncing, proposing and finishing, with the desktop's tests
//! behind it.
//!
//! What differs is where the board is. The desktop app pairs with the
//! board in Rust; the Companion already has a paired, encrypted session in
//! the webview (main.ts's Client), and a second one here would be a second
//! pairing for one set of methods. So the board never appears in this
//! file. A send is three calls with the webview in the middle: propose
//! here (the signer view goes back), zSign on the board there, and finish
//! here with the `.zsig` it returns. The view the board signed is the one
//! `finish` checks the `.zsig` against, by hash.
//!
//! Wallets are keyed by network, seed fingerprint and account: a BIP39
//! passphrase on the board gives another seed, so another ZIP 32 seed
//! fingerprint, and that is another wallet with its own directory and
//! address book (`wallets/<net>-<zfp16>-<account>`,
//! `contacts-<net>-<zfp16>.json` under `<app data>/zcash/`). A passphrase
//! wallet therefore never mixes with the plain one, and every command names
//! the network, the fingerprint and the account it acts on (the webview
//! learns them from the board's `zGetAccount`). The server setting is per
//! network and shared. Nothing here holds a spending key: the wallets are
//! the board's viewing keys.
//!
//! The first Companion builds kept one wallet per network
//! (`wallets/<net>`, `contacts-<net>.json`); such a wallet is moved to its
//! keyed place the first time its own board account asks for it
//! ([`ZState::profile`]).

use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;

use anyhow::{bail, Context, Result};
use serde::Serialize;
use tauri::{AppHandle, Emitter, Manager, State};
use zecz::wallet::{self, AccountSpec, InitOutcome};
use zecz::{Network, Profile, Wallet};

use crate::backend::{self, block_on, sapling_activation};
use crate::{zcontacts, zhardware, zsettings};

/// The event `zw_sync` emits per step: `{ network, progress }`.
pub const SYNC_EVENT: &str = "zw://sync";
/// The event `zw_finish` emits per step: `{ network, status }`.
pub const FINISH_EVENT: &str = "zw://finish";

const BUSY: &str = "this network's wallet is busy (a sync, an import or a send is running)";

/// The account name a board import gets in the wallet database (the
/// desktop app's, so a wallet directory moved between them reads the same).
const DEVICE_NAME: &str = zhardware::DEVICE_NAME;

pub struct ZState {
    /// `<app data>/zcash`: settings.json, wallets/<net>-<zfp16>-<account>,
    /// contacts-<net>-<zfp16>.json.
    root: PathBuf,
    /// One flag per network: a sync on testnet does not hold up mainnet.
    busy: [Arc<AtomicBool>; 2],
}

/// Which wallet a command is about: the board's seed fingerprint (ZIP 32,
/// 32 bytes) and account, on a network.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct WalletKey {
    net: Network,
    fingerprint: [u8; 32],
    account: u32,
}

impl WalletKey {
    pub fn parse(net: Network, wallet: &str, account: u32) -> Result<Self> {
        let fingerprint: [u8; 32] = hex::decode(wallet.trim())
            .ok()
            .and_then(|v| v.try_into().ok())
            .context("the wallet's seed fingerprint is not 32 bytes of hex; read the board's account first")?;
        if account >= 0x8000_0000 {
            bail!("account {account} is out of range");
        }
        Ok(WalletKey { net, fingerprint, account })
    }

    /// The first 8 bytes of the fingerprint, in hex: enough to keep wallets
    /// apart on one device, and the wallet itself records the whole origin
    /// (an import of another seed into a directory is refused).
    fn short(&self) -> String {
        hex::encode(&self.fingerprint[..8])
    }

    fn net_name(&self) -> &'static str {
        zecz::wallet::network_name(self.net)
    }

    /// `wallets/<test|main>-<zfp16>-<account>`.
    pub fn wallet_dir(&self, root: &std::path::Path) -> PathBuf {
        root.join("wallets").join(format!("{}-{}-{}", self.net_name(), self.short(), self.account))
    }

    /// `contacts-<test|main>-<zfp16>.json`: one book per seed, shared by its
    /// accounts, never by another seed.
    pub fn contacts_path(&self, root: &std::path::Path) -> PathBuf {
        root.join(format!("contacts-{}-{}.json", self.net_name(), self.short()))
    }
}

impl ZState {
    pub fn new(app: &AppHandle) -> Result<Self> {
        let root = app.path().app_data_dir().context("no app data directory")?.join("zcash");
        Ok(ZState { root, busy: [Arc::default(), Arc::default()] })
    }

    fn server(&self, net: Network) -> String {
        let saved = zsettings::load(&zsettings::path_in(&self.root)).unwrap_or_default();
        saved.server_for(net)
    }

    /// The profile of `key`'s wallet, adopting a wallet from the first
    /// Companion builds (one per network) when it is this key's: same seed
    /// fingerprint and account. Another seed's legacy wallet stays where it
    /// is, untouched, until its own board asks for it.
    fn profile(&self, key: &WalletKey) -> Profile {
        let server = self.server(key.net);
        let dir = key.wallet_dir(&self.root);
        if !dir.exists() {
            adopt_legacy(&self.root, key, &server);
        }
        Profile::new(key.net, dir, Some(server))
    }

    fn flag(&self, net: Network) -> &Arc<AtomicBool> {
        &self.busy[usize::from(net == Network::MainNetwork)]
    }
}

/// Moves `wallets/<net>` (and `contacts-<net>.json`) to `key`'s places when
/// the wallet there is `key`'s account. Best effort: a failure leaves the
/// legacy wallet where it was, and the user can import again.
fn adopt_legacy(root: &std::path::Path, key: &WalletKey, server: &str) -> bool {
    let legacy = zsettings::wallet_dir(root, key.net);
    if !legacy.is_dir() {
        return false;
    }
    let lp = Profile::new(key.net, legacy.clone(), Some(server.to_string()));
    let ours = matches!(zhardware::wallet_origin(&lp), Ok(Some(o))
        if o.seed_fingerprint == key.fingerprint && o.account_index == key.account);
    if !ours {
        return false;
    }
    let dir = key.wallet_dir(root);
    if let Some(parent) = dir.parent() {
        let _ = std::fs::create_dir_all(parent);
    }
    if std::fs::rename(&legacy, &dir).is_err() {
        return false;
    }
    let old_book = zcontacts::path_in(root, key.net);
    let book = key.contacts_path(root);
    if old_book.exists() && !book.exists() {
        let _ = std::fs::rename(old_book, book);
    }
    true
}

fn parse_net(network: &str) -> Result<Network, String> {
    match network {
        "test" => Ok(Network::TestNetwork),
        "main" => Ok(Network::MainNetwork),
        other => Err(format!("unknown network {other:?} (test or main)")),
    }
}

/// Runs `f` on a fresh thread and awaits it without blocking the runtime.
/// SQLite, lightwalletd and Halo 2 proving all block, some for minutes.
async fn off_thread<T, F>(f: F) -> Result<T, String>
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
        Ok(r) => r.map_err(|e| zhardware::explain(&e)),
        Err(_) => Err("the wallet worker stopped unexpectedly".into()),
    }
}

/// Clears a busy flag when the command ends, however it ends.
struct Busy(Arc<AtomicBool>);

impl Busy {
    fn take(flag: &Arc<AtomicBool>) -> Result<Self, String> {
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
    let origin = zhardware::wallet_origin(p)?;
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

/// The network, seed fingerprint and account every wallet command names.
fn key_of(network: &str, wallet: &str, account: u32) -> Result<WalletKey, String> {
    WalletKey::parse(parse_net(network)?, wallet, account).map_err(|e| format!("{e:#}"))
}

#[tauri::command]
pub async fn zw_status(state: State<'_, ZState>, network: String, wallet: String, account: u32) -> Result<StatusDto, String> {
    let key = key_of(&network, &wallet, account)?;
    let root = state.root.clone();
    let server = state.server(key.net);
    off_thread(move || {
        let dir = key.wallet_dir(&root);
        if !dir.exists() {
            adopt_legacy(&root, &key, &server);
        }
        status(&Profile::new(key.net, dir, Some(server)))
    })
    .await
}

// ------------------------------------------------------------------ import

/// Imports the board's account (as `zGetAccount` gave it to the webview)
/// as this network's wallet, born at `birthday`.
///
/// The board derives its address itself; its viewing key must lead to the
/// same one, or the key is not what the board showed. A wallet that
/// already tracks the account as watch-only is refused: it could never
/// put the ZIP 32 origin on a spend, so the board could never sign one.
pub fn import(p: &Profile, ufvk: &str, seed_fingerprint: &str, account: u32, ua: &str, birthday: u32) -> Result<backend::Imported> {
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
    let spec = AccountSpec::device(ufvk.trim().to_string(), fp, account, DEVICE_NAME);
    let (w, outcome) = block_on(Wallet::init(p.clone(), &spec, birthday, |_| {}))??;
    if w.key_origin()? != spec.origin {
        bail!(
            "This network's wallet already holds another account, or this one as watch-only. Forget the wallet in Settings and import again."
        );
    }
    Ok(match outcome {
        InitOutcome::AlreadyImported { addresses } => {
            backend::Imported { already_imported: true, birthday: w.birthday()?, addresses: addresses.into() }
        }
        InitOutcome::Imported(a) => backend::Imported { already_imported: false, birthday: a.birthday, addresses: a.addresses.into() },
    })
}

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
        import(&p, &ufvk, &seed_fingerprint, account, &ua, birthday)
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
struct Tagged<T: Serialize> {
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
    pub proposal: zhardware::ProposalDto,
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
        let (proposal, view) = zhardware::propose_send(&p, &to, &amount, memo.as_deref())?;
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
        let (proposal, view) = zhardware::propose_shield(&p)?;
        Ok(Proposed { proposal, view })
    })
    .await
}

/// Applies the board's `.zsig` to the kept full PCZT (each signature is
/// checked against its key), proves, stores and broadcasts. A mainnet
/// transaction needs `mainnet_confirmed`, the confirmation the user ticked
/// under the review.
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
) -> Result<zhardware::SentDto, String> {
    let key = key_of(&network, &wallet, account)?;
    let guard = Busy::take(state.flag(key.net))?;
    let p = state.profile(&key);
    off_thread(move || {
        let _guard = guard;
        zhardware::check_mainnet_confirmed(&p, mainnet_confirmed)?;
        zhardware::finish(&p, &view, &zsig, true, &|s| {
            let _ = app.emit(FINISH_EVENT, Tagged { network: network.clone(), inner: s });
        })
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
            let s = zsettings::check_server(s)?;
            let t = backend::tip(&s)?;
            wallet::check_chain(net, &t.chain_name)?;
        }
        let saved = zsettings::save_server(&zsettings::path_in(&root), net, server.as_deref())?;
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
pub async fn zw_contacts(state: State<'_, ZState>, network: String, wallet: String, account: u32) -> Result<Vec<zcontacts::Contact>, String> {
    let key = key_of(&network, &wallet, account)?;
    let path = key.contacts_path(&state.root);
    off_thread(move || zcontacts::list(&path)).await
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
) -> Result<Vec<zcontacts::Contact>, String> {
    let key = key_of(&network, &wallet, account)?;
    let net = key.net;
    let path = key.contacts_path(&state.root);
    off_thread(move || zcontacts::save(&path, net, &name, &address, replacing.as_deref())).await
}

#[tauri::command]
pub async fn zw_contact_remove(state: State<'_, ZState>, network: String, wallet: String, account: u32, name: String) -> Result<Vec<zcontacts::Contact>, String> {
    let path = key_of(&network, &wallet, account)?.contacts_path(&state.root);
    off_thread(move || zcontacts::remove(&path, &name)).await
}

#[cfg(test)]
mod tests {
    use super::*;

    fn key(name: &str) -> serde_json::Value {
        let v: Vec<serde_json::Value> =
            serde_json::from_slice(&std::fs::read(concat!(env!("CARGO_MANIFEST_DIR"), "/../../../vectors/keys.json")).unwrap()).unwrap();
        v.into_iter().find(|k| k["name"] == name).unwrap()
    }

    fn temp_profile(tag: &str, net: Network) -> Profile {
        let dir = std::env::temp_dir().join(format!("leek-zw-{tag}-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        Profile::new(net, dir, Some("https://127.0.0.1:9".into()))
    }

    #[test]
    fn an_import_whose_address_is_not_the_keys_is_refused_before_the_network() {
        let k = key("test/abandon-art/pp0/acct0");
        let other = key("test/abandon-art/pp0/acct1");
        let p = temp_profile("mismatch", Network::TestNetwork);
        let e = import(&p, k["ufvk"].as_str().unwrap(), k["seed_fingerprint"].as_str().unwrap(), 0, other["ua"].as_str().unwrap(), 3_000_000)
            .unwrap_err();
        assert!(format!("{e:#}").contains("does not match"), "{e:#}");
        assert!(!p.exists(), "nothing was written");
    }

    #[test]
    fn a_key_of_the_other_network_is_refused() {
        let k = key("main/abandon-art/pp0/acct0");
        let p = temp_profile("othernet", Network::TestNetwork);
        assert!(import(&p, k["ufvk"].as_str().unwrap(), k["seed_fingerprint"].as_str().unwrap(), 0, k["ua"].as_str().unwrap(), 3_000_000).is_err());
    }

    #[test]
    fn a_birthday_before_sapling_and_a_short_fingerprint_are_refused() {
        let k = key("test/abandon-art/pp0/acct0");
        let p = temp_profile("bday", Network::TestNetwork);
        let (u, fp, ua) = (k["ufvk"].as_str().unwrap(), k["seed_fingerprint"].as_str().unwrap(), k["ua"].as_str().unwrap());
        assert!(format!("{:#}", import(&p, u, fp, 0, ua, 1000).unwrap_err()).contains("Sapling"));
        assert!(format!("{:#}", import(&p, u, "abcd", 0, ua, 3_000_000).unwrap_err()).contains("32 bytes"));
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
        let mut p = temp_profile("online", Network::TestNetwork);
        p.server = wallet::default_server(Network::TestNetwork).to_string();
        let tip = backend::tip(&p.server).unwrap();
        wallet::check_chain(p.network, &tip.chain_name).unwrap();
        let birthday = tip.height as u32 - 50;
        let (u, fp, ua) = (k["ufvk"].as_str().unwrap(), k["seed_fingerprint"].as_str().unwrap(), k["ua"].as_str().unwrap());
        let r = import(&p, u, fp, 0, ua, birthday).unwrap();
        assert!(!r.already_imported);
        assert_eq!(r.addresses.unified, ua);
        assert!(import(&p, u, fp, 0, ua, birthday).unwrap().already_imported);
        let s = status(&p).unwrap();
        assert_eq!(s.seed_fingerprint.as_deref(), Some(fp));
        assert_eq!(s.account_index, Some(0));
        let done = backend::sync(&p, |_| {}).unwrap();
        assert!(done.fully_scanned >= birthday);
        assert!(backend::balances(&p).unwrap().is_some());
        backend::history(&p).unwrap();
        let other = key("test/abandon-art/pp0/acct1");
        assert!(import(&p, other["ufvk"].as_str().unwrap(), fp, 1, other["ua"].as_str().unwrap(), birthday).is_err());
        let _ = std::fs::remove_dir_all(&p.data_dir);
    }

    #[test]
    fn a_passphrase_wallet_has_its_own_directory_and_address_book() {
        let root = std::path::Path::new("/z");
        let plain = key("test/abandon-art/pp0/acct0");
        let fp = plain["seed_fingerprint"].as_str().unwrap();
        let k0 = WalletKey::parse(Network::TestNetwork, fp, 0).unwrap();
        // Another seed fingerprint: what a BIP39 passphrase gives.
        let other_fp = format!("{}{}", if fp.starts_with('0') { "1" } else { "0" }, &fp[1..]);
        let k1 = WalletKey::parse(Network::TestNetwork, &other_fp, 0).unwrap();
        assert_ne!(k0.wallet_dir(root), k1.wallet_dir(root));
        assert_ne!(k0.contacts_path(root), k1.contacts_path(root));
        let dir = k0.wallet_dir(root);
        let name = dir.file_name().unwrap().to_str().unwrap();
        assert_eq!(name, format!("test-{}-0", &fp[..16]));
        // Another account of one seed: another wallet, the same address book.
        let a1 = WalletKey::parse(Network::TestNetwork, fp, 1).unwrap();
        assert_ne!(k0.wallet_dir(root), a1.wallet_dir(root));
        assert_eq!(k0.contacts_path(root), a1.contacts_path(root));
        // The other network: never the same place.
        let m = WalletKey::parse(Network::MainNetwork, fp, 0).unwrap();
        assert_ne!(k0.wallet_dir(root), m.wallet_dir(root));
        assert_ne!(k0.contacts_path(root), m.contacts_path(root));
        // Never the legacy one-per-network places.
        assert_ne!(k0.wallet_dir(root), zsettings::wallet_dir(root, Network::TestNetwork));
        assert_ne!(k0.contacts_path(root), zcontacts::path_in(root, Network::TestNetwork));
    }

    #[test]
    fn a_wallet_key_needs_the_whole_fingerprint() {
        assert!(WalletKey::parse(Network::TestNetwork, "", 0).is_err());
        assert!(WalletKey::parse(Network::TestNetwork, "abcd", 0).is_err());
        assert!(WalletKey::parse(Network::TestNetwork, &"zz".repeat(32), 0).is_err());
        assert!(WalletKey::parse(Network::TestNetwork, &"ab".repeat(32), 0x8000_0000).is_err());
        assert!(WalletKey::parse(Network::TestNetwork, &format!(" {} ", "AB".repeat(32)), 7).is_ok());
    }

    #[test]
    fn a_legacy_directory_that_is_not_this_keys_wallet_stays_put() {
        let root = std::env::temp_dir().join(format!("leek-zw-legacy-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&root);
        let legacy = zsettings::wallet_dir(&root, Network::TestNetwork);
        std::fs::create_dir_all(&legacy).unwrap();
        std::fs::write(legacy.join("marker"), b"x").unwrap();
        let k = WalletKey::parse(Network::TestNetwork, &"ab".repeat(32), 0).unwrap();
        assert!(!adopt_legacy(&root, &k, "https://127.0.0.1:9"));
        assert!(legacy.join("marker").exists(), "the legacy directory was moved");
        assert!(!k.wallet_dir(&root).exists());
        let _ = std::fs::remove_dir_all(&root);
    }

    #[test]
    fn networks_are_named_not_guessed() {
        assert!(parse_net("test").is_ok() && parse_net("main").is_ok());
        assert!(parse_net("mainnet").is_err() && parse_net("").is_err());
    }
}
