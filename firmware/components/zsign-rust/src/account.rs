//! Account derivation and export: what the device shows and what a companion
//! imports (ZIP 32, ZIP 316).
//!
//! The account shape is Orchard + P2PKH. The transparent half (BIP44,
//! secp256k1) stays in trezor-crypto on the C side, which hands in the account
//! key and the first receiver; this keeps libsecp256k1 out of the Xtensa build
//! and means two independent implementations meet in every exported UFVK.

use alloc::string::String;
use alloc::vec::Vec;

use orchard::keys::{FullViewingKey, Scope, SpendingKey};
use zcash_address::unified::{self, Encoding, Revision, Uitem};
use zcash_protocol::consensus::NetworkType;
use zip32::{fingerprint::SeedFingerprint, AccountId};

pub const ZS_UFVK_MAX: usize = 512;
pub const ZS_UA_MAX: usize = 256;

/// Error codes shared with the C side (`zsign_rust.h`).
pub const ZS_OK: i32 = 0;
pub const ZS_ERR_ARG: i32 = -1;
pub const ZS_ERR_DERIVE: i32 = -2;
pub const ZS_ERR_ENCODE: i32 = -3;

#[repr(C)]
pub struct ZsAccount {
    pub seed_fingerprint: [u8; 32],
    pub orchard_fvk: [u8; 96],
    /// Raw Orchard receiver at diversifier index 0 (d || pk_d).
    pub orchard_address: [u8; 43],
    /// NUL-terminated ZIP 316 strings.
    pub ufvk: [u8; ZS_UFVK_MAX],
    pub ua: [u8; ZS_UA_MAX],
}

pub(crate) fn network(net: u8) -> Option<NetworkType> {
    match net {
        0 => Some(NetworkType::Main),
        1 => Some(NetworkType::Test),
        _ => None,
    }
}

/// ZIP 32 coin type: 133 on mainnet, 1 on testnet and regtest.
pub(crate) fn coin_type(net: NetworkType) -> u32 {
    match net {
        NetworkType::Main => 133,
        _ => 1,
    }
}

pub(crate) fn orchard_sk(seed: &[u8; 64], net: NetworkType, account: u32) -> Option<SpendingKey> {
    let acct = AccountId::try_from(account).ok()?;
    SpendingKey::from_zip32_seed(seed, coin_type(net), acct).ok()
}

fn put_str(dst: &mut [u8], s: &str) -> bool {
    let b = s.as_bytes();
    if b.len() + 1 > dst.len() {
        return false;
    }
    dst[..b.len()].copy_from_slice(b);
    dst[b.len()..].fill(0);
    true
}

fn encode_ufvk(fvk: &FullViewingKey, t_account: Option<&[u8; 65]>, net: NetworkType) -> Option<String> {
    let mut items = Vec::new();
    items.push(Uitem::Data(unified::Fvk::Orchard(fvk.to_bytes())));
    if let Some(t) = t_account {
        items.push(Uitem::Data(unified::Fvk::P2pkh(*t)));
    }
    let ufvk = unified::Ufvk::try_from_items(Revision::R0, items).ok()?;
    Some(ufvk.encode(&net))
}

fn encode_ua(orchard: [u8; 43], p2pkh: Option<&[u8; 20]>, net: NetworkType) -> Option<String> {
    let mut items = Vec::new();
    items.push(Uitem::Data(unified::Receiver::Orchard(orchard)));
    if let Some(h) = p2pkh {
        items.push(Uitem::Data(unified::Receiver::P2pkh(*h)));
    }
    let ua = unified::Address::try_from_items(Revision::R0, items).ok()?;
    Some(ua.encode(&net))
}

/// Derive account `account` from a 64-byte BIP39 seed.
///
/// `net`: 0 mainnet, 1 testnet. `t_account` (65 bytes: chain code || compressed
/// pubkey of m/44'/coin'/account') and `t_receiver` (20-byte HASH160 of the key
/// at .../0/0) may each be NULL, in which case that item is left out of the
/// UFVK or the UA. A P2PKH receiver at diversifier index 0 matches transparent
/// address index 0, as zcash_keys pairs them.
///
/// # Safety
/// Pointers must be valid for the stated sizes; `out` must be writable.
#[no_mangle]
pub unsafe extern "C" fn zs_account_derive(
    seed: *const u8,
    account: u32,
    net: u8,
    t_account: *const u8,
    t_receiver: *const u8,
    out: *mut ZsAccount,
) -> i32 {
    if seed.is_null() || out.is_null() {
        return ZS_ERR_ARG;
    }
    let Some(net) = network(net) else { return ZS_ERR_ARG };
    let seed = &*(seed as *const [u8; 64]);
    let out = &mut *out;
    let t_account = (!t_account.is_null()).then(|| &*(t_account as *const [u8; 65]));
    let t_receiver = (!t_receiver.is_null()).then(|| &*(t_receiver as *const [u8; 20]));

    let Some(fp) = SeedFingerprint::from_seed(seed) else { return ZS_ERR_DERIVE };
    let Some(sk) = orchard_sk(seed, net, account) else { return ZS_ERR_DERIVE };
    let fvk = FullViewingKey::from(&sk);
    let addr = fvk.address_at(0u32, Scope::External).to_raw_address_bytes();

    out.seed_fingerprint = fp.to_bytes();
    out.orchard_fvk = fvk.to_bytes();
    out.orchard_address = addr;

    let Some(ufvk) = encode_ufvk(&fvk, t_account, net) else { return ZS_ERR_ENCODE };
    let Some(ua) = encode_ua(addr, t_receiver, net) else { return ZS_ERR_ENCODE };
    if !put_str(&mut out.ufvk, &ufvk) || !put_str(&mut out.ua, &ua) {
        return ZS_ERR_ENCODE;
    }
    ZS_OK
}
