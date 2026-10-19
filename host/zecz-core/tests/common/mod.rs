//! Shared fixtures: a device-account wallet holding one transparent coin,
//! and the transactions such a wallet makes, all without a network.

#![allow(dead_code)]

use std::path::PathBuf;
use std::sync::atomic::{AtomicU32, Ordering};

use pczt::Pczt;
use rand::{rand_core::UnwrapErr, rngs::SysRng};
use zcash_client_backend::data_api::chain::{scan_cached_blocks, BlockCache, ChainState};
use zcash_client_backend::data_api::wallet::{
    create_pczt_from_proposal, input_selection::GreedyInputSelector, input_selection::SpendPolicy,
    input_selection::TransparentSpendPolicy, propose_transfer, ConfirmationsPolicy,
};
use zcash_client_backend::data_api::{WalletRead, WalletWrite};
use zcash_client_backend::fees::{standard::SingleOutputChangeStrategy, DustOutputPolicy, StandardFeeRule};
use zcash_client_backend::proto::compact_formats::{ChainMetadata, CompactBlock};
use zcash_client_backend::util::SystemClock;
use zcash_client_backend::wallet::{OvkPolicy, WalletTransparentOutput};
use zcash_client_sqlite::WalletDb;
use zcash_keys::address::Address;
use zcash_keys::keys::{ReceiverRequirement::*, UnifiedAddressRequest, UnifiedFullViewingKey};
use zcash_primitives::block::BlockHash;
use zcash_primitives::transaction::builder::BundlePadding;
use zcash_protocol::consensus::BlockHeight;
use zcash_protocol::value::Zatoshis;
use zcash_protocol::ShieldedPool;
use zcash_transparent::bundle::{OutPoint, TxOut};
use zcash_transparent::keys::TransparentKeyScope;
use zecz::keys::DevAccount;
use zecz::lwd::MemBlockCache;
use zecz::wallet::{AccountBirthday, AccountSpec};
use zecz::{Network, Profile, Wallet};

pub const TEST: Network = Network::TestNetwork;
pub const TEST_NU5: u32 = 1_842_420;
/// Past testnet NU6.3 (4,134,000): change goes to Ironwood.
pub const TIP: u32 = 4_200_000;
/// Where the fixture coin was mined.
pub const COIN_HEIGHT: u32 = TIP - 100;
/// The fixture coin's outpoint (a made-up funding transaction).
pub const COIN_TXID: [u8; 32] = [7; 32];
pub const ABANDON_ART: &str = "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon \
                               abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon \
                               abandon art";

pub struct TempDir(pub PathBuf);

impl TempDir {
    pub fn new(tag: &str) -> Self {
        static N: AtomicU32 = AtomicU32::new(0);
        let p = std::env::temp_dir().join(format!("zecz-{tag}-test-{}-{}", std::process::id(), N.fetch_add(1, Ordering::SeqCst)));
        let _ = std::fs::remove_dir_all(&p);
        std::fs::create_dir_all(&p).unwrap();
        TempDir(p)
    }
}

impl Drop for TempDir {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(&self.0);
    }
}

pub fn account() -> DevAccount {
    DevAccount::from_mnemonic(&TEST, ABANDON_ART, "", 0).unwrap()
}

pub fn spec() -> AccountSpec {
    AccountSpec::from_dev_account(TEST, &account(), "leekwallet-dev").unwrap()
}

/// The account's default t-address.
pub fn t_address() -> String {
    let ufvk = UnifiedFullViewingKey::decode(&TEST, &spec().ufvk).unwrap();
    let (ua, _) = ufvk.default_address(UnifiedAddressRequest::custom(Require, Omit, Require).unwrap()).unwrap();
    Address::Transparent(*ua.transparent().unwrap()).encode(&TEST)
}

pub fn db(profile: &Profile) -> WalletDb<rusqlite::Connection, Network, SystemClock, UnwrapErr<SysRng>> {
    WalletDb::for_path(profile.db_path(), TEST, SystemClock, UnwrapErr(SysRng)).unwrap()
}

/// A device-account wallet (abandon-art account 0) synced to [`TIP`], whose
/// t-address holds one coin of `value` zat mined at [`COIN_HEIGHT`], put
/// into its database as `sync`'s UTXO refresh does.
pub fn wallet_with_coin(dir: &std::path::Path, value: u64) -> Wallet {
    let spec = spec();
    let birthday =
        AccountBirthday::from_parts(ChainState::empty(BlockHeight::from_u32(TEST_NU5 - 1), BlockHash([0; 32])), None);
    let profile = Profile::new(TEST, dir.to_path_buf(), None);
    let (w, _) = Wallet::init_with_birthday(profile.clone(), &spec, &birthday).unwrap();
    drop(w);

    let mut db = db(&profile);
    db.update_chain_tip(BlockHeight::from_u32(TIP)).unwrap();
    // One empty block scanned below the tip: the note commitment trees get a
    // checkpoint, so the wallet has an anchor and a target height.
    let scanned = TIP - 20;
    let block = CompactBlock {
        height: u64::from(scanned),
        hash: vec![2; 32],
        prev_hash: vec![1; 32],
        time: 1_760_000_000,
        chain_metadata: Some(ChainMetadata::default()),
        ..Default::default()
    };
    let cache = MemBlockCache::with_progress(|_| {});
    tokio::runtime::Runtime::new().unwrap().block_on(cache.insert(vec![block])).unwrap();
    let from = ChainState::empty(BlockHeight::from_u32(scanned - 1), BlockHash([1; 32]));
    scan_cached_blocks(&TEST, &cache, &mut db, BlockHeight::from_u32(scanned), &from, 1).unwrap();
    let account = db.get_account_ids().unwrap()[0];
    let taddr = match Address::decode(&TEST, &t_address()).unwrap() {
        Address::Transparent(t) => t,
        _ => unreachable!(),
    };
    let coin = WalletTransparentOutput::from_parts(
        OutPoint::new(COIN_TXID, 0),
        TxOut::new(Zatoshis::from_u64(value).unwrap(), taddr.script().into()),
        Some(BlockHeight::from_u32(COIN_HEIGHT)),
        Some(account),
        Some(TransparentKeyScope::EXTERNAL),
        None,
    )
    .unwrap();
    db.put_received_transparent_utxo(&coin).unwrap();
    drop(db);
    Wallet::open(profile).unwrap()
}

/// Soft-signs the PCZT a wallet just stored in `pending/` (handed over as
/// `view`), and finishes it without broadcasting: the raw transaction, also
/// stored in the wallet as unmined.
pub fn sign_and_finish(w: &mut Wallet, view: &[u8]) -> Vec<u8> {
    let zsig = zecz::soft_signer::sign(view, &account(), 1).unwrap();
    let rt = tokio::runtime::Runtime::new().unwrap();
    rt.block_on(w.finish(view, &zsig.to_bytes().unwrap(), false, |_| {})).unwrap().raw
}

/// The account's shield of its coin, made by `wallet_with_coin` in its own
/// directory: as another wallet with the same keys (another machine, the
/// Companion) would make it. Returns the raw transaction.
pub fn shield_made_elsewhere(dir: &std::path::Path, value: u64) -> Vec<u8> {
    let mut other = wallet_with_coin(dir, value);
    let p = other.propose_shield(false).unwrap();
    sign_and_finish(&mut other, &p.signer_view)
}

/// A payment of `amount` from the coin to the account's own t-address,
/// change to Ironwood: a send to yourself. Built with librustzcash directly,
/// since `propose_send` spends shielded notes only. Returns the raw
/// transaction, stored in `w` as unmined.
pub fn send_coin_to_self(w: Wallet, amount: u64) -> (Wallet, Vec<u8>) {
    let profile = w.profile().clone();
    drop(w);
    let mut db = db(&profile);
    let account = db.get_account_ids().unwrap()[0];
    let to = zcash_address::ZcashAddress::try_from_encoded(&t_address()).unwrap();
    let payment = zip321::Payment::without_memo(to, Zatoshis::from_u64(amount).unwrap());
    let request = zip321::TransactionRequest::new(vec![payment]).unwrap();
    let selector = GreedyInputSelector::<_>::new();
    let change = SingleOutputChangeStrategy::<_>::new(
        StandardFeeRule::Zip317,
        None,
        ShieldedPool::Ironwood,
        DustOutputPolicy::default(),
    );
    let policy = SpendPolicy::shielded_pools([]).with_transparent(TransparentSpendPolicy::any_account_addr());
    let proposal = propose_transfer::<_, _, _, _, std::convert::Infallible>(
        &mut db,
        &TEST,
        account,
        &selector,
        &change,
        request,
        ConfirmationsPolicy::default(),
        &policy,
        None,
        None,
    )
    .unwrap();
    let pczt: Pczt = create_pczt_from_proposal::<_, _, std::convert::Infallible, _, std::convert::Infallible, _>(
        &mut db,
        &TEST,
        &SystemClock,
        &mut UnwrapErr(SysRng),
        account,
        OvkPolicy::Sender,
        &proposal,
        None,
        BundlePadding::DEFAULT,
    )
    .unwrap();
    drop(db);
    let bytes = pczt.serialize().unwrap();
    // Where `Wallet::finish` looks for the full PCZT of a signer view.
    let pending = profile.pending_dir();
    std::fs::create_dir_all(&pending).unwrap();
    std::fs::write(pending.join(format!("{}.pczt", hex::encode(zecz::zsig::sha256(&bytes)))), &bytes).unwrap();
    let mut w = Wallet::open(profile).unwrap();
    let raw = sign_and_finish(&mut w, &bytes);
    (w, raw)
}
