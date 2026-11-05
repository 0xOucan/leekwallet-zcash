//! A wallet database made by the released v0.1.0 (librustzcash rev 16f0723c),
//! opened by this version: `tests/fixtures/wallet-v0.1.0`, the abandon-art
//! testnet account with one transparent coin and one stored, unmined send to
//! itself, written by v0.1.0's own `wallet_with_coin` and `send_coin_to_self`.
//! It holds a public test account's viewing key and nothing else.

mod common;

use common::*;
use zecz::{Profile, Wallet};

const SEND_EXPIRY: u32 = TIP + 1 + 40;

fn fixture(d: &TempDir) -> Profile {
    let dir = d.0.join("w");
    std::fs::create_dir_all(&dir).unwrap();
    let from = std::path::PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("tests/fixtures/wallet-v0.1.0");
    for f in ["wallet.sqlite", "network"] {
        std::fs::copy(from.join(f), dir.join(f)).unwrap();
    }
    Profile::new(TEST, dir, None)
}

fn migrations(p: &Profile) -> i64 {
    let c = rusqlite::Connection::open(p.db_path()).unwrap();
    c.query_row("SELECT count(*) FROM schemer_migrations", [], |r| r.get(0)).unwrap()
}

#[test]
fn a_v0_1_0_database_opens_and_keeps_its_wallet() {
    let d = TempDir::new("old-db");
    let profile = fixture(&d);
    let before = migrations(&profile);

    let w = Wallet::open(profile.clone()).expect("the database opens");
    assert_eq!(w.ufvk().unwrap(), spec().ufvk);
    assert_eq!(w.addresses().unwrap().transparent.as_deref(), Some(t_address().as_str()));

    // The send it stored is still there, unmined, with its old 40-block expiry.
    let h = w.history(50).unwrap();
    assert_eq!(h.len(), 2, "the coin and the send: {h:?}");
    assert!(h[0].mined_height.is_none() && !h[0].expired_unmined);
    assert_eq!(h[0].expiry_height, Some(SEND_EXPIRY));
    let txid = h[0].txid.clone();
    drop(w);

    // zcash_client_sqlite 0.23.0 has the migrations of the rev v0.1.0 used, so
    // nothing runs and v0.1.0 can still open this database. When this fails, a
    // newer crate has added one: from then on the upgrade is one-way, and
    // docs/zcash/MAINNET.md and the changelog have to say so.
    assert_eq!(migrations(&profile), before);
    Wallet::open(profile.clone()).unwrap();
    assert_eq!(migrations(&profile), before);

    // The wallet works on it: past that expiry the coin is spendable again,
    // and the new build carries the new code's fields.
    set_tip(&profile, SEND_EXPIRY + 1);
    let mut w = Wallet::open(profile).unwrap();
    assert!(w.history(50).unwrap().iter().any(|t| t.txid == txid && t.expired_unmined));
    let p = w.propose_shield(true).unwrap();
    assert_eq!(p.upgrade, "NU6.3");
    assert_eq!(p.expiry_height, SEND_EXPIRY + 2 + 40);
}
