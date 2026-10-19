//! A transparent coin spent by a transaction this wallet did not make.
//!
//! Compact blocks carry no transparent inputs, so a scan never sees a coin
//! of the wallet being spent: a shield made by another wallet with the same
//! keys (zecz elsewhere, the Companion, the Chrome wallet) left the coin in
//! the balance for good, with "Shield transparent funds" offered over it.
//! `Wallet::fetch_transaction_data`, the second half of `sync`, finds the
//! spend. Here lightwalletd is a map in memory.

mod common;

use std::sync::{Arc, Mutex};

use anyhow::Result;
use async_trait::async_trait;
use common::*;
use zcash_primitives::transaction::Transaction;
use zcash_protocol::consensus::BranchId;
use zecz::lwd::{ChainData, ChainTx};
use zecz::wallet::TxKind;

/// Where the shield was mined.
const SHIELD_HEIGHT: u32 = TIP - 10;

#[derive(Default)]
struct FakeChain {
    /// Every transaction the chain knows.
    txs: Vec<ChainTx>,
    /// The t-address whose transactions `txs` are.
    address: String,
    /// The UTXO set (all of it at `address`).
    utxos: Vec<([u8; 32], u32)>,
    /// The calls made, for the assertions.
    calls: Arc<Mutex<Vec<String>>>,
}

fn txid(raw: &[u8]) -> [u8; 32] {
    let tx = Transaction::read(raw, BranchId::for_height(&TEST, TIP.into())).unwrap();
    *tx.txid().as_ref()
}

fn height(t: &ChainTx) -> u32 {
    t.mined_height.unwrap_or(u32::MAX)
}

#[async_trait]
impl ChainData for FakeChain {
    async fn transaction(&mut self, id: [u8; 32]) -> Result<Option<ChainTx>> {
        self.calls.lock().unwrap().push(format!("tx {}", hex::encode(id)));
        Ok(self.txs.iter().find(|t| txid(&t.raw) == id).cloned())
    }

    async fn address_transactions(&mut self, address: &str, start: u32, end: u32) -> Result<Vec<ChainTx>> {
        self.calls.lock().unwrap().push(format!("taddr {address} {start}..={end}"));
        if address != self.address {
            return Ok(vec![]);
        }
        Ok(self.txs.iter().filter(|t| (start..=end).contains(&height(t))).cloned().collect())
    }

    async fn unspent_outpoints(&mut self, addresses: &[String]) -> Result<Vec<([u8; 32], u32)>> {
        self.calls.lock().unwrap().push(format!("utxos {}", addresses.join(",")));
        Ok(if addresses.contains(&self.address) { self.utxos.clone() } else { vec![] })
    }
}

fn run(w: &mut zecz::Wallet, chain: &mut FakeChain) -> zecz::wallet::TxDataSummary {
    tokio::runtime::Runtime::new().unwrap().block_on(w.fetch_transaction_data(chain)).unwrap()
}

#[test]
fn a_coin_shielded_by_another_wallet_leaves_the_balance() {
    let d = TempDir::new("tspend");
    let raw = shield_made_elsewhere(&d.0.join("other"), 1_000_000);
    let mut w = wallet_with_coin(&d.0.join("w"), 1_000_000);
    assert_eq!(w.balances().unwrap().account.unwrap().transparent.total, 1_000_000);

    // The chain: the shield mined, and the coin gone from the UTXO set.
    let mut chain = FakeChain {
        txs: vec![ChainTx { raw: raw.clone(), mined_height: Some(SHIELD_HEIGHT) }],
        address: t_address(),
        ..Default::default()
    };
    let s = run(&mut w, &mut chain);
    assert_eq!(s.vanished_coins, 1, "{s:?}");
    assert_eq!(s.failed, 0, "{s:?}");
    assert!(s.stored >= 1, "{s:?}");
    // The search started where the coin was mined.
    let calls = chain.calls.lock().unwrap().clone();
    assert!(calls.contains(&format!("taddr {} {COIN_HEIGHT}..={TIP}", t_address())), "{calls:?}");

    // The coin is spent: no transparent balance, so no shield prompt; the
    // shield's output is the account's, and the history has the shield.
    let b = w.balances().unwrap().account.unwrap();
    assert_eq!(b.transparent.total, 0, "{b:?}");
    let h = w.history(10).unwrap();
    let shield = h.iter().find(|r| r.kind == TxKind::Shielded).unwrap_or_else(|| panic!("{h:?}"));
    assert_eq!(shield.mined_height, Some(SHIELD_HEIGHT));
    assert!(shield.amount_zat > 0 && shield.amount_zat < 1_000_000, "{shield:?}");
    assert!(w.propose_shield(true).is_err(), "a spent coin can still be shielded");

    // Settled: the next sync has nothing left to search.
    chain.calls.lock().unwrap().clear();
    let s = run(&mut w, &mut chain);
    assert_eq!((s.vanished_coins, s.failed), (0, 0), "{s:?}");
    let calls = chain.calls.lock().unwrap().clone();
    assert!(!calls.iter().any(|c| c.starts_with("taddr")), "{calls:?}");
    assert!(!calls.contains(&format!("utxos {}", t_address())), "{calls:?}");
}

#[test]
fn a_coin_still_unspent_stays() {
    let d = TempDir::new("tspend");
    let mut w = wallet_with_coin(&d.0.join("w"), 1_000_000);
    let mut chain = FakeChain { address: t_address(), utxos: vec![(COIN_TXID, 0)], ..Default::default() };
    let s = run(&mut w, &mut chain);
    assert_eq!(s, zecz::wallet::TxDataSummary::default());
    assert_eq!(w.balances().unwrap().account.unwrap().transparent.total, 1_000_000);
    // UTXO queries only (the coin's t-address, and librustzcash's checks of
    // the ZIP 320 ephemeral addresses): no transaction history searched.
    let calls = chain.calls.lock().unwrap().clone();
    assert!(calls.contains(&format!("utxos {}", t_address())), "{calls:?}");
    assert!(calls.iter().all(|c| c.starts_with("utxos ")), "{calls:?}");
}

#[test]
fn a_failing_server_is_counted_not_fatal() {
    struct Down;
    #[async_trait]
    impl ChainData for Down {
        async fn transaction(&mut self, _: [u8; 32]) -> Result<Option<ChainTx>> {
            anyhow::bail!("down")
        }
        async fn address_transactions(&mut self, _: &str, _: u32, _: u32) -> Result<Vec<ChainTx>> {
            anyhow::bail!("down")
        }
        async fn unspent_outpoints(&mut self, _: &[String]) -> Result<Vec<([u8; 32], u32)>> {
            anyhow::bail!("down")
        }
    }
    let d = TempDir::new("tspend");
    let mut w = wallet_with_coin(&d.0.join("w"), 1_000_000);
    let s = tokio::runtime::Runtime::new().unwrap().block_on(w.fetch_transaction_data(&mut Down)).unwrap();
    assert!(s.failed >= 1, "{s:?}");
    // Nothing is assumed spent when the server cannot say.
    assert_eq!(w.balances().unwrap().account.unwrap().transparent.total, 1_000_000);
}

/// The same against testnet lightwalletd: syncs a throwaway wallet of
/// zecz's dev account from a recent birthday. Spends nothing.
#[test]
#[ignore = "network: testnet lightwalletd"]
fn sync_fetches_transaction_data_on_testnet() {
    let d = TempDir::new("tspend-online");
    let rt = tokio::runtime::Runtime::new().unwrap();
    let profile = zecz::Profile::new(TEST, d.0.join("w"), None);
    let tip = rt.block_on(zecz::wallet::tip(&profile.server)).unwrap().height as u32;
    let (mut w, _) =
        rt.block_on(zecz::Wallet::init(profile, &spec(), tip.saturating_sub(200), |_| {})).unwrap();
    let got = Arc::new(Mutex::new(None));
    let g = got.clone();
    rt.block_on(w.sync(1000, move |e| {
        if let zecz::wallet::SyncEvent::Transactions(s) = e {
            *g.lock().unwrap() = Some(s);
        }
    }))
    .unwrap();
    let s = got.lock().unwrap().expect("no Transactions event");
    assert_eq!(s.failed, 0, "{s:?}");
}
