//! The transaction history, without a network.
//!
//! The history comes from a real transaction: vector `iw_send_shielded`
//! (abandon-art account 0 pays 0.004 TAZ with a memo to zoo-vote's
//! Orchard-only address, change back to itself), soft-signed, proven and
//! extracted here, then stored in watch-only wallets of both sides.

use std::path::PathBuf;
use std::sync::atomic::{AtomicU32, Ordering};

use pczt::Pczt;
use rand::{rand_core::UnwrapErr, rngs::SysRng};
use serde_json::Value;
use zcash_client_backend::data_api::chain::ChainState;
use zcash_primitives::block::BlockHash;
use zcash_protocol::consensus::BlockHeight;
use zecz::authorize::{apply_zsig, prove};
use zecz::keys::DevAccount;
use zecz::wallet::{AccountBirthday, AccountSpec, TxKind};
use zecz::{Network, Profile, Wallet};

const TEST: Network = Network::TestNetwork;
const TEST_NU5: u32 = 1_842_420;
/// A height past testnet NU6.3 (4,134,000), the vector's branch.
const MINED: u32 = 4_200_000;
const ABANDON_ART: &str = "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon \
                           abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon \
                           abandon art";

struct TempDir(PathBuf);

impl TempDir {
    fn new() -> Self {
        static N: AtomicU32 = AtomicU32::new(0);
        let p = std::env::temp_dir().join(format!("zecz-history-test-{}-{}", std::process::id(), N.fetch_add(1, Ordering::SeqCst)));
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

fn repo() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../..")
}

fn key_vector(name: &str) -> Value {
    let all: Vec<Value> = serde_json::from_slice(&std::fs::read(repo().join("vectors/keys.json")).unwrap()).unwrap();
    all.into_iter().find(|v| v["name"] == name).expect("key vector")
}

fn offline_birthday() -> AccountBirthday {
    AccountBirthday::from_parts(ChainState::empty(BlockHeight::from_u32(TEST_NU5 - 1), BlockHash([0; 32])), None)
}

fn wallet(d: &TempDir, name: &str, spec: &AccountSpec) -> Wallet {
    Wallet::init_with_birthday(Profile::new(TEST, d.0.join(name), None), spec, &offline_birthday()).unwrap().0
}

/// The vector's transaction, signed by the soft signer, proven, extracted.
fn iw_send_shielded_tx() -> Vec<u8> {
    let unsigned = std::fs::read(repo().join("vectors/pczt/iw_send_shielded.pczt")).unwrap();
    let acct = DevAccount::from_mnemonic(&TEST, ABANDON_ART, "", 0).unwrap();
    let zsig = zecz::soft_signer::sign(&unsigned, &acct, 1).unwrap();
    let signed = apply_zsig(Pczt::parse(&unsigned).unwrap(), &zsig).unwrap();
    let proven = prove(signed).unwrap();
    let finalized = pczt::roles::spend_finalizer::SpendFinalizer::new(proven).finalize_spends().unwrap();
    let tx = pczt::roles::tx_extractor::TransactionExtractor::new(finalized).extract(UnwrapErr(SysRng)).unwrap();
    let mut raw = Vec::new();
    tx.write(&mut raw).unwrap();
    raw
}

#[test]
fn history_of_both_sides_of_a_send() {
    let d = TempDir::new();
    let raw = iw_send_shielded_tx();
    let zoo = key_vector("test/zoo-vote/pp0/acct0");
    let abandon = key_vector("test/abandon-art/pp0/acct0");

    // The payee: a received payment with its memo.
    let mut payee = wallet(&d, "payee", &AccountSpec::view_only(zoo["ufvk"].as_str().unwrap().into(), "zoo"));
    assert!(payee.history(50).unwrap().is_empty());
    let txid = payee.store_transaction(&raw, Some(MINED)).unwrap();
    let h = payee.history(50).unwrap();
    assert_eq!(h.len(), 1, "{h:?}");
    let r = &h[0];
    assert_eq!(r.txid, txid);
    assert_eq!(r.mined_height, Some(MINED));
    assert_eq!(r.kind, TxKind::Received);
    assert_eq!((r.amount_zat, r.delta_zat), (400_000, 400_000));
    assert_eq!(r.memos, vec!["LeekWallet Zcash test".to_string()]);
    // A fully shielded transaction states its fee (the value balance).
    assert_eq!(r.fee_zat, Some(10_000));
    assert!(r.recipients.is_empty());

    // The payer's wallet, which never saw the spent note: it recovers the
    // payment with its outgoing viewing key and finds its change.
    let mut payer = wallet(&d, "payer", &AccountSpec::view_only(abandon["ufvk"].as_str().unwrap().into(), "abandon"));
    payer.store_transaction(&raw, Some(MINED)).unwrap();
    let h = payer.history(50).unwrap();
    assert_eq!(h.len(), 1, "{h:?}");
    assert_eq!(h[0].txid, txid);
    assert_eq!(h[0].recipients.len(), 1, "{h:?}");
    assert_eq!(h[0].recipients[0].value_zat, 400_000);
    assert_eq!(h[0].recipients[0].address, zoo["ua_orchard_only"].as_str().unwrap());
    assert_eq!(h[0].memos, vec!["LeekWallet Zcash test".to_string()]);

    // Limits apply; storing again changes nothing.
    payee.store_transaction(&raw, Some(MINED)).unwrap();
    assert_eq!(payee.history(50).unwrap().len(), 1);
    assert!(payee.history(0).unwrap().is_empty());
    assert!(payee.store_transaction(b"junk", Some(MINED)).is_err());
}

mod common;

/// A send to the account's own t-address: nothing went to anyone else, so
/// the line is "Sent to yourself" for the fee it cost, not "sent 0".
#[test]
fn a_send_to_yourself_shows_what_it_cost() {
    let d = common::TempDir::new("history-self");
    let w = common::wallet_with_coin(&d.0.join("w"), 1_000_000);
    let (mut w, raw) = common::send_coin_to_self(w, 300_000);

    let check = |w: &Wallet, mined: Option<u32>| {
        let h = w.history(10).unwrap();
        let r = h.iter().find(|r| r.kind != TxKind::Received).unwrap_or_else(|| panic!("{h:?}"));
        assert_eq!(r.kind, TxKind::SentToSelf, "{r:?}");
        assert_eq!(r.mined_height, mined);
        let fee = r.fee_zat.expect("the wallet knows its own fee");
        assert!(fee > 0);
        assert_eq!((r.delta_zat, r.amount_zat), (-(fee as i64), fee), "{r:?}");
        assert!(r.recipients.is_empty(), "{r:?}");
        let line = r.summary_line();
        assert!(line.starts_with("Sent to yourself"), "{line}");
        assert!(line.contains(&format!("-{}", zecz::wallet::format_zec(fee))), "{line}");
        assert!(!line.contains("-0.00000000"), "{line}");
    };
    check(&w, None);
    // And once mined, which is where "-0.00000000 TAZ, Fee 0.0002" was seen.
    w.store_transaction(&raw, Some(common::TIP - 5)).unwrap();
    check(&w, Some(common::TIP - 5));
}
