//! Cross-chain swaps through NEAR Intents' 1Click API (roadmap 8.6).
//!
//! The flow is the one Zodl ships (`NearApiProvider.kt`,
//! `NearSwapDataSource.kt`) and the 1Click docs describe
//! (<https://docs.near-intents.org/integration/distribution-channels/1click-api/quickstart/making-a-request>,
//! OpenAPI at <https://1click.chaindefuser.com/docs/v0/openapi.yaml>):
//!
//! 1. `GET /v0/tokens`: the assets, with their `assetId` and decimals.
//! 2. `POST /v0/quote`: origin and destination asset, amount in base units,
//!    slippage in basis points, recipient and refund address, deadline. A
//!    `dry` quote prices the swap without a deposit address; a real one
//!    returns the `depositAddress` the origin asset is paid to.
//! 3. The deposit, on the origin chain. From ZEC it is an ordinary send of
//!    the quoted `amountIn` to the deposit address, built here and signed on
//!    the LeekWallet like any other (the device shows the address it pays).
//! 4. `POST /v0/deposit/submit` with the txid (optional, speeds it up).
//! 5. `GET /v0/status?depositAddress=…` until SUCCESS, REFUNDED or FAILED.
//!
//! Nothing here holds a key or a secret. An API key (a partner JWT) is
//! optional: without one 1Click adds a 0.25% platform fee, and refuses
//! confidential quotes. It comes from `LEEKZ_1CLICK_API_KEY` or
//! `oneClickApiKey` in settings.json, never from the repository.
//!
//! The wallet's side of every swap is a fresh Orchard-only address: the
//! refund address of a ZEC deposit and the payout address of a swap into
//! ZEC. 1Click accepts unified addresses for both, so refunds and payouts
//! stay shielded and one swap cannot be linked to the next.
//!
//! Swaps are mainnet only: 1Click has no testnet. On testnet the screen can
//! only preview a price (a dry quote against a placeholder address).
//!
//! Every swap with a deposit address is kept in
//! `<app data dir>/swaps-main.json` (the quote, its signature, the deposit
//! txid and the last status), so a restart keeps following it.

use std::fs;
use std::path::{Path, PathBuf};
use std::time::Duration;

use anyhow::{anyhow, bail, Context, Result};
use serde::{Deserialize, Serialize};
use serde_json::Value;
use time::format_description::well_known::Rfc3339;
use time::OffsetDateTime;
use zecz::Network;

/// The 1Click API.
pub const BASE_URL: &str = "https://1click.chaindefuser.com";
/// ZEC on NEAR Intents (bridged through omft), 8 decimals like zatoshis.
pub const ZEC_ASSET: &str = "nep141:zec.omft.near";
pub const ZEC_DECIMALS: u32 = 8;
/// Where an optional 1Click API key (partner JWT) comes from.
pub const API_KEY_ENV: &str = "LEEKZ_1CLICK_API_KEY";
/// The slippage offered by default (1%), and the most accepted (10%).
pub const DEFAULT_SLIPPAGE_BPS: u32 = 100;
pub const MAX_SLIPPAGE_BPS: u32 = 1000;
/// The deadline asked for: refunds start if the swap has not completed by
/// then. Two hours, as Zodl asks: a ZEC deposit needs a few blocks.
pub const QUOTE_LIFETIME_SECS: i64 = 2 * 60 * 60;
/// A deposit is not signed with less time than this before the deadline:
/// it has to be mined and seen first.
pub const MIN_TIME_LEFT_SECS: i64 = 15 * 60;
/// How long 1Click may wait for market makers' quotes.
pub const QUOTE_WAITING_MS: u32 = 3000;
/// The least a ZEC deposit costs on top of its amount (ZIP 317: two
/// Orchard actions and a transparent output come to 15,000 zat; 10,000 is
/// the floor of any transaction).
pub const MIN_FEE_ZAT: u64 = 10_000;
/// The address a dry quote names as the wallet's side on testnet, where
/// the wallet has no mainnet address: the Orchard-only address of the
/// public test seed ("abandon … art", vectors/keys.json). A dry quote makes
/// no deposit address, so nothing can be paid anywhere.
pub const PREVIEW_ADDRESS: &str =
    "u1gsqvqxx6lmmqg05uvx57gjdg5j3a54nxw09z4vq4z0yp7dfdcjrqk5wq64quwzrufmujd5e8xu5jn7cyewjaptxc8lsqwa2lk559u4cd";
/// Where a deposit address can be looked up by hand.
pub const EXPLORER: &str = "https://explorer.near-intents.org";

// ------------------------------------------------------------------- http

/// A response: status and body, whatever the status.
#[derive(Clone, Debug)]
pub struct HttpResponse {
    pub status: u16,
    pub body: String,
}

/// The HTTP the client needs; [`Http`] in the app, a recorded one in tests.
pub trait Transport: Send + Sync {
    fn get(&self, url: &str, query: &[(&str, &str)], headers: &[(&str, &str)]) -> Result<HttpResponse>;
    fn post(&self, url: &str, headers: &[(&str, &str)], body: &str) -> Result<HttpResponse>;
}

/// Blocking HTTPS (ureq over rustls with ring and the webpki roots, the
/// TLS stack lightwalletd already uses), 30 s per request.
pub struct Http {
    agent: ureq::Agent,
}

impl Default for Http {
    fn default() -> Self {
        let config = ureq::Agent::config_builder()
            .timeout_global(Some(Duration::from_secs(30)))
            .http_status_as_error(false)
            .user_agent(concat!("leekwallet-zcash-desktop/", env!("CARGO_PKG_VERSION")))
            .build();
        Http { agent: config.into() }
    }
}

impl Transport for Http {
    fn get(&self, url: &str, query: &[(&str, &str)], headers: &[(&str, &str)]) -> Result<HttpResponse> {
        let mut req = self.agent.get(url);
        for (k, v) in query {
            req = req.query(*k, *v);
        }
        for (k, v) in headers {
            req = req.header(*k, *v);
        }
        let mut resp = req.call().map_err(|e| anyhow!("cannot reach 1Click ({url}): {e}"))?;
        let status = resp.status().as_u16();
        let body = resp.body_mut().read_to_string().with_context(|| format!("reading the answer of {url}"))?;
        Ok(HttpResponse { status, body })
    }

    fn post(&self, url: &str, headers: &[(&str, &str)], body: &str) -> Result<HttpResponse> {
        let mut req = self.agent.post(url).header("Content-Type", "application/json");
        for (k, v) in headers {
            req = req.header(*k, *v);
        }
        let mut resp = req.send(body.as_bytes()).map_err(|e| anyhow!("cannot reach 1Click ({url}): {e}"))?;
        let status = resp.status().as_u16();
        let body = resp.body_mut().read_to_string().with_context(|| format!("reading the answer of {url}"))?;
        Ok(HttpResponse { status, body })
    }
}

// -------------------------------------------------------------------- api

/// A refusal from 1Click: its HTTP status and `message`.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ApiError {
    pub status: u16,
    pub message: String,
}

impl std::fmt::Display for ApiError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "1Click answered {}: {}", self.status, self.message)
    }
}

impl std::error::Error for ApiError {}

/// One asset 1Click swaps.
#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct Token {
    pub asset_id: String,
    pub decimals: u32,
    pub blockchain: String,
    pub symbol: String,
    #[serde(default)]
    pub price: Option<f64>,
    #[serde(default)]
    pub contract_address: Option<String>,
}

/// `POST /v0/quote`. Deposits and refunds on the origin chain, payout on
/// the destination chain: the plain cross-chain swap.
#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct QuoteRequest {
    pub dry: bool,
    /// EXACT_INPUT (from ZEC: the deposit is exactly `amount`) or
    /// FLEX_INPUT (into ZEC: the payer's wallet may send a bit more or less).
    pub swap_type: String,
    pub slippage_tolerance: u32,
    pub origin_asset: String,
    pub deposit_type: String,
    pub destination_asset: String,
    /// In the origin asset's base units, as a decimal string.
    pub amount: String,
    pub refund_to: String,
    pub refund_type: String,
    pub recipient: String,
    pub recipient_type: String,
    /// RFC 3339.
    pub deadline: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub quote_waiting_time_ms: Option<u32>,
    /// "public" (the default), "basic" or "advanced" (Confidential Intents;
    /// needs an API key).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub confidentiality: Option<String>,
}

/// The `quote` of a quote response. Amounts are base-unit strings.
#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct Quote {
    /// Absent from a dry quote.
    #[serde(default)]
    pub deposit_address: Option<String>,
    /// Chains such as Stellar need it with the address; never for ZEC.
    #[serde(default)]
    pub deposit_memo: Option<String>,
    pub amount_in: String,
    pub amount_in_formatted: String,
    #[serde(default)]
    pub amount_in_usd: Option<String>,
    #[serde(default)]
    pub min_amount_in: Option<String>,
    pub amount_out: String,
    pub amount_out_formatted: String,
    #[serde(default)]
    pub amount_out_usd: Option<String>,
    pub min_amount_out: String,
    /// When the deposit address stops taking deposits (absent when dry).
    #[serde(default)]
    pub deadline: Option<String>,
    #[serde(default)]
    pub time_when_inactive: Option<String>,
    /// Seconds from a confirmed deposit to the payout.
    #[serde(default)]
    pub time_estimate: Option<f64>,
    /// Base units of the origin asset charged for a refund.
    #[serde(default)]
    pub refund_fee: Option<String>,
    /// Base units of the destination asset, already out of `amountOut`.
    #[serde(default)]
    pub withdraw_fee: Option<String>,
}

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct QuoteResponse {
    #[serde(default)]
    pub correlation_id: Option<String>,
    #[serde(default)]
    pub timestamp: Option<String>,
    /// 1Click's signature of the quote, kept for disputes.
    #[serde(default)]
    pub signature: Option<String>,
    /// The request as 1Click understood it.
    pub quote_request: Value,
    pub quote: Quote,
}

/// A transaction of the swap on either chain.
#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct TxLink {
    pub hash: String,
    #[serde(default)]
    pub explorer_url: String,
}

/// What happened, as `/v0/status` reports it.
#[derive(Clone, Debug, Default, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct SwapDetails {
    #[serde(default)]
    pub amount_in_formatted: Option<String>,
    #[serde(default)]
    pub amount_out_formatted: Option<String>,
    #[serde(default)]
    pub deposited_amount_formatted: Option<String>,
    #[serde(default)]
    pub refunded_amount_formatted: Option<String>,
    #[serde(default)]
    pub refund_reason: Option<String>,
    #[serde(default)]
    pub origin_chain_tx_hashes: Vec<TxLink>,
    #[serde(default)]
    pub destination_chain_tx_hashes: Vec<TxLink>,
}

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct StatusResponse {
    /// KNOWN_DEPOSIT_TX, PENDING_DEPOSIT, INCOMPLETE_DEPOSIT, PROCESSING,
    /// SUCCESS, REFUNDED or FAILED.
    pub status: String,
    #[serde(default)]
    pub updated_at: Option<String>,
    #[serde(default)]
    pub swap_details: Option<SwapDetails>,
}

/// The 1Click client over a [`Transport`].
pub struct OneClick<T: Transport> {
    transport: T,
    base: String,
    api_key: Option<String>,
}

impl<T: Transport> OneClick<T> {
    pub fn new(transport: T, api_key: Option<String>) -> Self {
        Self::with_base(transport, BASE_URL, api_key)
    }

    pub fn with_base(transport: T, base: &str, api_key: Option<String>) -> Self {
        let api_key = api_key.map(|k| k.trim().to_string()).filter(|k| !k.is_empty());
        OneClick { transport, base: base.trim_end_matches('/').to_string(), api_key }
    }

    pub fn has_api_key(&self) -> bool {
        self.api_key.is_some()
    }

    fn auth(&self) -> Vec<(&str, String)> {
        // Zodl sends its partner JWT as a bearer token; the docs accept
        // that and X-API-Key alike.
        self.api_key.iter().map(|k| ("Authorization", format!("Bearer {k}"))).collect()
    }

    fn checked(resp: HttpResponse) -> Result<String> {
        if (200..300).contains(&resp.status) {
            return Ok(resp.body);
        }
        let message = serde_json::from_str::<Value>(&resp.body)
            .ok()
            .and_then(|v| v.get("message").and_then(Value::as_str).map(str::to_string))
            .unwrap_or_else(|| resp.body.chars().take(200).collect());
        Err(ApiError { status: resp.status, message }.into())
    }

    fn get(&self, path: &str, query: &[(&str, &str)]) -> Result<String> {
        let auth = self.auth();
        let headers: Vec<(&str, &str)> = auth.iter().map(|(k, v)| (*k, v.as_str())).collect();
        Self::checked(self.transport.get(&format!("{}{path}", self.base), query, &headers)?)
    }

    fn post(&self, path: &str, body: &impl Serialize) -> Result<String> {
        let auth = self.auth();
        let headers: Vec<(&str, &str)> = auth.iter().map(|(k, v)| (*k, v.as_str())).collect();
        let body = serde_json::to_string(body)?;
        Self::checked(self.transport.post(&format!("{}{path}", self.base), &headers, &body)?)
    }

    /// The assets 1Click swaps, one entry per symbol, chain and decimals
    /// (as Zodl lists them), sorted by symbol then chain.
    pub fn tokens(&self) -> Result<Vec<Token>> {
        let body = self.get("/v0/tokens", &[])?;
        let all: Vec<Token> = serde_json::from_str(&body).context("1Click sent a token list this app cannot read")?;
        // The first of each in 1Click's order, as Zodl's distinctBy keeps it.
        let mut seen = std::collections::HashSet::new();
        let mut out: Vec<Token> =
            all.into_iter().filter(|t| seen.insert((t.symbol.clone(), t.blockchain.clone(), t.decimals))).collect();
        out.sort_by(|a, b| (a.symbol.to_lowercase(), &a.blockchain).cmp(&(b.symbol.to_lowercase(), &b.blockchain)));
        Ok(out)
    }

    /// A quote, and the response as sent (kept whole with the swap).
    pub fn quote(&self, req: &QuoteRequest) -> Result<(QuoteResponse, Value)> {
        let body = self.post("/v0/quote", req)?;
        let raw: Value = serde_json::from_str(&body).context("1Click sent a quote this app cannot read")?;
        let parsed: QuoteResponse = serde_json::from_value(raw.clone()).context("1Click sent a quote this app cannot read")?;
        Ok((parsed, raw))
    }

    /// Tells 1Click the deposit's txid, so it looks for it at once.
    pub fn submit_deposit(&self, tx_hash: &str, deposit_address: &str, memo: Option<&str>) -> Result<()> {
        let mut body = serde_json::json!({ "txHash": tx_hash, "depositAddress": deposit_address });
        if let Some(m) = memo {
            body["memo"] = Value::from(m);
        }
        self.post("/v0/deposit/submit", &body)?;
        Ok(())
    }

    /// The swap's status. None while 1Click does not know the address yet
    /// (404), which reads as "waiting for the deposit".
    pub fn status(&self, deposit_address: &str, memo: Option<&str>) -> Result<Option<StatusResponse>> {
        let mut query = vec![("depositAddress", deposit_address)];
        if let Some(m) = memo {
            query.push(("depositMemo", m));
        }
        match self.get("/v0/status", &query) {
            Ok(body) => Ok(Some(serde_json::from_str(&body).context("1Click sent a status this app cannot read")?)),
            Err(e) if e.downcast_ref::<ApiError>().is_some_and(|a| a.status == 404) => Ok(None),
            Err(e) => Err(e),
        }
    }
}

/// The API key: `LEEKZ_1CLICK_API_KEY`, else `oneClickApiKey` in settings.
pub fn api_key(from_settings: Option<&str>) -> Option<String> {
    std::env::var(API_KEY_ENV)
        .ok()
        .or_else(|| from_settings.map(str::to_string))
        .map(|k| k.trim().to_string())
        .filter(|k| !k.is_empty())
}

/// A 1Click refusal in words, for the quote of `amount_asset`.
pub fn explain(e: &anyhow::Error, amount_asset: Option<&Token>) -> String {
    let Some(a) = e.chain().find_map(|c| c.downcast_ref::<ApiError>()) else {
        return format!("{e:#}");
    };
    let m = a.message.as_str();
    let lower = m.to_lowercase();
    if let Some(rest) = lower.find("try at least").map(|i| &lower[i + "try at least".len()..]) {
        let min = rest.trim().trim_end_matches('.');
        if let Some(t) = amount_asset.filter(|_| min.chars().all(|c| c.is_ascii_digit()) && !min.is_empty()) {
            return format!(
                "The amount is too small for this route: 1Click needs at least {} {}.",
                format_units(min, t.decimals).unwrap_or_else(|_| min.to_string()),
                t.symbol
            );
        }
        return format!("The amount is too small for this route ({m}).");
    }
    if lower.contains("no quotes found") || lower.contains("failed to get quote") {
        return "No market maker quoted this swap. Try a larger amount, a higher slippage or another asset.".into();
    }
    if a.status == 401 && lower.contains("confidential") {
        return format!(
            "Confidential swaps need a 1Click API key ({API_KEY_ENV}, or oneClickApiKey in settings.json). Turn the option off, or add a key."
        );
    }
    if a.status == 401 {
        return format!("1Click refused the API key ({m}). Check {API_KEY_ENV} or oneClickApiKey in settings.json.");
    }
    if a.status >= 500 {
        return format!("1Click failed ({}: {m}). Try again in a moment; a very small amount can cause this too.", a.status);
    }
    format!("1Click refused the request ({}: {m}).", a.status)
}

// ---------------------------------------------------------------- amounts

/// A decimal amount with up to `decimals` places as base units.
pub fn parse_units(s: &str, decimals: u32) -> Result<u128> {
    let s = s.trim();
    let (int, frac) = s.split_once('.').unwrap_or((s, ""));
    if (int.is_empty() && frac.is_empty()) || !int.chars().chain(frac.chars()).all(|c| c.is_ascii_digit()) {
        bail!("{s:?} is not an amount");
    }
    if frac.len() > decimals as usize {
        bail!("at most {decimals} decimals for this asset");
    }
    let scale = 10u128.checked_pow(decimals).ok_or_else(|| anyhow!("too many decimals"))?;
    let int: u128 = if int.is_empty() { 0 } else { int.parse().context("amount too large")? };
    let frac: u128 = if frac.is_empty() { 0 } else { format!("{frac:0<width$}", width = decimals as usize).parse()? };
    let v = int.checked_mul(scale).and_then(|v| v.checked_add(frac)).ok_or_else(|| anyhow!("amount too large"))?;
    if v == 0 {
        bail!("the amount must be more than zero");
    }
    Ok(v)
}

/// Base units (a decimal string) as a plain decimal, trailing zeros trimmed.
pub fn format_units(base: &str, decimals: u32) -> Result<String> {
    let v: u128 = base.trim().parse().with_context(|| format!("{base:?} is not an amount"))?;
    let scale = 10u128.checked_pow(decimals).ok_or_else(|| anyhow!("too many decimals"))?;
    let (int, frac) = (v / scale, v % scale);
    if frac == 0 {
        return Ok(int.to_string());
    }
    let frac = format!("{frac:0>width$}", width = decimals as usize);
    Ok(format!("{int}.{}", frac.trim_end_matches('0')))
}

/// Zatoshis as the ZEC string `propose_send` takes.
pub fn zat_to_zec(zat: u64) -> String {
    format!("{}.{:08}", zat / 100_000_000, zat % 100_000_000)
}

// ------------------------------------------------------------------- time

pub fn rfc3339(t: OffsetDateTime) -> String {
    t.replace_nanosecond(0).unwrap_or(t).format(&Rfc3339).unwrap_or_default()
}

pub fn parse_time(s: &str) -> Result<OffsetDateTime> {
    OffsetDateTime::parse(s.trim(), &Rfc3339).with_context(|| format!("{s:?} is not a time"))
}

// ------------------------------------------------------------------ swaps

/// Which side ZEC is on.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub enum Direction {
    /// ZEC from this wallet, signed on the LeekWallet, to an asset elsewhere.
    FromZec,
    /// An asset paid from another wallet, into this wallet as ZEC.
    ToZec,
}

/// Where a swap stands. Final: Success, Refunded, Failed.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub enum SwapState {
    /// A deposit address exists; nothing was paid to it yet.
    Quoted,
    /// The deadline passed with no deposit; a late one is refunded.
    Expired,
    /// This wallet broadcast the deposit.
    Deposited,
    /// 1Click saw the deposit (KNOWN_DEPOSIT_TX).
    DepositSeen,
    /// Less than the quote needs arrived (INCOMPLETE_DEPOSIT).
    Incomplete,
    /// The swap is running (PROCESSING).
    Processing,
    Success,
    Refunded,
    Failed,
}

impl SwapState {
    pub fn is_final(self) -> bool {
        matches!(self, SwapState::Success | SwapState::Refunded | SwapState::Failed)
    }

    /// How far along: a status never moves a swap backwards.
    fn rank(self) -> u8 {
        match self {
            SwapState::Quoted => 0,
            SwapState::Expired => 1,
            SwapState::Deposited => 2,
            SwapState::DepositSeen => 3,
            SwapState::Incomplete => 4,
            SwapState::Processing => 5,
            SwapState::Success | SwapState::Refunded | SwapState::Failed => 6,
        }
    }
}

/// The next state, from the current one, what 1Click reports (None: it
/// does not know the address yet), whether this wallet sent the deposit,
/// and whether the deadline passed. Final states stay; 1Click's final
/// answers win over anything else; otherwise a swap only moves forward.
pub fn next_state(cur: SwapState, api: Option<&str>, deposit_sent: bool, deadline_passed: bool) -> SwapState {
    if cur.is_final() {
        return cur;
    }
    let reported = match api {
        Some("SUCCESS") => return SwapState::Success,
        Some("REFUNDED") => return SwapState::Refunded,
        Some("FAILED") => return SwapState::Failed,
        Some("PROCESSING") => SwapState::Processing,
        Some("KNOWN_DEPOSIT_TX") => SwapState::DepositSeen,
        Some("INCOMPLETE_DEPOSIT") => SwapState::Incomplete,
        // PENDING_DEPOSIT, not known yet, or a status this app does not know.
        _ if deposit_sent => SwapState::Deposited,
        _ if deadline_passed => SwapState::Expired,
        _ => SwapState::Quoted,
    };
    if reported.rank() >= cur.rank() {
        reported
    } else {
        cur
    }
}

/// An asset as a swap names it.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct AssetRef {
    pub asset_id: String,
    pub symbol: String,
    pub blockchain: String,
    pub decimals: u32,
}

impl From<&Token> for AssetRef {
    fn from(t: &Token) -> Self {
        AssetRef { asset_id: t.asset_id.clone(), symbol: t.symbol.clone(), blockchain: t.blockchain.clone(), decimals: t.decimals }
    }
}

/// A swap: its quote, its deposit and what 1Click last said. A dry quote
/// has the same shape, with no deposit address, and is not stored.
#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct SwapRecord {
    /// The deposit address ("dry" for a preview).
    pub id: String,
    pub dry: bool,
    pub created_at: String,
    pub direction: Direction,
    pub origin: AssetRef,
    pub destination: AssetRef,
    /// The deposit, base units of the origin asset.
    pub amount_in: String,
    pub amount_in_formatted: String,
    #[serde(default)]
    pub amount_in_usd: Option<String>,
    /// For FLEX_INPUT: the least deposit that is swapped.
    #[serde(default)]
    pub min_amount_in_formatted: Option<String>,
    pub amount_out_formatted: String,
    #[serde(default)]
    pub amount_out_usd: Option<String>,
    /// Received at worst, after slippage.
    pub min_amount_out_formatted: String,
    pub slippage_bps: u32,
    /// When the deposit address stops taking deposits; refunds start.
    #[serde(default)]
    pub deadline: Option<String>,
    #[serde(default)]
    pub time_estimate_secs: Option<f64>,
    /// What a refund costs, in the origin asset.
    #[serde(default)]
    pub refund_fee_formatted: Option<String>,
    #[serde(default)]
    pub deposit_address: Option<String>,
    #[serde(default)]
    pub deposit_memo: Option<String>,
    /// The deposit address is a transparent one: the deposit is public.
    #[serde(default)]
    pub deposit_transparent: bool,
    /// Who receives the destination asset.
    pub recipient: String,
    /// Where a refund of the origin asset goes.
    pub refund_to: String,
    /// "public", "basic" or "advanced".
    pub confidentiality: String,
    /// Whether the request carried an API key.
    pub authenticated: bool,
    /// The quote response as 1Click sent it, signature included.
    pub quote: Value,
    /// From ZEC: the proposal built to pay the deposit.
    #[serde(default)]
    pub proposal_id: Option<String>,
    #[serde(default)]
    pub deposit_txid: Option<String>,
    #[serde(default)]
    pub deposit_submitted: bool,
    pub state: SwapState,
    #[serde(default)]
    pub api_status: Option<String>,
    #[serde(default)]
    pub updated_at: Option<String>,
    #[serde(default)]
    pub checked_at: Option<String>,
    #[serde(default)]
    pub details: Option<SwapDetails>,
    /// The last error while following it (network, API).
    #[serde(default)]
    pub last_error: Option<String>,
}

impl SwapRecord {
    pub fn deadline_passed(&self, now: OffsetDateTime) -> bool {
        self.deadline.as_deref().and_then(|d| parse_time(d).ok()).is_some_and(|d| d <= now)
    }
}

/// What the swap screen asks for.
#[derive(Clone, Debug, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct QuoteParams {
    pub direction: Direction,
    /// The asset on the other side of ZEC.
    pub asset: String,
    /// The amount paid in, in the origin asset's units ("0.01").
    pub amount: String,
    /// From ZEC: the address on the destination chain.
    #[serde(default)]
    pub recipient: Option<String>,
    /// Into ZEC: the payer's address on the origin chain, for refunds.
    #[serde(default)]
    pub refund_to: Option<String>,
    #[serde(default)]
    pub slippage_bps: Option<u32>,
    /// A price preview: no deposit address, nothing stored.
    #[serde(default)]
    pub dry: bool,
    /// Ask for Confidential Intents ("basic"); needs an API key.
    #[serde(default)]
    pub confidential: bool,
}

/// The wallet's part in a quote, so tests need no wallet.
pub struct QuoteContext<'a> {
    pub network: Network,
    /// The wallet's address for this swap (refunds from ZEC, payout into
    /// ZEC): a fresh Orchard-only one for a real quote.
    pub own_address: &'a dyn Fn() -> Result<String>,
    /// The spendable balance in zatoshis, None before a sync.
    pub spendable: &'a dyn Fn() -> Result<Option<u64>>,
    pub now: OffsetDateTime,
}

fn check_foreign_address(what: &str, a: Option<&str>) -> Result<String> {
    let a = a.map(str::trim).unwrap_or("");
    if a.is_empty() {
        bail!("enter {what}");
    }
    if a.len() > 128 || a.chars().any(|c| c.is_whitespace() || c.is_control()) {
        bail!("{what} does not look like an address");
    }
    Ok(a.to_string())
}

/// Spendable ZEC must cover the deposit and at least the smallest fee.
pub fn check_funds(spendable: Option<u64>, amount_zat: u64) -> Result<()> {
    let Some(have) = spendable else {
        bail!("Sync the wallet first: the swap needs a known spendable balance.");
    };
    let need = amount_zat.saturating_add(MIN_FEE_ZAT);
    if have < need {
        bail!(
            "Not enough spendable ZEC: the swap deposits {} ZEC plus the fee; the wallet can spend {} ZEC.",
            zat_to_zec(amount_zat),
            zat_to_zec(have)
        );
    }
    Ok(())
}

fn find<'t>(tokens: &'t [Token], asset: &str) -> Result<&'t Token> {
    tokens.iter().find(|t| t.asset_id == asset).ok_or_else(|| anyhow!("1Click does not list the asset {asset}"))
}

/// Asks 1Click for a quote and checks the answer against the request. A
/// real quote (not dry) is mainnet only and gets a deposit address; the
/// caller stores the record it returns.
pub fn request_quote<T: Transport>(client: &OneClick<T>, tokens: &[Token], ctx: &QuoteContext, p: &QuoteParams) -> Result<SwapRecord> {
    let mainnet = ctx.network == Network::MainNetwork;
    if !mainnet && !p.dry {
        bail!("NEAR Intents swaps are mainnet only. On testnet you can preview a price (dry run); switch to Mainnet to swap.");
    }
    if p.asset == ZEC_ASSET {
        bail!("choose an asset other than ZEC");
    }
    let zec = find(tokens, ZEC_ASSET)?;
    let other = find(tokens, &p.asset)?;
    let (origin, destination) = match p.direction {
        Direction::FromZec => (zec, other),
        Direction::ToZec => (other, zec),
    };
    let slippage = p.slippage_bps.unwrap_or(DEFAULT_SLIPPAGE_BPS);
    if slippage == 0 || slippage > MAX_SLIPPAGE_BPS {
        bail!("slippage must be between 0.01% and {}%", MAX_SLIPPAGE_BPS / 100);
    }
    let amount = parse_units(&p.amount, origin.decimals)?;
    if p.direction == Direction::FromZec && !p.dry {
        let zat = u64::try_from(amount).map_err(|_| anyhow!("amount too large"))?;
        check_funds((ctx.spendable)()?, zat)?;
    }
    if p.confidential && !client.has_api_key() {
        bail!("Confidential swaps need a 1Click API key ({API_KEY_ENV}, or oneClickApiKey in settings.json).");
    }
    // The wallet's own address: a fresh Orchard-only one for a real quote;
    // a preview on testnet names the public placeholder.
    let own = if mainnet { (ctx.own_address)()? } else { PREVIEW_ADDRESS.to_string() };
    let (recipient, refund_to) = match p.direction {
        Direction::FromZec => (check_foreign_address("the recipient's address", p.recipient.as_deref())?, own),
        Direction::ToZec => (own, check_foreign_address("your refund address on the paying chain", p.refund_to.as_deref())?),
    };
    let deadline = rfc3339(ctx.now + time::Duration::seconds(QUOTE_LIFETIME_SECS));
    let req = QuoteRequest {
        dry: p.dry,
        swap_type: match p.direction {
            Direction::FromZec => "EXACT_INPUT",
            Direction::ToZec => "FLEX_INPUT",
        }
        .into(),
        slippage_tolerance: slippage,
        origin_asset: origin.asset_id.clone(),
        deposit_type: "ORIGIN_CHAIN".into(),
        destination_asset: destination.asset_id.clone(),
        amount: amount.to_string(),
        refund_to: refund_to.clone(),
        refund_type: "ORIGIN_CHAIN".into(),
        recipient: recipient.clone(),
        recipient_type: "DESTINATION_CHAIN".into(),
        deadline,
        quote_waiting_time_ms: Some(QUOTE_WAITING_MS),
        confidentiality: p.confidential.then(|| "basic".to_string()),
    };
    let (resp, raw) = client.quote(&req).map_err(|e| anyhow!(explain(&e, Some(origin))))?;
    check_echo(&req, &resp)?;
    let q = &resp.quote;
    if p.direction == Direction::FromZec && q.amount_in != req.amount {
        bail!("1Click quoted a deposit of {} instead of {}; not using this quote", q.amount_in, req.amount);
    }
    let mut deposit_transparent = false;
    let id = if p.dry {
        "dry".to_string()
    } else {
        let d = q.deposit_address.clone().filter(|d| !d.trim().is_empty());
        let d = d.ok_or_else(|| anyhow!("1Click sent a quote without a deposit address"))?;
        if p.direction == Direction::FromZec {
            // The deposit is a send from this wallet: it must be an address
            // zecz can pay on mainnet (unified or transparent; a TEX address
            // needs a two-step send the device does not sign).
            let kind = zecz::wallet::check_address(ctx.network, &d)
                .with_context(|| format!("1Click's deposit address {d} cannot be paid from this wallet"))?;
            deposit_transparent = kind == "transparent";
            if q.deposit_memo.is_some() {
                bail!("1Click asks for a deposit memo with ZEC; this app does not send one, so it does not use this quote");
            }
        }
        d
    };
    Ok(SwapRecord {
        id: id.clone(),
        dry: p.dry,
        created_at: rfc3339(ctx.now),
        direction: p.direction,
        origin: origin.into(),
        destination: destination.into(),
        amount_in: q.amount_in.clone(),
        amount_in_formatted: q.amount_in_formatted.clone(),
        amount_in_usd: q.amount_in_usd.clone(),
        min_amount_in_formatted: q.min_amount_in.as_deref().and_then(|m| format_units(m, origin.decimals).ok()),
        amount_out_formatted: q.amount_out_formatted.clone(),
        amount_out_usd: q.amount_out_usd.clone(),
        min_amount_out_formatted: format_units(&q.min_amount_out, destination.decimals)?,
        slippage_bps: slippage,
        deadline: q.deadline.clone().or_else(|| (!p.dry).then(|| req.deadline.clone())),
        time_estimate_secs: q.time_estimate,
        refund_fee_formatted: q.refund_fee.as_deref().and_then(|f| format_units(f, origin.decimals).ok()),
        deposit_address: (!p.dry).then_some(id),
        deposit_memo: q.deposit_memo.clone(),
        deposit_transparent,
        recipient,
        refund_to,
        confidentiality: req.confidentiality.clone().unwrap_or_else(|| "public".into()),
        authenticated: client.has_api_key(),
        quote: raw,
        proposal_id: None,
        deposit_txid: None,
        deposit_submitted: false,
        state: SwapState::Quoted,
        api_status: None,
        updated_at: None,
        checked_at: None,
        details: None,
        last_error: None,
    })
}

/// 1Click echoes the request it quoted. A quote for other assets, another
/// amount or other addresses is not used (as Zodl refuses a mismatch).
fn check_echo(req: &QuoteRequest, resp: &QuoteResponse) -> Result<()> {
    let echo = &resp.quote_request;
    let field = |k: &str| echo.get(k).and_then(Value::as_str).unwrap_or("");
    for (k, want) in [
        ("originAsset", req.origin_asset.as_str()),
        ("destinationAsset", req.destination_asset.as_str()),
        ("amount", req.amount.as_str()),
        ("recipient", req.recipient.as_str()),
        ("refundTo", req.refund_to.as_str()),
        ("swapType", req.swap_type.as_str()),
    ] {
        if field(k) != want {
            bail!("1Click quoted another swap than the one asked ({k} {:?}, asked {want:?}); not using it", field(k));
        }
    }
    Ok(())
}

/// Checks a stored swap can be paid from this wallet now and returns the
/// deposit amount in zatoshis.
pub fn check_executable(rec: &SwapRecord, network: Network, now: OffsetDateTime) -> Result<u64> {
    if network != Network::MainNetwork {
        bail!("NEAR Intents swaps are mainnet only.");
    }
    if rec.direction != Direction::FromZec {
        bail!("this swap is paid from another wallet, to its deposit address; nothing to sign here");
    }
    if let Some(txid) = &rec.deposit_txid {
        bail!("this swap's deposit was already sent ({txid})");
    }
    if rec.state != SwapState::Quoted {
        bail!("this swap is {:?}, not waiting for its deposit; request a new quote", rec.state);
    }
    let deadline = rec.deadline.as_deref().ok_or_else(|| anyhow!("this quote has no deadline; request a new one"))?;
    let deadline = parse_time(deadline)?;
    if deadline - now < time::Duration::seconds(MIN_TIME_LEFT_SECS) {
        bail!(
            "This quote expires at {}: too soon for a ZEC deposit to be mined. Request a new quote.",
            rfc3339(deadline)
        );
    }
    let address = rec.deposit_address.as_deref().ok_or_else(|| anyhow!("this quote has no deposit address"))?;
    zecz::wallet::check_address(network, address)?;
    let zat: u64 = rec.amount_in.parse().map_err(|_| anyhow!("bad deposit amount {:?}", rec.amount_in))?;
    if zat == 0 {
        bail!("bad deposit amount 0");
    }
    Ok(zat)
}

/// Asks 1Click where `rec` stands and updates it: tells it the deposit
/// txid first if it has not heard it, then reads the status. A failure
/// (network, API) is kept in `last_error`; the record is still returned.
pub fn refresh_status<T: Transport>(client: &OneClick<T>, rec: &mut SwapRecord, now: OffsetDateTime) {
    let Some(address) = rec.deposit_address.clone() else {
        return;
    };
    rec.last_error = None;
    if let (Some(txid), false) = (rec.deposit_txid.clone(), rec.deposit_submitted) {
        match client.submit_deposit(&txid, &address, rec.deposit_memo.as_deref()) {
            Ok(()) => rec.deposit_submitted = true,
            // Optional: 1Click finds the deposit by itself too.
            Err(e) => rec.last_error = Some(format!("could not tell 1Click the deposit txid: {}", explain(&e, None))),
        }
    }
    match client.status(&address, rec.deposit_memo.as_deref()) {
        Ok(st) => {
            let api = st.as_ref().map(|s| s.status.clone());
            rec.state = next_state(rec.state, api.as_deref(), rec.deposit_txid.is_some(), rec.deadline_passed(now));
            if let Some(s) = st {
                rec.api_status = Some(s.status);
                rec.updated_at = s.updated_at;
                if s.swap_details.is_some() {
                    rec.details = s.swap_details;
                }
            }
            rec.checked_at = Some(rfc3339(now));
        }
        Err(e) => rec.last_error = Some(explain(&e, None)),
    }
}

// ------------------------------------------------------------------ store

/// Format version of the swaps file.
const VERSION: u32 = 1;

#[derive(Debug, Serialize, Deserialize)]
struct Book {
    version: u32,
    swaps: Vec<SwapRecord>,
}

/// `<app data dir>/swaps-<test|main>.json`.
pub fn path_in(app_data: &Path, network: Network) -> PathBuf {
    app_data.join(format!("swaps-{}.json", zecz::wallet::network_name(network)))
}

/// The stored swaps, newest first; none when the file does not exist yet.
pub fn list(path: &Path) -> Result<Vec<SwapRecord>> {
    let bytes = match fs::read(path) {
        Ok(b) => b,
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => return Ok(Vec::new()),
        Err(e) => return Err(e).with_context(|| format!("reading {}", path.display())),
    };
    let book: Book = serde_json::from_slice(&bytes).with_context(|| format!("{} is not a swaps file", path.display()))?;
    if book.version != VERSION {
        bail!("{} has swaps format {}; this app reads {VERSION}", path.display(), book.version);
    }
    let mut s = book.swaps;
    s.sort_by(|a, b| b.created_at.cmp(&a.created_at));
    Ok(s)
}

fn write(path: &Path, swaps: Vec<SwapRecord>) -> Result<()> {
    if let Some(dir) = path.parent() {
        fs::create_dir_all(dir)?;
    }
    let json = serde_json::to_vec_pretty(&Book { version: VERSION, swaps })?;
    let tmp = path.with_extension("json.tmp");
    fs::write(&tmp, json).with_context(|| format!("writing {}", tmp.display()))?;
    fs::rename(&tmp, path).with_context(|| format!("replacing {}", path.display()))?;
    Ok(())
}

/// Adds `rec`, or replaces the stored swap with its id.
pub fn save(path: &Path, rec: &SwapRecord) -> Result<()> {
    if rec.dry {
        bail!("a dry quote is not stored");
    }
    let mut all = list(path)?;
    all.retain(|s| s.id != rec.id);
    all.push(rec.clone());
    write(path, all)
}

pub fn get(path: &Path, id: &str) -> Result<SwapRecord> {
    list(path)?.into_iter().find(|s| s.id == id).ok_or_else(|| anyhow!("there is no swap with deposit address {id}"))
}

/// Records the txid of a deposit signed and broadcast for proposal `proposal_id`.
/// Returns the swap it belonged to, if any.
pub fn record_deposit(path: &Path, proposal_id: &str, txid: &str) -> Result<Option<SwapRecord>> {
    let mut all = list(path)?;
    let Some(rec) = all.iter_mut().find(|s| s.proposal_id.as_deref() == Some(proposal_id) && s.deposit_txid.is_none()) else {
        return Ok(None);
    };
    rec.deposit_txid = Some(txid.to_string());
    rec.state = next_state(rec.state, None, true, false);
    let out = rec.clone();
    write(path, all)?;
    Ok(Some(out))
}

/// Forgets a swap that can no longer move funds: a final one, an expired
/// one, or a quote from ZEC whose deposit was never sent.
pub fn forget(path: &Path, id: &str) -> Result<Vec<SwapRecord>> {
    let mut all = list(path)?;
    let rec = all.iter().find(|s| s.id == id).ok_or_else(|| anyhow!("there is no swap with deposit address {id}"))?;
    let unfunded_own = rec.direction == Direction::FromZec && rec.deposit_txid.is_none() && rec.state == SwapState::Quoted;
    if !(rec.state.is_final() || rec.state == SwapState::Expired || unfunded_own) {
        bail!("this swap may still move funds; it can be removed once it succeeded, was refunded, failed or expired");
    }
    all.retain(|s| s.id != id);
    write(path, all.clone())?;
    Ok(all)
}
