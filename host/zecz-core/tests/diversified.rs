//! Diversified-address vectors for the firmware's zShowAddress check
//! (firmware/sim/test_zcash_account.c), from librustzcash: the abandon-art
//! testnet account 0 at several diversifier indices. The firmware must accept
//! each as the account's own and recover its index.

use zcash_keys::keys::{ReceiverRequirement::*, UnifiedAddressRequest, UnifiedFullViewingKey};
use zcash_protocol::consensus::TEST_NETWORK;
use zip32::DiversifierIndex;

/// ufvk of vectors/keys.json "test/abandon-art/pp0/acct0".
fn ufvk() -> UnifiedFullViewingKey {
    let keys: serde_json::Value =
        serde_json::from_str(&std::fs::read_to_string(concat!(env!("CARGO_MANIFEST_DIR"), "/../../vectors/keys.json")).unwrap()).unwrap();
    let v = keys.as_array().unwrap().iter().find(|v| v["name"] == "test/abandon-art/pp0/acct0").unwrap();
    UnifiedFullViewingKey::decode(&TEST_NETWORK, v["ufvk"].as_str().unwrap()).unwrap()
}

fn ua(index: u128, transparent: bool) -> String {
    let req = UnifiedAddressRequest::custom(Require, Omit, if transparent { Require } else { Omit }).unwrap();
    let j = DiversifierIndex::try_from(index).unwrap();
    ufvk().address(j, req).unwrap().encode(&TEST_NETWORK)
}

/// Pinned in firmware/sim/test_zcash_account.c too: (index, t-address the
/// UA carries or "", UA).
const PINNED: [(u128, &str, &str); 4] = [
    (0x1, "tmVNYZhAQMp61GkauCYV5R4eVKpxzzMZBJF", "utest1nggn4c6e0gmlr6fjlt4sel2wnph2r5ytca36uffhetvjpshjm3zy4xzeedhu56sj5qkyaafm6xq5xc75st8nj959mzwvz6qca2gzllzgfuxapwdxmcrafvpj5vm7tmed83mku2umc9g"),
    (0x7, "", "utest183eh57gnncle3r4258239e5r0hdwzsalftrwrsajxe30xggcu8h66d56nwllzcn3h2npt6edmtnx4ecrxtfrhtwy7gqx8jjd2ga528d4"),
    (0x8000_0005, "", "utest1l99gwjde6achwrewlh59cxhyup79fff5ytzmq0kw02rmcrzrtde4xcpvsqqhsj7aua6wt32z4f2fcz47r808rexmgtgj7l5pe596pzru"),
    (0x0102_0304_0506_0708_090a, "", "utest17jlmywqwc7rvr5agvxwmjhst0uumfs3yc4207ka9adxycdrykkmy3fcmsl8q03k76vr5ff4sfy5agta3h2d9dfnax20dq5f73sapazp0"),
];

#[test]
fn diversified_vectors() {
    for (j, taddr, expected) in PINNED {
        let t = !taddr.is_empty();
        assert_eq!(ua(j, t), expected, "index {j:#x}");
        if t {
            let req = UnifiedAddressRequest::custom(Require, Omit, Require).unwrap();
            let a = ufvk().address(DiversifierIndex::try_from(j).unwrap(), req).unwrap();
            let enc = zcash_keys::encoding::AddressCodec::encode(a.transparent().unwrap(), &TEST_NETWORK);
            assert_eq!(enc, taddr, "t-address at index {j:#x}");
        }
    }
}

/// A UA whose Orchard receiver is the account's but which also carries a
/// Sapling receiver: the device cannot check Sapling and must refuse it.
const WITH_SAPLING: &str = "utest1dumh7z6x5xf4ay3wnkvc60uru4hu54y55u0t3xm78v2cqs5nk0zhsqspa4dqcpcj82hmtt049f30kx5z58yzxg3d7y3lpy69pg9vsdy7hdh2qfw8jj2u6vd46ztw7tzf88lydf8p3w7nre6zenfqeayq4e4njagajwvrmelywq76akhk";

#[test]
fn sapling_vector() {
    use zcash_keys::keys::UnifiedSpendingKey;
    let mnemonic = "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon art";
    let seed = bip39::Mnemonic::parse(mnemonic).unwrap().to_seed("");
    let usk = UnifiedSpendingKey::from_seed(&TEST_NETWORK, &seed, zip32::AccountId::ZERO).unwrap();
    let req = UnifiedAddressRequest::custom(Require, Require, Omit).unwrap();
    let (ua, _) = usk.to_unified_full_viewing_key().default_address(req).unwrap();
    let s = ua.encode(&TEST_NETWORK);
    assert_eq!(s, WITH_SAPLING);
}
