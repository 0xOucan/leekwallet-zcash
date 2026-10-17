//! The one step the Zodl Android SDK cannot do for a LeekWallet device.
//!
//! The device returns spend-authorization signatures only (a `.zsig`,
//! docs/zcash/FORMATS.md), and, for transparent inputs (version 2), the
//! ECDSA signature and pubkey of each. The SDK combines a *signed PCZT* with the proven
//! one (`createTransactionFromPczt`), as Keystone and Ledger hand it one, but
//! has no call to insert raw signatures. This does it, with the `pczt`
//! crate's Signer role - which checks each signature against the action's
//! rk before storing it - exactly as `zecz finish` does on the PC.
//!
//! The `.zsig` names the SHA-256 of the PCZT the device checked; signatures
//! are only applied to those exact bytes, or, when the device was sent the
//! compact view, to the full PCZT that view was made from.

pub mod device;

use leek_device::zsig::{Pool, Zsig};
use pczt::roles::signer::Signer;
use pczt::Pczt;
use sha2::{Digest, Sha256};

const MAGIC: &[u8; 4] = b"ZSIG";
const HEADER: usize = 4 + 1 + 32 + 2;

/// The proprietary fields `zcash_client_backend` adds for itself; no signer
/// needs them (the same names its `redact_pczt_for_signer` removes).
const PROPRIETARY_PROPOSAL_INFO: &str = "zcash_client_backend:proposal_info";
const PROPRIETARY_OUTPUT_INFO: &str = "zcash_client_backend:output_info";

/// The compact signer view of `pczt` (v2 encoding), as
/// `zcash_client_backend`'s `redact_pczt_for_signer(.., SignerView::Compact)`
/// builds it: no proofs, no binding-signature keys, no v6 anchors, and the
/// Orchard-protocol fields the signer can rebuild (cv_net, cmx, and a memo
/// ciphertext replaced by its plaintext) left out. About half the bytes, so
/// half the QR frames. Keep `pczt` and apply the signatures to it with
/// [`apply_signatures_to`].
pub fn compact_view(pczt: &[u8]) -> Result<Vec<u8>, String> {
    use pczt::roles::redactor::{orchard::OrchardRedactor, Redactor};
    let pczt = Pczt::parse(pczt).map_err(|e| format!("PCZT: {e:?}"))?;
    let v6 = *pczt.global().tx_version() == zcash_protocol::constants::V6_TX_VERSION;

    fn redact_bundle(mut r: OrchardRedactor<'_>, v6: bool) {
        r.clear_zkproof();
        r.clear_bsk();
        r.redact_actions(|mut a| {
            a.clear_spend_witness();
            a.clear_spend_dummy_sk();
            a.redact_output_proprietary(PROPRIETARY_OUTPUT_INFO);
        });
        r.compact_resolvable_fields();
        if v6 {
            r.clear_anchor();
        }
    }

    Redactor::new(pczt)
        .redact_global_with(|mut r| r.redact_proprietary(PROPRIETARY_PROPOSAL_INFO))
        .redact_transparent_with(|mut r| {
            r.redact_outputs(|mut o| o.redact_proprietary(PROPRIETARY_OUTPUT_INFO));
        })
        .redact_sapling_with(|mut r| {
            r.clear_bsk();
            r.redact_spends(|mut s| {
                s.clear_zkproof();
                s.clear_dummy_ask();
            });
            r.redact_outputs(|mut o| {
                o.clear_zkproof();
                o.redact_proprietary(PROPRIETARY_OUTPUT_INFO);
            });
            if v6 {
                r.clear_anchor();
            }
        })
        .redact_orchard_with(|r| redact_bundle(r, v6))
        .redact_ironwood_with(|r| redact_bundle(r, v6))
        .finish()
        .serialize()
        .map_err(|e| format!("PCZT encoding: {e:?}"))
}

/// Apply `zsig` to `pczt` (the bytes that were sent to the device) and
/// return the signed PCZT, serialized.
pub fn apply_signatures(pczt: &[u8], zsig: &[u8]) -> Result<Vec<u8>, String> {
    apply_signatures_to(pczt, pczt, zsig)
}

/// Apply `zsig`, made for the bytes `sent` to the device (a compact view of
/// `pczt`, or `pczt` itself), to `pczt`, and return it signed, serialized.
/// Refused unless `sent` is what the device signed and has the same
/// transaction (sighash) as `pczt`.
pub fn apply_signatures_to(pczt: &[u8], sent: &[u8], zsig: &[u8]) -> Result<Vec<u8>, String> {
    if zsig.len() < HEADER || &zsig[..4] != MAGIC {
        return Err("not a .zsig".into());
    }
    let hash: [u8; 32] = Sha256::digest(sent).into();
    if zsig.len() >= HEADER && zsig[5..37] != hash {
        return Err("the .zsig was made for a different PCZT".into());
    }
    // Versions 1 and 2 (transparent entries), docs/zcash/FORMATS.md.
    let zsig = Zsig::parse(zsig).map_err(|e| e.to_string())?;

    let mut parsed = Pczt::parse(pczt).map_err(|e| format!("PCZT: {e:?}"))?;
    // The pczt Signer finds a P2PKH input's key through hash160_preimages,
    // which wallets leave empty: add each transparent entry's key (a key
    // that is not the script's is then just not found, and refused).
    let ts: Vec<(usize, [u8; 33])> =
        zsig.entries.iter().filter_map(|e| e.transparent.as_ref().map(|t| (e.index as usize, t.pubkey))).collect();
    if !ts.is_empty() {
        let n = parsed.transparent().inputs().len();
        if let Some((i, _)) = ts.iter().find(|(i, _)| *i >= n) {
            return Err(format!("transparent signature for input {i}, but the PCZT has {n} inputs"));
        }
        parsed = pczt::roles::updater::Updater::new(parsed)
            .update_transparent_with(|mut u| {
                for (i, pk) in &ts {
                    u.update_input_with(*i, |mut iu| {
                        iu.set_hash160_preimage(pk.to_vec());
                        Ok(())
                    })?;
                }
                Ok(())
            })
            .map_err(|e| format!("PCZT transparent update: {e:?}"))?
            .finish();
    }
    let mut signer = Signer::new(parsed).map_err(|e| format!("Signer: {e:?}"))?;
    if sent != pczt {
        // Parsing resolves the compacted fields, so this is the full sighash
        // (with transparent inputs it commits to their amounts and scripts).
        let view = Pczt::parse(sent).map_err(|e| format!("sent PCZT: {e:?}"))?;
        let view_sighash = Signer::new(view).map_err(|e| format!("Signer: {e:?}"))?.shielded_sighash();
        if view_sighash != signer.shielded_sighash() {
            return Err("the PCZT sent to the device is a different transaction".into());
        }
    }
    for e in &zsig.entries {
        let index = e.index as usize;
        match (e.pool, &e.transparent) {
            (Pool::Transparent, Some(t)) => {
                if t.sighash_type() != 0x01 {
                    return Err(format!("transparent signature for input {index} is not SIGHASH_ALL"));
                }
                let sig = secp256k1::ecdsa::Signature::from_der(t.der())
                    .map_err(|err| format!("transparent signature for input {index}: {err}"))?;
                signer
                    .append_transparent_signature(index, sig)
                    .map_err(|err| format!("signature for transparent input {index} rejected: {err:?}"))?;
            }
            (Pool::Orchard | Pool::Ironwood, None) => {
                let sig =
                    orchard::primitives::redpallas::Signature::<orchard::primitives::redpallas::SpendAuth>::from(e.sig);
                if e.pool == Pool::Orchard {
                    signer.apply_orchard_signature(index, sig)
                } else {
                    signer.apply_ironwood_signature(index, sig)
                }
                .map_err(|err| format!("signature for action {index} rejected: {err:?}"))?;
            }
            (p, _) => return Err(format!("malformed entry for pool {p:?}")),
        }
    }
    signer.finish().serialize().map_err(|e| format!("PCZT encoding: {e:?}"))
}

/// `io.leekwallet.zcash.hw.PcztSignatures.apply(pczt: ByteArray, zsig: ByteArray): ByteArray`
/// Throws IllegalArgumentException with the reason on any refusal.
#[no_mangle]
pub extern "system" fn Java_io_leekwallet_zcash_hw_PcztSignatures_apply<'l>(
    mut env: jni::JNIEnv<'l>,
    _class: jni::objects::JClass<'l>,
    pczt: jni::objects::JByteArray<'l>,
    zsig: jni::objects::JByteArray<'l>,
) -> jni::objects::JByteArray<'l> {
    let result = (|| -> Result<Vec<u8>, String> {
        let p = env.convert_byte_array(&pczt).map_err(|e| e.to_string())?;
        let z = env.convert_byte_array(&zsig).map_err(|e| e.to_string())?;
        apply_signatures(&p, &z)
    })();
    match result.and_then(|out| env.byte_array_from_slice(&out).map_err(|e| e.to_string())) {
        Ok(arr) => arr,
        Err(msg) => {
            let _ = env.throw_new("java/lang/IllegalArgumentException", msg);
            jni::objects::JByteArray::default()
        }
    }
}

/// `io.leekwallet.zcash.hw.PcztSignatures.applyTo(pczt, sent, zsig): ByteArray`
/// The .zsig signs `sent` (e.g. the compact view); the signatures go into `pczt`.
#[no_mangle]
pub extern "system" fn Java_io_leekwallet_zcash_hw_PcztSignatures_applyTo<'l>(
    mut env: jni::JNIEnv<'l>,
    _class: jni::objects::JClass<'l>,
    pczt: jni::objects::JByteArray<'l>,
    sent: jni::objects::JByteArray<'l>,
    zsig: jni::objects::JByteArray<'l>,
) -> jni::objects::JByteArray<'l> {
    let result = (|| -> Result<Vec<u8>, String> {
        let p = env.convert_byte_array(&pczt).map_err(|e| e.to_string())?;
        let s = env.convert_byte_array(&sent).map_err(|e| e.to_string())?;
        let z = env.convert_byte_array(&zsig).map_err(|e| e.to_string())?;
        apply_signatures_to(&p, &s, &z)
    })();
    byte_array_or_throw(&mut env, result)
}

/// `io.leekwallet.zcash.hw.PcztSignatures.compact(pczt): ByteArray`
#[no_mangle]
pub extern "system" fn Java_io_leekwallet_zcash_hw_PcztSignatures_compact<'l>(
    mut env: jni::JNIEnv<'l>,
    _class: jni::objects::JClass<'l>,
    pczt: jni::objects::JByteArray<'l>,
) -> jni::objects::JByteArray<'l> {
    let result = env.convert_byte_array(&pczt).map_err(|e| e.to_string()).and_then(|p| compact_view(&p));
    byte_array_or_throw(&mut env, result)
}

fn byte_array_or_throw<'l>(env: &mut jni::JNIEnv<'l>, result: Result<Vec<u8>, String>) -> jni::objects::JByteArray<'l> {
    match result.and_then(|out| env.byte_array_from_slice(&out).map_err(|e| e.to_string())) {
        Ok(arr) => arr,
        Err(msg) => {
            let _ = env.throw_new("java/lang/IllegalArgumentException", msg);
            jni::objects::JByteArray::default()
        }
    }
}
