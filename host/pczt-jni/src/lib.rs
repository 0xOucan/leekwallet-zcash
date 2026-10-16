//! The one step the Zodl Android SDK cannot do for a LeekWallet device.
//!
//! The device returns spend-authorization signatures only (a `.zsig`,
//! docs/zcash/FORMATS.md). The SDK combines a *signed PCZT* with the proven
//! one (`createTransactionFromPczt`), as Keystone and Ledger hand it one, but
//! has no call to insert raw signatures. This does it, with the `pczt`
//! crate's Signer role - which checks each signature against the action's
//! rk before storing it - exactly as `zecz finish` does on the PC.
//!
//! The `.zsig` names the SHA-256 of the PCZT the device checked; signatures
//! are only applied to those exact bytes.

use pczt::roles::signer::Signer;
use pczt::Pczt;
use sha2::{Digest, Sha256};

const MAGIC: &[u8; 4] = b"ZSIG";
const HEADER: usize = 4 + 1 + 32 + 2;
const ENTRY: usize = 1 + 4 + 64;

/// Apply `zsig` to `pczt` (the bytes that were sent to the device) and
/// return the signed PCZT, serialized.
pub fn apply_signatures(pczt: &[u8], zsig: &[u8]) -> Result<Vec<u8>, String> {
    if zsig.len() < HEADER || &zsig[..4] != MAGIC {
        return Err("not a .zsig".into());
    }
    if zsig[4] != 1 {
        return Err(format!(".zsig version {} is not supported", zsig[4]));
    }
    let hash: [u8; 32] = Sha256::digest(pczt).into();
    if zsig[5..37] != hash {
        return Err("the .zsig was made for a different PCZT".into());
    }
    let n = u16::from_le_bytes([zsig[37], zsig[38]]) as usize;
    if zsig.len() != HEADER + n * ENTRY {
        return Err(".zsig length does not match its count".into());
    }

    let parsed = Pczt::parse(pczt).map_err(|e| format!("PCZT: {e:?}"))?;
    let mut signer = Signer::new(parsed).map_err(|e| format!("Signer: {e:?}"))?;
    for i in 0..n {
        let e = &zsig[HEADER + i * ENTRY..HEADER + (i + 1) * ENTRY];
        let index = u32::from_le_bytes([e[1], e[2], e[3], e[4]]) as usize;
        let mut sig = [0u8; 64];
        sig.copy_from_slice(&e[5..69]);
        let sig = orchard::primitives::redpallas::Signature::<orchard::primitives::redpallas::SpendAuth>::from(sig);
        match e[0] {
            1 => signer.apply_orchard_signature(index, sig),
            2 => signer.apply_ironwood_signature(index, sig),
            p => return Err(format!("pool {p} is not supported")),
        }
        .map_err(|err| format!("signature for action {index} rejected: {err:?}"))?;
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
