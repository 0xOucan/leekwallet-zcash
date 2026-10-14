//! Generate key-derivation vectors for LeekWallet Zcash.
//!
//! Each vector goes from a BIP39 mnemonic (and optional passphrase) to
//! everything the device must reproduce: seed, ZIP 32 seed fingerprint,
//! Orchard FVK and raw address, transparent account key and address, and
//! the ZIP 316 UFVK / UA strings for an Orchard + P2PKH account (no Sapling,
//! because the device does not spend Sapling).
//!
//! Output: vectors/keys.json and vectors/keys_vectors.h (for C tests).

use std::fmt::Write as _;
use std::fs;

use orchard::keys::{FullViewingKey, Scope, SpendingKey};
use serde::Serialize;
use zcash_keys::address::Address;
use zcash_transparent::keys::IncomingViewingKey;
use zcash_keys::keys::{ReceiverRequirement, UnifiedAddressRequest, UnifiedFullViewingKey, UnifiedSpendingKey};
use zcash_protocol::consensus::{MainNetwork, NetworkConstants, Parameters, TestNetwork};
use zcash_transparent::address::TransparentAddress;
use zip32::{fingerprint::SeedFingerprint, AccountId};

#[derive(Serialize)]
struct KeyVector {
    name: String,
    network: String,
    mnemonic: String,
    passphrase: String,
    account: u32,
    seed: String,
    seed_fingerprint: String,
    orchard_fvk: String,
    orchard_address_raw: String,
    transparent_account_pubkey: String,
    transparent_address: String,
    transparent_receiver: String,
    ufvk: String,
    ua: String,
    ua_orchard_only: String,
}

const MNEMONICS: &[(&str, &str)] = &[
    ("abandon-art", "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon art"),
    ("zoo-vote", "zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo vote"),
    ("legal-winner", "legal winner thank year wave sausage worth useful legal winner thank year wave sausage worth useful legal winner thank year wave sausage worth title"),
    ("abandon-about-12", "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about"),
];
const PASSPHRASES: &[&str] = &["", "TREZOR", "LeekWallet Zcash 2026!"];
const ACCOUNTS: &[u32] = &[0, 1];

fn vector<P: Parameters>(params: &P, net: &str, name: &str, mnemonic: &str, passphrase: &str, account: u32) -> KeyVector {
    let m = bip39::Mnemonic::parse_normalized(mnemonic).expect("valid mnemonic");
    let seed = m.to_seed_normalized(passphrase);
    let acct = AccountId::try_from(account).unwrap();

    let usk = UnifiedSpendingKey::from_seed(params, &seed, acct).expect("usk");
    let full = usk.to_unified_full_viewing_key();

    // Orchard + P2PKH only: the account shape the device exports.
    let ofvk = FullViewingKey::from(&SpendingKey::from_zip32_seed(&seed, params.coin_type(), acct).unwrap());
    let tpub = full.transparent().expect("transparent").clone();
    let ufvk = UnifiedFullViewingKey::new(Some(tpub.clone()), None, Some(ofvk.clone())).expect("ufvk");

    let req = UnifiedAddressRequest::custom(ReceiverRequirement::Require, ReceiverRequirement::Omit, ReceiverRequirement::Require).unwrap();
    let (ua, idx) = ufvk.default_address(req).expect("ua");
    assert_eq!(u128::from(idx), 0, "diversifier index 0 is always valid for Orchard");
    let (ua_o, _) = ufvk.default_address(UnifiedAddressRequest::ORCHARD).expect("ua orchard");

    let taddr = tpub
        .derive_external_ivk()
        .unwrap()
        .derive_address(zcash_transparent::keys::NonHardenedChildIndex::ZERO)
        .unwrap();

    let TransparentAddress::PublicKeyHash(t_hash) = taddr else { panic!("p2pkh") };

    KeyVector {
        name: format!("{net}/{name}/pp{}/acct{account}", PASSPHRASES.iter().position(|p| *p == passphrase).unwrap()),
        network: net.into(),
        mnemonic: mnemonic.into(),
        passphrase: passphrase.into(),
        account,
        seed: hex::encode(seed),
        seed_fingerprint: hex::encode(SeedFingerprint::from_seed(&seed).unwrap().to_bytes()),
        orchard_fvk: hex::encode(ofvk.to_bytes()),
        orchard_address_raw: hex::encode(ofvk.address_at(0u32, Scope::External).to_raw_address_bytes()),
        transparent_account_pubkey: hex::encode(tpub.serialize()),
        transparent_address: Address::Transparent(taddr).encode(params),
        transparent_receiver: hex::encode(t_hash),
        ufvk: ufvk.encode(params).expect("ufvk encoding"),
        ua: ua.encode(params),
        ua_orchard_only: ua_o.encode(params),
    }
}

fn c_bytes(hexs: &str) -> String {
    let b = hex::decode(hexs).unwrap();
    let mut s = String::from("{");
    for (i, x) in b.iter().enumerate() {
        if i % 16 == 0 { s.push_str("\n        "); }
        write!(s, "0x{x:02x},").unwrap();
    }
    s.push_str("\n    }");
    s
}

fn main() {
    let mut vs = Vec::new();
    for (name, m) in MNEMONICS {
        for p in PASSPHRASES {
            for a in ACCOUNTS {
                vs.push(vector(&MainNetwork, "main", name, m, p, *a));
                vs.push(vector(&TestNetwork, "test", name, m, p, *a));
            }
        }
    }

    let out = std::env::args().nth(1).unwrap_or_else(|| "..".into());
    fs::write(format!("{out}/keys.json"), serde_json::to_string_pretty(&vs).unwrap() + "\n").unwrap();

    let mut h = String::new();
    h.push_str("/* Generated by vectors/gen from librustzcash. Do not edit. */\n");
    h.push_str("#pragma once\n#include <stdint.h>\n\n");
    h.push_str("typedef struct {\n    const char *name, *mnemonic, *passphrase;\n    uint32_t account;\n    uint8_t network; /* 0 main, 1 test */\n");
    h.push_str("    uint8_t seed[64], seed_fingerprint[32], orchard_fvk[96], orchard_address_raw[43];\n");
    h.push_str("    uint8_t transparent_account_pubkey[65], transparent_receiver[20];\n");
    h.push_str("    const char *transparent_address, *ufvk, *ua, *ua_orchard_only;\n} zv_key_vector;\n\n");
    h.push_str("static const zv_key_vector ZV_KEYS[] = {\n");
    for v in &vs {
        writeln!(h, "  {{ {:?}, {:?}, {:?}, {}, {},", v.name, v.mnemonic, v.passphrase, v.account, u8::from(v.network == "test")).unwrap();
        for f in [&v.seed, &v.seed_fingerprint, &v.orchard_fvk, &v.orchard_address_raw, &v.transparent_account_pubkey, &v.transparent_receiver] {
            writeln!(h, "    {},", c_bytes(f)).unwrap();
        }
        writeln!(h, "    {:?}, {:?}, {:?}, {:?} }},", v.transparent_address, v.ufvk, v.ua, v.ua_orchard_only).unwrap();
    }
    h.push_str("};\n#define ZV_KEYS_COUNT (sizeof ZV_KEYS / sizeof ZV_KEYS[0])\n");
    fs::write(format!("{out}/keys_vectors.h"), h).unwrap();
    eprintln!("wrote {} key vectors to {out}", vs.len());
}
