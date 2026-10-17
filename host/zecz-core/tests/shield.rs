//! `Wallet::propose_shield` without a network: a wallet with a chain tip
//! and one transparent coin on the account's t-address, put straight into
//! its database, as a sync would.

use std::path::PathBuf;
use std::sync::atomic::{AtomicU32, Ordering};

use rand::{rand_core::UnwrapErr, rngs::SysRng};
use zcash_client_backend::data_api::chain::{scan_cached_blocks, BlockCache, ChainState};
use zcash_client_backend::proto::compact_formats::{ChainMetadata, CompactBlock};
use zecz::lwd::MemBlockCache;
use zcash_client_backend::data_api::{WalletRead, WalletWrite};
use zcash_client_backend::util::SystemClock;
use zcash_client_backend::wallet::WalletTransparentOutput;
use zcash_client_sqlite::WalletDb;
use zcash_keys::keys::{ReceiverRequirement::*, UnifiedAddressRequest, UnifiedFullViewingKey};
use zcash_primitives::block::BlockHash;
use zcash_protocol::consensus::BlockHeight;
use zcash_protocol::value::Zatoshis;
use zcash_transparent::bundle::{OutPoint, TxOut};
use zcash_transparent::keys::TransparentKeyScope;
use zecz::keys::DevAccount;
use zecz::wallet::{AccountBirthday, AccountSpec};
use zecz::{Network, Profile, Wallet};

const TEST: Network = Network::TestNetwork;
const TEST_NU5: u32 = 1_842_420;
/// Past testnet NU6.3 (4,134,000): shielding goes to Ironwood.
const TIP: u32 = 4_200_000;
const ABANDON_ART: &str = "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon \
                           abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon \
                           abandon art";

struct TempDir(PathBuf);

impl TempDir {
    fn new() -> Self {
        static N: AtomicU32 = AtomicU32::new(0);
        let p = std::env::temp_dir().join(format!("zecz-shield-test-{}-{}", std::process::id(), N.fetch_add(1, Ordering::SeqCst)));
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

/// A device-account wallet (abandon-art account 0) whose t-address holds
/// one coin of `value` zat, mined 100 blocks below the tip.
fn wallet_with_coin(d: &TempDir, value: u64) -> Wallet {
    let acct = DevAccount::from_mnemonic(&TEST, ABANDON_ART, "", 0).unwrap();
    let spec = AccountSpec::from_dev_account(TEST, &acct, "leekwallet-dev").unwrap();
    let birthday =
        AccountBirthday::from_parts(ChainState::empty(BlockHeight::from_u32(TEST_NU5 - 1), BlockHash([0; 32])), None);
    let profile = Profile::new(TEST, d.0.join("w"), None);
    let (w, _) = Wallet::init_with_birthday(profile.clone(), &spec, &birthday).unwrap();
    drop(w);

    let mut db = WalletDb::for_path(profile.db_path(), TEST, SystemClock, UnwrapErr(SysRng)).unwrap();
    db.update_chain_tip(BlockHeight::from_u32(TIP)).unwrap();
    // One empty block scanned well below the tip: the note commitment trees
    // get a checkpoint, so the wallet has an anchor and a target height.
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
    let ufvk = UnifiedFullViewingKey::decode(&TEST, &spec.ufvk).unwrap();
    let (ua, _) = ufvk.default_address(UnifiedAddressRequest::custom(Require, Omit, Require).unwrap()).unwrap();
    let taddr = *ua.transparent().unwrap();
    let coin = WalletTransparentOutput::from_parts(
        OutPoint::new([7; 32], 0),
        TxOut::new(Zatoshis::from_u64(value).unwrap(), taddr.script().into()),
        Some(BlockHeight::from_u32(TIP - 100)),
        Some(account),
        Some(TransparentKeyScope::EXTERNAL),
        None,
    )
    .unwrap();
    db.put_received_transparent_utxo(&coin).unwrap();
    drop(db);
    Wallet::open(profile).unwrap()
}

fn err_text<T>(r: anyhow::Result<T>) -> String {
    match r {
        Ok(_) => panic!("expected an error"),
        Err(e) => format!("{e:#}"),
    }
}

#[test]
fn a_shield_the_fee_would_swallow_is_refused() {
    let d = TempDir::new();
    // The ZIP 317 fee of one coin into the shielded pool is 15,000 zat: a
    // coin of exactly that would leave nothing to shield.
    let mut w = wallet_with_coin(&d, 15_000);
    assert_eq!(w.balances().unwrap().account.unwrap().transparent.total, 15_000);
    let e = err_text(w.propose_shield(true));
    assert!(e.contains("shielding would move nothing"), "{e}");
    assert!(e.contains("Receive more on the t-address"), "{e}");
    // Nothing was built.
    assert!(!w.profile().pending_dir().exists());
}

#[test]
fn a_shield_short_of_the_fee_is_refused() {
    let d = TempDir::new();
    let mut w = wallet_with_coin(&d, 12_000);
    let e = err_text(w.propose_shield(true));
    assert!(e.contains("nothing to shield") && e.contains("Insufficient funds"), "{e}");
    assert!(!w.profile().pending_dir().exists());
}

#[test]
fn a_shield_that_moves_value_is_built() {
    let d = TempDir::new();
    let mut w = wallet_with_coin(&d, 1_000_000);
    let p = w.propose_shield(true).unwrap();
    assert!(p.payments.is_empty());
    assert_eq!(p.change.len(), 1);
    assert_eq!(p.change[0].pool, "Ironwood");
    assert_eq!(p.change[0].value_zat + p.fee_zat, 1_000_000);
    assert!(p.compact && p.pending_path.exists());
}

#[test]
fn a_shield_below_the_threshold_is_refused() {
    let d = TempDir::new();
    // Below the 10,000-zat shielding threshold.
    let mut w = wallet_with_coin(&d, 5_000);
    let e = err_text(w.propose_shield(true));
    assert!(e.contains("nothing to shield"), "{e}");
}
