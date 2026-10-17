//! The wallet: one network, one OPFS database, any number of accounts.
//!
//! The operations of `host/zecz-core/src/wallet.rs` (init, sync, balances,
//! addresses, propose, finish, history), restated over an OPFS SQLite
//! connection and a gRPC-web client. zecz-core cannot be compiled for
//! wasm32 as it is: it keeps a data directory with `std::fs`, connects with
//! tonic's HTTP/2 transport and runs on tokio. The librustzcash calls, the
//! account shape, the change pool rule, the signer view, the sighash check
//! before applying a `.zsig` and the history queries are the same; where
//! they are copied the zecz-core function is named, so a change there can
//! be carried here.
//!
//! Differences from zecz: several accounts per wallet (the extension will
//! hold several: board accounts now, software wallets in 9.4), so the
//! account is a parameter; and the pending and sent files live in a second
//! database (see [`crate::store`]).

use std::cell::Cell;
use std::convert::Infallible;
use std::time::{Duration, SystemTime, UNIX_EPOCH};

use anyhow::{anyhow, bail, Context, Result};
use pczt::roles::signer::Signer;
use pczt::Pczt;
use rand::{rand_core::UnwrapErr, rngs::SysRng};
use rusqlite::{named_params, Connection, OptionalExtension};
use serde::{Deserialize, Serialize};
use serde_json::{json, Value};
use zcash_client_backend::data_api::wallet::{
    create_pczt_from_proposal, extract_and_store_transaction_from_pczt, input_selection::GreedyInputSelector,
    input_selection::SpendPolicy, propose_transfer, redact_pczt_for_signer, ConfirmationsPolicy, SignerView,
};
use zcash_client_backend::data_api::{
    Account as _, AccountBirthday, AccountPurpose, WalletRead, WalletWrite, Zip32Derivation,
};
use zcash_client_backend::fees::{standard::SingleOutputChangeStrategy, DustOutputPolicy, StandardFeeRule};
use zcash_client_backend::proto::service::{BlockId, ChainSpec, Empty, RawTransaction};
use zcash_client_backend::util::Clock;
use zcash_client_backend::wallet::OvkPolicy;
use zcash_client_sqlite::{wallet::init::init_wallet_db, AccountUuid, ReceivedNoteId, WalletDb};
use zcash_keys::address::Address;
use zcash_keys::keys::{ReceiverRequirement, UnifiedAddressRequest, UnifiedFullViewingKey};
use zcash_primitives::transaction::builder::BundlePadding;
use zcash_protocol::consensus::{BlockHeight, Network, NetworkConstants, NetworkType, NetworkUpgrade, Parameters};
use zcash_protocol::memo::{Memo, MemoBytes};
use zcash_protocol::value::Zatoshis;
use zcash_protocol::{PoolType, ShieldedPool};
use zip321::{Payment, TransactionRequest};
use zip32::fingerprint::SeedFingerprint;

use crate::authorize::{apply_zsig, prove_with, ProveEvent};
use crate::lwd::{self, MemBlockCache};
use crate::store;
use crate::zsig::{self, Zsig};

/// `SystemTime::now()` panics on wasm32-unknown-unknown; use JS time.
#[derive(Clone, Copy)]
pub struct JsClock;

impl Clock for JsClock {
    fn now(&self) -> SystemTime {
        UNIX_EPOCH + Duration::from_millis(js_sys::Date::now() as u64)
    }
}

pub fn now_ms() -> f64 {
    js_sys::Date::now()
}

type Db<C> = WalletDb<C, Network, JsClock, UnwrapErr<SysRng>>;

/// A progress callback: each event is a JSON object with an `event` field.
pub type OnEvent<'a> = &'a dyn Fn(Value);

/// The gRPC-web endpoint used when the user has not set one: ChainSafe's
/// proxies (the public lightwalletd servers speak plain gRPC only).
pub fn default_server(network: Network) -> &'static str {
    match network {
        Network::TestNetwork => "https://zcash-testnet.chainsafe.dev",
        Network::MainNetwork => "https://zcash-mainnet.chainsafe.dev",
    }
}

/// "test" or "main", as zecz's `network_name`.
pub fn network_name(network: Network) -> &'static str {
    match network.network_type() {
        NetworkType::Main => "main",
        _ => "test",
    }
}

pub fn parse_network(s: &str) -> Result<Network> {
    match s {
        "test" | "testnet" => Ok(Network::TestNetwork),
        "main" | "mainnet" => Ok(Network::MainNetwork),
        _ => bail!("unknown network {s:?} (test or main)"),
    }
}

// ------------------------------------------------------------------ types

/// The ZIP 32 origin of a device account's key (zecz `KeyOrigin`).
#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct KeyOrigin {
    /// ZIP 32 seed fingerprint, hex (32 bytes).
    pub seed_fingerprint: String,
    pub account_index: u32,
}

/// The account to import (zecz `AccountSpec`): a UFVK and, for a device
/// account, where its spending key lives. Without an origin the account is
/// view-only: it syncs and shows balances, but no device can sign its PCZTs.
#[derive(Clone, Debug, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct AccountSpec {
    pub ufvk: String,
    #[serde(default)]
    pub origin: Option<KeyOrigin>,
    #[serde(default = "default_name")]
    pub name: String,
}

fn default_name() -> String {
    "Account".into()
}

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Addresses {
    /// Unified address with Orchard and P2PKH receivers.
    pub unified: String,
    pub diversifier_index: String,
    pub transparent: Option<String>,
    /// Unified address with only an Orchard receiver.
    pub orchard_only: String,
}

/// A fresh diversified address (zecz `DiversifiedAddress`).
#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct DiversifiedAddress {
    pub unified: String,
    pub diversifier_index: String,
    /// Its P2PKH receiver on its own; None for an Orchard-only address.
    pub transparent: Option<String>,
}

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct AccountInfo {
    pub uuid: String,
    pub name: Option<String>,
    pub birthday: u32,
    /// "device" (has a ZIP 32 origin) or "viewOnly".
    pub kind: &'static str,
    pub origin: Option<KeyOrigin>,
    /// The device's short seed fingerprint ("ZFP"), for a device account.
    pub short_fingerprint: Option<String>,
    pub ufvk: String,
}

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct ImportOutcome {
    /// False when the same UFVK was already imported (nothing changed).
    pub imported: bool,
    pub account: AccountInfo,
    pub addresses: Addresses,
}

#[derive(Clone, Copy, Debug, Default, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct SyncStatus {
    pub chain_tip: u32,
    pub fully_scanned: u32,
    /// Scan progress as numerator / denominator (zecz `SyncStatus`).
    pub progress_num: u64,
    pub progress_den: u64,
    pub synced: bool,
}

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct SyncOutcome {
    pub cancelled: bool,
    pub tip: u32,
    pub scanned_blocks: u64,
    pub elapsed_ms: f64,
    pub status: Option<SyncStatus>,
}

#[derive(Clone, Copy, Debug, Default, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct PoolBalance {
    pub total: u64,
    pub spendable: u64,
    pub pending: u64,
}

#[derive(Clone, Copy, Debug, Default, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct AccountBalances {
    pub orchard: PoolBalance,
    pub ironwood: PoolBalance,
    pub transparent: PoolBalance,
    pub total: u64,
    pub spendable: u64,
}

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Balances {
    pub account: String,
    pub chain_tip: u32,
    pub fully_scanned: u32,
    pub synced: bool,
    pub balances: Option<AccountBalances>,
}

#[derive(Clone, Debug, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct SendRequest {
    pub to: String,
    pub amount_zat: u64,
    #[serde(default)]
    pub memo: Option<String>,
    /// Hand the signer the compact view (v2 PCZT). Default true: the board
    /// takes it over every link, and it is what fits a QR.
    #[serde(default = "yes")]
    pub compact: bool,
}

fn yes() -> bool {
    true
}

#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct PaymentSummary {
    pub recipient: String,
    pub amount_zat: u64,
    pub pool: String,
    pub has_memo: bool,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct ChangeSummary {
    pub value_zat: u64,
    pub pool: String,
}

/// A built, unsigned send (zecz `SendProposal`). The full PCZT stays in the
/// engine under `sha256`; `signer_view` is what goes to the signer.
#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct SendProposal {
    pub account: String,
    pub target_height: u32,
    pub payments: Vec<PaymentSummary>,
    pub change: Vec<ChangeSummary>,
    pub fee_zat: u64,
    /// Hex of the PCZT to hand the signer.
    pub signer_view: String,
    pub compact: bool,
    /// Hex SHA-256 of the signer view: the handle of this send.
    pub sha256: String,
}

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Extracted {
    pub txid: String,
    pub size: usize,
    pub raw: String,
}

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Recipient {
    pub address: String,
    pub value_zat: u64,
}

/// One transaction in the history (zecz `TxRecord`).
#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct TxRecord {
    pub txid: String,
    pub mined_height: Option<u32>,
    pub block_time: Option<i64>,
    pub expiry_height: Option<u32>,
    pub expired_unmined: bool,
    /// "received", "sent", "shielded" or "internal".
    pub kind: &'static str,
    pub delta_zat: i64,
    pub amount_zat: u64,
    pub fee_zat: Option<u64>,
    pub memos: Vec<String>,
    pub recipients: Vec<Recipient>,
}

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct TipInfo {
    pub server: String,
    pub chain_name: String,
    pub height: u64,
    pub consensus_branch_id: String,
    pub vendor: String,
    pub version: String,
}

// ------------------------------------------------------------- pure logic

/// The device's short seed fingerprint: the first four bytes, upper-case hex.
pub fn short_fingerprint(fp: &[u8; 32]) -> String {
    hex::encode_upper(&fp[..4])
}

fn memo_text(bytes: &[u8]) -> Option<String> {
    match Memo::from_bytes(bytes).ok()? {
        Memo::Text(t) if !t.trim().is_empty() => Some(t.to_string()),
        _ => None,
    }
}

/// Orchard + P2PKH, the receivers the device can verify (zecz `ua_request`).
fn ua_request() -> UnifiedAddressRequest {
    UnifiedAddressRequest::custom(ReceiverRequirement::Require, ReceiverRequirement::Omit, ReceiverRequirement::Require)
        .expect("valid request")
}

/// zecz `parse_ufvk`: the device account's shape, Orchard and no Sapling.
pub fn parse_ufvk(network: Network, ufvk: &str) -> Result<UnifiedFullViewingKey> {
    let ufvk =
        UnifiedFullViewingKey::decode(&network, ufvk.trim()).map_err(|e| anyhow!("bad UFVK for this network: {e}"))?;
    if ufvk.sapling().is_some() {
        bail!("UFVK has a Sapling item; the LeekWallet account is Orchard + P2PKH only");
    }
    if ufvk.orchard().is_none() {
        bail!("UFVK has no Orchard item");
    }
    Ok(ufvk)
}

/// zecz `addresses_of`.
pub fn addresses_of(network: Network, ufvk: &UnifiedFullViewingKey) -> Result<Addresses> {
    let (ua, idx) = ufvk.default_address(ua_request()).map_err(|e| anyhow!("address derivation: {e:?}"))?;
    let (ua_o, _) = ufvk.default_address(UnifiedAddressRequest::ORCHARD).map_err(|e| anyhow!("{e:?}"))?;
    Ok(Addresses {
        unified: ua.encode(&network),
        diversifier_index: u128::from(idx).to_string(),
        transparent: ua.transparent().map(|t| Address::Transparent(*t).encode(&network)),
        orchard_only: ua_o.encode(&network),
    })
}

/// zecz `check_address`: unified or transparent, not Sapling.
pub fn check_address(network: Network, addr: &str) -> Result<&'static str> {
    match Address::decode(&network, addr.trim()) {
        Some(Address::Unified(_)) => Ok("unified"),
        Some(Address::Transparent(_)) => Ok("transparent"),
        Some(Address::Sapling(_)) => bail!("Sapling addresses are not supported"),
        Some(_) => bail!("this kind of address is not supported"),
        None => bail!("{} is not a valid {} address", addr.trim(), network_name(network)),
    }
}

fn change_pool(network: Network, target: BlockHeight) -> ShieldedPool {
    // Ironwood once NU6.3 is active, Orchard before (zecz propose_send).
    if network.is_nu_active(NetworkUpgrade::Nu6_3, target) {
        ShieldedPool::Ironwood
    } else {
        ShieldedPool::Orchard
    }
}

fn hex32(s: &str) -> Result<[u8; 32]> {
    hex::decode(s.trim())
        .ok()
        .and_then(|v| v.try_into().ok())
        .ok_or_else(|| anyhow!("expected 32 bytes of hex"))
}

// ----------------------------------------------------------------- network

pub async fn tip(server: &str) -> Result<TipInfo> {
    let mut client = lwd::connect(server);
    let info = client.get_lightd_info(Empty {}).await.context("GetLightdInfo")?.into_inner();
    let tip = client.get_latest_block(ChainSpec {}).await.context("GetLatestBlock")?.into_inner();
    Ok(TipInfo {
        server: server.to_string(),
        chain_name: info.chain_name,
        height: tip.height,
        consensus_branch_id: info.consensus_branch_id,
        vendor: info.vendor,
        version: info.version,
    })
}

/// zecz `broadcast`, over gRPC-web.
pub async fn broadcast_raw(server: &str, raw: &[u8]) -> Result<()> {
    let mut client = lwd::connect(server);
    let resp = client
        .send_transaction(RawTransaction { data: raw.to_vec(), height: 0 })
        .await
        .context("SendTransaction")?
        .into_inner();
    if resp.error_code != 0 {
        bail!("lightwalletd rejected the transaction: code {} {}", resp.error_code, resp.error_message);
    }
    Ok(())
}

// ------------------------------------------------------------------ wallet

pub struct Wallet {
    pub network: Network,
    pub server: String,
    conn: Connection,
    leek: Connection,
}

impl Wallet {
    /// Opens the network's wallet on OPFS, creating and migrating it if
    /// needed. `install_opfs` must have run.
    pub fn open(network: Network, server: Option<String>) -> Result<Self> {
        let net = network_name(network);
        let mut conn = store::open(&store::wallet_path(net))?;
        rusqlite::vtab::array::load_module(&conn)?;
        {
            let mut db: Db<&mut Connection> = WalletDb::from_connection(&mut conn, network, JsClock, UnwrapErr(SysRng));
            init_wallet_db(&mut db, None).map_err(|e| anyhow!("wallet database migration failed: {e}"))?;
        }
        let leek = store::open_leek(net)?;
        let server = server.filter(|s| !s.trim().is_empty()).unwrap_or_else(|| default_server(network).to_string());
        Ok(Wallet { network, server, conn, leek })
    }

    fn db(&mut self) -> Db<&mut Connection> {
        WalletDb::from_connection(&mut self.conn, self.network, JsClock, UnwrapErr(SysRng))
    }

    fn rdb(&self) -> Db<&Connection> {
        WalletDb::from_connection(&self.conn, self.network, JsClock, UnwrapErr(SysRng))
    }

    pub fn net_name(&self) -> &'static str {
        network_name(self.network)
    }

    pub fn db_bytes(&self) -> Result<(u64, u64)> {
        Ok((store::db_bytes(&self.conn)?, store::db_bytes(&self.leek)?))
    }

    // ----------------------------------------------------------- accounts

    pub fn accounts(&self) -> Result<Vec<AccountInfo>> {
        let db = self.rdb();
        let mut out = Vec::new();
        for id in db.get_account_ids()? {
            out.push(self.account_info(id)?);
        }
        Ok(out)
    }

    fn account_info(&self, id: AccountUuid) -> Result<AccountInfo> {
        let db = self.rdb();
        let acct = db.get_account(id)?.ok_or_else(|| anyhow!("account vanished"))?;
        let ufvk = acct.ufvk().ok_or_else(|| anyhow!("account has no UFVK"))?;
        let origin = acct
            .source()
            .key_derivation()
            .map(|d| (d.seed_fingerprint().to_bytes(), u32::from(d.account_index())));
        Ok(AccountInfo {
            uuid: id.expose_uuid().to_string(),
            name: acct.name().map(str::to_string),
            birthday: u32::from(db.get_account_birthday(id)?),
            kind: if origin.is_some() { "device" } else { "viewOnly" },
            short_fingerprint: origin.map(|(fp, _)| short_fingerprint(&fp)),
            origin: origin.map(|(fp, i)| KeyOrigin { seed_fingerprint: hex::encode(fp), account_index: i }),
            ufvk: ufvk.encode(&self.network)?,
        })
    }

    /// The account `uuid`, or the only/first one when `uuid` is None.
    fn account(&self, uuid: Option<&str>) -> Result<(AccountUuid, UnifiedFullViewingKey)> {
        let db = self.rdb();
        let ids = db.get_account_ids()?;
        let id = match uuid {
            Some(u) => *ids
                .iter()
                .find(|id| id.expose_uuid().to_string() == u)
                .ok_or_else(|| anyhow!("no account {u} in the {} wallet", self.net_name()))?,
            None => *ids.first().ok_or_else(|| anyhow!("the {} wallet has no account yet", self.net_name()))?,
        };
        let acct = db.get_account(id)?.ok_or_else(|| anyhow!("account vanished"))?;
        let ufvk = acct.ufvk().cloned().ok_or_else(|| anyhow!("account has no UFVK"))?;
        Ok((id, ufvk))
    }

    /// zecz `Wallet::init`: imports `spec` born at `birthday` (None: the
    /// chain tip, for a new account), fetching the tree state before it.
    /// Importing the same UFVK again changes nothing.
    pub async fn import(&mut self, spec: &AccountSpec, birthday: Option<u32>, on_event: OnEvent<'_>) -> Result<ImportOutcome> {
        let ufvk = parse_ufvk(self.network, &spec.ufvk)?;
        let enc = ufvk.encode(&self.network)?;
        for a in self.accounts()? {
            if a.ufvk == enc {
                let addresses = addresses_of(self.network, &ufvk)?;
                return Ok(ImportOutcome { imported: false, account: a, addresses });
            }
        }
        let mut client = lwd::connect(&self.server);
        let birthday = match birthday {
            Some(b) => b,
            None => client.get_latest_block(ChainSpec {}).await.context("GetLatestBlock")?.into_inner().height as u32,
        };
        let height = birthday.saturating_sub(1);
        on_event(json!({"event": "import", "phase": "treeState", "height": height, "server": self.server}));
        let ts = client
            .get_tree_state(BlockId { height: u64::from(height), hash: vec![] })
            .await
            .context("GetTreeState")?
            .into_inner();
        let birthday = AccountBirthday::from_treestate(ts, None).map_err(|e| anyhow!("bad tree state: {e:?}"))?;
        let purpose = match &spec.origin {
            Some(o) => {
                let account_index = zip32::AccountId::try_from(o.account_index).map_err(|_| anyhow!("bad account index"))?;
                let fp = SeedFingerprint::from_bytes(hex32(&o.seed_fingerprint).context("seed fingerprint")?);
                // As zecz: the ZIP 32 derivation puts zip32_derivation on our
                // spends, which the device uses to find its key.
                AccountPurpose::Spending { derivation: Some(Zip32Derivation::new(fp, account_index)) }
            }
            None => AccountPurpose::ViewOnly,
        };
        let acct = self.db().import_account_ufvk(&spec.name, &ufvk, &birthday, purpose, Some("leekwallet"))?;
        Ok(ImportOutcome {
            imported: true,
            account: self.account_info(acct.id())?,
            addresses: addresses_of(self.network, &ufvk)?,
        })
    }

    pub fn addresses(&self, account: Option<&str>) -> Result<Addresses> {
        let (_, ufvk) = self.account(account)?;
        addresses_of(self.network, &ufvk)
    }

    /// zecz `Wallet::next_address`: a fresh diversified address, Orchard and
    /// P2PKH while the transparent gap limit allows, then Orchard only.
    pub fn next_address(&mut self, account: Option<&str>) -> Result<DiversifiedAddress> {
        use zcash_client_sqlite::error::SqliteClientError;
        let (id, _) = self.account(account)?;
        let net = self.network;
        let mut db = self.db();
        for _ in 0..2 {
            let (ua, j) = match db.get_next_available_address(id, ua_request()) {
                Ok(Some(a)) => a,
                Ok(None) => bail!("account vanished"),
                Err(SqliteClientError::ReachedGapLimit(..)) => db
                    .get_next_available_address(id, UnifiedAddressRequest::ORCHARD)
                    .map_err(|e| anyhow!("address generation: {e}"))?
                    .ok_or_else(|| anyhow!("account vanished"))?,
                Err(e) => bail!("address generation: {e}"),
            };
            if u128::from(j) == 0 {
                continue;
            }
            return Ok(DiversifiedAddress {
                unified: ua.encode(&net),
                diversifier_index: u128::from(j).to_string(),
                transparent: ua.transparent().map(|t| Address::Transparent(*t).encode(&net)),
            });
        }
        bail!("the wallet keeps handing out its default address")
    }

    // --------------------------------------------------------------- sync

    /// zecz `Wallet::sync`: scans until the wallet is up to date, `batch`
    /// blocks at a time, or until `cancel_sync`. What was scanned stays
    /// scanned: the next sync starts from there.
    pub async fn sync(&mut self, batch: u32, on_event: OnEvent<'_>) -> Result<SyncOutcome> {
        if self.rdb().get_account_ids()?.is_empty() {
            bail!("the {} wallet has no account to sync", self.net_name());
        }
        let t0 = now_ms();
        let mut client = lwd::connect(&self.server);
        let tip = client.get_latest_block(ChainSpec {}).await.context("GetLatestBlock")?.into_inner().height as u32;
        let before = self.sync_status()?;
        on_event(json!({
            "event": "sync", "phase": "started", "server": self.server, "tip": tip,
            "fullyScanned": before.map(|s| s.fully_scanned),
        }));
        lwd::clear_cancel();
        let scanned = Cell::new(0u64);
        let cache = MemBlockCache::with_progress(|r| {
            scanned.set(scanned.get() + u64::from(r.end - r.start));
            on_event(json!({
                "event": "sync", "phase": "scanned", "start": r.start, "end": r.end, "tip": tip,
                "scannedBlocks": scanned.get(), "elapsedMs": now_ms() - t0,
            }));
        });
        let network = self.network;
        let res = {
            let mut db = self.db();
            zcash_client_backend::sync::run(&mut client, &network, &cache, &mut db, batch).await
        };
        let cancelled = match res {
            Ok(()) => false,
            Err(_) if lwd::cancel_requested() => true,
            Err(e) => {
                lwd::clear_cancel();
                bail!("sync failed: {e}")
            }
        };
        lwd::clear_cancel();
        Ok(SyncOutcome {
            cancelled,
            tip,
            scanned_blocks: scanned.get(),
            elapsed_ms: now_ms() - t0,
            status: self.sync_status()?,
        })
    }

    pub fn sync_status(&self) -> Result<Option<SyncStatus>> {
        Ok(self.rdb().get_wallet_summary(ConfirmationsPolicy::default())?.map(|s| SyncStatus {
            chain_tip: u32::from(s.chain_tip_height()),
            fully_scanned: u32::from(s.fully_scanned_height()),
            progress_num: *s.progress().scan().numerator(),
            progress_den: *s.progress().scan().denominator(),
            synced: s.is_synced(),
        }))
    }

    /// zecz `Wallet::balances`.
    pub fn balances(&self, account: Option<&str>) -> Result<Balances> {
        let (id, _) = self.account(account)?;
        let Some(s) = self.rdb().get_wallet_summary(ConfirmationsPolicy::default())? else {
            return Ok(Balances {
                account: id.expose_uuid().to_string(),
                chain_tip: 0,
                fully_scanned: 0,
                synced: false,
                balances: None,
            });
        };
        let pool = |b: &zcash_client_backend::data_api::Balance| PoolBalance {
            total: u64::from(b.total()),
            spendable: u64::from(b.spendable_value()),
            pending: u64::from(b.change_pending_confirmation()) + u64::from(b.value_pending_spendability()),
        };
        let balances = s.account_balances().get(&id).map(|b| AccountBalances {
            orchard: pool(b.orchard_balance()),
            ironwood: pool(b.ironwood_balance()),
            transparent: pool(&b.unshielded_balance()),
            total: u64::from(b.total()),
            spendable: u64::from(b.spendable_value()),
        });
        Ok(Balances {
            account: id.expose_uuid().to_string(),
            chain_tip: u32::from(s.chain_tip_height()),
            fully_scanned: u32::from(s.fully_scanned_height()),
            synced: s.is_synced(),
            balances,
        })
    }

    // -------------------------------------------------------------- sends

    /// zecz `Wallet::propose_send`.
    pub fn propose_send(&mut self, account: Option<&str>, req: &SendRequest) -> Result<SendProposal> {
        let params = self.network;
        let (account_id, _) = self.account(account)?;
        let to_addr = Address::decode(&params, req.to.trim())
            .ok_or_else(|| anyhow!("{} is not a valid {} address", req.to.trim(), self.net_name()))?;
        if matches!(to_addr, Address::Sapling(_)) {
            bail!("Sapling recipients are not supported");
        }
        let value = Zatoshis::from_u64(req.amount_zat).map_err(|_| anyhow!("amount out of range"))?;
        if value == Zatoshis::ZERO {
            bail!("amount must be positive");
        }
        let memo = req
            .memo
            .as_deref()
            .filter(|m| !m.is_empty())
            .map(|m| -> Result<MemoBytes> { Ok(MemoBytes::from(Memo::from_bytes(m.as_bytes()).map_err(|e| anyhow!("memo: {e:?}"))?)) })
            .transpose()?;
        let payment = Payment::new(to_addr.to_zcash_address(&params), Some(value), memo, None, None, vec![])
            .map_err(|e| anyhow!("payment: {e:?}"))?;
        let request = TransactionRequest::new(vec![payment]).map_err(|e| anyhow!("request: {e:?}"))?;

        let (target, _) = self
            .rdb()
            .get_target_and_anchor_heights(ConfirmationsPolicy::default().trusted())?
            .ok_or_else(|| anyhow!("wallet is not synced"))?;
        let target_h = BlockHeight::from(target);
        let input_selector = GreedyInputSelector::<Db<&mut Connection>>::new();
        let change_strategy = SingleOutputChangeStrategy::<Db<&mut Connection>>::new(
            StandardFeeRule::Zip317,
            None,
            change_pool(params, target_h),
            DustOutputPolicy::default(),
        );
        let proposal = propose_transfer::<_, _, _, _, Infallible>(
            &mut self.db(),
            &params,
            account_id,
            &input_selector,
            &change_strategy,
            request,
            ConfirmationsPolicy::default(),
            &SpendPolicy::default(),
            None,
            None,
        )
        .map_err(|e| anyhow!("cannot propose the transfer: {e}"))?;
        self.build_proposed(account_id, &proposal, target_h, req.compact)
    }

    /// zecz `Wallet::propose_shield`: every transparent coin of the account
    /// into its shielded pool.
    pub fn propose_shield(&mut self, account: Option<&str>, compact: bool) -> Result<SendProposal> {
        use zcash_client_backend::data_api::{wallet::propose_shielding, CoinbaseFilter};
        let params = self.network;
        let (account_id, _) = self.account(account)?;
        let (target, _) = self
            .rdb()
            .get_target_and_anchor_heights(ConfirmationsPolicy::default().trusted())?
            .ok_or_else(|| anyhow!("wallet is not synced"))?;
        let target_h = BlockHeight::from(target);
        let from: Vec<_> = self.rdb().get_transparent_receivers(account_id, true, true)?.into_keys().collect();
        if from.is_empty() {
            bail!("the account has no transparent addresses");
        }
        let input_selector = GreedyInputSelector::<Db<&mut Connection>>::new();
        let change_strategy = SingleOutputChangeStrategy::<Db<&mut Connection>>::new(
            StandardFeeRule::Zip317,
            None,
            change_pool(params, target_h),
            DustOutputPolicy::default(),
        );
        let proposal = propose_shielding::<_, _, _, _, Infallible>(
            &mut self.db(),
            &params,
            &input_selector,
            &change_strategy,
            Zatoshis::const_from_u64(10_000),
            &from,
            account_id,
            ConfirmationsPolicy::default(),
            CoinbaseFilter::AllTransparentOutputs,
            None,
        )
        .map_err(|e| anyhow!("nothing to shield: {e}"))?;
        let step = proposal.steps().first();
        let shielded: u64 = step.balance().proposed_change().iter().map(|c| u64::from(c.value())).sum();
        if shielded == 0 {
            bail!("shielding would move nothing: the fee takes the whole transparent balance");
        }
        self.build_proposed(account_id, &proposal, target_h, compact)
    }

    /// zecz `build_proposed`: the PCZT of a one-step proposal; the full PCZT
    /// is kept in `pending`, the signer view returned.
    fn build_proposed<N: std::fmt::Display>(
        &mut self,
        account_id: AccountUuid,
        proposal: &zcash_client_backend::proposal::Proposal<StandardFeeRule, N>,
        target_h: BlockHeight,
        compact: bool,
    ) -> Result<SendProposal> {
        let params = self.network;
        let step = proposal.steps().first();
        if step.input_in_pool(PoolType::SAPLING) {
            bail!("the proposal spends Sapling notes; not supported");
        }
        let mut rng = UnwrapErr(SysRng);
        let pczt = create_pczt_from_proposal::<_, _, Infallible, _, Infallible, _>(
            &mut self.db(),
            &params,
            &JsClock,
            &mut rng,
            account_id,
            OvkPolicy::Sender,
            proposal,
            None,
            BundlePadding::DEFAULT,
        )
        .map_err(|e| anyhow!("cannot create the PCZT: {e}"))?;
        if !pczt.sapling().spends().is_empty() {
            bail!("PCZT has Sapling spends; refusing");
        }
        let view = redact_pczt_for_signer(&pczt, if compact { SignerView::Compact } else { SignerView::Full });
        let view_bytes = view.serialize().map_err(|e| anyhow!("PCZT encoding: {e:?}"))?;
        let full_bytes = pczt.serialize().map_err(|e| anyhow!("PCZT encoding: {e:?}"))?;
        let hash = zsig::sha256(&view_bytes);

        let payments = step
            .transaction_request()
            .payments()
            .iter()
            .map(|(i, p)| PaymentSummary {
                recipient: p.recipient_address().encode(),
                amount_zat: u64::from(p.amount().unwrap_or(Zatoshis::ZERO)),
                pool: step.payment_pools().get(i).map(|p| format!("{p}")).unwrap_or_default(),
                has_memo: p.memo().is_some(),
            })
            .collect();
        let change = step
            .balance()
            .proposed_change()
            .iter()
            .map(|c| ChangeSummary { value_zat: u64::from(c.value()), pool: format!("{}", c.output_pool()) })
            .collect();
        let proposal = SendProposal {
            account: account_id.expose_uuid().to_string(),
            target_height: u32::from(target_h),
            payments,
            change,
            fee_zat: u64::from(step.balance().fee_required()),
            signer_view: hex::encode(&view_bytes),
            compact,
            sha256: hex::encode(hash),
        };
        let summary = serde_json::to_string(&SendProposal { signer_view: String::new(), ..proposal.clone() })?;
        self.leek.execute(
            "INSERT OR REPLACE INTO pending (view_sha256, account_uuid, created_ms, summary, signer_view, full_pczt)
             VALUES (?1, ?2, ?3, ?4, ?5, ?6)",
            rusqlite::params![&hash[..], proposal.account, now_ms() as i64, summary, view_bytes, full_bytes],
        )?;
        Ok(proposal)
    }

    fn pending_blob(&self, sha: &[u8; 32], column: &str) -> Result<Option<Vec<u8>>> {
        let sql = format!("SELECT {column} FROM pending WHERE view_sha256 = ?1");
        let row: Option<Option<Vec<u8>>> = self.leek.query_row(&sql, [&sha[..]], |r| r.get(0)).optional()?;
        match row {
            None => bail!("no pending send {} in this wallet; propose it again", hex::encode(sha)),
            Some(v) => Ok(v),
        }
    }

    /// The sends proposed and not yet extracted, newest first.
    pub fn pending(&self) -> Result<Vec<Value>> {
        let mut st = self.leek.prepare(
            "SELECT summary, signed_pczt IS NOT NULL, proven_pczt IS NOT NULL, txid, created_ms
             FROM pending ORDER BY created_ms DESC",
        )?;
        let rows = st.query_map([], |r| {
            Ok((r.get::<_, String>(0)?, r.get::<_, bool>(1)?, r.get::<_, bool>(2)?, r.get::<_, Option<String>>(3)?, r.get::<_, i64>(4)?))
        })?;
        let mut out = Vec::new();
        for row in rows {
            let (summary, signed, proven, txid, created) = row?;
            let mut v: Value = serde_json::from_str(&summary)?;
            if let Some(o) = v.as_object_mut() {
                o.remove("signerView");
                let stage = if txid.is_some() { "extracted" } else if proven { "proven" } else if signed { "signed" } else { "unsigned" };
                o.insert("stage".into(), json!(stage));
                o.insert("txid".into(), json!(txid));
                o.insert("createdMs".into(), json!(created));
            }
            out.push(v);
        }
        Ok(out)
    }

    /// The first step of zecz `Wallet::finish`: checks the `.zsig` against
    /// the signer view it signs, checks the view against the stored full
    /// PCZT (same shielded sighash), and applies the signatures, each
    /// verified against its rk or its input's key. Returns the view's hash.
    pub fn apply_zsig(&mut self, signer_view: &[u8], zsig_bytes: &[u8]) -> Result<(String, usize)> {
        let hash = zsig::sha256(signer_view);
        let z = Zsig::parse_for(zsig_bytes, signer_view)?;
        if z.entries.is_empty() {
            bail!("the .zsig carries no signatures");
        }
        let full_bytes = self.pending_blob(&hash, "full_pczt")?.ok_or_else(|| anyhow!("pending PCZT is empty"))?;
        let full = Pczt::parse(&full_bytes).map_err(|e| anyhow!("pending PCZT: {e:?}"))?;
        let view = Pczt::parse(signer_view).map_err(|e| anyhow!("unsigned PCZT: {e:?}"))?;
        if !full.sapling().spends().is_empty() {
            bail!("PCZT has Sapling spends; not supported");
        }
        let sighash = Signer::new(full.clone()).map_err(|e| anyhow!("{e:?}"))?.shielded_sighash();
        if sighash != Signer::new(view).map_err(|e| anyhow!("{e:?}"))?.shielded_sighash() {
            bail!("the unsigned PCZT does not match the pending one (sighash differs)");
        }
        let signed = apply_zsig(full, &z)?;
        let bytes = signed.serialize().map_err(|e| anyhow!("PCZT encoding: {e:?}"))?;
        self.leek.execute(
            "UPDATE pending SET signed_pczt = ?2, proven_pczt = NULL WHERE view_sha256 = ?1",
            rusqlite::params![&hash[..], bytes],
        )?;
        Ok((hex::encode(hash), z.entries.len()))
    }

    /// zecz `finish`'s proving step (Orchard and Ironwood bundles), with
    /// progress events. The proving key is built the first time (seconds).
    pub fn prove(&mut self, sha256: &str, on_event: OnEvent<'_>) -> Result<f64> {
        let hash = hex32(sha256)?;
        let signed = self.pending_blob(&hash, "signed_pczt")?.ok_or_else(|| anyhow!("apply the .zsig first"))?;
        let pczt = Pczt::parse(&signed).map_err(|e| anyhow!("signed PCZT: {e:?}"))?;
        let t0 = now_ms();
        let proven = prove_with(pczt, |e| {
            let v = match e {
                ProveEvent::LoadingKey { pool, circuit } => {
                    json!({"event": "prove", "phase": "loadingKey", "pool": pool, "circuit": circuit, "elapsedMs": now_ms() - t0})
                }
                ProveEvent::Proving { pool } => json!({"event": "prove", "phase": "proving", "pool": pool, "elapsedMs": now_ms() - t0}),
            };
            on_event(v);
        })?;
        let ms = now_ms() - t0;
        let bytes = proven.serialize().map_err(|e| anyhow!("PCZT encoding: {e:?}"))?;
        self.leek
            .execute("UPDATE pending SET proven_pczt = ?2 WHERE view_sha256 = ?1", rusqlite::params![&hash[..], bytes])?;
        Ok(ms)
    }

    /// zecz `finish`'s extraction: the transaction, stored in the wallet
    /// (so its spends show as pending) and in `sent`.
    pub fn extract(&mut self, sha256: &str) -> Result<Extracted> {
        let hash = hex32(sha256)?;
        let proven = self.pending_blob(&hash, "proven_pczt")?.ok_or_else(|| anyhow!("prove the PCZT first"))?;
        let pczt = Pczt::parse(&proven).map_err(|e| anyhow!("proven PCZT: {e:?}"))?;
        let mut rng = UnwrapErr(SysRng);
        let txid = extract_and_store_transaction_from_pczt::<_, ReceivedNoteId>(&mut self.db(), &JsClock, &mut rng, pczt, None, None)
            .map_err(|e| anyhow!("transaction extraction failed: {e}"))?;
        let tx = self.rdb().get_transaction(txid)?.ok_or_else(|| anyhow!("stored transaction not found"))?;
        let mut raw = Vec::new();
        tx.write(&mut raw)?;
        let txid = txid.to_string();
        self.leek.execute(
            "INSERT OR REPLACE INTO sent (txid, raw, created_ms) VALUES (?1, ?2, ?3)",
            rusqlite::params![txid, raw, now_ms() as i64],
        )?;
        self.leek.execute("UPDATE pending SET txid = ?2 WHERE view_sha256 = ?1", rusqlite::params![&hash[..], txid])?;
        Ok(Extracted { txid, size: raw.len(), raw: hex::encode(&raw) })
    }

    /// Sends an extracted transaction through the gRPC-web endpoint.
    pub async fn broadcast(&mut self, txid: &str) -> Result<()> {
        let raw: Vec<u8> = self
            .leek
            .query_row("SELECT raw FROM sent WHERE txid = ?1", [txid], |r| r.get(0))
            .optional()?
            .ok_or_else(|| anyhow!("no extracted transaction {txid}"))?;
        let res = broadcast_raw(&self.server, &raw).await;
        let err = res.as_ref().err().map(|e| format!("{e:#}"));
        self.leek.execute(
            "UPDATE sent SET broadcast_ms = ?2, broadcast_error = ?3 WHERE txid = ?1",
            rusqlite::params![txid, now_ms() as i64, err],
        )?;
        res
    }

    // ------------------------------------------------------------ history

    /// zecz `Wallet::history`: the account's transactions, newest first
    /// (unmined on top), from zcash_client_sqlite's views.
    pub fn history(&self, account: Option<&str>, limit: u32) -> Result<Vec<TxRecord>> {
        let (id, _) = self.account(account)?;
        let conn = &self.conn;
        let mut txs = conn.prepare(
            "SELECT t.txid, t.mined_height, t.block_time, t.expiry_height, t.expired_unmined,
                    t.account_balance_delta, t.fee_paid, t.is_shielding, t.total_received,
                    t.pool_crossing_value
             FROM v_transactions t
             WHERE t.account_uuid = :uuid
             ORDER BY t.mined_height IS NOT NULL, t.mined_height DESC, t.tx_index DESC
             LIMIT :limit",
        )?;
        let mut outs = conn.prepare(
            "SELECT o.value, o.is_change, o.memo, o.to_address, o.to_account_uuid IS NULL
             FROM v_tx_outputs o
             WHERE o.txid = :txid
             ORDER BY o.output_pool, o.output_index",
        )?;
        let rows = txs.query_map(named_params![":uuid": id.expose_uuid().as_bytes(), ":limit": limit], |r| {
            Ok((
                r.get::<_, Vec<u8>>(0)?,
                r.get::<_, Option<u32>>(1)?,
                r.get::<_, Option<i64>>(2)?,
                r.get::<_, Option<u32>>(3)?,
                r.get::<_, Option<bool>>(4)?,
                r.get::<_, i64>(5)?,
                r.get::<_, Option<i64>>(6)?,
                r.get::<_, bool>(7)?,
                r.get::<_, i64>(8)?,
                r.get::<_, Option<i64>>(9)?,
            ))
        })?;
        let mut history = Vec::new();
        for row in rows {
            let (txid, mined, time, expiry, expired, delta, fee, shielding, received, crossing) = row?;
            let mut memos: Vec<String> = Vec::new();
            let mut recipients = Vec::new();
            let mut paid_out = 0u64;
            let out_rows = outs.query_map(named_params![":txid": txid], |r| {
                Ok((
                    r.get::<_, i64>(0)?,
                    r.get::<_, bool>(1)?,
                    r.get::<_, Option<Vec<u8>>>(2)?,
                    r.get::<_, Option<String>>(3)?,
                    r.get::<_, bool>(4)?,
                ))
            })?;
            for o in out_rows {
                let (value, is_change, memo, to, external) = o?;
                if external {
                    paid_out += value.max(0) as u64;
                    if let Some(to) = to {
                        recipients.push(Recipient { address: to, value_zat: value.max(0) as u64 });
                    }
                }
                if let Some(text) = memo.as_deref().filter(|_| !is_change).and_then(memo_text) {
                    if !memos.contains(&text) {
                        memos.push(text);
                    }
                }
            }
            let fee_zat = fee.map(|f| f.max(0) as u64);
            let (kind, amount_zat) = if shielding {
                ("shielded", received.max(0) as u64)
            } else if let Some(c) = crossing {
                ("internal", c.max(0) as u64)
            } else if delta < 0 {
                let amount = if paid_out > 0 { paid_out } else { delta.unsigned_abs().saturating_sub(fee_zat.unwrap_or(0)) };
                ("sent", amount)
            } else {
                ("received", delta as u64)
            };
            let mut txid = txid;
            txid.reverse();
            history.push(TxRecord {
                txid: hex::encode(txid),
                mined_height: mined,
                block_time: time,
                expiry_height: expiry.filter(|&e| e != 0),
                expired_unmined: expired.unwrap_or(false),
                kind,
                delta_zat: delta,
                amount_zat,
                fee_zat,
                memos,
                recipients,
            });
        }
        Ok(history)
    }

    /// DEV ONLY, testnet only: signs a signer view with zecz's soft signer
    /// (keys from a mnemonic, no checks) and returns the `.zsig`, exactly
    /// what the board would. It stands in for the board (9.3) and for the
    /// software wallets (9.4) so the send path can be exercised now.
    pub fn sign_soft(&self, signer_view: &[u8], mnemonic: &str, account_index: u32) -> Result<Vec<u8>> {
        if self.network != Network::TestNetwork {
            bail!("the dev soft signer is testnet only");
        }
        let acct = crate::keys::DevAccount::from_mnemonic(&self.network, mnemonic, "", account_index)?;
        let z = crate::soft_signer::sign(signer_view, &acct, self.network.network_type().coin_type())?;
        Ok(z.to_bytes()?)
    }
}

/// The UFVK and ZIP 32 origin of account `index` of a mnemonic, as the
/// device exports them (DEV ONLY: the board derives these on-chip).
pub fn dev_account_spec(network: Network, mnemonic: &str, index: u32) -> Result<Value> {
    let acct = crate::keys::DevAccount::from_mnemonic(&network, mnemonic, "", index)?;
    Ok(json!({
        "ufvk": acct.ufvk.encode(&network)?,
        "origin": { "seedFingerprint": hex::encode(acct.seed_fingerprint.to_bytes()), "accountIndex": index },
    }))
}
