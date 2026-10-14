//! PCZT check, summary and signing (ZIP 374 Signer, with the Verifier checks
//! a hardware wallet owes its user before it signs anything).
//!
//! The device recomputes every commitment it is asked to sign over, from
//! data in the PCZT and from its own keys. Nothing it displays is taken on
//! the host's word: output values and recipients are bound to their note
//! commitments, spends are bound to this seed's full viewing key, the
//! memo is recovered with this account's own outgoing viewing key, and the
//! fee is computed here.

use alloc::boxed::Box;
use alloc::string::String;
use alloc::vec::Vec;

use orchard::keys::{FullViewingKey, OutgoingViewingKey, Scope, SpendAuthorizingKey};
use orchard::ValuePool;
use pczt::roles::signer::{extract_orchard_spend_auth_signatures, Signer};
use pczt::roles::verifier::{OrchardError, Verifier};
use pczt::Pczt;
use zcash_address::unified::{self, Container, Encoding};
use zcash_protocol::consensus::NetworkType;

use crate::account::{coin_type, network, orchard_sk, ZS_UA_MAX};
use crate::platform::PlatformRng;

pub const ZS_MAX_OUTPUTS: usize = 16;
pub const ZS_MAX_SIGS: usize = 32;

/// Refusal codes. Each one names exactly why the device will not sign.
pub const ZS_E_PARSE: i32 = -10;
pub const ZS_E_UNSUPPORTED_BRANCH: i32 = -11;
pub const ZS_E_SPEND_NOT_OURS: i32 = -12;
pub const ZS_E_OUTPUT_COMMITMENT: i32 = -13;
pub const ZS_E_USER_ADDRESS: i32 = -14;
pub const ZS_E_VALUE_BALANCE: i32 = -15;
pub const ZS_E_UNSUPPORTED_BUNDLE: i32 = -16;
pub const ZS_E_NETWORK: i32 = -17;
pub const ZS_E_TOO_MANY: i32 = -18;
pub const ZS_E_MISSING_DATA: i32 = -19;
pub const ZS_E_DUMMY_KEY: i32 = -20;
pub const ZS_E_NOT_CHECKED: i32 = -21;
pub const ZS_E_SIGN: i32 = -22;
pub const ZS_E_INTERNAL: i32 = -23;

/// NU6.3 (Ironwood) and NU7. Anything else is refused: an older branch id
/// would produce a signature for a transaction the network rejects, or one
/// meant for a chain this firmware does not know.
const ALLOWED_BRANCHES: [u32; 2] = [0x37a5_165b, 0x7719_0ad9];

pub const ZS_POOL_TRANSPARENT: u8 = 0;
pub const ZS_POOL_ORCHARD: u8 = 1;
pub const ZS_POOL_IRONWOOD: u8 = 2;

pub const ZS_KIND_EXTERNAL: u8 = 0;
pub const ZS_KIND_CHANGE: u8 = 1;
pub const ZS_KIND_SELF: u8 = 2;

pub const ZS_MEMO_NONE: u8 = 0;
pub const ZS_MEMO_TEXT: u8 = 1;
pub const ZS_MEMO_BINARY: u8 = 2;
pub const ZS_MEMO_UNRECOVERED: u8 = 3;

pub const ZS_WARN_TRANSPARENT_OUT: u32 = 1 << 0;
pub const ZS_WARN_NO_USER_ADDRESS: u32 = 1 << 1;

#[repr(C)]
pub struct ZsOutput {
    pub pool: u8,
    pub kind: u8,
    pub memo_kind: u8,
    pub _pad: u8,
    pub memo_len: u16,
    pub _pad2: u16,
    pub value: u64,
    pub address: [u8; ZS_UA_MAX],
    pub memo: [u8; 512],
}

#[repr(C)]
pub struct ZsSummary {
    pub branch_id: u32,
    pub tx_version: u32,
    pub warnings: u32,
    pub n_outputs: u8,
    pub n_spends: u8,
    pub n_dummies: u8,
    pub _pad: u8,
    pub total_in: u64,
    pub fee: u64,
    pub sighash: [u8; 32],
    pub outputs: [ZsOutput; ZS_MAX_OUTPUTS],
}

#[repr(C)]
pub struct ZsSig {
    pub pool: u8,
    pub _pad: [u8; 3],
    pub index: u32,
    pub sig: [u8; 64],
}

pub struct ZsCtx {
    pczt: Pczt,
    checked: Option<FullViewingKey>,
}

struct Acc<'a> {
    fvk: &'a FullViewingKey,
    net: NetworkType,
    outputs: Vec<(u8, u8, u64, String, u8, Vec<u8>)>,
    n_spends: u8,
    n_dummies: u8,
    total_in: u64,
    warnings: u32,
}

fn put_str(dst: &mut [u8], s: &str) {
    let b = s.as_bytes();
    let n = b.len().min(dst.len() - 1);
    dst[..n].copy_from_slice(&b[..n]);
    dst[n..].fill(0);
}

fn orchard_ua(raw: [u8; 43], net: NetworkType) -> Option<String> {
    let items = alloc::vec![unified::Uitem::Data(unified::Receiver::Orchard(raw))];
    Some(unified::Address::try_from_items(unified::Revision::R0, items).ok()?.encode(&net))
}

/// The UA the host claims the user typed must contain the receiver the note
/// commitment binds, on this network.
fn user_address_matches(ua: &str, raw: &[u8; 43], net: NetworkType) -> bool {
    match unified::Address::decode(ua) {
        Ok((n, _, addr)) if n == net => addr
            .items()
            .iter()
            .any(|r| matches!(r, unified::Receiver::Orchard(b) if b == raw)),
        _ => false,
    }
}

/// ZIP 302 memo classification.
fn classify_memo(m: &[u8; 512]) -> (u8, Vec<u8>) {
    if m[0] == 0xF6 && m[1..].iter().all(|&b| b == 0) {
        return (ZS_MEMO_NONE, Vec::new());
    }
    if m[0] <= 0xF4 {
        let end = m.iter().rposition(|&b| b != 0).map_or(0, |i| i + 1);
        if core::str::from_utf8(&m[..end]).is_ok() {
            return (ZS_MEMO_TEXT, m[..end].to_vec());
        }
    }
    (ZS_MEMO_BINARY, m.to_vec())
}

fn check_bundle(
    bundle: &orchard::pczt::Bundle,
    pool: u8,
    acc: &mut Acc<'_>,
) -> Result<(), i32> {
    let version = *bundle.bundle_version();
    bundle.verify_cross_address_restriction().map_err(|_| ZS_E_OUTPUT_COMMITMENT)?;
    let ovks: [OutgoingViewingKey; 2] =
        [acc.fvk.to_ovk(Scope::External), acc.fvk.to_ovk(Scope::Internal)];

    let mut sum: i128 = 0;
    for action in bundle.actions() {
        let spend = action.spend();
        if spend.dummy_sk().is_some() {
            return Err(ZS_E_DUMMY_KEY);
        }
        let s_value = spend.value().ok_or(ZS_E_MISSING_DATA)?.inner();
        if spend.spend_auth_sig().is_none() {
            // Ours to sign, so it must be provably ours.
            spend.verify_nullifier(Some(acc.fvk)).map_err(|_| ZS_E_SPEND_NOT_OURS)?;
            spend.verify_rk(Some(acc.fvk)).map_err(|_| ZS_E_SPEND_NOT_OURS)?;
            if s_value > 0 {
                acc.n_spends += 1;
                acc.total_in += s_value;
            }
        } else if s_value != 0 {
            // A pre-signed spend is only acceptable as zero-value padding.
            return Err(ZS_E_SPEND_NOT_OURS);
        }

        let out = action.output();
        out.verify_note_commitment(spend).map_err(|_| ZS_E_OUTPUT_COMMITMENT)?;
        if action.rcv().is_some() {
            action.verify_cv_net().map_err(|_| ZS_E_VALUE_BALANCE)?;
        }
        let recipient = out.recipient().ok_or(ZS_E_MISSING_DATA)?;
        let o_value = out.value().ok_or(ZS_E_MISSING_DATA)?.inner();
        sum += i128::from(s_value) - i128::from(o_value);

        let raw = recipient.to_raw_address_bytes();
        let scope = acc.fvk.scope_for_address(&recipient);
        if o_value == 0 && out.user_address().is_none() {
            acc.n_dummies += 1;
            continue;
        }
        let (kind, address) = match (scope, out.user_address()) {
            (Some(Scope::Internal), _) => (ZS_KIND_CHANGE, String::new()),
            (_, Some(ua)) => {
                if !user_address_matches(ua, &raw, acc.net) {
                    return Err(ZS_E_USER_ADDRESS);
                }
                let k = if scope.is_some() { ZS_KIND_SELF } else { ZS_KIND_EXTERNAL };
                (k, ua.clone())
            }
            (Some(Scope::External), None) => {
                (ZS_KIND_SELF, orchard_ua(raw, acc.net).ok_or(ZS_E_INTERNAL)?)
            }
            (None, None) => {
                acc.warnings |= ZS_WARN_NO_USER_ADDRESS;
                (ZS_KIND_EXTERNAL, orchard_ua(raw, acc.net).ok_or(ZS_E_INTERNAL)?)
            }
        };

        let mut memo = (ZS_MEMO_UNRECOVERED, Vec::new());
        for ovk in &ovks {
            if let Ok(Some((note, addr, m))) = action.recover_output_with_ovk(ovk, version) {
                if addr.to_raw_address_bytes() == raw && note.value().inner() == o_value {
                    memo = classify_memo(&m);
                    break;
                }
            }
        }
        acc.outputs.push((pool, kind, o_value, address, memo.0, memo.1));
    }

    let value_sum: i64 = (*bundle.value_sum()).try_into().map_err(|_| ZS_E_VALUE_BALANCE)?;
    if sum != i128::from(value_sum) {
        return Err(ZS_E_VALUE_BALANCE);
    }
    Ok(())
}

fn t_address(script: &[u8], net: NetworkType) -> Option<String> {
    use zcash_address::{ToAddress, ZcashAddress};
    match script {
        [0x76, 0xa9, 0x14, h @ .., 0x88, 0xac] if h.len() == 20 => {
            Some(ZcashAddress::from_transparent_p2pkh(net, h.try_into().ok()?).encode())
        }
        [0xa9, 0x14, h @ .., 0x87] if h.len() == 20 => {
            Some(ZcashAddress::from_transparent_p2sh(net, h.try_into().ok()?).encode())
        }
        _ => None,
    }
}

fn check(ctx: &ZsCtx, seed: &[u8; 64], account: u32, net: NetworkType, out: &mut ZsSummary)
    -> Result<FullViewingKey, i32>
{
    let pczt = &ctx.pczt;
    let g = pczt.global();
    let branch = u32::from(*g.consensus_branch_id());
    if !ALLOWED_BRANCHES.contains(&branch) {
        return Err(ZS_E_UNSUPPORTED_BRANCH);
    }
    if *g.coin_type() != coin_type(net) {
        return Err(ZS_E_NETWORK);
    }
    let sk = orchard_sk(seed, net, account).ok_or(ZS_E_INTERNAL)?;
    let fvk = FullViewingKey::from(&sk);

    // Not yet supported on the device: refuse rather than sign blind.
    if !pczt.transparent().inputs().is_empty() {
        return Err(ZS_E_UNSUPPORTED_BUNDLE);
    }
    if !pczt.sapling().spends().is_empty() || !pczt.sapling().outputs().is_empty() {
        return Err(ZS_E_UNSUPPORTED_BUNDLE);
    }

    let mut acc = Acc {
        fvk: &fvk, net, outputs: Vec::new(),
        n_spends: 0, n_dummies: 0, total_in: 0, warnings: 0,
    };

    let mut t_out: u64 = 0;
    for o in pczt.transparent().outputs() {
        let v = *o.value();
        let addr = t_address(o.script_pubkey(), net).ok_or(ZS_E_UNSUPPORTED_BUNDLE)?;
        t_out = t_out.checked_add(v).ok_or(ZS_E_VALUE_BALANCE)?;
        acc.warnings |= ZS_WARN_TRANSPARENT_OUT;
        acc.outputs.push((ZS_POOL_TRANSPARENT, ZS_KIND_EXTERNAL, v, addr, ZS_MEMO_NONE, Vec::new()));
    }

    let mut err: i32 = 0;
    let verifier = Verifier::new(pczt.clone());
    let verifier = verifier.with_orchard::<(), _>(|b| {
        check_bundle(b, ZS_POOL_ORCHARD, &mut acc).map_err(|e| { err = e; OrchardError::Custom(()) })
    });
    let verifier = match verifier {
        Ok(v) => v,
        Err(_) => return Err(if err != 0 { err } else { ZS_E_PARSE }),
    };
    let r = verifier.with_ironwood::<(), _>(|b| {
        check_bundle(b, ZS_POOL_IRONWOOD, &mut acc).map_err(|e| { err = e; OrchardError::Custom(()) })
    });
    if r.is_err() {
        return Err(if err != 0 { err } else { ZS_E_PARSE });
    }

    // Fee = everything in - everything out, computed here from bound values.
    let shielded_in = acc.total_in;
    let shielded_out: u64 = acc.outputs.iter().filter(|o| o.0 != ZS_POOL_TRANSPARENT).map(|o| o.2).sum();
    let fee = shielded_in
        .checked_sub(shielded_out)
        .and_then(|x| x.checked_sub(t_out))
        .ok_or(ZS_E_VALUE_BALANCE)?;

    if acc.outputs.len() > ZS_MAX_OUTPUTS {
        return Err(ZS_E_TOO_MANY);
    }
    let signer = Signer::new(pczt.clone()).map_err(|_| ZS_E_PARSE)?;

    out.branch_id = branch;
    out.tx_version = *g.tx_version();
    out.warnings = acc.warnings;
    out.n_outputs = acc.outputs.len() as u8;
    out.n_spends = acc.n_spends;
    out.n_dummies = acc.n_dummies;
    out.total_in = acc.total_in;
    out.fee = fee;
    out.sighash = signer.shielded_sighash();
    for (slot, (pool, kind, value, addr, mk, memo)) in out.outputs.iter_mut().zip(acc.outputs.iter()) {
        slot.pool = *pool;
        slot.kind = *kind;
        slot.value = *value;
        put_str(&mut slot.address, addr);
        slot.memo_kind = *mk;
        slot.memo_len = memo.len() as u16;
        slot.memo[..memo.len()].copy_from_slice(memo);
    }
    Ok(fvk)
}

/// Parse a PCZT. Returns NULL and sets `*err` on failure.
///
/// # Safety
/// `bytes` must be valid for `len` bytes; `err` may be NULL.
#[no_mangle]
pub unsafe extern "C" fn zs_pczt_new(bytes: *const u8, len: usize, err: *mut i32) -> *mut ZsCtx {
    let set = |e: i32| if !err.is_null() { *err = e };
    if bytes.is_null() {
        set(ZS_E_PARSE);
        return core::ptr::null_mut();
    }
    match Pczt::parse(core::slice::from_raw_parts(bytes, len)) {
        Ok(pczt) => {
            set(0);
            Box::into_raw(Box::new(ZsCtx { pczt, checked: None }))
        }
        Err(_) => {
            set(ZS_E_PARSE);
            core::ptr::null_mut()
        }
    }
}

/// # Safety
/// `ctx` must come from `zs_pczt_new` and not be used afterwards.
#[no_mangle]
pub unsafe extern "C" fn zs_pczt_free(ctx: *mut ZsCtx) {
    if !ctx.is_null() {
        drop(Box::from_raw(ctx));
    }
}

/// Run every check and fill the summary the UI must show. Signing is only
/// possible afterwards, and only with the same seed and account.
///
/// # Safety
/// Pointers must be valid; `out` must be writable.
#[no_mangle]
pub unsafe extern "C" fn zs_pczt_check(
    ctx: *mut ZsCtx, seed: *const u8, account: u32, net: u8, out: *mut ZsSummary,
) -> i32 {
    if ctx.is_null() || seed.is_null() || out.is_null() {
        return crate::account::ZS_ERR_ARG;
    }
    let Some(net) = network(net) else { return crate::account::ZS_ERR_ARG };
    let ctx = &mut *ctx;
    ctx.checked = None;
    core::ptr::write_bytes(out, 0, 1);
    match check(ctx, &*(seed as *const [u8; 64]), account, net, &mut *out) {
        Ok(fvk) => {
            ctx.checked = Some(fvk);
            0
        }
        Err(e) => e,
    }
}

/// Sign every spend of ours. Call only after the user accepted the summary
/// from `zs_pczt_check`. Writes up to `max` signatures and their count.
///
/// # Safety
/// Pointers must be valid; `sigs` must hold `max` entries.
#[no_mangle]
pub unsafe extern "C" fn zs_pczt_sign(
    ctx: *mut ZsCtx, seed: *const u8, account: u32, net: u8,
    sigs: *mut ZsSig, max: u32, n_out: *mut u32,
) -> i32 {
    if ctx.is_null() || seed.is_null() || sigs.is_null() || n_out.is_null() {
        return crate::account::ZS_ERR_ARG;
    }
    let Some(net) = network(net) else { return crate::account::ZS_ERR_ARG };
    let ctx = &mut *ctx;
    let Some(checked_fvk) = ctx.checked.take() else { return ZS_E_NOT_CHECKED };
    let Some(sk) = orchard_sk(&*(seed as *const [u8; 64]), net, account) else { return ZS_E_INTERNAL };
    if FullViewingKey::from(&sk) != checked_fvk {
        return ZS_E_NOT_CHECKED;
    }
    let ask = SpendAuthorizingKey::from(&sk);

    let mut signer = match Signer::new(ctx.pczt.clone()) {
        Ok(s) => s,
        Err(_) => return ZS_E_PARSE,
    };
    let need = |b: &pczt::orchard::Bundle| -> Vec<usize> {
        b.actions().iter().enumerate()
            .filter(|(_, a)| a.spend().spend_auth_sig().is_none())
            .map(|(i, _)| i).collect()
    };
    for i in need(ctx.pczt.orchard()) {
        if signer.sign_orchard(PlatformRng, i, &ask).is_err() {
            return ZS_E_SIGN;
        }
    }
    for i in need(ctx.pczt.ironwood()) {
        if signer.sign_ironwood(PlatformRng, i, &ask).is_err() {
            return ZS_E_SIGN;
        }
    }
    let signed = signer.finish();
    let list = extract_orchard_spend_auth_signatures(&signed);
    let ours: Vec<_> = list.into_iter().filter(|s| {
        let b = match s.value_pool() { ValuePool::Orchard => ctx.pczt.orchard(), _ => ctx.pczt.ironwood() };
        b.actions()[s.action_index()].spend().spend_auth_sig().is_none()
    }).collect();
    if ours.len() > max as usize {
        return ZS_E_TOO_MANY;
    }
    let out = core::slice::from_raw_parts_mut(sigs, max as usize);
    for (slot, s) in out.iter_mut().zip(ours.iter()) {
        slot.pool = match s.value_pool() { ValuePool::Orchard => ZS_POOL_ORCHARD, _ => ZS_POOL_IRONWOOD };
        slot._pad = [0; 3];
        slot.index = s.action_index() as u32;
        slot.sig = *s.signature();
    }
    *n_out = ours.len() as u32;
    0
}

/// Check one signature the way a companion applies it: against the action's
/// randomized key and the shielded sighash. Returns 0 if it verifies.
///
/// # Safety
/// `ctx` and `sig` must be valid.
#[no_mangle]
pub unsafe extern "C" fn zs_pczt_verify_sig(ctx: *const ZsCtx, sig: *const ZsSig) -> i32 {
    if ctx.is_null() || sig.is_null() {
        return crate::account::ZS_ERR_ARG;
    }
    let (ctx, sig) = (&*ctx, &*sig);
    let Ok(mut signer) = Signer::new(ctx.pczt.clone()) else { return ZS_E_PARSE };
    let s: orchard::primitives::redpallas::Signature<orchard::primitives::redpallas::SpendAuth> =
        sig.sig.into();
    let r = match sig.pool {
        ZS_POOL_ORCHARD => signer.apply_orchard_signature(sig.index as usize, s),
        ZS_POOL_IRONWOOD => signer.apply_ironwood_signature(sig.index as usize, s),
        _ => return crate::account::ZS_ERR_ARG,
    };
    if r.is_ok() { 0 } else { ZS_E_SIGN }
}
