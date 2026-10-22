//! NEAR Intents swaps (roadmap 8.6), on every platform the app runs on:
//! the commands over app/near-intents (`leek_near_intents`), with the
//! wallet's part (its fresh Orchard-only address, its balance, the address
//! checks) from the wallet on screen.
//!
//! A swap from ZEC is paid by an ordinary send signed on the board:
//! `swap_execute` proposes the quoted amount to the deposit address and
//! hands the signer view to the webview like `zw_propose_send` does, the
//! board signs it, and `zw_finish` records the deposit txid with the swap
//! ([`deposit_sent`]). A swap into ZEC is paid from another wallet, to the
//! deposit address shown; this app only follows it.
//!
//! Swaps are mainnet only (1Click has no testnet): on testnet the screen
//! previews a price with a dry quote and nothing else. Every swap with a
//! deposit address is kept in its wallet's swaps file
//! (`leek_zcash_wallet::keys::WalletKey::swaps_path`).

use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use leek_near_intents as near;
use leek_zcash_wallet::{backend, settings};
use serde::Serialize;
use tauri::State;
use zecz::Network;

use crate::zwallet::{key_of, off_thread, parse_net, Busy, Proposed, ZState};

/// Held while a swaps file is read and rewritten (a status poll and a
/// deposit being recorded may meet).
static FILES: Mutex<()> = Mutex::new(());

/// How long the token list (with its USD prices) is reused.
const TOKENS_FRESH: Duration = Duration::from_secs(300);

type TokenCache = Arc<Mutex<Option<(Instant, Vec<near::Token>)>>>;

/// 1Click's token list and when it was fetched.
#[derive(Default)]
pub struct SwapState {
    tokens: TokenCache,
}

fn lock() -> std::sync::MutexGuard<'static, ()> {
    FILES.lock().unwrap_or_else(|p| p.into_inner())
}

/// The 1Click client, with the API key if one is set (environment first,
/// then settings.json).
fn one_click(z: &ZState) -> near::OneClick<near::Http> {
    let saved = settings::load(&settings::path_in(&z.root)).unwrap_or_default();
    near::OneClick::new(near::Http::default(), near::api_key(saved.one_click_api_key.as_deref()))
}

fn tokens_cached(cache: &TokenCache, client: &near::OneClick<near::Http>) -> anyhow::Result<Vec<near::Token>> {
    if let Some((at, t)) = &*cache.lock().unwrap_or_else(|p| p.into_inner()) {
        if at.elapsed() < TOKENS_FRESH {
            return Ok(t.clone());
        }
    }
    let t = client.tokens().map_err(|e| anyhow::anyhow!(near::explain(&e, None)))?;
    *cache.lock().unwrap_or_else(|p| p.into_inner()) = Some((Instant::now(), t.clone()));
    Ok(t)
}

/// zecz's address check on `net`, as the swap crate takes it.
fn checker(net: Network) -> impl Fn(&str) -> anyhow::Result<String> {
    move |a| zecz::wallet::check_address(net, a).map(str::to_string)
}

/// Records the txid of a deposit signed for proposal `proposal_id` with
/// its swap, if the send was one. Called by `zw_finish` after every send:
/// a failure here must not turn a broadcast send into an error, so it is
/// only said.
pub fn deposit_sent(path: &std::path::Path, proposal_id: &str, txid: &str) {
    let _lock = lock();
    if let Err(e) = near::record_deposit(path, proposal_id, txid) {
        eprintln!("zcash: deposit {txid} not recorded with its swap: {e:#}");
    }
}

/// What the swap screen needs to know before anything else.
#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct SwapInfo {
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
pub async fn swap_info(z: State<'_, ZState>, network: String) -> Result<SwapInfo, String> {
    let net = parse_net(&network)?;
    Ok(SwapInfo {
        network: zecz::wallet::network_name(net).to_string(),
        authenticated: one_click(&z).has_api_key(),
        api_key_env: near::API_KEY_ENV,
        explorer: near::EXPLORER,
        default_slippage_bps: near::DEFAULT_SLIPPAGE_BPS,
        max_slippage_bps: near::MAX_SLIPPAGE_BPS,
    })
}

/// The assets 1Click swaps (cached for five minutes).
#[tauri::command]
pub async fn swap_tokens(z: State<'_, ZState>, s: State<'_, SwapState>) -> Result<Vec<near::Token>, String> {
    let (cache, client) = (s.tokens.clone(), one_click(&z));
    off_thread(move || tokens_cached(&cache, &client)).await
}

/// A quote. A dry one only prices the swap (testnet: only this), and needs
/// no wallet. A real one, on mainnet, needs the wallet on screen: it gets a
/// deposit address and a fresh Orchard-only address of this wallet for the
/// refund (from ZEC) or the payout (into ZEC), and is stored with its swaps.
#[tauri::command]
pub async fn swap_quote(
    z: State<'_, ZState>,
    s: State<'_, SwapState>,
    network: String,
    wallet: Option<String>,
    account: Option<u32>,
    params: near::QuoteParams,
) -> Result<near::SwapRecord, String> {
    let net = parse_net(&network)?;
    let key = match wallet.as_deref().filter(|w| !w.is_empty()) {
        Some(w) => Some(key_of(&network, w, account.unwrap_or(0))?),
        None if params.dry => None,
        None => return Err("Open a wallet first: a swap pays to and from this wallet.".into()),
    };
    // A real quote records a new address in the wallet.
    let guard = if params.dry { None } else { Some(Busy::take(z.flag(net))?) };
    let profile = key.as_ref().map(|k| z.profile(k));
    let path = key.as_ref().map(|k| k.swaps_path(&z.root));
    let (cache, client) = (s.tokens.clone(), one_click(&z));
    off_thread(move || {
        let _guard = guard;
        if net == Network::MainNetwork && !params.dry {
            let p = profile.as_ref().expect("a real quote has a wallet");
            let st = backend::profile_status(p)?;
            if !st.has_account {
                anyhow::bail!("Import an account first: a swap pays to and from this wallet.");
            }
            if params.direction == near::Direction::FromZec && !st.hardware {
                anyhow::bail!(
                    "This is a watch-only wallet; nothing could sign the deposit. Open the board's wallet to swap from ZEC."
                );
            }
        }
        let tokens = tokens_cached(&cache, &client)?;
        let fresh = !params.dry;
        let own = || -> anyhow::Result<String> {
            match profile.as_ref().map(|p| backend::swap_address(p, fresh)) {
                Some(Ok(a)) => Ok(a),
                // A price preview needs no wallet.
                Some(Err(_)) | None if !fresh => Ok(near::PREVIEW_ADDRESS.to_string()),
                Some(Err(e)) => Err(e),
                None => anyhow::bail!("open a wallet first"),
            }
        };
        let spendable = || -> anyhow::Result<Option<u64>> {
            match profile.as_ref() {
                Some(p) => Ok(backend::balances(p)?.map(|b| b.spendable)),
                None => Ok(None),
            }
        };
        let check = checker(net);
        let ctx = near::QuoteContext {
            mainnet: net == Network::MainNetwork,
            own_address: &own,
            spendable: &spendable,
            check_address: &check,
            now: time::OffsetDateTime::now_utc(),
        };
        let rec = near::request_quote(&client, &tokens, &ctx, &params)?;
        if !rec.dry {
            let _lock = lock();
            near::save(path.as_ref().expect("a real quote has a wallet"), &rec)?;
        }
        Ok(rec)
    })
    .await
}

/// Builds the deposit of a swap from ZEC: the quoted amount to its deposit
/// address, a send like any other (compact signer view), returned for the
/// review; the webview has the board sign it and `zw_finish` records the
/// txid with the swap. Refused off mainnet, for an expired quote, or
/// without the funds.
#[tauri::command]
pub async fn swap_execute(z: State<'_, ZState>, network: String, wallet: String, account: u32, id: String) -> Result<Proposed, String> {
    let key = key_of(&network, &wallet, account)?;
    let guard = Busy::take(z.flag(key.net))?;
    let profile = z.profile(&key);
    let path = key.swaps_path(&z.root);
    let net = key.net;
    off_thread(move || {
        let _guard = guard;
        let _lock = lock();
        let mut rec = near::get(&path, &id)?;
        let zat = near::check_executable(&rec, net == Network::MainNetwork, &checker(net), time::OffsetDateTime::now_utc())?;
        near::check_funds(backend::balances(&profile)?.map(|b| b.spendable), zat)?;
        let address = rec.deposit_address.clone().unwrap_or_default();
        let (proposal, view) = leek_zcash_wallet::hardware::propose_send(&profile, &address, &near::zat_to_zec(zat), None)?;
        // What the board will be asked to sign is exactly the deposit.
        let pays = proposal.payments.len() == 1 && proposal.payments[0].recipient == address && proposal.payments[0].amount_zat == zat;
        if !pays {
            anyhow::bail!("the proposal does not pay exactly the quoted deposit; not signing it");
        }
        rec.proposal_id = Some(proposal.id.clone());
        near::save(&path, &rec)?;
        Ok(Proposed { proposal, view })
    })
    .await
}

/// Asks 1Click where a swap stands (telling it the deposit txid first if
/// it has not heard it) and returns the updated swap.
#[tauri::command]
pub async fn swap_status(z: State<'_, ZState>, network: String, wallet: String, account: u32, id: String) -> Result<near::SwapRecord, String> {
    let path = key_of(&network, &wallet, account)?.swaps_path(&z.root);
    let client = one_click(&z);
    off_thread(move || {
        let mut rec = {
            let _lock = lock();
            near::get(&path, &id)?
        };
        // The network calls run without the lock.
        near::refresh_status(&client, &mut rec, time::OffsetDateTime::now_utc());
        let _lock = lock();
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

/// The swaps of this wallet, newest first.
#[tauri::command]
pub async fn swap_list(z: State<'_, ZState>, network: String, wallet: String, account: u32) -> Result<Vec<near::SwapRecord>, String> {
    let path = key_of(&network, &wallet, account)?.swaps_path(&z.root);
    off_thread(move || {
        let _lock = lock();
        near::list(&path)
    })
    .await
}

/// Removes a swap that can no longer move funds. Returns the rest.
#[tauri::command]
pub async fn swap_forget(z: State<'_, ZState>, network: String, wallet: String, account: u32, id: String) -> Result<Vec<near::SwapRecord>, String> {
    let path = key_of(&network, &wallet, account)?.swaps_path(&z.root);
    off_thread(move || {
        let _lock = lock();
        near::forget(&path, &id)
    })
    .await
}
