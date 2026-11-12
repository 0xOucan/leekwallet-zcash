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

/// The text of the QR a wallet shows for the board's **Check address**
/// (Main menu → Check address): the plain address, as a
/// wallet's Receive QR carries it. `text` is an address (unified, Sapling or
/// transparent) or a `zcash:` URI (ZIP 321) with one address; the result is
/// the address alone in its canonical encoding (Bech32 lowercase; Base58 as
/// it is), never uppercased, so a transparent address keeps its case.
/// Anything else, or a URI with several payments, is [`LeekError::Parse`].
pub fn address_check_payload(text: &str) -> Result<String, LeekError> {
    let t = text.trim();
    let addr = match t.get(..6) {
        Some(p) if p.eq_ignore_ascii_case("zcash:") => {
            let rest = &t[6..];
            let (path, query) = rest.split_once('?').unwrap_or((rest, ""));
            if !path.is_empty() {
                path
            } else {
                // `zcash:?address=...`: exactly one payment, unnumbered.
                let mut found = None;
                for kv in query.split('&') {
                    let (k, v) = kv.split_once('=').unwrap_or((kv, ""));
                    if k == "address" && found.is_none() {
                        found = Some(v);
                    } else if k.starts_with("address.") || k == "address" {
                        return Err(LeekError::Parse);
                    }
                }
                found.ok_or(LeekError::Parse)?
            }
        }
        _ => t,
    };
    let parse = |a: &str| zcash_address::ZcashAddress::try_from_encoded(a).ok().map(|a| a.encode());
    // An all-uppercase Bech32 address (from an alphanumeric QR) is the same
    // address; a Base58 one never parses lowercased, so this is safe.
    let upper = !addr.bytes().any(|b| b.is_ascii_lowercase());
    parse(addr).or_else(|| upper.then(|| parse(&addr.to_ascii_lowercase())).flatten()).ok_or(LeekError::Parse)
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
    fn the_check_address_payload_is_the_plain_address() {
        let full = ua(vec![Receiver::Orchard([1; 43]), Receiver::Sapling([2; 43]), Receiver::P2pkh([3; 20])]);
        assert_eq!(address_check_payload(&full).unwrap(), full);
        assert_eq!(address_check_payload(&format!("  {full}\n")).unwrap(), full);
        assert_eq!(address_check_payload(&full.to_uppercase()).unwrap(), full);
        assert_eq!(address_check_payload(&format!("zcash:{full}?amount=1.5&memo=AA")).unwrap(), full);
        assert_eq!(address_check_payload(&format!("ZCASH:{full}")).unwrap(), full);
        assert_eq!(address_check_payload(&format!("zcash:?address={full}&amount=1")).unwrap(), full);
        // Transparent (Base58): case is kept.
        let t = "tmBsTi2xWTjUdEXnuTceL7fecEQKeWaPDJd";
        assert_eq!(address_check_payload(t).unwrap(), t);
        assert_eq!(address_check_payload(&format!("zcash:{t}")).unwrap(), t);
        for bad in ["", "zcash:", "not an address", "zcash:?amount=1", &format!("zcash:?address={full}&address.1={full}")] {
            assert_eq!(address_check_payload(bad), Err(LeekError::Parse), "{bad}");
        }
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
