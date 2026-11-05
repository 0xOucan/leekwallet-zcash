//! NU7 on mainnet, without a network: the propose and build path either side
//! of the activation height, the SDK's preflight on what it builds, upstream's
//! height-dependent expiry, and a transaction signed before the activation
//! that the chain has moved past.

mod common;

use common::*;
use pczt::Pczt;
use zcash_primitives::transaction::{Transaction, TxVersion};
use zcash_protocol::consensus::{BlockHeight, BranchId, NetworkUpgrade, Parameters, MAIN_NETWORK};
use zecz::keys::DevAccount;
use zecz::wallet::{check_broadcastable, check_pczt_broadcastable, BroadcastRefused};
use zecz::{Network, Wallet};

const MAIN: Network = Network::MainNetwork;
const NU7: u32 = 3_543_405;
const NU6_3_BRANCH: u32 = 0x37a5_165b;
const NU7_BRANCH: u32 = 0x7719_0ad9;

fn main_wallet(dir: &TempDir, tip: u32) -> Wallet {
    wallet_with_coin_at(&dir.0, 500_000, MAIN, tip)
}

fn sign_and_finish_main(w: &mut Wallet, view: &[u8]) -> Vec<u8> {
    let acct = DevAccount::from_mnemonic(&MAIN, ABANDON_ART, "", 0).unwrap();
    let zsig = zecz::soft_signer::sign(view, &acct, 133).unwrap();
    let rt = tokio::runtime::Runtime::new().unwrap();
    rt.block_on(w.finish(view, &zsig.to_bytes().unwrap(), false, |_| {})).unwrap().raw
}

#[test]
fn the_activation_height_is_the_one_upstream_set() {
    assert_eq!(MAIN_NETWORK.activation_height(NetworkUpgrade::Nu7), Some(BlockHeight::from_u32(NU7)));
    assert_eq!(u32::from(BranchId::for_height(&MAIN, BlockHeight::from_u32(NU7 - 1))), NU6_3_BRANCH);
    assert_eq!(u32::from(BranchId::for_height(&MAIN, BlockHeight::from_u32(NU7))), NU7_BRANCH);
}

/// The branch comes from the target height, the chain tip plus one, through
/// propose and build; the SDK's preflight accepts both on mainnet; the expiry
/// is upstream's for the branch, never a fixed 40 blocks.
#[test]
fn propose_selects_the_branch_and_expiry_of_the_target_height() {
    let main = {
        let acct = DevAccount::from_mnemonic(&MAIN, ABANDON_ART, "", 0).unwrap();
        leek_zcash::AccountMeta {
            seed_fingerprint: acct.seed_fingerprint.to_bytes(),
            index: 0,
            network: leek_zcash::Network::Main,
            ufvk: Some(acct.ufvk.encode(&MAIN).unwrap()),
        }
    };
    for (tip, branch, delta, upgrade, secs) in [
        (NU7 - 2, NU6_3_BRANCH, 40, "NU6.3", 40 * 25),
        (NU7 - 1, NU7_BRANCH, 120, "NU7", 120 * 25),
        (NU7, NU7_BRANCH, 120, "NU7", 120 * 25),
        (NU7 + 100_000, NU7_BRANCH, 120, "NU7", 120 * 25),
    ] {
        let d = TempDir::new("nu7");
        let mut w = main_wallet(&d, tip);
        let p = w.propose_shield(true).unwrap();
        assert_eq!(p.target_height, tip + 1);
        assert_eq!(p.upgrade, upgrade, "tip {tip}");
        assert_eq!(p.expiry_height, tip + 1 + delta, "tip {tip}");
        assert_eq!(p.expires_in_secs, secs, "tip {tip}");

        let pczt = Pczt::parse(&p.signer_view).unwrap();
        let g = pczt.global();
        assert_eq!(*g.consensus_branch_id(), branch, "tip {tip}");
        assert_eq!(*g.expiry_height(), p.expiry_height, "tip {tip}");
        assert_eq!(*g.tx_version(), 6, "tip {tip}: nothing we build is v4");
        assert_eq!(*g.coin_type(), 133);

        leek_zcash::preflight(&leek_zcash::pczt::Pczt::parse(&p.signer_view).unwrap(), &main, &leek_zcash::Features::default())
            .unwrap_or_else(|e| panic!("preflight at tip {tip}: {e}"));
        check_pczt_broadcastable(MAIN, &pczt, tip).unwrap();
    }
}

#[test]
fn v4_is_not_valid_under_nu7() {
    assert_eq!(TxVersion::suggested_for_branch(BranchId::Nu7), TxVersion::V6);
    assert!(!TxVersion::V4.valid_in_branch(BranchId::Nu7));
    assert!(TxVersion::V6.valid_in_branch(BranchId::Nu7));
}

#[test]
fn the_broadcast_check_tells_old_rules_from_a_lagging_server_and_expiry() {
    let (old, new) = (BranchId::Nu6_3, BranchId::Nu7);
    check_broadcastable(MAIN, old, NU7 + 39, NU7 - 2).unwrap();
    check_broadcastable(MAIN, new, NU7 + 120, NU7 - 1).unwrap();
    // Zero is "never expires".
    check_broadcastable(MAIN, new, 0, NU7 + 5).unwrap();

    let e = check_broadcastable(MAIN, old, NU7 + 39, NU7 - 1).unwrap_err();
    assert_eq!(e, BroadcastRefused::OldBranch { signed: old, chain: new, tip: NU7 - 1, expiry_height: NU7 + 39 });
    let text = e.to_string();
    assert!(text.contains("signed for NU6.3") && text.contains("moved to NU7"), "{text}");
    assert!(text.contains("not sent") && text.contains("sign it on the board"), "{text}");

    // A server behind the wallet is not the same advice.
    let e = check_broadcastable(MAIN, new, NU7 + 120, NU7 - 2).unwrap_err();
    assert_eq!(e, BroadcastRefused::ServerBehind { signed: new, chain: old, tip: NU7 - 2 });
    let text = e.to_string();
    assert!(text.contains("still under NU6.3") && text.contains("another server"), "{text}");

    assert_eq!(
        check_broadcastable(MAIN, new, NU7 + 120, NU7 + 120).unwrap_err(),
        BroadcastRefused::Expired { tip: NU7 + 120, expiry_height: NU7 + 120 }
    );
    check_broadcastable(MAIN, new, NU7 + 120, NU7 + 119).unwrap();
}

/// A shield signed and stored just before the activation, then the chain moves
/// on: every check refuses it with the same clear answer, it stays pending
/// until its expiry height, and after that the coin is spendable again and a
/// new shield is built under NU7.
#[test]
fn a_transaction_signed_before_nu7_expires_and_its_coin_comes_back() {
    let d = TempDir::new("nu7-stale");
    let tip = NU7 - 10;
    let mut w = main_wallet(&d, tip);
    let profile = w.profile().clone();

    let p = w.propose_shield(true).unwrap();
    assert_eq!(p.upgrade, "NU6.3");
    let expiry = p.expiry_height;
    assert_eq!(expiry, tip + 1 + 40);
    let raw = sign_and_finish_main(&mut w, &p.signer_view);

    let tx = Transaction::read(&raw[..], BranchId::Nu6_3).unwrap();
    assert_eq!(tx.version(), TxVersion::V6);
    assert_eq!(u32::from(tx.consensus_branch_id()), NU6_3_BRANCH);
    let txid = tx.txid().to_string();

    // Good until the chain's next block is NU7's first.
    assert!(check_broadcastable(MAIN, tx.consensus_branch_id(), expiry, NU7 - 2).is_ok());
    let past = NU7 + 3;
    let refused = check_broadcastable(MAIN, tx.consensus_branch_id(), expiry, past).unwrap_err();
    assert!(matches!(refused, BroadcastRefused::OldBranch { .. }));
    assert!(refused.to_string().contains(&format!("free again after block {expiry}")), "{refused}");
    // The SDK answers the same over the raw bytes, and a retry cannot change it.
    for _ in 0..3 {
        assert_eq!(leek_zcash::check_broadcast(&raw, leek_zcash::Network::Main, past).unwrap_err(), leek_zcash::LeekError::WrongBranch);
    }

    let record = |w: &Wallet| w.history(50).unwrap().into_iter().find(|t| t.txid == txid).expect("in the history");
    // Stored and not mined: pending, and the coin is held, below its expiry.
    let t = record(&w);
    assert!(t.mined_height.is_none() && !t.expired_unmined, "{t:?}");
    assert_eq!(t.expiry_height, Some(expiry));
    drop(w);

    // Scanned past it: expired, the coin back, and a new shield under NU7.
    let tip = expiry + 1;
    set_tip(&profile, tip);
    let mut w = Wallet::open(profile).unwrap();
    assert!(record(&w).expired_unmined);
    let again = w.propose_shield(true).unwrap();
    assert_eq!(again.upgrade, "NU7");
    assert_eq!(again.expiry_height, tip + 1 + 120);
    check_pczt_broadcastable(MAIN, &Pczt::parse(&again.signer_view).unwrap(), tip).unwrap();
}

/// A wallet that stopped syncing below the activation builds for the old
/// branch. The check against the server's tip catches it before the board.
#[test]
fn a_view_built_from_a_stale_tip_is_caught() {
    let d = TempDir::new("nu7-stale-tip");
    let mut w = main_wallet(&d, NU7 - 5);
    let p = w.propose_shield(true).unwrap();
    assert_eq!(p.upgrade, "NU6.3");
    let pczt = Pczt::parse(&p.signer_view).unwrap();
    let e = check_pczt_broadcastable(MAIN, &pczt, NU7 + 1).unwrap_err();
    assert!(matches!(e.downcast_ref::<BroadcastRefused>(), Some(BroadcastRefused::OldBranch { .. })), "{e:#}");
}
