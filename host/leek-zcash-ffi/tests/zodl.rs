//! The PCZT Zodl's Android app hands a LeekWallet board, through the FFI.
//!
//! Zodl (Android SDK 6.0.0, zcash_client_backend 0.25.0) builds a hardware
//! account's transaction as it does Keystone's:
//!
//! 1. `createPcztFromProposal`: `create_pczt_from_proposal` for an account
//!    imported with `importAccountByUfvk(.., AccountPurpose.Spending(seedFp,
//!    index), keySource = "leekwallet")`, so with its ZIP 32 derivation;
//! 2. `addProofsToPczt` on a copy of that PCZT, in the background;
//! 3. `redactPcztForSigner`: `redact_pczt_for_signer(.., SignerView::Full)`,
//!    the PCZT the board signs (`KeystoneProposalRepository.createSignerPczt`);
//! 4. `createTransactionFromPczt(pcztWithProofs, pcztWithSignatures)`:
//!    combine, finalize, extract.
//!
//! zecz-core's wallet makes 1 and 3 with the same calls and the same
//! account purpose (its fixture: a testnet wallet holding one transparent
//! coin, shielded to Ironwood), so these are Zodl's bytes. They must pass
//! `preflight` (which `sign_pczt` runs and the JNI library never did),
//! sign on the simulated board in both views, and combine with the proven
//! PCZT into a transaction whose txid is `SignResult.txid`. An account
//! imported without its derivation is the case `prepare_for_signer` mends.

#[path = "../../zecz-core/tests/common/mod.rs"]
mod wallet_fixture;

use std::path::PathBuf;
use std::process::{Child, Command, Stdio};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use leek_transport_serial::SerialTransport;
use leek_zcash::pczt::roles::{combiner::Combiner, redactor::Redactor, spend_finalizer::SpendFinalizer, tx_extractor::TransactionExtractor};
use leek_zcash::pczt::Pczt;
use leek_zcash_ffi::*;
use rand::{rand_core::UnwrapErr, rngs::SysRng};

fn repo() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../..")
}

fn test0() -> AccountMeta {
    let a = wallet_fixture::account();
    AccountMeta {
        seed_fingerprint: a.seed_fingerprint.to_bytes().to_vec(),
        index: 0,
        network: Network::Test,
        ufvk: Some(wallet_fixture::spec().ufvk),
    }
}

/// What Zodl holds for a shield of a 200,000 zat coin: the PCZT from
/// `createPcztFromProposal` (proposalPczt) and its `redactPcztForSigner`
/// view (the signer PCZT).
fn zodl_shield(dir: &std::path::Path) -> (Vec<u8>, Vec<u8>) {
    let mut w = wallet_fixture::wallet_with_coin(dir, 200_000);
    let p = w.propose_shield(false).expect("propose and build the shield");
    let pending = w.profile().pending_dir().join(format!("{}.pczt", hex::encode(zecz::zsig::sha256(&p.signer_view))));
    (std::fs::read(pending).expect("the full PCZT"), p.signer_view)
}

/// `createTransactionFromPczt(pcztWithProofs, pcztWithSignatures)`.
fn create_transaction(with_proofs: &Pczt, with_signatures: &[u8]) -> [u8; 32] {
    let combined = Combiner::new(vec![with_proofs.clone(), Pczt::parse(with_signatures).unwrap()]).combine().expect("combine");
    let finalized = SpendFinalizer::new(combined).finalize_spends().expect("finalize");
    let tx = TransactionExtractor::new(finalized).extract(UnwrapErr(SysRng)).expect("proofs and signatures verify");
    *tx.txid().as_ref()
}

#[test]
fn zodl_pczts_pass_preflight_unchanged() {
    let dir = wallet_fixture::TempDir::new("ffi-zodl-preflight");
    let (full, signer) = zodl_shield(&dir.0);
    let p = Pczt::parse(&signer).unwrap();
    assert!(!p.transparent().inputs().is_empty() && !p.ironwood().actions().is_empty(), "a shield to Ironwood");
    let fp = test0().seed_fingerprint;

    preflight(signer.clone(), test0(), None).expect("the signer PCZT");
    preflight(full.clone(), test0(), None).expect("the full PCZT");
    // With nothing missing, prepare_for_signer changes nothing: a wallet can
    // always call it.
    let ufvk = test0().ufvk.unwrap();
    assert_eq!(prepare_for_signer(signer.clone(), ufvk.clone(), fp.clone(), 0).unwrap(), signer);
    // Both views of it fit the board.
    for view in [SignerView::Full, SignerView::Compact] {
        assert!(signer_view_bytes(signer.clone(), view).unwrap().len() < 512 * 1024);
    }

    // The same account imported without its derivation (AccountPurpose
    // ViewOnly, or a derivation the wallet did not record): no paths in the
    // PCZT. preflight refuses it as 405 before anything is uploaded;
    // prepare_for_signer finds the keys from the UFVK and restores exactly
    // what create_pczt_from_proposal would have written.
    let bare = Redactor::new(p)
        .redact_transparent_with(|mut r| r.redact_inputs(|mut i| i.clear_bip32_derivation()))
        .redact_orchard_with(|mut r| r.redact_actions(|mut a| a.clear_spend_zip32_derivation()))
        .redact_ironwood_with(|mut r| r.redact_actions(|mut a| a.clear_spend_zip32_derivation()))
        .finish()
        .serialize()
        .unwrap();
    // create_pczt_from_proposal had written the paths: taking them out
    // changes the bytes.
    assert_ne!(bare, signer);
    let e = preflight(bare.clone(), test0(), None).unwrap_err();
    assert!(matches!(e, LeekError::Account { code: 405, recovery: Recovery::DoNotRetry, .. }), "{e:?}");
    let prepared = prepare_for_signer(bare, ufvk, fp, 0).unwrap();
    preflight(prepared.clone(), test0(), None).expect("prepared");
    assert_eq!(prepared, signer);
}

// ---------------------------------------------------------------- zdev

struct Zdev {
    child: Child,
    link: PathBuf,
}

impl Drop for Zdev {
    fn drop(&mut self) {
        let _ = self.child.kill();
        let _ = self.child.wait();
        let _ = std::fs::remove_file(&self.link);
    }
}

fn start(tag: &str) -> Option<Zdev> {
    let sim = repo().join("firmware/sim");
    if !Command::new("make").arg("-C").arg(&sim).arg("build/zdev").status().is_ok_and(|s| s.success()) {
        return None;
    }
    let dir = std::env::temp_dir().join(format!("leek-zcash-ffi-zodl-{tag}-{}", std::process::id()));
    std::fs::create_dir_all(&dir).unwrap();
    let link = dir.join("port");
    let child = Command::new(sim.join("build/zdev"))
        .args(["--network", "test", "--yes", "--link"])
        .arg(&link)
        .stdin(Stdio::piped())
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .spawn()
        .ok()?;
    let deadline = Instant::now() + Duration::from_secs(10);
    while !link.exists() && Instant::now() < deadline {
        std::thread::sleep(Duration::from_millis(50));
    }
    Some(Zdev { child, link })
}

/// Whole frames, as Zodl's Bluetooth link (`LeekBleLink`) carries them.
struct FramesHost(Mutex<SerialTransport>);

impl FrameTransport for FramesHost {
    fn send(&self, f: Vec<u8>) -> Result<(), TransportFailure> {
        self.0.lock().unwrap().send(f[2], &f[3..]).map_err(|_| TransportFailure::Disconnected)
    }

    fn recv(&self, timeout_ms: u32) -> Result<Vec<u8>, TransportFailure> {
        match self.0.lock().unwrap().recv(Duration::from_millis(timeout_ms.into())) {
            Ok((t, p)) => {
                let mut f = ((p.len() + 1) as u16).to_be_bytes().to_vec();
                f.push(t);
                f.extend_from_slice(&p);
                Ok(f)
            }
            Err(leek_transport_serial::TransportError::Timeout) => Ok(vec![]),
            Err(_) => Err(TransportFailure::Disconnected),
        }
    }
}

struct NoCode;

impl PairingListener for NoCode {
    fn on_pairing_code(&self, _: String) {}
}

#[test]
fn zodl_pczts_sign_on_the_board_and_extract() {
    let Some(z) = start("sign") else {
        eprintln!("SKIPPED: zdev could not be built (make -C firmware/sim build/zdev)");
        return;
    };
    let dir = wallet_fixture::TempDir::new("ffi-zodl-sign");
    let (full, signer) = zodl_shield(&dir.0);
    // addProofsToPczt(proposalPczt.clonePczt()), once for both signings.
    let with_proofs = zecz::authorize::prove(Pczt::parse(&full).unwrap()).expect("prove");

    let port = SerialTransport::open(z.link.to_str().unwrap()).expect("open zdev");
    let dev = LeekDevice::new(Arc::new(FramesHost(Mutex::new(port))), LinkKind::Frames);
    dev.pair(Arc::new(NoCode)).expect("pair");
    dev.ensure_unlocked().expect("unlocked");

    // The fork's default (compact over the link), with the seed check, and
    // the full view.
    for (view, verify) in [(SignerView::Compact, true), (SignerView::Full, false)] {
        let r = dev.sign_pczt(signer.clone(), test0(), view, verify, None).unwrap_or_else(|e| panic!("{view:?}: {e}"));
        assert!(r.signature_count >= 1, "{view:?}");
        // setPcztWithSignatures(Pczt(r.signedPczt)), then submit.
        assert_eq!(create_transaction(&with_proofs, &r.signed_pczt), r.txid[..], "{view:?}: txid before proving");
        // The JNI path (compact, then applyTo) gives the same signed PCZT.
        let sent = signer_view_bytes(signer.clone(), view).unwrap();
        if view == SignerView::Compact {
            assert_eq!(leek_android::compact_view(&signer).unwrap(), sent);
        }
        assert_eq!(leek_android::apply_signatures_to(&signer, &sent, &r.zsig).unwrap(), r.signed_pczt, "{view:?}");
        assert_eq!(apply_zsig(signer.clone(), sent, r.zsig.clone()).unwrap(), r.signed_pczt, "{view:?}");
    }
}
