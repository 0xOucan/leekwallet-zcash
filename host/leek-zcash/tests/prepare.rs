//! prepare_for_signer: PCZTs from builders that leave derivations out, or
//! write a zero seed fingerprint (zkool2), become ones the board accepts,
//! and the signatures over them still apply to the wallet's original.

mod common;

use common::*;
use leek_zcash::pczt::roles::redactor::Redactor;
use leek_zcash::pczt::roles::updater::Updater;
use leek_zcash::pczt::roles::verifier::Verifier;
use leek_zcash::pczt::Pczt;
use leek_zcash::{apply_zsig, preflight, prepare_for_signer, Features, LeekError, Network};

fn seed_fp() -> [u8; 32] {
    test0().seed_fingerprint
}

fn ufvk() -> String {
    test0().ufvk.unwrap()
}

/// (pubkey, seed fingerprint, path)
type Derivation = ([u8; 33], [u8; 32], Vec<u32>);

/// Each transparent input's derivations.
fn t_derivations(p: &Pczt) -> Vec<Vec<Derivation>> {
    let mut out = Vec::new();
    Verifier::new(p.clone())
        .with_transparent::<(), _>(|b| {
            for i in b.inputs() {
                out.push(
                    i.bip32_derivation()
                        .iter()
                        .map(|(pk, d)| (*pk, *d.seed_fingerprint(), d.derivation_path().iter().map(|c| c.0).collect()))
                        .collect(),
                );
            }
            Ok(())
        })
        .unwrap();
    out
}

fn strip_t(p: Pczt) -> Pczt {
    Redactor::new(p).redact_transparent_with(|mut r| r.redact_inputs(|mut i| i.clear_bip32_derivation())).finish()
}

fn strip_shielded(p: Pczt) -> Pczt {
    Redactor::new(p)
        .redact_orchard_with(|mut r| r.redact_actions(|mut a| a.clear_spend_zip32_derivation()))
        .redact_ironwood_with(|mut r| r.redact_actions(|mut a| a.clear_spend_zip32_derivation()))
        .finish()
}

fn ser(p: &Pczt) -> Vec<u8> {
    p.clone().serialize().unwrap()
}

#[test]
fn missing_transparent_derivations_are_found() {
    for name in ["t_shield", "t_mixed", "t_send_tovk"] {
        let original = parse(&v(name));
        let stripped = strip_t(original.clone());
        assert_eq!(preflight(&stripped, &test0(), &Features::default()), Err(LeekError::MissingDerivation), "{name}");
        let prepared = prepare_for_signer(stripped, &ufvk(), &seed_fp(), 0).unwrap();
        assert_eq!(t_derivations(&prepared), t_derivations(&original), "{name}");
        preflight(&prepared, &test0(), &Features::default()).unwrap();
    }
}

#[test]
fn a_zero_fingerprint_gets_ours_once_its_path_checks_out() {
    let original = parse(&v("t_shield"));
    let derivs = t_derivations(&original);
    // What zkool2 writes: the right path under a zero fingerprint.
    let zeroed = Updater::new(strip_t(original.clone()))
        .update_transparent_with(|mut u| {
            for (i, ds) in derivs.iter().enumerate() {
                for (pk, _, path) in ds {
                    let d = zcash_transparent::pczt::Bip32Derivation::parse([0; 32], path.clone()).unwrap();
                    u.update_input_with(i, |mut iu| {
                        iu.set_bip32_derivation(*pk, d);
                        Ok(())
                    })?;
                }
            }
            Ok(())
        })
        .unwrap()
        .finish();
    assert_eq!(preflight(&zeroed, &test0(), &Features::default()), Err(LeekError::MissingDerivation));
    let prepared = prepare_for_signer(zeroed, &ufvk(), &seed_fp(), 0).unwrap();
    assert_eq!(t_derivations(&prepared), derivs);
}

#[test]
fn missing_shielded_derivations_are_filled() {
    for name in ["iw_send_shielded", "orchard_to_ironwood", "t_mixed"] {
        let original = parse(&v(name));
        let stripped = strip_shielded(original.clone());
        let prepared = prepare_for_signer(stripped.clone(), &ufvk(), &seed_fp(), 0).unwrap();
        // The original vectors carry exactly the derivation prepare writes.
        assert_eq!(ser(&prepared), ser(&original), "{name}");
        // And the board's signatures over the prepared PCZT go into the
        // wallet's own, unprepared copy: derivations are not signed.
        let sent = ser(&prepared);
        let zsig = soft_sign(&sent, Network::Test);
        proves_and_extracts(apply_zsig(&stripped, &sent, &zsig).unwrap());
    }
}

#[test]
fn another_accounts_pczt_is_left_alone() {
    let stripped = strip_t(strip_shielded(parse(&v("t_mixed"))));
    let other = meta("test/zoo-vote/pp0/acct0");
    let prepared =
        prepare_for_signer(stripped.clone(), other.ufvk.as_ref().unwrap(), &other.seed_fingerprint, 0).unwrap();
    assert_eq!(ser(&prepared), ser(&stripped));
    assert!(preflight(&prepared, &other, &Features::default()).is_err());
    // A mainnet UFVK for a testnet PCZT.
    let main = meta("main/abandon-art/pp0/acct0");
    assert_eq!(
        prepare_for_signer(stripped, main.ufvk.as_ref().unwrap(), &main.seed_fingerprint, 0).unwrap_err(),
        LeekError::WrongNetwork
    );
}
