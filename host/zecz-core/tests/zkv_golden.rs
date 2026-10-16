//! The .zkv format against the device's C implementation: the golden file
//! in vectors/zkv/ was written by firmware/sim/test_zkv; this crate must
//! produce the same bytes from the same inputs, and open it.

use std::path::PathBuf;

use zecz::zkv::{self, Contents};

const ABANDON_ART: &str = "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon \
                           abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon \
                           abandon art";

fn golden() -> Vec<u8> {
    std::fs::read(PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../vectors/zkv/golden-v1.zkv")).unwrap()
}

fn golden_contents() -> Contents {
    let m = bip39::Mnemonic::parse_normalized(ABANDON_ART).unwrap();
    Contents {
        entropy: m.to_entropy(),
        birthday: 2_900_000,
        account: 0,
        flags: zkv::FLAG_USE_QSK,
        label: "LeekWallet test".into(),
    }
}

#[test]
fn rust_reproduces_the_devices_golden_file() {
    let salt: [u8; 16] = std::array::from_fn(|i| 0x10 + i as u8);
    let nonce: [u8; 12] = std::array::from_fn(|i| 0xA0 + i as u8);
    let mine = zkv::seal_with(&golden_contents(), "LeekWallet zkv test", zkv::ITERATIONS_MIN, salt, nonce).unwrap();
    assert_eq!(hex::encode(&mine), hex::encode(golden()));
}

#[test]
fn rust_opens_the_devices_golden_file() {
    let c = zkv::open(&golden(), "LeekWallet zkv test").unwrap();
    assert_eq!(c, golden_contents());
    assert!(zkv::open(&golden(), "LeekWallet zkv tesT").is_err());
}

#[test]
fn every_bit_flip_is_refused() {
    let g = golden();
    for i in 0..g.len() * 8 {
        let mut t = g.clone();
        t[i / 8] ^= 1 << (i % 8);
        assert!(zkv::open(&t, "LeekWallet zkv test").is_err(), "flip {i} accepted");
    }
}

#[test]
fn fresh_seals_round_trip() {
    let c = golden_contents();
    let a = zkv::seal(&c, "another passphrase", zkv::ITERATIONS_MIN).unwrap();
    let b = zkv::seal(&c, "another passphrase", zkv::ITERATIONS_MIN).unwrap();
    assert_ne!(a, b, "salt and nonce are fresh");
    assert_eq!(zkv::open(&a, "another passphrase").unwrap(), c);
}
