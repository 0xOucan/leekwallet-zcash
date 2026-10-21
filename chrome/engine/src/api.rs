//! The engine's message API: `{ method, params }` in, a JSON result or an
//! error out, and progress events on the side. The extension's worker
//! (`chrome/extension/src/engine-worker.js`) forwards these as postMessage
//! JSON; chrome/README.md lists them with their parameters.
//!
//! The engine holds at most one open wallet (one network) at a time. Calls
//! run one at a time: a call that arrives while another holds the wallet
//! (a sync, say) fails with "engine busy", so the worker queues them, and
//! only `cancelSync` (a free function, see `web.rs`) gets through.

use std::cell::RefCell;
use std::rc::Rc;

use anyhow::{anyhow, bail, Context, Result};
use serde::Deserialize;
use serde_json::{json, Value};
use zcash_protocol::consensus::{BlockHeight, BranchId, Network};

use crate::store;
use crate::wallet::{self, AccountSpec, OnEvent, SendRequest, Wallet};

pub type State = Rc<RefCell<Option<Wallet>>>;

pub const API_VERSION: u32 = 1;

fn p<T: for<'de> Deserialize<'de>>(params: &Value) -> Result<T> {
    serde_json::from_value(params.clone()).map_err(|e| anyhow!("bad parameters: {e}"))
}

fn opt_str<'a>(params: &'a Value, key: &str) -> Option<&'a str> {
    params.get(key).and_then(Value::as_str).filter(|s| !s.is_empty())
}

fn req_str<'a>(params: &'a Value, key: &str) -> Result<&'a str> {
    opt_str(params, key).ok_or_else(|| anyhow!("missing parameter {key:?}"))
}

fn hex_param(params: &Value, key: &str) -> Result<Vec<u8>> {
    hex::decode(req_str(params, key)?.trim()).with_context(|| format!("{key} is not hex"))
}

fn status(w: &Wallet) -> Result<Value> {
    let (wallet_bytes, leek_bytes) = w.db_bytes()?;
    Ok(json!({
        "open": true,
        "network": w.net_name(),
        "server": w.server,
        "accounts": w.accounts()?,
        "sync": w.sync_status()?,
        "dbBytes": wallet_bytes,
        "leekDbBytes": leek_bytes,
    }))
}

/// Runs one call. `state` is borrowed for the whole call, across awaits.
pub async fn dispatch(state: &State, method: &str, params: Value, on_event: OnEvent<'_>) -> Result<Value> {
    // Calls that need no open wallet.
    match method {
        "engineInfo" => {
            return Ok(json!({
                "apiVersion": API_VERSION,
                "engineVersion": env!("CARGO_PKG_VERSION"),
                "librustzcash": "16f0723c385957cecc32b4f0c71bb411053b708b",
                "threads": cfg!(feature = "threads"),
                "opfsFiles": store::files()?,
            }))
        }
        "defaultServer" => {
            let net = wallet::parse_network(req_str(&params, "network")?)?;
            return Ok(json!(wallet::default_server(net)));
        }
        "tip" => {
            let server = match opt_str(&params, "server") {
                Some(s) => s.to_string(),
                None => match state.try_borrow().ok().and_then(|w| w.as_ref().map(|w| w.server.clone())) {
                    Some(s) => s,
                    None => wallet::default_server(wallet::parse_network(opt_str(&params, "network").unwrap_or("test"))?)
                        .to_string(),
                },
            };
            return Ok(serde_json::to_value(wallet::tip(&server).await?)?);
        }
        "checkAddress" => {
            let net = match opt_str(&params, "network") {
                Some(n) => wallet::parse_network(n)?,
                None => state.try_borrow().ok().and_then(|w| w.as_ref().map(|w| w.network)).unwrap_or(Network::TestNetwork),
            };
            return Ok(json!({ "kind": wallet::check_address(net, req_str(&params, "address")?)? }));
        }
        "devAccountSpec" => {
            // DEV ONLY (testnet): what the board would export for a mnemonic.
            let net = wallet::parse_network(opt_str(&params, "network").unwrap_or("test"))?;
            if net != Network::TestNetwork {
                bail!("devAccountSpec is testnet only");
            }
            let index = params.get("accountIndex").and_then(Value::as_u64).unwrap_or(0) as u32;
            return wallet::dev_account_spec(net, req_str(&params, "mnemonic")?, index);
        }
        _ => {}
    }

    let mut slot = state.try_borrow_mut().map_err(|_| anyhow!("engine busy"))?;

    match method {
        "open" => {
            let net = wallet::parse_network(opt_str(&params, "network").unwrap_or("test"))?;
            // Close first: one connection per database (OPFS sync handles).
            *slot = None;
            let existed = store::wallet_exists(wallet::network_name(net))?;
            let w = Wallet::open(net, opt_str(&params, "server").map(str::to_string))?;
            let mut s = status(&w)?;
            s["existed"] = json!(existed);
            *slot = Some(w);
            return Ok(s);
        }
        "close" => {
            *slot = None;
            return Ok(json!({ "open": false }));
        }
        "wipe" => {
            // Deletes a network's wallet from OPFS. The caller confirms.
            let net = wallet::parse_network(req_str(&params, "network")?)?;
            if slot.as_ref().is_some_and(|w| w.network == net) {
                *slot = None;
            }
            let removed = store::remove_network(wallet::network_name(net))?;
            return Ok(json!({ "removed": removed }));
        }
        "status" => {
            return match slot.as_ref() {
                Some(w) => status(w),
                None => Ok(json!({ "open": false })),
            };
        }
        _ => {}
    }

    let w = slot.as_mut().ok_or_else(|| anyhow!("no wallet is open; call open first"))?;
    let account = opt_str(&params, "account").map(str::to_string);
    let account = account.as_deref();
    Ok(match method {
        "setServer" => {
            w.server = req_str(&params, "server")?.trim().to_string();
            json!({ "server": w.server })
        }
        "accounts" => json!(w.accounts()?),
        "importAccount" => {
            let spec: AccountSpec = p(&params)?;
            let birthday = params.get("birthday").and_then(Value::as_u64).map(|b| b as u32);
            serde_json::to_value(w.import(&spec, birthday, on_event).await?)?
        }
        "deleteAccount" => {
            // "Forget this board": the account must be named, never defaulted.
            let removed = w.delete_account(req_str(&params, "account")?)?;
            json!({ "removed": removed })
        }
        "addresses" => serde_json::to_value(w.addresses(account)?)?,
        "nextAddress" => serde_json::to_value(w.next_address(account)?)?,
        "sync" => {
            let batch = params.get("batch").and_then(Value::as_u64).unwrap_or(1000) as u32;
            serde_json::to_value(w.sync(batch.max(1), on_event).await?)?
        }
        "syncStatus" => json!(w.sync_status()?),
        "balances" => serde_json::to_value(w.balances(account)?)?,
        "proposeSend" => {
            let req: SendRequest = p(&params)?;
            serde_json::to_value(w.propose_send(account, &req)?)?
        }
        "proposeShield" => {
            let compact = params.get("compact").and_then(Value::as_bool).unwrap_or(true);
            serde_json::to_value(w.propose_shield(account, compact)?)?
        }
        "pending" => json!(w.pending()?),
        "applyZsig" => {
            let (sha256, n) = w.apply_zsig(&hex_param(&params, "signerView")?, &hex_param(&params, "zsig")?)?;
            json!({ "sha256": sha256, "signatures": n })
        }
        "prove" => json!({ "proveMs": w.prove(req_str(&params, "sha256")?, on_event)? }),
        "extract" => serde_json::to_value(w.extract(req_str(&params, "sha256")?)?)?,
        "broadcast" => {
            let txid = req_str(&params, "txid")?;
            w.broadcast(txid).await?;
            json!({ "txid": txid, "broadcast": true })
        }
        "finish" => {
            // zecz `finish`: apply, prove, extract, and broadcast if asked.
            let (sha256, n) = w.apply_zsig(&hex_param(&params, "signerView")?, &hex_param(&params, "zsig")?)?;
            on_event(json!({ "event": "finish", "phase": "signaturesApplied", "count": n }));
            let prove_ms = w.prove(&sha256, on_event)?;
            let x = w.extract(&sha256)?;
            on_event(json!({ "event": "finish", "phase": "stored", "txid": x.txid, "size": x.size }));
            let broadcast = params.get("broadcast").and_then(Value::as_bool).unwrap_or(false);
            if broadcast {
                w.broadcast(&x.txid).await?;
            }
            json!({ "sha256": sha256, "signatures": n, "proveMs": prove_ms, "txid": x.txid, "size": x.size, "raw": x.raw, "broadcast": broadcast })
        }
        "history" => {
            let limit = params.get("limit").and_then(Value::as_u64).unwrap_or(50) as u32;
            serde_json::to_value(w.history(account, limit)?)?
        }
        "warmProver" => {
            // Builds the proving key of the Orchard-protocol circuit current
            // at the chain tip, ahead of a send (seconds; blocks the engine).
            let height = w.sync_status()?.map(|s| s.chain_tip + 1).unwrap_or(0);
            let branch = BranchId::for_height(&w.network, BlockHeight::from_u32(height));
            let t0 = wallet::now_ms();
            let mut built = Vec::new();
            for pool in [orchard::ValuePool::Ironwood, orchard::ValuePool::Orchard] {
                if let Some(v) = zcash_primitives::transaction::components::orchard::bundle_version_for_branch(branch, pool) {
                    zcash_primitives::transaction::builder::cached_orchard_proving_key(v.circuit_version());
                    built.push(format!("{pool:?}/{:?}", v.circuit_version()));
                    break;
                }
            }
            json!({ "built": built, "ms": wallet::now_ms() - t0 })
        }
        "devForgetTransparentSpends" => {
            // DEV ONLY, testnet: for the e2e check of the transparent spend search.
            json!({ "forgotten": w.dev_forget_transparent_spends()? })
        }
        "signSoft" => {
            // DEV ONLY, testnet: zecz's soft signer standing in for the board.
            let index = params.get("accountIndex").and_then(Value::as_u64).unwrap_or(0) as u32;
            let z = w.sign_soft(&hex_param(&params, "signerView")?, req_str(&params, "mnemonic")?, index)?;
            json!({ "zsig": hex::encode(z) })
        }
        other => bail!("unknown method {other:?}"),
    })
}
