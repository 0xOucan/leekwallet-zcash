//! Orchard/Ironwood proving, the same work the companion does in `pczt::roles::prover`.
//!
//! A proof over an output-only bundle (one real output plus the padding
//! action `BundleType::DEFAULT` adds, so 2 actions) costs what a minimal
//! send's Ironwood bundle costs; a 1-in/2-out send also has 2 actions.
//! Spends do not change the circuit size, only the witness.

use orchard::{
    builder::{Builder, BundleType},
    bundle::BundleVersion,
    circuit::{OrchardCircuitVersion, ProvingKey, VerifyingKey},
    keys::{FullViewingKey, Scope, SpendingKey},
    value::NoteValue,
    Anchor, Bundle,
};
use rand::{rand_core::UnwrapErr, rngs::SysRng};

/// The circuit our chain proves with: NU6.3, for both Orchard and Ironwood.
pub const CIRCUIT: OrchardCircuitVersion = OrchardCircuitVersion::PostNu6_3;

pub fn build_pk() -> ProvingKey {
    ProvingKey::build(CIRCUIT)
}

pub fn build_vk() -> VerifyingKey {
    VerifyingKey::build(CIRCUIT)
}

/// Builds an Ironwood (v3) bundle with `outputs` outputs, proves it, and
/// returns (actions, proof bytes).
pub fn prove(pk: &ProvingKey, outputs: usize) -> (usize, Vec<u8>) {
    let rng = UnwrapErr(SysRng);
    let sk = SpendingKey::from_bytes([7; 32]).unwrap();
    let recipient = FullViewingKey::from(&sk).address_at(0u32, Scope::External);
    let version = BundleVersion::ironwood_v3();
    let mut builder = Builder::new(
        BundleType::DEFAULT,
        version,
        version.default_flags(),
        Anchor::from_bytes([0; 32]).unwrap(),
    )
    .unwrap();
    for _ in 0..outputs {
        builder
            .add_output(None, recipient, NoteValue::from_raw(10), [0; 512])
            .unwrap();
    }
    let bundle: Bundle<_, i64> = builder.build(rng).unwrap().unwrap().0;
    let actions = bundle.actions().len();
    let instances: Vec<_> = bundle
        .actions()
        .iter()
        .map(|a| a.to_instance(*bundle.flags(), *bundle.anchor()))
        .collect();
    let proof = bundle.authorization().create_proof(pk, &instances, rng).unwrap();
    (actions, proof.as_ref().to_vec())
}
