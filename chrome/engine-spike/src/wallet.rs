//! zecz-core's wallet flow in the browser (feature `sqlite`, wasm32 only):
//! `zcash_client_sqlite` on SQLite compiled by sqlite-wasm-rs (in-memory
//! here), `zcash_client_backend::sync::run` over gRPC-web, then
//! `propose_transfer` + `create_pczt_from_proposal` for a send to self,
//! then sign (dev soft signer) / prove / extract. Nothing is broadcast.
//!
//! The block cache and the clock follow host/zecz-core (lwd.rs, wallet.rs).

use std::collections::BTreeMap;
use std::convert::Infallible;
use std::sync::Mutex;
use std::time::{Duration, SystemTime, UNIX_EPOCH};

use async_trait::async_trait;
use rand::{rand_core::UnwrapErr, rngs::SysRng};
use zcash_client_backend::data_api::chain::{error::Error as ChainError, BlockCache, BlockSource};
use zcash_client_backend::data_api::scanning::ScanRange;
use zcash_client_backend::data_api::wallet::{
    create_pczt_from_proposal, input_selection::GreedyInputSelector, input_selection::SpendPolicy, propose_transfer,
    ConfirmationsPolicy,
};
use zcash_client_backend::data_api::{
    Account as _, AccountBirthday, AccountPurpose, WalletRead, WalletWrite, Zip32Derivation,
};
use zcash_client_backend::fees::{standard::SingleOutputChangeStrategy, DustOutputPolicy, StandardFeeRule};
use zcash_client_backend::proto::compact_formats::CompactBlock;
use zcash_client_backend::proto::service::{
    compact_tx_streamer_client::CompactTxStreamerClient, BlockId, ChainSpec,
};
use zcash_client_backend::util::Clock;
use zcash_client_backend::wallet::OvkPolicy;
use zcash_client_sqlite::{wallet::init::init_wallet_db, WalletDb};
use zcash_keys::keys::{ReceiverRequirement, UnifiedAddressRequest};
use zcash_primitives::transaction::builder::BundlePadding;
use zcash_protocol::consensus::{BlockHeight, NetworkUpgrade, Parameters};
use zcash_protocol::value::Zatoshis;
use zcash_protocol::ShieldedPool;
use zip321::{Payment, TransactionRequest};

use crate::engine::NETWORK;

#[wasm_bindgen::prelude::wasm_bindgen]
extern "C" {
    #[wasm_bindgen(js_namespace = console)]
    fn log(s: &str);
}

/// `SystemTime::now()` panics on wasm32-unknown-unknown; use JS time.
#[derive(Clone, Copy)]
pub struct JsClock;

impl Clock for JsClock {
    fn now(&self) -> SystemTime {
        UNIX_EPOCH + Duration::from_millis(js_sys::Date::now() as u64)
    }
}

/// Forwards librustzcash's `tracing` events (INFO and above) to
/// console.log, so a stalled sync shows which step it is in.
struct ConsoleTracing;

impl tracing::Subscriber for ConsoleTracing {
    fn enabled(&self, m: &tracing::Metadata<'_>) -> bool {
        *m.level() <= tracing::Level::INFO
    }
    fn new_span(&self, _: &tracing::span::Attributes<'_>) -> tracing::span::Id {
        tracing::span::Id::from_u64(1)
    }
    fn record(&self, _: &tracing::span::Id, _: &tracing::span::Record<'_>) {}
    fn record_follows_from(&self, _: &tracing::span::Id, _: &tracing::span::Id) {}
    fn event(&self, ev: &tracing::Event<'_>) {
        struct V(String);
        impl tracing::field::Visit for V {
            fn record_debug(&mut self, f: &tracing::field::Field, v: &dyn std::fmt::Debug) {
                if f.name() == "message" {
                    self.0 += &format!("{v:?} ");
                } else {
                    self.0 += &format!("{}={v:?} ", f.name());
                }
            }
        }
        let mut v = V(String::new());
        ev.record(&mut v);
        let msg = v.0;
        log(&format!("{} {}: {}", ev.metadata().level(), ev.metadata().target(), msg.chars().take(300).collect::<String>()));
    }
    fn enter(&self, _: &tracing::span::Id) {}
    fn exit(&self, _: &tracing::span::Id) {}
}

type Db = WalletDb<rusqlite::Connection, zcash_protocol::consensus::Network, JsClock, UnwrapErr<SysRng>>;

#[derive(Debug)]
pub struct CacheError(String);
impl std::fmt::Display for CacheError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "block cache: {}", self.0)
    }
}
impl std::error::Error for CacheError {}

#[derive(Default)]
pub struct MemBlockCache {
    blocks: Mutex<BTreeMap<u32, CompactBlock>>,
    pub scanned_blocks: Mutex<u64>,
}

impl BlockSource for MemBlockCache {
    type Error = CacheError;
    fn with_blocks<F, WalletErrT>(
        &self,
        from_height: Option<BlockHeight>,
        limit: Option<usize>,
        mut with_block: F,
    ) -> Result<(), ChainError<WalletErrT, Self::Error>>
    where
        F: FnMut(CompactBlock) -> Result<(), ChainError<WalletErrT, Self::Error>>,
    {
        let start = from_height.map(u32::from).unwrap_or(0);
        let blocks: Vec<CompactBlock> = self
            .blocks
            .lock()
            .unwrap()
            .range(start..)
            .take(limit.unwrap_or(usize::MAX))
            .map(|(_, b)| b.clone())
            .collect();
        for b in blocks {
            with_block(b)?;
        }
        Ok(())
    }
}

#[async_trait]
impl BlockCache for MemBlockCache {
    fn get_tip_height(&self, range: Option<&ScanRange>) -> Result<Option<BlockHeight>, Self::Error> {
        let blocks = self.blocks.lock().unwrap();
        let tip = match range {
            Some(r) => {
                let (s, e) = (u32::from(r.block_range().start), u32::from(r.block_range().end));
                blocks.range(s..e).next_back().map(|(k, _)| *k)
            }
            None => blocks.keys().next_back().copied(),
        };
        Ok(tip.map(BlockHeight::from_u32))
    }

    async fn read(&self, range: &ScanRange) -> Result<Vec<CompactBlock>, Self::Error> {
        let (s, e) = (u32::from(range.block_range().start), u32::from(range.block_range().end));
        let blocks = self.blocks.lock().unwrap();
        let mut out = Vec::new();
        for (expected, (k, b)) in (s..).zip(blocks.range(s..e)) {
            if *k != expected {
                break;
            }
            out.push(b.clone());
        }
        Ok(out)
    }

    async fn insert(&self, compact_blocks: Vec<CompactBlock>) -> Result<(), Self::Error> {
        let mut blocks = self.blocks.lock().unwrap();
        for b in compact_blocks {
            blocks.insert(b.height as u32, b);
        }
        Ok(())
    }

    async fn delete(&self, range: ScanRange) -> Result<(), Self::Error> {
        let (s, e) = (u32::from(range.block_range().start), u32::from(range.block_range().end));
        self.blocks.lock().unwrap().retain(|k, _| *k < s || *k >= e);
        *self.scanned_blocks.lock().unwrap() += u64::from(e - s);
        Ok(())
    }
}

#[derive(Debug, Default)]
pub struct WalletRun {
    pub birthday: u32,
    pub tip: u64,
    pub init_ms: f64,
    pub sync_ms: f64,
    pub scanned_blocks: u64,
    pub spendable_zat: u64,
    pub total_zat: u64,
    pub propose_ms: f64,
    pub pczt: Vec<u8>,
    pub fee_zat: u64,
    /// Set when sync worked but propose / create PCZT did not (e.g. no funds).
    pub propose_error: Option<String>,
}

fn e(x: impl std::fmt::Display) -> String {
    x.to_string()
}

/// Init (birthday `blocks_back` below the tip), sync, and build the PCZT of
/// a `send_zat` payment to the account's own address.
pub async fn run(url: &str, mnemonic: &str, blocks_back: u32, send_zat: u64) -> Result<WalletRun, String> {
    let _ = tracing::subscriber::set_global_default(ConsoleTracing);
    let now = || js_sys::Date::now();
    let mut r = WalletRun::default();
    let mut client = CompactTxStreamerClient::new(tonic_web_wasm_client::Client::new(url.to_string()));

    let t0 = now();
    let ufvk = crate::engine::ufvk_for(mnemonic)?;
    let seed = bip39::Mnemonic::parse_normalized(mnemonic).map_err(e)?.to_seed_normalized("");
    let fp = zip32::fingerprint::SeedFingerprint::from_seed(&seed).ok_or("seed")?;
    // ":memory:" is SQLite's in-memory database; the extension would use
    // sqlite-wasm-rs's OPFS VFS from a dedicated worker instead.
    log("wallet: opening sqlite");
    let mut db: Db = WalletDb::for_path(":memory:", NETWORK, JsClock, UnwrapErr(SysRng)).map_err(e)?;
    log("wallet: migrating schema");
    init_wallet_db(&mut db, None).map_err(|x| format!("init_wallet_db: {x}"))?;
    log("wallet: schema ready");
    r.tip = client.get_latest_block(ChainSpec {}).await.map_err(e)?.into_inner().height;
    r.birthday = (r.tip as u32).saturating_sub(blocks_back);
    let ts = client
        .get_tree_state(BlockId { height: u64::from(r.birthday - 1), hash: vec![] })
        .await
        .map_err(e)?
        .into_inner();
    let birthday = AccountBirthday::from_treestate(ts, None).map_err(|x| format!("treestate: {x:?}"))?;
    let purpose = AccountPurpose::Spending {
        derivation: Some(Zip32Derivation::new(fp, zip32::AccountId::ZERO)),
    };
    log("wallet: importing account");
    let account = db
        .import_account_ufvk("spike", &ufvk, &birthday, purpose, Some("leekwallet"))
        .map_err(e)?;
    let account_id = account.id();
    r.init_ms = now() - t0;

    let t0 = now();
    let cache = MemBlockCache::default();
    log("wallet: sync::run");
    zcash_client_backend::sync::run(&mut client, &NETWORK, &cache, &mut db, 1000)
        .await
        .map_err(|x| format!("sync: {x}"))?;
    r.sync_ms = now() - t0;
    r.scanned_blocks = *cache.scanned_blocks.lock().unwrap();
    if let Some(s) = db.get_wallet_summary(ConfirmationsPolicy::default()).map_err(e)? {
        if let Some(b) = s.account_balances().get(&account_id) {
            r.spendable_zat = u64::from(b.spendable_value());
            r.total_zat = u64::from(b.total());
        }
    }

    if let Err(x) = propose(&mut db, &ufvk, account_id, send_zat, &mut r) {
        r.propose_error = Some(x);
    }
    Ok(r)
}

/// `propose_transfer` + `create_pczt_from_proposal` for `send_zat` to self.
fn propose(
    db: &mut Db,
    ufvk: &zcash_keys::keys::UnifiedFullViewingKey,
    account_id: zcash_client_sqlite::AccountUuid,
    send_zat: u64,
    r: &mut WalletRun,
) -> Result<(), String> {
    let now = || js_sys::Date::now();
    let t0 = now();
    let req = UnifiedAddressRequest::unsafe_custom(ReceiverRequirement::Require, ReceiverRequirement::Omit, ReceiverRequirement::Omit);
    let (ua, _) = ufvk.default_address(req).map_err(|x| format!("address: {x:?}"))?;
    let payment = Payment::new(
        zcash_keys::address::Address::from(ua).to_zcash_address(&NETWORK),
        Some(Zatoshis::from_u64(send_zat).map_err(|_| "amount")?),
        None,
        None,
        None,
        vec![],
    )
    .map_err(|x| format!("payment: {x:?}"))?;
    let request = TransactionRequest::new(vec![payment]).map_err(|x| format!("request: {x:?}"))?;
    let (target, _) = db
        .get_target_and_anchor_heights(ConfirmationsPolicy::default().trusted())
        .map_err(e)?
        .ok_or("not synced")?;
    let change_pool = if NETWORK.is_nu_active(NetworkUpgrade::Nu6_3, BlockHeight::from(target)) {
        ShieldedPool::Ironwood
    } else {
        ShieldedPool::Orchard
    };
    log("wallet: proposing");
    let input_selector = GreedyInputSelector::<Db>::new();
    let change_strategy =
        SingleOutputChangeStrategy::<Db>::new(StandardFeeRule::Zip317, None, change_pool, DustOutputPolicy::default());
    let proposal = propose_transfer::<_, _, _, _, Infallible>(
        db,
        &NETWORK,
        account_id,
        &input_selector,
        &change_strategy,
        request,
        ConfirmationsPolicy::default(),
        &SpendPolicy::default(),
        None,
        None,
    )
    .map_err(|x| format!("propose: {x}"))?;
    r.fee_zat = u64::from(proposal.steps().first().balance().fee_required());
    log("wallet: creating PCZT");
    let mut rng = UnwrapErr(SysRng);
    let pczt = create_pczt_from_proposal::<_, _, Infallible, _, Infallible, _>(
        db,
        &NETWORK,
        &JsClock,
        &mut rng,
        account_id,
        OvkPolicy::Sender,
        &proposal,
        None,
        BundlePadding::DEFAULT,
    )
    .map_err(|x| format!("create_pczt: {x}"))?;
    r.pczt = pczt.serialize().map_err(|x| format!("serialize: {x:?}"))?;
    r.propose_ms = now() - t0;
    Ok(())
}
