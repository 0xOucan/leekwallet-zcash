//! A unified address the board can check.

use zcash_address::unified::{self, Container, Encoding, Receiver, Uitem};

use crate::error::LeekError;

/// `ua` without its Sapling receiver, for [`show_address`](crate::show_address).
///
/// The board has no Sapling key, so it refuses to show an address that
/// carries a Sapling receiver (it cannot prove that receiver is its own).
/// Wallets such as Zodl display Orchard + Sapling + P2PKH addresses; this
/// is the same address with the Sapling receiver dropped, which the board can
/// check end to end. Other receivers and metadata are kept. An address with
/// nothing the board can check left (Sapling only, or Sapling + P2PKH, which
/// ZIP 316 does not allow without a shielded receiver) is
/// [`LeekError::AddressUncheckable`]; an address that does not parse is
/// [`LeekError::Parse`].
pub fn to_checkable_ua(ua: &str) -> Result<String, LeekError> {
    let (net, revision, addr) = unified::Address::decode(ua).map_err(|_| LeekError::Parse)?;
    let items: Vec<Uitem<Receiver>> = addr
        .items_as_parsed()
        .iter()
        .filter(|i| !matches!(i, Uitem::Data(Receiver::Sapling(_))))
        .cloned()
        .collect();
    // A shielded receiver other than Sapling must remain (Orchard, or a
    // newer typecode the board may know).
    if !items.iter().any(|i| matches!(i, Uitem::Data(Receiver::Orchard(_) | Receiver::Unknown { .. }))) {
        return Err(LeekError::AddressUncheckable);
    }
    if items.len() == addr.items_as_parsed().len() {
        return Ok(ua.to_string());
    }
    let out = unified::Address::try_from_items(revision, items).map_err(|_| LeekError::AddressUncheckable)?;
    Ok(out.encode(&net))
}

#[cfg(test)]
mod tests {
    use super::*;
    use zcash_address::unified::Revision;
    use zcash_protocol::consensus::NetworkType;

    fn ua(items: Vec<Receiver>) -> String {
        unified::Address::try_from_items(Revision::R0, items.into_iter().map(Uitem::Data).collect())
            .unwrap()
            .encode(&NetworkType::Test)
    }

    fn receivers(s: &str) -> Vec<Receiver> {
        unified::Address::decode(s).unwrap().2.items()
    }

    #[test]
    fn the_sapling_receiver_is_dropped() {
        let full = ua(vec![Receiver::Orchard([1; 43]), Receiver::Sapling([2; 43]), Receiver::P2pkh([3; 20])]);
        let checkable = to_checkable_ua(&full).unwrap();
        assert!(checkable.starts_with("utest1"));
        assert_eq!(receivers(&checkable), vec![Receiver::Orchard([1; 43]), Receiver::P2pkh([3; 20])]);
        // Already checkable: returned unchanged.
        assert_eq!(to_checkable_ua(&checkable).unwrap(), checkable);
    }

    #[test]
    fn nothing_checkable_left_is_refused() {
        assert_eq!(to_checkable_ua(&ua(vec![Receiver::Sapling([2; 43])])), Err(LeekError::AddressUncheckable));
        assert_eq!(
            to_checkable_ua(&ua(vec![Receiver::Sapling([2; 43]), Receiver::P2pkh([3; 20])])),
            Err(LeekError::AddressUncheckable)
        );
        assert_eq!(to_checkable_ua("not an address"), Err(LeekError::Parse));
    }

    #[test]
    fn the_board_accounts_address_is_already_checkable() {
        let keys: serde_json::Value = serde_json::from_str(
            &std::fs::read_to_string(concat!(env!("CARGO_MANIFEST_DIR"), "/../../vectors/keys.json")).unwrap(),
        )
        .unwrap();
        for k in keys.as_array().unwrap().iter().take(4) {
            let a = k["ua"].as_str().unwrap();
            assert_eq!(to_checkable_ua(a).unwrap(), a);
        }
    }
}
