//! Hardware accounts: the LeekWallet holds the seed, this app holds the
//! watch-only wallet for the account it exports.
//!
//! The flows of `zecz send --compact` and `finish` (host/zecz-cli), split
//! around the board: [`propose_send`] or [`propose_shield`] builds the
//! compact signer view, the webview has the board sign it (zSign, over its
//! paired session), and [`finish`] applies the `.zsig` it returns, proves
//! and broadcasts. The view the board signed is the one `finish` checks
//! the `.zsig` against, by hash.

use anyhow::{bail, Result};
use serde::Serialize;
use zecz::wallet::{self, FinishEvent, KeyOrigin, SendRequest};
use zecz::{Profile, Wallet};

use crate::backend::block_on;

/// The account name a device import gets in the wallet database.
pub const DEVICE_NAME: &str = "leekwallet";

/// One step of [`finish`], for the status line under the button.
#[derive(Clone, Debug, PartialEq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Step {
    /// "prove" or "broadcast".
    pub step: &'static str,
    pub message: String,
}

impl Step {
    pub fn new(step: &'static str, message: impl Into<String>) -> Self {
        Step { step, message: message.into() }
    }
}

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct PaymentDto {
    pub recipient: String,
    pub amount_zat: u64,
    pub pool: String,
    pub has_memo: bool,
}

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct ChangeDto {
    pub value_zat: u64,
    pub pool: String,
}

/// A built, unsigned send, as the review screen shows it.
#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct ProposalDto {
    /// SHA-256 of the signer view, hex ([`proposal_id`]): the name its full
    /// PCZT is kept under, and how a swap finds the deposit it signed.
    pub id: String,
    /// "send", or "shield": no payments, the change is what is shielded.
    pub kind: &'static str,
    pub target_height: u32,
    pub payments: Vec<PaymentDto>,
    pub change: Vec<ChangeDto>,
    pub fee_zat: u64,
    /// Payments plus fee: what leaves the wallet.
    pub total_zat: u64,
    /// Size of the PCZT the device receives.
    pub signer_view_bytes: usize,
    pub compact: bool,
}

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct SentDto {
    pub txid: String,
    pub signatures: usize,
    pub size: usize,
    pub broadcast: bool,
}

/// The wallet's device account, if it has one: its ZIP 32 origin.
pub fn wallet_origin(profile: &Profile) -> Result<Option<KeyOrigin>> {
    if !profile.exists() {
        return Ok(None);
    }
    let w = Wallet::open(profile.clone())?;
    if !w.has_account()? {
        return Ok(None);
    }
    w.key_origin()
}

/// A mainnet send or shielding moves real funds: the app asks the user to
/// confirm it once more, after the review and before the device. Testnet
/// needs no confirmation.
pub fn check_mainnet_confirmed(profile: &Profile, confirmed: bool) -> Result<()> {
    if profile.net_name() == "main" && !confirmed {
        bail!("This is a mainnet transaction of real ZEC. Tick the confirmation under the review, then sign.");
    }
    Ok(())
}

/// Proposes a payment and builds the compact signer view for the board.
/// The full PCZT stays in the wallet's `pending/` for [`finish`]. Returns
/// the summary and the signer view.
pub fn propose_send(profile: &Profile, to: &str, amount_zec: &str, memo: Option<&str>) -> Result<(ProposalDto, Vec<u8>)> {
    let to = to.trim();
    if to.is_empty() {
        bail!("enter the recipient's address");
    }
    let amount_zat = wallet::parse_zec(amount_zec.trim())?;
    let memo = memo.map(str::trim_end).filter(|m| !m.is_empty()).map(str::to_string);
    let mut w = Wallet::open(profile.clone())?;
    if w.key_origin()?.is_none() {
        bail!("This is a watch-only account; nothing could sign the send. Import the account from a LeekWallet to spend.");
    }
    let p = w.propose_send(&SendRequest { to: to.to_string(), amount_zat, memo, compact: true })?;
    Ok(proposal_dto(p, "send"))
}

/// Proposes shielding every transparent coin of the account into its
/// shielded pool (Ironwood from NU6.3), with the compact signer view for
/// the device, which signs each coin with the key it re-derives (roadmap
/// 10.1). Refused when the fee would take the whole transparent balance.
pub fn propose_shield(profile: &Profile) -> Result<(ProposalDto, Vec<u8>)> {
    let mut w = Wallet::open(profile.clone())?;
    if w.key_origin()?.is_none() {
        bail!("This is a watch-only account; nothing could sign the shielding. Import the account from a LeekWallet to spend.");
    }
    let p = w.propose_shield(true)?;
    Ok(proposal_dto(p, "shield"))
}

fn proposal_dto(p: wallet::SendProposal, kind: &'static str) -> (ProposalDto, Vec<u8>) {
    let payments: Vec<PaymentDto> = p
        .payments
        .iter()
        .map(|x| PaymentDto { recipient: x.recipient.clone(), amount_zat: x.amount_zat, pool: x.pool.clone(), has_memo: x.has_memo })
        .collect();
    let total_zat = payments.iter().map(|x| x.amount_zat).sum::<u64>() + p.fee_zat;
    let dto = ProposalDto {
        id: proposal_id(&p.signer_view),
        kind,
        target_height: p.target_height,
        payments,
        change: p.change.iter().map(|c| ChangeDto { value_zat: c.value_zat, pool: c.pool.clone() }).collect(),
        fee_zat: p.fee_zat,
        total_zat,
        signer_view_bytes: p.signer_view.len(),
        compact: p.compact,
    };
    (dto, p.signer_view)
}

/// The id a proposal goes by: the hex SHA-256 of its signer view, the
/// name its full PCZT is kept under in `pending/`.
pub fn proposal_id(view: &[u8]) -> String {
    zecz::zsig::sha256(view).iter().map(|b| format!("{b:02x}")).collect()
}

/// Applies the board's signatures (`zsig`) to the kept full PCZT (each
/// checked against its rk), proves, stores the transaction and, if
/// `broadcast`, sends it through lightwalletd.
pub fn finish(profile: &Profile, view: &[u8], zsig: &[u8], broadcast: bool, on_status: &dyn Fn(Step)) -> Result<SentDto> {
    use zecz::authorize::ProveEvent;
    let mut w = Wallet::open(profile.clone())?;
    let f = block_on(w.finish(view, zsig, broadcast, |e| match e {
        FinishEvent::SignaturesApplied { count } => {
            on_status(Step::new("prove", format!("{count} signature(s) applied. Proving…")))
        }
        FinishEvent::Proving(ProveEvent::LoadingKey { pool, .. }) => {
            on_status(Step::new("prove", format!("Loading the {pool} proving key (slow the first time)…")))
        }
        FinishEvent::Proving(ProveEvent::Proving { pool }) => on_status(Step::new("prove", format!("Proving the {pool} bundle…"))),
        FinishEvent::Stored { txid, .. } if broadcast => on_status(Step::new("broadcast", format!("Broadcasting {txid}…"))),
        FinishEvent::Stored { .. } => {}
    }))??;
    Ok(SentDto { txid: f.txid, signatures: f.signatures, size: f.raw.len(), broadcast: f.broadcast })
}
