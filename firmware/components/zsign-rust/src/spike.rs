//! Phase 0.4 spike: the three operations whose cost decides the design.
//! - ZIP 32 Orchard-protocol key + FVK derivation (includes commit_ivk / Sinsemilla)
//! - default address derivation (FF1 diversifier)
//! - RedPallas SpendAuth signature with a randomized key
//! - note commitment (cmx, Sinsemilla NoteCommit)

use orchard::keys::{FullViewingKey, Scope, SpendAuthorizingKey, SpendingKey};
use orchard::note::{ExtractedNoteCommitment, Note, NoteVersion, RandomSeed, Rho};
use orchard::value::NoteValue;
use pasta_curves::pallas;
use ff::PrimeField;
use zip32::AccountId;

use crate::platform::PlatformRng;

const COIN_TYPE_MAINNET: u32 = 133;

fn sk_from_seed(seed: &[u8; 64], account: u32) -> Option<SpendingKey> {
    let acct = AccountId::try_from(account).ok()?;
    SpendingKey::from_zip32_seed(seed, COIN_TYPE_MAINNET, acct).ok()
}

/// Derive the 96-byte Orchard FVK (ak || nk || rivk). Returns 0 on success.
#[no_mangle]
pub unsafe extern "C" fn zs_spike_fvk(seed: *const u8, account: u32, out_fvk: *mut u8) -> i32 {
    let seed = &*(seed as *const [u8; 64]);
    let Some(sk) = sk_from_seed(seed, account) else { return -1 };
    let fvk = FullViewingKey::from(&sk);
    core::ptr::copy_nonoverlapping(fvk.to_bytes().as_ptr(), out_fvk, 96);
    0
}

/// Raw 43-byte default external address (d || pk_d). Returns 0 on success.
#[no_mangle]
pub unsafe extern "C" fn zs_spike_address(seed: *const u8, account: u32, out_addr: *mut u8) -> i32 {
    let seed = &*(seed as *const [u8; 64]);
    let Some(sk) = sk_from_seed(seed, account) else { return -1 };
    let addr = FullViewingKey::from(&sk).address_at(0u32, Scope::External);
    core::ptr::copy_nonoverlapping(addr.to_raw_address_bytes().as_ptr(), out_addr, 43);
    0
}

/// RedPallas SpendAuth signature over `sighash` with rsk = ask + alpha.
/// `alpha` is 32 bytes LE (must be canonical). Writes 64 bytes. Returns 0 on success.
#[no_mangle]
pub unsafe extern "C" fn zs_spike_sign(
    seed: *const u8,
    account: u32,
    alpha: *const u8,
    sighash: *const u8,
    out_sig: *mut u8,
) -> i32 {
    let seed = &*(seed as *const [u8; 64]);
    let Some(sk) = sk_from_seed(seed, account) else { return -1 };
    let mut a = [0u8; 32];
    a.copy_from_slice(core::slice::from_raw_parts(alpha, 32));
    let alpha: Option<pallas::Scalar> = pallas::Scalar::from_repr(a).into();
    let Some(alpha) = alpha else { return -2 };
    let rsk = SpendAuthorizingKey::from(&sk).randomize(&alpha);
    let msg = core::slice::from_raw_parts(sighash, 32);
    let sig = rsk.sign(PlatformRng, msg);
    let bytes: [u8; 64] = (&sig).into();
    core::ptr::copy_nonoverlapping(bytes.as_ptr(), out_sig, 64);
    0
}

/// Note commitment of a fixed test note to our own address. Writes 32-byte cmx.
#[no_mangle]
pub unsafe extern "C" fn zs_spike_cmx(seed: *const u8, value: u64, out_cmx: *mut u8) -> i32 {
    let seed = &*(seed as *const [u8; 64]);
    let Some(sk) = sk_from_seed(seed, 0) else { return -1 };
    let addr = FullViewingKey::from(&sk).address_at(0u32, Scope::External);
    // Deterministic rho/rseed so host and device produce the same cmx.
    let rho: Option<Rho> = Rho::from_bytes(&pallas::Base::from(7u64).to_repr()).into();
    let Some(rho) = rho else { return -2 };
    let mut rs = [0u8; 32];
    rs[0] = 42;
    let rseed: Option<RandomSeed> = RandomSeed::from_bytes(rs, &rho).into();
    let Some(rseed) = rseed else { return -3 };
    let note: Option<Note> = Note::from_parts(addr, NoteValue::from_raw(value), rho, rseed, NoteVersion::V3).into();
    let Some(note) = note else { return -4 };
    let cmx = ExtractedNoteCommitment::from(note.commitment());
    core::ptr::copy_nonoverlapping(cmx.to_bytes().as_ptr(), out_cmx, 32);
    0
}


/// Verify a SpendAuth signature against rk = ak + [alpha]G, where ak comes from
/// the validating (public) half of the key. Returns 0 if valid, 1 if invalid.
#[no_mangle]
pub unsafe extern "C" fn zs_spike_verify(
    seed: *const u8,
    account: u32,
    alpha: *const u8,
    sighash: *const u8,
    sig: *const u8,
) -> i32 {
    use orchard::keys::SpendValidatingKey;
    use orchard::primitives::redpallas::{Signature, SpendAuth};
    let seed = &*(seed as *const [u8; 64]);
    let Some(sk) = sk_from_seed(seed, account) else { return -1 };
    let mut a = [0u8; 32];
    a.copy_from_slice(core::slice::from_raw_parts(alpha, 32));
    let alpha: Option<pallas::Scalar> = pallas::Scalar::from_repr(a).into();
    let Some(alpha) = alpha else { return -2 };
    let ak = SpendValidatingKey::from(&SpendAuthorizingKey::from(&sk));
    let rk = ak.randomize(&alpha);
    let mut s = [0u8; 64];
    s.copy_from_slice(core::slice::from_raw_parts(sig, 64));
    let sig: Signature<SpendAuth> = s.into();
    let msg = core::slice::from_raw_parts(sighash, 32);
    if rk.verify(msg, &sig).is_ok() { 0 } else { 1 }
}
