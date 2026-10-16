//! Generate PCZT vectors for the LeekWallet Zcash signer.
//!
//! Each PCZT is built the way a companion builds one for a hardware signer:
//! zcash_primitives Builder -> Creator -> IO Finalizer -> Updater (ZIP 32
//! derivation on spends and change, user_address on external outputs). It is
//! then left unproven, because the device never sees or needs the proof.
//!
//! Every vector records the verdict the device must reach, and for valid ones
//! the shielded sighash and the summary it must display. Tampered vectors are
//! byte patches of a valid PCZT that the device must refuse.
//!
//! Output: vectors/pczt/<name>.pczt and vectors/pczt.json

use std::fs;

use incrementalmerkletree::Retention;
use orchard::keys::{FullViewingKey, Scope, SpendingKey};
use orchard::tree::MerkleHashOrchard;
use pczt::roles::{
    creator::Creator, io_finalizer::IoFinalizer, redactor::Redactor, signer::Signer, updater::Updater,
};
use pczt::Pczt;
use rand::SeedableRng;
use rand_chacha::ChaCha20Rng;
use serde::Serialize;
use shardtree::{store::memory::MemoryShardStore, ShardTree};
use zcash_note_encryption::try_note_decryption;
use zcash_primitives::transaction::builder::{BuildConfig, Builder, BundlePadding, PcztResult};
use zcash_primitives::transaction::fees::zip317;
use zcash_protocol::consensus::{BlockHeight, NetworkConstants, Parameters, TestNetwork};
use zcash_protocol::memo::{Memo, MemoBytes};
use zcash_protocol::value::Zatoshis;
use zcash_transparent::address::TransparentAddress;
use zip32::{fingerprint::SeedFingerprint, AccountId};

/// Testnet heights: NU6.3 activates at 4,134,000 and NU7 at 4,465,026.
const H_NU6_2: u32 = 4_100_000;
const H_NU6_3: u32 = 4_200_000;
const H_NU7: u32 = 4_500_000;

const SENDER: &str = "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon art";
const RECIPIENT: &str = "zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo vote";

#[derive(Serialize, Clone)]
struct OutputSummary {
    pool: &'static str,
    kind: &'static str, // "external" | "change" | "dummy"
    address: String,
    value: u64,
    memo: String,
}

#[derive(Serialize)]
struct PcztVector {
    name: String,
    description: String,
    file: String,
    signer_mnemonic: String,
    signer_passphrase: String,
    account: u32,
    network: &'static str,
    expect: String, // "ok" or a refusal code
    sighash: Option<String>,
    consensus_branch_id: Option<String>,
    fee: Option<u64>,
    outputs: Vec<OutputSummary>,
}

struct Account {
    seed: [u8; 64],
    fvk: FullViewingKey,
}

fn account(mnemonic: &str) -> Account {
    let seed = bip39::Mnemonic::parse_normalized(mnemonic).unwrap().to_seed_normalized("");
    let sk = SpendingKey::from_zip32_seed(&seed, TestNetwork.coin_type(), AccountId::ZERO).unwrap();
    Account { seed, fvk: FullViewingKey::from(&sk) }
}

/// A received Ironwood note plus a one-leaf tree to spend it from.
/// Deterministic RNG, so regenerating leaves committed vectors unchanged.
fn rng(tag: u64) -> ChaCha20Rng {
    ChaCha20Rng::seed_from_u64(0x4c45_454b_5a45_4300 ^ tag)
}

fn ironwood_note(fvk: &FullViewingKey, value: u64, tag: u64) -> (orchard::Note, orchard::Anchor, orchard::tree::MerklePath) {
    let mut rng = rng(tag);
    let recipient = fvk.address_at(0u32, Scope::External);
    let version = orchard::bundle::BundleVersion::ironwood_v3();
    let mut b = orchard::builder::Builder::new(
        orchard::builder::BundleType::DEFAULT,
        version,
        version.default_flags(),
        orchard::Anchor::empty_tree(),
    )
    .unwrap();
    b.add_output(None, recipient, orchard::value::NoteValue::from_raw(value), Memo::Empty.encode().into_bytes())
        .unwrap();
    let (bundle, meta) = b.build::<i64>(&mut rng).unwrap().unwrap();
    let action = bundle.actions().get(meta.output_action_index(0).unwrap()).unwrap();
    let domain = orchard::note_encryption::IronwoodDomain::for_action(action);
    let (note, _, _) = try_note_decryption(&domain, &fvk.to_ivk(Scope::External).prepare(), action).unwrap();

    let cmx: orchard::note::ExtractedNoteCommitment = note.commitment().into();
    let leaf = MerkleHashOrchard::from_cmx(&cmx);
    let mut tree = ShardTree::<_, 32, 16>::new(MemoryShardStore::<MerkleHashOrchard, u32>::empty(), 100);
    tree.append(leaf, Retention::Marked).unwrap();
    tree.checkpoint(9_999_999).unwrap();
    let path = tree.witness_at_checkpoint_depth(0.into(), 0).unwrap().unwrap();
    let anchor = path.root(leaf);
    (note, anchor.into(), path.into())
}

enum Dest {
    /// Ironwood output to an external unified address (string, raw receiver).
    Shielded(String, orchard::Address),
    /// Transparent P2PKH output.
    Transparent(String, TransparentAddress),
}

struct Built {
    pczt: Pczt,
    outputs: Vec<OutputSummary>,
    fee: u64,
}

fn build_send(tag: u64, height: u32, sender: &Account, input: u64, dest: Dest, amount: u64, memo: &str, fee: u64) -> Built {
    let (note, anchor, path) = ironwood_note(&sender.fvk, input, tag);
    let change = input - amount - fee;
    let change_addr = sender.fvk.address_at(0u32, Scope::Internal);

    let mut builder = Builder::new(
        TestNetwork,
        BlockHeight::from_u32(height),
        BuildConfig::Standard {
            sapling_anchor: None,
            orchard_anchor: None,
            ironwood_anchor: Some(anchor),
            orchard_padding: BundlePadding::DEFAULT,
            ironwood_padding: BundlePadding::DEFAULT,
        },
    );
    builder.add_ironwood_spend::<zip317::FeeRule>(sender.fvk.clone(), note, path).unwrap();

    let mut outputs = Vec::new();
    let mut external_ua: Option<(orchard::Address, String)> = None;
    match &dest {
        Dest::Shielded(ua, recipient) => {
            let memo_bytes = MemoBytes::from(Memo::from_bytes(memo.as_bytes()).unwrap());
            builder
                .add_ironwood_output::<zip317::FeeRule>(
                    Some(sender.fvk.to_ovk(Scope::External)),
                    *recipient,
                    Zatoshis::const_from_u64(amount),
                    memo_bytes,
                )
                .unwrap();
            external_ua = Some((*recipient, ua.clone()));
            outputs.push(OutputSummary { pool: "ironwood", kind: "external", address: ua.clone(), value: amount, memo: memo.into() });
        }
        Dest::Transparent(taddr_s, taddr) => {
            builder.add_transparent_output(taddr, Zatoshis::const_from_u64(amount)).unwrap();
            outputs.push(OutputSummary { pool: "transparent", kind: "external", address: taddr_s.clone(), value: amount, memo: String::new() });
        }
    }
    builder
        .add_ironwood_output::<zip317::FeeRule>(
            Some(sender.fvk.to_ovk(Scope::Internal)),
            change_addr,
            Zatoshis::const_from_u64(change),
            MemoBytes::empty(),
        )
        .unwrap();
    outputs.push(OutputSummary { pool: "ironwood", kind: "change", address: String::new(), value: change, memo: String::new() });

    let PcztResult { pczt_parts, .. } = builder
        .build_for_pczt(rng(tag + 100), &zip317::FeeRule::standard())
        .unwrap();
    let pczt = Creator::build_from_parts(pczt_parts).unwrap();
    let pczt = IoFinalizer::new(pczt).finalize_io(rng(tag + 200)).unwrap();

    // Updater: what a wallet adds for an external signer.
    let fp = SeedFingerprint::from_seed(&sender.seed).unwrap().to_bytes();
    let path = vec![
        zip32::ChildIndex::hardened(32).index(),
        zip32::ChildIndex::hardened(TestNetwork.network_type().coin_type()).index(),
        zip32::ChildIndex::hardened(0).index(),
    ];
    let pczt = Updater::new(pczt)
        .update_ironwood_with(|mut u| {
            let n = u.bundle().actions().len();
            for i in 0..n {
                let action = &u.bundle().actions()[i];
                let needs_sig = action.spend().spend_auth_sig().is_none();
                let out_recipient = *action.output().recipient();
                u.update_action_with(i, |mut a| {
                    if needs_sig {
                        a.set_spend_zip32_derivation(orchard::pczt::Zip32Derivation::parse(fp, path.clone()).unwrap());
                    }
                    if let (Some((r, ua)), Some(rb)) = (&external_ua, out_recipient) {
                        if rb.to_raw_address_bytes() == r.to_raw_address_bytes() {
                            a.set_output_user_address(ua.clone());
                        }
                    }
                    Ok(())
                })?;
            }
            Ok(())
        })
        .unwrap()
        .finish();

    Built { pczt, outputs, fee }
}

/// An Orchard-pool note (note plaintext version 2) and a one-leaf tree, as
/// `ironwood_note`. It is minted in a pre-NU6.3 bundle (protocol V2), whose
/// outputs need no matching spend; the note itself is the same in any.
fn orchard_note(
    fvk: &FullViewingKey,
    value: u64,
    tag: u64,
) -> (orchard::Note, orchard::Anchor, orchard::tree::MerklePath) {
    let mut rng = rng(tag);
    let recipient = fvk.address_at(0u32, Scope::External);
    let version = orchard::bundle::BundleVersion::orchard_v2();
    let mut b = orchard::builder::Builder::new(
        orchard::builder::BundleType::DEFAULT,
        version,
        version.default_flags(),
        orchard::Anchor::empty_tree(),
    )
    .unwrap();
    b.add_output(None, recipient, orchard::value::NoteValue::from_raw(value), Memo::Empty.encode().into_bytes())
        .unwrap();
    let (bundle, meta) = b.build::<i64>(&mut rng).unwrap().unwrap();
    let action = bundle.actions().get(meta.output_action_index(0).unwrap()).unwrap();
    let domain = orchard::note_encryption::OrchardDomain::for_action(action);
    let (note, _, _) = try_note_decryption(&domain, &fvk.to_ivk(Scope::External).prepare(), action).unwrap();

    let cmx: orchard::note::ExtractedNoteCommitment = note.commitment().into();
    let leaf = MerkleHashOrchard::from_cmx(&cmx);
    let mut tree = ShardTree::<_, 32, 16>::new(MemoryShardStore::<MerkleHashOrchard, u32>::empty(), 100);
    tree.append(leaf, Retention::Marked).unwrap();
    tree.checkpoint(9_999_999).unwrap();
    let path = tree.witness_at_checkpoint_depth(0.into(), 0).unwrap().unwrap();
    let anchor = path.root(leaf);
    (note, anchor.into(), path.into())
}

/// The Updater pass of `build_send` for one Orchard-protocol bundle: ZIP 32
/// derivation on the spends we sign, user_address on the external output.
fn annotate(
    mut u: orchard::pczt::Updater<'_>,
    fp: [u8; 32],
    path: &[u32],
    external_ua: &Option<(orchard::Address, String)>,
) -> Result<(), orchard::pczt::UpdaterError> {
    let n = u.bundle().actions().len();
    for i in 0..n {
        let action = &u.bundle().actions()[i];
        let needs_sig = action.spend().spend_auth_sig().is_none() && action.spend().value().map_or(false, |v| v.inner() > 0);
        let out_recipient = *action.output().recipient();
        u.update_action_with(i, |mut a| {
            if needs_sig {
                a.set_spend_zip32_derivation(orchard::pczt::Zip32Derivation::parse(fp, path.to_vec()).unwrap());
            }
            if let (Some((r, ua)), Some(rb)) = (external_ua, out_recipient) {
                if rb.to_raw_address_bytes() == r.to_raw_address_bytes() {
                    a.set_output_user_address(ua.clone());
                }
            }
            Ok(())
        })?;
    }
    Ok(())
}

/// Spend an Orchard-pool note to an external UA (with memo) plus change, in
/// the Ironwood pool from NU6.3 on (a migration out of Orchard) or in the
/// Orchard pool before it.
#[allow(clippy::too_many_arguments)]
fn build_orchard_send(
    tag: u64,
    height: u32,
    sender: &Account,
    input: u64,
    ua: &str,
    recipient: orchard::Address,
    amount: u64,
    memo: &str,
    fee: u64,
) -> Built {
    let ironwood = height >= H_NU6_3;
    let (note, anchor, path) = orchard_note(&sender.fvk, input, tag);
    let change = input - amount - fee;
    let change_addr = sender.fvk.address_at(0u32, Scope::Internal);

    let mut builder = Builder::new(
        TestNetwork,
        BlockHeight::from_u32(height),
        BuildConfig::Standard {
            sapling_anchor: None,
            orchard_anchor: Some(anchor),
            ironwood_anchor: ironwood.then(orchard::Anchor::empty_tree),
            orchard_padding: BundlePadding::DEFAULT,
            ironwood_padding: BundlePadding::DEFAULT,
        },
    );
    builder.add_orchard_spend::<zip317::FeeRule>(sender.fvk.clone(), note, path).unwrap();
    let pool = if ironwood { "ironwood" } else { "orchard" };
    let memo_bytes = MemoBytes::from(Memo::from_bytes(memo.as_bytes()).unwrap());
    let ovk_ext = Some(sender.fvk.to_ovk(Scope::External));
    let ovk_int = Some(sender.fvk.to_ovk(Scope::Internal));
    if ironwood {
        builder.add_ironwood_output::<zip317::FeeRule>(ovk_ext, recipient, Zatoshis::const_from_u64(amount), memo_bytes).unwrap();
        builder.add_ironwood_output::<zip317::FeeRule>(ovk_int, change_addr, Zatoshis::const_from_u64(change), MemoBytes::empty()).unwrap();
    } else {
        builder.add_orchard_output::<zip317::FeeRule>(ovk_ext, recipient, Zatoshis::const_from_u64(amount), memo_bytes).unwrap();
        builder.add_orchard_output::<zip317::FeeRule>(ovk_int, change_addr, Zatoshis::const_from_u64(change), MemoBytes::empty()).unwrap();
    }
    let outputs = vec![
        OutputSummary { pool, kind: "external", address: ua.into(), value: amount, memo: memo.into() },
        OutputSummary { pool, kind: "change", address: String::new(), value: change, memo: String::new() },
    ];

    let PcztResult { pczt_parts, .. } = builder
        .build_for_pczt(rng(tag + 100), &zip317::FeeRule::standard())
        .unwrap();
    let pczt = Creator::build_from_parts(pczt_parts).unwrap();
    let pczt = IoFinalizer::new(pczt).finalize_io(rng(tag + 200)).unwrap();

    let fp = SeedFingerprint::from_seed(&sender.seed).unwrap().to_bytes();
    let zpath = vec![
        zip32::ChildIndex::hardened(32).index(),
        zip32::ChildIndex::hardened(TestNetwork.network_type().coin_type()).index(),
        zip32::ChildIndex::hardened(0).index(),
    ];
    let external_ua = Some((recipient, ua.to_string()));
    let pczt = Updater::new(pczt)
        .update_orchard_with(|u| annotate(u, fp, &zpath, &external_ua))
        .unwrap()
        .update_ironwood_with(|u| annotate(u, fp, &zpath, &external_ua))
        .unwrap()
        .finish();

    Built { pczt, outputs, fee }
}

/// The v2 compact signer view, as `zcash_client_backend`'s
/// `redact_pczt_for_signer(.., SignerView::Compact)` builds it for both
/// Orchard-protocol pools: no proof, bsk, witnesses or dummy keys; cv_net, cmx
/// and decryptable ciphertexts compacted; v6 anchors absent. These vectors are
/// unproven and carry no proprietary fields, so those steps are no-ops here.
fn compact_view(b: &Built) -> Built {
    let v6 = *b.pczt.global().tx_version() == 6;
    let bundle = |mut r: pczt::roles::redactor::orchard::OrchardRedactor<'_>| {
        r.clear_zkproof();
        r.clear_bsk();
        r.redact_actions(|mut a| {
            a.clear_spend_witness();
            a.clear_spend_dummy_sk();
        });
        r.compact_resolvable_fields();
        if v6 {
            r.clear_anchor();
        }
    };
    let pczt = Redactor::new(b.pczt.clone())
        .redact_sapling_with(|mut r| {
            r.clear_bsk();
            if v6 {
                r.clear_anchor();
            }
        })
        .redact_orchard_with(bundle)
        .redact_ironwood_with(bundle)
        .finish();
    Built { pczt, outputs: b.outputs.clone(), fee: b.fee }
}

fn write(dir: &str, name: &str, bytes: &[u8]) -> String {
    let file = format!("pczt/{name}.pczt");
    fs::write(format!("{dir}/{file}"), bytes).unwrap();
    file
}

/// Replace the unique occurrence of `needle` in `hay` with `with` (same length).
fn patch_unique(hay: &[u8], needle: &[u8], with: &[u8]) -> Vec<u8> {
    assert_eq!(needle.len(), with.len());
    let hits: Vec<usize> = hay.windows(needle.len()).enumerate().filter(|(_, w)| *w == needle).map(|(i, _)| i).collect();
    assert_eq!(hits.len(), 1, "patch target must be unique");
    let mut out = hay.to_vec();
    out[hits[0]..hits[0] + needle.len()].copy_from_slice(with);
    out
}

fn code(expect: &str) -> i32 {
    match expect {
        "ok" => 0,
        "parse_error" => -10,
        "unsupported_branch" => -11,
        "spend_not_ours" => -12,
        "output_commitment_mismatch" => -13,
        "user_address_mismatch" => -14,
        "value_balance" => -15,
        "unsupported_bundle" => -16,
        "network_mismatch" => -17,
        other => panic!("unknown expectation {other}"),
    }
}

fn c_header(vs: &[PcztVector]) -> String {
    use std::fmt::Write as _;
    let pool = |p: &str| match p { "transparent" => 0, "orchard" => 1, _ => 2 };
    let kind = |k: &str| match k { "external" => 0, "change" => 1, _ => 2 };
    let mut h = String::from("/* Generated by vectors/gen (pczt-vectors). Do not edit. */\n#pragma once\n#include <stdint.h>\n\n");
    h.push_str("typedef struct { uint8_t pool, kind; uint64_t value; const char *address, *memo; } zv_pczt_output;\n");
    h.push_str("typedef struct {\n    const char *name, *file, *mnemonic, *passphrase;\n    uint32_t account; uint8_t network;\n    int32_t expect;\n    const char *sighash; /* hex, NULL for refusals */\n    uint32_t branch_id; uint64_t fee;\n    uint8_t n_outputs; zv_pczt_output outputs[4];\n} zv_pczt_vector;\n\n");
    h.push_str("static const zv_pczt_vector ZV_PCZT[] = {\n");
    for v in vs {
        write!(h, "  {{ {:?}, {:?}, {:?}, {:?}, {}, {}, {},\n    ", v.name, format!("../../vectors/{}", v.file), v.signer_mnemonic, v.signer_passphrase, v.account, u8::from(v.network == "test"), code(&v.expect)).unwrap();
        match &v.sighash { Some(x) => write!(h, "{x:?}, ").unwrap(), None => h.push_str("0, ") }
        let bid = v.consensus_branch_id.as_deref().map(|b| u32::from_str_radix(b, 16).unwrap()).unwrap_or(0);
        write!(h, "0x{bid:08x}, {}, {}, {{", v.fee.unwrap_or(0), v.outputs.len()).unwrap();
        for o in &v.outputs {
            write!(h, "\n      {{ {}, {}, {}, {:?}, {:?} }},", pool(o.pool), kind(o.kind), o.value, o.address, o.memo).unwrap();
        }
        h.push_str(" } },\n");
    }
    h.push_str("};\n#define ZV_PCZT_COUNT (sizeof ZV_PCZT / sizeof ZV_PCZT[0])\n");
    h
}

fn varint(mut v: u64) -> Vec<u8> {
    let mut out = Vec::new();
    loop {
        let b = (v & 0x7f) as u8;
        v >>= 7;
        if v == 0 {
            out.push(b);
            return out;
        }
        out.push(b | 0x80);
    }
}

fn main() {
    let dir = std::env::args().nth(1).unwrap_or_else(|| "..".into());
    fs::create_dir_all(format!("{dir}/pczt")).unwrap();

    let sender = account(SENDER);
    let rcpt = account(RECIPIENT);
    let rcpt_addr = rcpt.fvk.address_at(0u32, Scope::External);
    // The recipient's Orchard-only UA, as a wallet would display it.
    let rcpt_ua = {
        use zcash_address::unified::{self, Encoding};
        unified::Address::try_from_items(
            unified::Revision::R0,
            vec![unified::Uitem::Data(unified::Receiver::Orchard(rcpt_addr.to_raw_address_bytes()))],
        )
        .unwrap()
        .encode(&zcash_protocol::consensus::NetworkType::Test)
    };
    let rcpt_taddr = TransparentAddress::PublicKeyHash([0x42; 20]);
    let rcpt_taddr_s = zcash_keys::address::Address::Transparent(rcpt_taddr).encode(&TestNetwork);

    let mut vs = Vec::new();
    let push_ok = |name: &str, desc: &str, b: &Built, vs: &mut Vec<PcztVector>| {
        let bytes = b.pczt.clone().serialize().unwrap();
        let signer = Signer::new(b.pczt.clone()).unwrap();
        vs.push(PcztVector {
            name: name.into(),
            description: desc.into(),
            file: write(&dir, name, &bytes),
            signer_mnemonic: SENDER.into(),
            signer_passphrase: String::new(),
            account: 0,
            network: "test",
            expect: "ok".into(),
            sighash: Some(hex::encode(signer.shielded_sighash())),
            consensus_branch_id: Some(format!("{:08x}", u32::from(*b.pczt.global().consensus_branch_id()))),
            fee: Some(b.fee),
            outputs: b.outputs.clone(),
        });
        bytes
    };
    let refuse = |name: &str, desc: &str, bytes: &[u8], mnemonic: &str, code: &str| PcztVector {
        name: name.into(),
        description: desc.into(),
        file: write(&dir, name, bytes),
        signer_mnemonic: mnemonic.into(),
        signer_passphrase: String::new(),
        account: 0,
        network: "test",
        expect: code.into(),
        sighash: None,
        consensus_branch_id: None,
        fee: None,
        outputs: vec![],
    };

    // 1. Ironwood -> external Ironwood address with memo, plus change.
    let a = build_send(1, H_NU6_3, &sender, 1_000_000, Dest::Shielded(rcpt_ua.clone(), rcpt_addr), 400_000, "LeekWallet Zcash test", 10_000);
    let a_bytes = push_ok("iw_send_shielded", "Ironwood note to an external UA with memo, change back to self (v6, NU6.3)", &a, &mut vs);

    // 2. Ironwood -> transparent (the shape of a NEAR 1Click deposit).
    let b = build_send(2, H_NU6_3, &sender, 1_000_000, Dest::Transparent(rcpt_taddr_s.clone(), rcpt_taddr), 300_000, "", 15_000);
    push_ok("iw_send_transparent", "Ironwood note to a tm address, change back to self (v6, NU6.3)", &b, &mut vs);

    // 3. The same kind of send under the NU7 branch id (v6, same rules).
    let c = build_send(3, H_NU7, &sender, 1_000_000, Dest::Shielded(rcpt_ua.clone(), rcpt_addr), 250_000, "NU7 branch", 10_000);
    push_ok("iw_send_nu7", "Ironwood note to an external UA under NU7 (v6, branch 0x77190ad9)", &c, &mut vs);

    // 4. An Orchard-pool note spent into Ironwood: the Orchard bundle carries
    // the spend paired with a fabricated zero-valued output (ZIP 326).
    let d = build_orchard_send(4, H_NU6_3, &sender, 2_000_000, &rcpt_ua, rcpt_addr, 500_000, "out of Orchard", 20_000);
    push_ok("orchard_to_ironwood", "Orchard note to an external Ironwood UA with memo, Ironwood change (v6, NU6.3)", &d, &mut vs);

    // 5. Vector 1 in the v2 compact form: cv_net and cmx redacted, decryptable
    // ciphertexts replaced by their memo plaintext, the anchor left absent.
    let compact = Redactor::new(a.pczt.clone())
        .redact_ironwood_with(|mut r| {
            r.compact_resolvable_fields();
            r.clear_anchor();
        })
        .finish();
    let e = Built { pczt: compact, outputs: a.outputs.clone(), fee: a.fee };
    let e_bytes = push_ok("iw_send_shielded_compact", "Vector iw_send_shielded with cv_net, cmx and memo ciphertext compacted (v2)", &e, &mut vs);

    // 6-8. The other valid vectors as a companion sends them: the compact
    // signer view (no witnesses either), Orchard and Ironwood bundles alike.
    push_ok("iw_send_transparent_compact", "Vector iw_send_transparent in the compact signer view (v2)", &compact_view(&b), &mut vs);
    push_ok("iw_send_nu7_compact", "Vector iw_send_nu7 in the compact signer view (v2)", &compact_view(&c), &mut vs);
    push_ok("orchard_to_ironwood_compact", "Vector orchard_to_ironwood in the compact signer view, both bundles compacted (v2)", &compact_view(&d), &mut vs);

    // 9. The memo is in no commitment the compact view carries (cmx and epk
    // do not cover it), so a changed memo is a different, self-consistent
    // transaction: the device must show the new memo and sign the new
    // sighash, which the host then refuses to apply to its own PCZT.
    let memo_b = b"LeekWallet Zcash test";
    let memo_t = b"LeekWallet Zcash TEST";
    let g_bytes = patch_unique(&e_bytes, memo_b, memo_t);
    let mut g_outputs = a.outputs.clone();
    g_outputs[0].memo = String::from_utf8(memo_t.to_vec()).unwrap();
    let g = Built { pczt: Pczt::parse(&g_bytes).unwrap(), outputs: g_outputs, fee: a.fee };
    let g_round = push_ok("iw_send_shielded_compact_memo_changed", "Vector iw_send_shielded_compact with the memo plaintext changed: a different transaction, shown and signed as such", &g, &mut vs);
    assert_eq!(g_round, g_bytes, "the patched compact PCZT reserializes unchanged");
    assert_ne!(vs.last().unwrap().sighash, vs.iter().find(|v| v.name == "iw_send_shielded_compact").unwrap().sighash);

    // Refusals.
    vs.push(refuse("r_foreign_spend", "Valid PCZT, but the spend belongs to another seed", &a_bytes, RECIPIENT, "spend_not_ours"));

    let rb = rcpt_addr.to_raw_address_bytes();
    let mut needle = rb.to_vec();
    needle.push(0x01);
    needle.extend(varint(400_000));
    let mut with = rb.to_vec();
    with.push(0x01);
    with.extend(varint(400_001));
    vs.push(refuse("r_output_value_tampered", "External output value changed after the note commitment was made", &patch_unique(&a_bytes, &needle, &with), SENDER, "output_commitment_mismatch"));

    let mut rb2 = rb;
    rb2[42] ^= 1;
    let mut needle = rb.to_vec();
    needle.push(0x01);
    let mut with = rb2.to_vec();
    with.push(0x01);
    vs.push(refuse("r_output_recipient_tampered", "External output recipient changed after the note commitment was made", &patch_unique(&a_bytes, &needle, &with), SENDER, "output_commitment_mismatch"));

    let ua_b = rcpt_ua.as_bytes();
    let mut ua_t = ua_b.to_vec();
    let last = ua_t.len() - 1;
    ua_t[last] = if ua_t[last] == b'q' { b'p' } else { b'q' };
    vs.push(refuse("r_user_address_tampered", "Displayed user_address no longer matches the output's receiver", &patch_unique(&a_bytes, ua_b, &ua_t), SENDER, "user_address_mismatch"));

    vs.push(refuse("r_truncated", "PCZT cut short", &a_bytes[..a_bytes.len() / 2], SENDER, "parse_error"));
    let mut magic = a_bytes.clone();
    magic[0] = b'X';
    vs.push(refuse("r_bad_magic", "Wrong magic bytes", &magic, SENDER, "parse_error"));

    // An Ironwood spend on NU6.2 cannot exist; build an old-branch PCZT instead by
    // targeting NU6.2 with a transparent-only... not possible without funds, so use
    // the branch-id patch on a valid PCZT: 0x37a5165b -> 0x5437f330 (NU6.2).
    let bid = |v: u32| varint(v as u64);
    let old = patch_unique(&a_bytes, &bid(0x37a5165b), &{
        let mut x = bid(0x5437f330);
        x.resize(bid(0x37a5165b).len(), 0);
        x
    });
    let _ = H_NU6_2;
    vs.push(refuse("r_old_branch", "Consensus branch id rewritten to NU6.2", &old, SENDER, "unsupported_branch"));

    // With cmx and cv_net both resolved from the fields, a changed value is a
    // different but self-consistent action; only the value balance catches it.
    let mut needle = rb.to_vec();
    needle.push(0x01);
    needle.extend(varint(400_000));
    let mut with = rb.to_vec();
    with.push(0x01);
    with.extend(varint(400_001));
    vs.push(refuse("r_compact_value_tampered", "Compact PCZT: external output value changed, cmx and cv_net absent", &patch_unique(&e_bytes, &needle, &with), SENDER, "value_balance"));

    // The v1 encoding: an Orchard v5 PCZT from before NU6.3 (the only shape v1
    // can carry). Parsed by both backends, refused for its branch.
    let f = build_orchard_send(6, H_NU6_2, &sender, 1_000_000, &rcpt_ua, rcpt_addr, 200_000, "v1", 10_000);
    let f_bytes = f.pczt.clone().serialize().unwrap();
    assert_eq!(&f_bytes[4..8], &1u32.to_le_bytes(), "a v5 Orchard PCZT serializes as v1");
    vs.push(refuse("r_v1_nu6_2", "v1-encoded Orchard PCZT on the NU6.2 branch", &f_bytes, SENDER, "unsupported_branch"));

    // Compact output fields that no longer agree. With cmx absent it is
    // recomputed from whatever recipient is carried, so the commitment cannot
    // catch a swap; the displayed user_address and the carried epk must.
    let mut needle = rb.to_vec();
    needle.push(0x01);
    let mut with = rb2.to_vec();
    with.push(0x01);
    vs.push(refuse("r_compact_recipient_tampered", "Compact PCZT: external output pk_d changed, cmx absent; user_address no longer matches", &patch_unique(&e_bytes, &needle, &with), SENDER, "user_address_mismatch"));

    // A new diversifier moves g_d, so the carried epk = [esk] g_d no longer
    // matches what re-encrypting the memo plaintext gives.
    let mut rb3 = rb;
    rb3[0] ^= 1;
    let mut with = rb3.to_vec();
    with.push(0x01);
    vs.push(refuse("r_compact_diversifier_tampered", "Compact PCZT: external output diversifier changed; memo re-encryption gives a different epk", &patch_unique(&e_bytes, &needle, &with), SENDER, "parse_error"));

    fs::write(format!("{dir}/pczt.json"), serde_json::to_string_pretty(&vs).unwrap() + "\n").unwrap();
    fs::write(format!("{dir}/pczt_vectors.h"), c_header(&vs)).unwrap();
    eprintln!("wrote {} PCZT vectors to {dir}", vs.len());
}
