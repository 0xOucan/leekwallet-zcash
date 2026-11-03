//! preflight: what the board would refuse is refused on the host first,
//! with the precise error; what it signs passes.

mod common;

use common::*;
use leek_zcash::{preflight, Features, LeekError, Unsupported};

fn check(name: &str, account: &leek_zcash::AccountMeta) -> Result<(), LeekError> {
    preflight(&parse(&v(name)), account, &Features::default())
}

#[test]
fn every_vector_the_board_signs_passes() {
    let test = test0();
    let main = meta("main/abandon-art/pp0/acct0");
    for name in [
        "iw_send_shielded", "iw_send_transparent", "iw_send_nu7", "orchard_to_ironwood", "iw_send_shielded_compact",
        "iw_send_transparent_compact", "iw_send_nu7_compact", "orchard_to_ironwood_compact", "t_shield", "t_mixed",
        "t_send_tovk", "t_shield_compact", "t_mixed_compact",
    ] {
        check(name, &test).unwrap_or_else(|e| panic!("{name}: {e}"));
        // Without the UFVK (only the fingerprint and index) too.
        let bare = leek_zcash::AccountMeta { ufvk: None, ..test.clone() };
        check(name, &bare).unwrap_or_else(|e| panic!("{name} without UFVK: {e}"));
    }
    for name in ["main_iw_send_shielded", "main_iw_send_shielded_compact", "main_t_shield", "main_t_shield_compact"] {
        check(name, &main).unwrap_or_else(|e| panic!("{name}: {e}"));
    }
}

#[test]
fn the_other_network_is_refused() {
    let main = meta("main/abandon-art/pp0/acct0");
    assert_eq!(check("r_main_on_test", &test0()), Err(LeekError::WrongNetwork));
    assert_eq!(check("r_main_shield_on_test", &test0()), Err(LeekError::WrongNetwork));
    assert_eq!(check("r_test_on_main", &main), Err(LeekError::WrongNetwork));
    assert_eq!(check("r_test_shield_on_main", &main), Err(LeekError::WrongNetwork));
    // A mainnet UFVK given for a testnet account.
    let mixed = leek_zcash::AccountMeta { ufvk: main.ufvk.clone(), ..test0() };
    assert_eq!(check("iw_send_shielded", &mixed), Err(LeekError::WrongNetwork));
}

#[test]
fn branches_and_versions_outside_the_board_are_refused() {
    assert_eq!(check("r_old_branch", &test0()), Err(LeekError::Unsupported(Unsupported::Branch)));
    let e = check("r_v1_nu6_2", &test0()).unwrap_err();
    assert!(
        matches!(e, LeekError::Unsupported(Unsupported::Branch | Unsupported::Version)),
        "{e:?}"
    );
    let only_v5 = Features { tx_versions: vec![5], ..Features::default() };
    assert_eq!(
        preflight(&parse(&v("iw_send_shielded")), &test0(), &only_v5),
        Err(LeekError::Unsupported(Unsupported::Version))
    );
    let no_nu7 = Features { branch_ids: vec![leek_zcash::KNOWN_BRANCH_IDS[0]], ..Features::default() };
    assert_eq!(
        preflight(&parse(&v("iw_send_nu7")), &test0(), &no_nu7),
        Err(LeekError::Unsupported(Unsupported::Branch))
    );
}

#[test]
fn someone_elses_spends_are_refused() {
    let t = test0();
    // Built by the abandon-art wallet, sent to a board holding zoo-vote.
    let zoo = meta("test/zoo-vote/pp0/acct0");
    assert_eq!(check("r_foreign_spend", &zoo), Err(LeekError::WrongAccount));
    assert_eq!(check("r_t_foreign_seed", &zoo), Err(LeekError::WrongAccount));
    // And caught by the keys alone, when the derivations are not there.
    let bare = |name: &str| {
        let p = leek_zcash::pczt::roles::redactor::Redactor::new(parse(&v(name)))
            .redact_ironwood_with(|mut r| r.redact_actions(|mut a| a.clear_spend_zip32_derivation()))
            .finish();
        preflight(&p, &zoo, &Features::default())
    };
    assert_eq!(bare("r_foreign_spend"), Err(LeekError::SpendNotOurs));
    // Transparent inputs: the board's own refusals, each typed.
    assert_eq!(check("r_t_missing_derivation", &t), Err(LeekError::MissingDerivation));
    assert_eq!(check("r_t_wrong_account", &t), Err(LeekError::WrongAccount));
    assert_eq!(check("r_t_wrong_path", &t), Err(LeekError::SpendNotOurs));
    assert_eq!(check("r_t_input_not_ours", &t), Err(LeekError::SpendNotOurs));
    assert_eq!(check("r_t_sighash_none", &t), Err(LeekError::Unsupported(Unsupported::Pool)));
}

#[test]
fn a_different_seed_or_account_on_the_board_is_caught_before_signing() {
    // The same PCZT for the board's account 1, or a board holding another
    // seed: the derivations name a different account.
    for other in ["test/abandon-art/pp0/acct1", "test/zoo-vote/pp0/acct0", "test/abandon-art/pp1/acct0"] {
        for name in ["iw_send_shielded", "t_shield", "t_mixed"] {
            let e = check(name, &meta(other)).unwrap_err();
            assert!(matches!(e, LeekError::WrongAccount | LeekError::SpendNotOurs), "{other} {name}: {e:?}");
        }
    }
}

#[test]
fn limits_are_the_boards() {
    let few = Features { max_transparent_inputs: 1, ..Features::default() };
    assert_eq!(preflight(&parse(&v("t_shield")), &test0(), &few), Err(LeekError::TooMany));
    let tiny = Features { max_pczt_bytes: 1000, ..Features::default() };
    assert_eq!(preflight(&parse(&v("iw_send_shielded")), &test0(), &tiny), Err(LeekError::TooLarge));
    let no_t = Features { spend_pools: vec!["orchard".into(), "ironwood".into()], ..Features::default() };
    assert_eq!(
        preflight(&parse(&v("t_shield")), &test0(), &no_t),
        Err(LeekError::Unsupported(Unsupported::Pool))
    );
}
