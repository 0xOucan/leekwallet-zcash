//! The account record a wallet stores or passes on after an import.
//!
//! Its CBOR body has the shape wallets already parse for a hardware signer's
//! Zcash accounts (as in Keystone's `zcash-accounts`), so their import code
//! carries over:
//!
//! ```text
//! {
//!   1: bytes(32)  seed fingerprint (ZIP 32)
//!   2: [ { 1: text UFVK, 2: uint ZIP 32 account index, ? 3: text name,
//!          ? 4: uint birthday height }, ... ]
//! }
//! ```
//!
//! Key 4 (birthday) is ours; readers that do not know it skip it. The
//! account maps are written untagged; a decoder accepts them tagged too.
//! It is our own type ([`ACCOUNT_RECORD_UR_TYPE`]), not Keystone's.

use ciborium::value::{Integer, Value};
use zcash_address::unified::{self, Encoding};

use crate::account::{Account, Network};
use crate::error::LeekError;

/// The type name for this record, for a UR or a file label.
pub const ACCOUNT_RECORD_UR_TYPE: &str = "leek-zcash-accounts";

/// One account of an [`AccountRecord`].
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct AccountEntry {
    /// The unified full viewing key (Orchard + P2PKH for a board account).
    pub ufvk: String,
    /// The ZIP 32 account index.
    pub index: u32,
    /// A label the user chose, if any.
    pub name: Option<String>,
    /// The height to scan from, if known.
    pub birthday: Option<u32>,
}

impl AccountEntry {
    /// The network the UFVK is for.
    pub fn network(&self) -> Result<Network, LeekError> {
        let (net, _, _) = unified::Ufvk::decode(&self.ufvk).map_err(|_| LeekError::Parse)?;
        Network::from_type(net)
    }
}

impl From<&Account> for AccountEntry {
    fn from(a: &Account) -> Self {
        AccountEntry { ufvk: a.ufvk.clone(), index: a.index, name: None, birthday: a.birthday }
    }
}

/// One seed's accounts: what a wallet imports and keeps.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct AccountRecord {
    /// The ZIP 32 seed fingerprint all the accounts derive from.
    pub seed_fingerprint: [u8; 32],
    pub accounts: Vec<AccountEntry>,
}

/// ZIP 32 account indices are below 2^31.
const MAX_INDEX: u32 = 0x7FFF_FFFF;

impl AccountRecord {
    /// A record of accounts read from one board ([`get_account`](crate::get_account)).
    /// Accounts of another seed are [`LeekError::WrongAccount`].
    pub fn from_accounts(accounts: &[Account]) -> Result<Self, LeekError> {
        let first = accounts.first().ok_or(LeekError::MissingData)?;
        if accounts.iter().any(|a| a.seed_fingerprint != first.seed_fingerprint) {
            return Err(LeekError::WrongAccount);
        }
        Ok(AccountRecord {
            seed_fingerprint: first.seed_fingerprint,
            accounts: accounts.iter().map(AccountEntry::from).collect(),
        })
    }

    /// The CBOR body (deterministic: keys in ascending order, shortest
    /// integer forms).
    pub fn encode(&self) -> Result<Vec<u8>, LeekError> {
        let accounts = self
            .accounts
            .iter()
            .map(|a| {
                if a.index > MAX_INDEX {
                    return Err(LeekError::WrongAccount);
                }
                let mut m = vec![(uint(1), Value::Text(a.ufvk.clone())), (uint(2), uint(a.index.into()))];
                if let Some(n) = &a.name {
                    m.push((uint(3), Value::Text(n.clone())));
                }
                if let Some(b) = a.birthday {
                    m.push((uint(4), uint(b.into())));
                }
                Ok(Value::Map(m))
            })
            .collect::<Result<Vec<_>, _>>()?;
        let body = Value::Map(vec![
            (uint(1), Value::Bytes(self.seed_fingerprint.to_vec())),
            (uint(2), Value::Array(accounts)),
        ]);
        let mut out = Vec::new();
        ciborium::into_writer(&body, &mut out).map_err(|_| LeekError::Parse)?;
        Ok(out)
    }

    /// Reads a body written by [`encode`](Self::encode) or by a signer using
    /// the same shape. Unknown keys are skipped; a missing or ill-typed known
    /// key, an index of 2^31 or more, a UFVK that does not parse, or trailing
    /// bytes are [`LeekError::Parse`].
    pub fn decode(bytes: &[u8]) -> Result<Self, LeekError> {
        let mut rd = bytes;
        let v: Value = ciborium::from_reader(&mut rd).map_err(|_| LeekError::Parse)?;
        if !rd.is_empty() {
            return Err(LeekError::Parse);
        }
        let m = map(&v)?;
        let seed_fingerprint = match get(m, 1) {
            Some(Value::Bytes(b)) => b.as_slice().try_into().map_err(|_| LeekError::Parse)?,
            _ => return Err(LeekError::Parse),
        };
        let Some(Value::Array(items)) = get(m, 2) else { return Err(LeekError::Parse) };
        let accounts = items
            .iter()
            .map(|item| {
                let m = map(untag(item))?;
                let ufvk = match get(m, 1) {
                    Some(Value::Text(t)) => t.clone(),
                    _ => return Err(LeekError::Parse),
                };
                unified::Ufvk::decode(&ufvk).map_err(|_| LeekError::Parse)?;
                let index = get_u32(m, 2)?.ok_or(LeekError::Parse)?;
                if index > MAX_INDEX {
                    return Err(LeekError::Parse);
                }
                let name = match get(m, 3) {
                    None => None,
                    Some(Value::Text(t)) => Some(t.clone()),
                    Some(_) => return Err(LeekError::Parse),
                };
                Ok(AccountEntry { ufvk, index, name, birthday: get_u32(m, 4)? })
            })
            .collect::<Result<Vec<_>, _>>()?;
        Ok(AccountRecord { seed_fingerprint, accounts })
    }
}

fn uint(n: u64) -> Value {
    Value::Integer(n.into())
}

fn untag(v: &Value) -> &Value {
    match v {
        Value::Tag(_, inner) => inner,
        v => v,
    }
}

fn map(v: &Value) -> Result<&[(Value, Value)], LeekError> {
    match v {
        Value::Map(m) => Ok(m),
        _ => Err(LeekError::Parse),
    }
}

fn get(m: &[(Value, Value)], key: u64) -> Option<&Value> {
    m.iter().find(|(k, _)| matches!(k, Value::Integer(i) if *i == Integer::from(key))).map(|(_, v)| v)
}

fn get_u32(m: &[(Value, Value)], key: u64) -> Result<Option<u32>, LeekError> {
    match get(m, key) {
        None => Ok(None),
        Some(Value::Integer(i)) => u32::try_from(i128::from(*i)).map(Some).map_err(|_| LeekError::Parse),
        Some(_) => Err(LeekError::Parse),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn ufvk(i: usize) -> String {
        let keys: serde_json::Value = serde_json::from_str(
            &std::fs::read_to_string(concat!(env!("CARGO_MANIFEST_DIR"), "/../../vectors/keys.json")).unwrap(),
        )
        .unwrap();
        keys[i]["ufvk"].as_str().unwrap().to_string()
    }

    fn record() -> AccountRecord {
        AccountRecord {
            seed_fingerprint: [7; 32],
            accounts: vec![
                AccountEntry { ufvk: ufvk(0), index: 0, name: Some("Board".into()), birthday: Some(3_000_123) },
                AccountEntry { ufvk: ufvk(1), index: 1, name: None, birthday: None },
            ],
        }
    }

    #[test]
    fn round_trips() {
        let r = record();
        let bytes = r.encode().unwrap();
        assert_eq!(AccountRecord::decode(&bytes).unwrap(), r);
        // Deterministic.
        assert_eq!(AccountRecord::decode(&bytes).unwrap().encode().unwrap(), bytes);
        let empty = AccountRecord { seed_fingerprint: [0; 32], accounts: vec![] };
        assert_eq!(AccountRecord::decode(&empty.encode().unwrap()).unwrap(), empty);
    }

    #[test]
    fn the_body_has_the_shared_shape() {
        let bytes = record().encode().unwrap();
        // A map of two: 1 => 32 bytes, 2 => an array of two maps.
        assert_eq!(&bytes[..4], &[0xA2, 0x01, 0x58, 0x20]);
        assert_eq!(&bytes[4..36], &[7; 32]);
        assert_eq!(&bytes[36..39], &[0x02, 0x82, 0xA4]);
        let v: Value = ciborium::from_reader(bytes.as_slice()).unwrap();
        let accts = match get(map(&v).unwrap(), 2) {
            Some(Value::Array(a)) => a.clone(),
            _ => panic!(),
        };
        // The second account: only UFVK and index, as a signer without
        // names or birthdays writes it.
        assert_eq!(map(&accts[1]).unwrap().len(), 2);
    }

    #[test]
    fn tagged_accounts_and_unknown_keys_are_read() {
        let body = Value::Map(vec![
            (uint(1), Value::Bytes(vec![9; 32])),
            (
                uint(2),
                Value::Array(vec![Value::Tag(
                    49203,
                    Box::new(Value::Map(vec![
                        (uint(1), Value::Text(ufvk(0))),
                        (uint(2), uint(5)),
                        (uint(3), Value::Text("x".into())),
                        (uint(99), Value::Bool(true)),
                    ])),
                )]),
            ),
            (uint(7), Value::Null),
        ]);
        let mut bytes = Vec::new();
        ciborium::into_writer(&body, &mut bytes).unwrap();
        let r = AccountRecord::decode(&bytes).unwrap();
        assert_eq!(r.seed_fingerprint, [9; 32]);
        assert_eq!(r.accounts, vec![AccountEntry { ufvk: ufvk(0), index: 5, name: Some("x".into()), birthday: None }]);
        let expected = if ufvk(0).starts_with("uviewtest") { Network::Test } else { Network::Main };
        assert_eq!(r.accounts[0].network().unwrap(), expected);
    }

    #[test]
    fn malformed_records_are_refused() {
        let good = record().encode().unwrap();
        assert_eq!(AccountRecord::decode(&good[..good.len() - 1]), Err(LeekError::Parse));
        let mut trailing = good.clone();
        trailing.push(0);
        assert_eq!(AccountRecord::decode(&trailing), Err(LeekError::Parse));
        assert_eq!(AccountRecord::decode(b""), Err(LeekError::Parse));

        let mut bad = record();
        bad.accounts[0].ufvk = "uview1notakey".into();
        assert_eq!(AccountRecord::decode(&bad.encode().unwrap()), Err(LeekError::Parse));
        let mut bad = record();
        bad.accounts[0].index = 1 << 31;
        assert_eq!(bad.encode(), Err(LeekError::WrongAccount));

        let short_fp = Value::Map(vec![(uint(1), Value::Bytes(vec![1; 31])), (uint(2), Value::Array(vec![]))]);
        let mut bytes = Vec::new();
        ciborium::into_writer(&short_fp, &mut bytes).unwrap();
        assert_eq!(AccountRecord::decode(&bytes), Err(LeekError::Parse));
    }

    #[test]
    fn accounts_of_two_seeds_are_not_one_record() {
        let a = Account {
            index: 0,
            network: Network::Test,
            seed_fingerprint: [1; 32],
            ufvk: ufvk(0),
            ua: String::new(),
            t_address: String::new(),
            birthday: Some(10),
        };
        let mut b = a.clone();
        b.index = 1;
        let r = AccountRecord::from_accounts(&[a.clone(), b.clone()]).unwrap();
        assert_eq!(r.accounts[1].index, 1);
        assert_eq!(r.accounts[0].birthday, Some(10));
        b.seed_fingerprint = [2; 32];
        assert_eq!(AccountRecord::from_accounts(&[a, b]), Err(LeekError::WrongAccount));
        assert_eq!(AccountRecord::from_accounts(&[]), Err(LeekError::MissingData));
    }
}
