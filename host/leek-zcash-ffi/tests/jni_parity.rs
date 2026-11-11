//! The JNI library (`leek-android`) against the FFI, call for call, on every
//! valid vector: what the Zodl fork did with `PcztSignatures.compact`,
//! `apply` and `applyTo`, `signer_view_bytes` and `apply_zsig` do byte for
//! byte, and what the JNI refused the FFI refuses with a typed code. The
//! `.zsig` files come from zecz's soft signer with the vectors' seed, as
//! the board would sign them.

use std::path::PathBuf;

use leek_zcash_ffi::*;
use serde_json::Value;

const SENDER: &str = "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon \
                      abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon \
                      abandon art";

fn repo() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../..")
}

fn v(name: &str) -> Vec<u8> {
    std::fs::read(repo().join("vectors/pczt").join(format!("{name}.pczt"))).unwrap()
}

/// The valid vectors with their network, compact views left out (each is
/// made again from its full PCZT here).
fn valid() -> Vec<(String, bool)> {
    let all: Vec<Value> = serde_json::from_slice(&std::fs::read(repo().join("vectors/pczt.json")).unwrap()).unwrap();
    all.iter()
        .filter(|e| e["expect"] == "ok")
        .map(|e| (e["name"].as_str().unwrap().to_string(), e["network"] == "main"))
        .filter(|(n, _)| !n.ends_with("_compact"))
        .collect()
}

fn soft_sign(sent: &[u8], main: bool) -> Option<Vec<u8>> {
    use zcash_protocol::consensus::{MAIN_NETWORK, TEST_NETWORK};
    let (acct, coin) = if main {
        (zecz::keys::DevAccount::from_mnemonic(&MAIN_NETWORK, SENDER, "", 0).unwrap(), 133)
    } else {
        (zecz::keys::DevAccount::from_mnemonic(&TEST_NETWORK, SENDER, "", 0).unwrap(), 1)
    };
    zecz::soft_signer::sign(sent, &acct, coin).ok()?.to_bytes().ok()
}

#[test]
fn the_compact_view_is_the_jni_one() {
    let vs = valid();
    assert!(vs.len() >= 10, "{vs:?}");
    for (name, _) in &vs {
        let full = v(name);
        let jni = leek_android::compact_view(&full).unwrap();
        assert_eq!(signer_view_bytes(full.clone(), SignerView::Compact).unwrap(), jni, "{name}");
        // Full: the PCZT as the wallet redacted it, as the JNI sent it.
        let f = signer_view_bytes(full.clone(), SignerView::Full).unwrap();
        assert_eq!(signer_view_bytes(f.clone(), SignerView::Full).unwrap(), f, "{name}: idempotent");
    }
}

#[test]
fn signatures_apply_as_the_jni_applied_them() {
    let mut signed = 0;
    for (name, main) in valid() {
        let full = v(&name);
        let compact = signer_view_bytes(full.clone(), SignerView::Compact).unwrap();
        for sent in [full.clone(), compact] {
            // Vectors signed by another seed than the soft signer's are out
            // of scope here (zdev covers them).
            let Some(zsig) = soft_sign(&sent, main) else { continue };
            let jni = leek_android::apply_signatures_to(&full, &sent, &zsig);
            let ffi = apply_zsig(full.clone(), sent.clone(), zsig.clone());
            match (jni, ffi) {
                (Ok(j), Ok(f)) => assert_eq!(f, j, "{name}"),
                (j, f) => panic!("{name}: JNI {:?}, FFI {:?}", j.err(), f.err()),
            }
            if sent == full {
                assert_eq!(
                    leek_android::apply_signatures(&full, &zsig).unwrap(),
                    apply_zsig(full.clone(), full.clone(), zsig).unwrap(),
                    "{name}: apply(pczt, zsig)"
                );
            }
            signed += 1;
        }
    }
    assert!(signed >= 10, "{signed} signings compared");
}

#[test]
fn what_the_jni_refused_the_ffi_refuses() {
    let full = v("iw_send_shielded");
    let zsig = std::fs::read(repo().join("vectors/zsig/iw_send_shielded.zsig")).unwrap();
    let code = |r: Result<Vec<u8>, LeekError>| r.unwrap_err().code();

    // Signatures for other bytes.
    let mut other = full.clone();
    *other.last_mut().unwrap() ^= 1;
    assert!(leek_android::apply_signatures(&other, &zsig).is_err());
    assert_eq!(code(apply_zsig(other.clone(), other, zsig.clone())), 600);

    // A corrupted signature, and one moved to another action.
    let mut bad = zsig.clone();
    let n = bad.len();
    bad[n - 10] ^= 1;
    assert!(leek_android::apply_signatures(&full, &bad).is_err());
    assert_eq!(code(apply_zsig(full.clone(), full.clone(), bad)), 602);
    let mut moved = zsig.clone();
    moved[40] = 1;
    assert!(leek_android::apply_signatures(&full, &moved).is_err());
    assert!(apply_zsig(full.clone(), full.clone(), moved).is_err());

    // Truncated.
    assert!(leek_android::apply_signatures(&full, b"ZSIG").is_err());
    assert_eq!(code(apply_zsig(full.clone(), full.clone(), b"ZSIG".to_vec())), 601);

    // The board signed another transaction than the wallet's: a changed
    // memo it showed and the user accepted.
    let changed = v("iw_send_shielded_compact_memo_changed");
    let z = soft_sign(&changed, false).unwrap();
    assert!(leek_android::apply_signatures_to(&full, &changed, &z).is_err());
    assert_eq!(code(apply_zsig(full.clone(), changed, z.clone())), 604);
    // Named as the honest view, the hash check refuses it.
    let honest = signer_view_bytes(full.clone(), SignerView::Compact).unwrap();
    assert!(leek_android::apply_signatures_to(&full, &honest, &z).is_err());
    assert_eq!(code(apply_zsig(full, honest, z)), 600);
}
