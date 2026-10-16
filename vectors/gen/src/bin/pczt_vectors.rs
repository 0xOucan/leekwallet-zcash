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

/// A transparent input as the device must show it ("from your t-address").
#[derive(Serialize, Clone)]
struct TInputSummary {
    address: String,
    value: u64,
    scope: u32,
    index: u32,
    /// ZIP 244 transparent sighash of this input (SIGHASH_ALL).
    sighash: String,
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
    /// Left out when empty, so the shielded-only entries are unchanged.
    #[serde(skip_serializing_if = "Vec::is_empty")]
    t_inputs: Vec<TInputSummary>,
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
    t_inputs: Vec<TInputSummary>,
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

    Built { pczt, outputs, fee, t_inputs: vec![] }
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

    Built { pczt, outputs, fee, t_inputs: vec![] }
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
    Built { pczt, outputs: b.outputs.clone(), fee: b.fee, t_inputs: b.t_inputs.clone() }
}

/// A P2PKH coin of the sender at m/44'/1'/0'/scope/index.
struct TCoin {
    scope: u32,
    index: u32,
    value: u64,
}

fn t_account_key(a: &Account) -> zcash_transparent::keys::AccountPrivKey {
    zcash_transparent::keys::AccountPrivKey::from_seed(&TestNetwork, &a.seed, AccountId::ZERO).unwrap()
}

fn t_secret(a: &Account, scope: u32, index: u32) -> secp256k1::SecretKey {
    use zcash_transparent::keys::{NonHardenedChildIndex, TransparentKeyScope};
    t_account_key(a)
        .derive_secret_key(TransparentKeyScope::custom(scope).unwrap(), NonHardenedChildIndex::from_index(index).unwrap())
        .unwrap()
}

fn t_addr_string(pk: &secp256k1::PublicKey) -> String {
    zcash_keys::address::Address::Transparent(TransparentAddress::from_pubkey(pk)).encode(&TestNetwork)
}

fn bip44_path(account: u32, scope: u32, index: u32) -> Vec<u32> {
    vec![44 | 0x8000_0000, TestNetwork.network_type().coin_type() | 0x8000_0000, account | 0x8000_0000, scope, index]
}

/// How the Updater describes each transparent input (the refusal vectors
/// change it).
#[derive(Clone, Copy, PartialEq)]
enum TDeriv {
    /// The input's own key and path, as zcash_client_backend adds them.
    Ours,
    /// No bip32_derivation at all.
    Missing,
    /// The right key, but a path one address index off.
    WrongIndex,
    /// The right key, but account 1 in the path.
    WrongAccount,
}

/// Spend transparent coins of `sender` (and optionally one Ironwood note) to
/// an external UA with a memo, or, with `dest` None, shield everything to the
/// sender's own internal Ironwood address. The Updater pass is what
/// `zcash_client_backend` adds: a BIP 44 derivation per transparent input.
/// `ext_ovk` encrypts the external output's out_ciphertext. With `foreign`,
/// the first coin pays to the recipient's key while the derivation still
/// names ours.
#[allow(clippy::too_many_arguments)]
fn build_t_send(
    tag: u64,
    sender: &Account,
    coins: &[TCoin],
    note: Option<u64>,
    dest: Option<(&str, orchard::Address, u64, &str)>,
    ext_ovk: Option<orchard::keys::OutgoingViewingKey>,
    fee: u64,
    deriv: TDeriv,
    foreign: Option<&Account>,
) -> Built {
    let fvk = &sender.fvk;
    let spent = note.map(|v| ironwood_note(fvk, v, tag));
    let anchor = spent.as_ref().map_or(orchard::Anchor::empty_tree(), |s| s.1);
    let mut builder = Builder::new(
        TestNetwork,
        BlockHeight::from_u32(H_NU6_3),
        BuildConfig::Standard {
            sapling_anchor: None,
            orchard_anchor: None,
            ironwood_anchor: Some(anchor),
            orchard_padding: BundlePadding::DEFAULT,
            ironwood_padding: BundlePadding::DEFAULT,
        },
    );
    let mut t_inputs = Vec::new();
    let mut total = 0u64;
    for (i, c) in coins.iter().enumerate() {
        let owner = if i == 0 { foreign.unwrap_or(sender) } else { sender };
        let pk = t_secret(owner, c.scope, c.index).public_key();
        let script = TransparentAddress::from_pubkey(&pk).script();
        let mut txid = [0u8; 32];
        txid[0] = tag as u8;
        txid[1] = i as u8;
        txid[31] = 0x7a;
        builder
            .add_transparent_p2pkh_input(
                pk,
                zcash_transparent::bundle::OutPoint::new(txid, i as u32),
                zcash_transparent::bundle::TxOut::new(Zatoshis::const_from_u64(c.value), (&script).into()),
            )
            .unwrap();
        total += c.value;
        t_inputs.push(TInputSummary { address: t_addr_string(&pk), value: c.value, scope: c.scope, index: c.index, sighash: String::new() });
    }
    if let Some((n, _, path)) = spent {
        builder.add_ironwood_spend::<zip317::FeeRule>(fvk.clone(), n, path).unwrap();
        total += note.unwrap();
    }

    let mut outputs = Vec::new();
    let mut external_ua: Option<(orchard::Address, String)> = None;
    let mut change = total - fee;
    if let Some((ua, recipient, amount, memo)) = dest {
        let memo_bytes = MemoBytes::from(Memo::from_bytes(memo.as_bytes()).unwrap());
        builder
            .add_ironwood_output::<zip317::FeeRule>(ext_ovk, recipient, Zatoshis::const_from_u64(amount), memo_bytes)
            .unwrap();
        external_ua = Some((recipient, ua.to_string()));
        outputs.push(OutputSummary { pool: "ironwood", kind: "external", address: ua.into(), value: amount, memo: memo.into() });
        change -= amount;
    }
    // Change (or the shielded funds) to the internal address. A wallet's
    // Sender OVK policy leaves change without an OVK.
    builder
        .add_ironwood_output::<zip317::FeeRule>(None, fvk.address_at(0u32, Scope::Internal), Zatoshis::const_from_u64(change), MemoBytes::empty())
        .unwrap();
    outputs.push(OutputSummary { pool: "ironwood", kind: "change", address: String::new(), value: change, memo: String::new() });

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
    let pczt = Updater::new(pczt)
        .update_ironwood_with(|u| annotate(u, fp, &zpath, &external_ua))
        .unwrap()
        .update_transparent_with(|mut u| {
            for (i, c) in coins.iter().enumerate() {
                let pk = t_secret(sender, c.scope, c.index).public_key();
                let path = match deriv {
                    TDeriv::Ours | TDeriv::Missing => bip44_path(0, c.scope, c.index),
                    TDeriv::WrongIndex => bip44_path(0, c.scope, c.index + 1),
                    TDeriv::WrongAccount => bip44_path(1, c.scope, c.index),
                };
                if deriv == TDeriv::Missing {
                    continue;
                }
                u.update_input_with(i, |mut iu| {
                    iu.set_bip32_derivation(pk.serialize(), zcash_transparent::pczt::Bip32Derivation::parse(fp, path).unwrap());
                    Ok(())
                })?;
            }
            Ok(())
        })
        .unwrap()
        .finish();
    // The signer view: no dummy spend keys (the IO Finalizer used them) and
    // no witnesses, as redact_pczt_for_signer leaves it.
    let pczt = Redactor::new(pczt)
        .redact_ironwood_with(|mut r| {
            r.redact_actions(|mut a| {
                a.clear_spend_witness();
                a.clear_spend_dummy_sk();
            });
        })
        .finish();
    Built { pczt, outputs, fee, t_inputs }
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
        "missing_data" => -19,
        other => panic!("unknown expectation {other}"),
    }
}

fn c_header(vs: &[PcztVector]) -> String {
    use std::fmt::Write as _;
    let pool = |p: &str| match p { "transparent" => 0, "orchard" => 1, _ => 2 };
    let kind = |k: &str| match k { "external" => 0, "change" => 1, _ => 2 };
    let mut h = String::from("/* Generated by vectors/gen (pczt-vectors). Do not edit. */\n#pragma once\n#include <stdint.h>\n\n");
    h.push_str("typedef struct { uint8_t pool, kind; uint64_t value; const char *address, *memo; } zv_pczt_output;\n");
    h.push_str("typedef struct { uint64_t value; uint32_t scope, index; const char *address, *sighash; } zv_pczt_tinput;\n");
    h.push_str("typedef struct {\n    const char *name, *file, *mnemonic, *passphrase;\n    uint32_t account; uint8_t network;\n    int32_t expect;\n    const char *sighash; /* hex, NULL for refusals */\n    uint32_t branch_id; uint64_t fee;\n    uint8_t n_outputs; zv_pczt_output outputs[4];\n    uint8_t n_tinputs; zv_pczt_tinput tinputs[4];\n} zv_pczt_vector;\n\n");
    h.push_str("static const zv_pczt_vector ZV_PCZT[] = {\n");
    for v in vs {
        write!(h, "  {{ {:?}, {:?}, {:?}, {:?}, {}, {}, {},\n    ", v.name, format!("../../vectors/{}", v.file), v.signer_mnemonic, v.signer_passphrase, v.account, u8::from(v.network == "test"), code(&v.expect)).unwrap();
        match &v.sighash { Some(x) => write!(h, "{x:?}, ").unwrap(), None => h.push_str("0, ") }
        let bid = v.consensus_branch_id.as_deref().map(|b| u32::from_str_radix(b, 16).unwrap()).unwrap_or(0);
        write!(h, "0x{bid:08x}, {}, {}, {{", v.fee.unwrap_or(0), v.outputs.len()).unwrap();
        for o in &v.outputs {
            write!(h, "\n      {{ {}, {}, {}, {:?}, {:?} }},", pool(o.pool), kind(o.kind), o.value, o.address, o.memo).unwrap();
        }
        write!(h, " }},\n    {}, {{", v.t_inputs.len()).unwrap();
        for t in &v.t_inputs {
            write!(h, "\n      {{ {}, {}, {}, {:?}, {:?} }},", t.value, t.scope, t.index, t.address, t.sighash).unwrap();
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
        let mut t_inputs = b.t_inputs.clone();
        for (i, t) in t_inputs.iter_mut().enumerate() {
            t.sighash = hex::encode(signer.transparent_sighash(i).unwrap());
        }
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
            t_inputs,
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
        t_inputs: vec![],
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
    let e = Built { pczt: compact, outputs: a.outputs.clone(), fee: a.fee, t_inputs: vec![] };
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
    let g = Built { pczt: Pczt::parse(&g_bytes).unwrap(), outputs: g_outputs, fee: a.fee, t_inputs: vec![] };
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

    // Transparent inputs (roadmap 10.1). Every one is a P2PKH coin of the
    // sender's account, with the BIP 44 derivation a wallet adds.
    let t_ovk_ext = {
        let ovks = t_account_key(&sender).to_account_pubkey().ovks_for_shielding();
        orchard::keys::OutgoingViewingKey::from(ovks.external().as_bytes())
    };

    // 1. Shield: two coins (an external and a change address) to Ironwood.
    let shield_coins = [TCoin { scope: 0, index: 0, value: 600_000 }, TCoin { scope: 1, index: 3, value: 400_000 }];
    let ts = build_t_send(20, &sender, &shield_coins, None, None, None, 20_000, TDeriv::Ours, None);
    let ts_bytes = push_ok("t_shield", "Two transparent coins of this account shielded to its Ironwood change address (v6, NU6.3)", &ts, &mut vs);

    // 2. Mixed: an Ironwood note and a transparent coin to an external UA.
    let mixed_coins = [TCoin { scope: 0, index: 2, value: 500_000 }];
    let tm = build_t_send(
        21, &sender, &mixed_coins, Some(1_000_000),
        Some((&rcpt_ua, rcpt_addr, 1_200_000, "from t and Ironwood")),
        Some(sender.fvk.to_ovk(Scope::External)), 15_000, TDeriv::Ours, None,
    );
    let tm_bytes = push_ok("t_mixed", "Ironwood note plus a transparent coin to an external UA with memo, Ironwood change (v6, NU6.3)", &tm, &mut vs);

    // 3. Transparent only, to an external UA: the memo is under the account's
    // transparent external OVK (ZIP 316), as a wallet encrypts it.
    let tovk_coins = [TCoin { scope: 2, index: 1, value: 800_000 }];
    let tv = build_t_send(
        22, &sender, &tovk_coins, None,
        Some((&rcpt_ua, rcpt_addr, 300_000, "refund via ephemeral")),
        Some(t_ovk_ext), 15_000, TDeriv::Ours, None,
    );
    push_ok("t_send_tovk", "Transparent (ephemeral-scope) coin to an external UA, memo under the transparent OVK (v6, NU6.3)", &tv, &mut vs);

    // 4-5. The compact signer view of 1 and 2.
    push_ok("t_shield_compact", "Vector t_shield in the compact signer view (v2)", &compact_view(&ts), &mut vs);
    push_ok("t_mixed_compact", "Vector t_mixed in the compact signer view (v2)", &compact_view(&tm), &mut vs);

    // Refusals of transparent inputs.
    let foreign = build_t_send(23, &sender, &[TCoin { scope: 0, index: 0, value: 600_000 }], None, None, None, 15_000, TDeriv::Ours, Some(&rcpt));
    vs.push(refuse("r_t_input_not_ours", "Transparent input pays another seed's key; its derivation names ours", &foreign.pczt.serialize().unwrap(), SENDER, "spend_not_ours"));
    let wrong = build_t_send(24, &sender, &shield_coins, None, None, None, 20_000, TDeriv::WrongIndex, None);
    vs.push(refuse("r_t_wrong_path", "Transparent input derivation path one address index off its key", &wrong.pczt.serialize().unwrap(), SENDER, "spend_not_ours"));
    let wrong = build_t_send(25, &sender, &shield_coins, None, None, None, 20_000, TDeriv::WrongAccount, None);
    vs.push(refuse("r_t_wrong_account", "Transparent input derivation path names account 1", &wrong.pczt.serialize().unwrap(), SENDER, "spend_not_ours"));
    let missing = build_t_send(26, &sender, &shield_coins, None, None, None, 20_000, TDeriv::Missing, None);
    vs.push(refuse("r_t_missing_derivation", "Transparent input without a BIP 32 derivation", &missing.pczt.serialize().unwrap(), SENDER, "missing_data"));
    vs.push(refuse("r_t_foreign_seed", "Valid shield PCZT, but the transparent coins belong to another seed", &ts_bytes, RECIPIENT, "spend_not_ours"));

    // The coin's value is committed by every sighash, so a lowered value is a
    // different transaction; once outputs exceed inputs it cannot balance.
    let TransparentAddress::PublicKeyHash(h) = TransparentAddress::from_pubkey(&t_secret(&sender, 0, 2).public_key()) else { unreachable!() };
    let script = [&[0x76, 0xa9, 0x14][..], &h, &[0x88, 0xac]].concat();
    let mut needle = varint(500_000);
    needle.push(script.len() as u8);
    needle.extend(&script);
    // A varint of the same length, so the patch keeps every other byte.
    let with = {
        let mut w = varint(400_000);
        assert_eq!(w.len(), varint(500_000).len());
        w.push(script.len() as u8);
        w.extend(&script);
        w
    };
    vs.push(refuse("r_t_value_tampered", "Transparent input value lowered by 0.001: outputs exceed inputs", &patch_unique(&tm_bytes, &needle, &with), SENDER, "value_balance"));

    // SIGHASH_NONE would let anyone redirect the outputs: refused.
    let pk = t_secret(&sender, 0, 2).public_key().serialize();
    let mut needle = vec![0x00, 0x00, 0x01, 0x01];
    needle.extend(pk);
    let mut with = vec![0x00, 0x00, 0x02, 0x01];
    with.extend(pk);
    vs.push(refuse("r_t_sighash_none", "Transparent input asks for SIGHASH_NONE", &patch_unique(&tm_bytes, &needle, &with), SENDER, "unsupported_bundle"));

    fs::write(format!("{dir}/pczt.json"), serde_json::to_string_pretty(&vs).unwrap() + "\n").unwrap();
    fs::write(format!("{dir}/pczt_vectors.h"), c_header(&vs)).unwrap();
    eprintln!("wrote {} PCZT vectors to {dir}", vs.len());
}
