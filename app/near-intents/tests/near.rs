//! NEAR Intents swaps (src/lib.rs) without the network: the 1Click
//! client against recorded answers, the quote checks, the refusals, the
//! status state machine and the swaps file.
//!
//! The fixtures in tests/fixtures/near are 1Click answers recorded on
//! 2026-10-17 (the token list, trimmed; two dry quotes; the errors). A
//! real quote's answer is the recorded dry one with the fields a real one
//! adds (`depositAddress`, `deadline`, `timeWhenInactive`; OpenAPI
//! `Quote`), echoing the request as 1Click does.
//!
//! One test is #[ignore]d: it reads the live token list and asks for a dry
//! quote, which creates no deposit address and moves nothing. Run it with
//! `cargo test --test near -- --ignored`.

use std::collections::VecDeque;
use std::path::PathBuf;
use std::sync::Mutex;

use leek_near_intents::{
    self as near, check_funds, format_units, next_state, parse_units, Direction, HttpResponse, OneClick,
    QuoteContext, QuoteParams, SwapRecord, SwapState, Token, Transport, PREVIEW_ADDRESS, ZEC_ASSET,
};
use serde_json::{json, Value};
use time::OffsetDateTime;
use zecz::Network;

const MAIN: Network = Network::MainNetwork;
const TEST: Network = Network::TestNetwork;

/// The address check the app hands the crate: zecz's, on `net`.
fn checker(net: Network) -> impl Fn(&str) -> anyhow::Result<String> {
    move |a| zecz::wallet::check_address(net, a).map(str::to_string)
}

/// [`near::check_executable`] for a wallet on `net`.
fn check_executable(rec: &SwapRecord, net: Network, now: OffsetDateTime) -> anyhow::Result<u64> {
    near::check_executable(rec, net == MAIN, &checker(net), now)
}
const USDC_SOL: &str = "nep141:sol-5ce3bf3a31af18be40ba30f721101b4341690186.omft.near";
const SOL_WALLET: &str = "9WzDXwBbmkg8ZTbNMqUxvQRAyrZzDsGYdLVL9zYtAWWM";
/// A mainnet transparent address (vectors), standing in for a deposit address.
const T_DEPOSIT: &str = "t1VZkHQ44vp9utYhVg2eokmxuzojjX6qL9v";
/// A mainnet Orchard-only address (vectors), standing in for a fresh one.
const OWN: &str = "u1yr0x0lh5svjdqqq20rfgwzkyzy3wv94740zjgr07yjkngmpquc4gjzlsp32kscwtw6z43rtx9xzvf96l597m7ctv3v93m87tkqwzs2m9";

fn fixture(name: &str) -> String {
    std::fs::read_to_string(PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("tests/fixtures/near").join(name)).unwrap()
}

fn error(name: &str) -> HttpResponse {
    let all: Value = serde_json::from_str(&fixture("errors.json")).unwrap();
    let e = &all[name];
    HttpResponse { status: e["status"].as_u64().unwrap() as u16, body: e["body"].to_string() }
}

fn ok(body: impl Into<String>) -> HttpResponse {
    HttpResponse { status: 200, body: body.into() }
}

fn now() -> OffsetDateTime {
    near::parse_time("2026-10-17T20:13:00Z").unwrap()
}

#[derive(Clone, Debug)]
struct Call {
    method: &'static str,
    url: String,
    query: Vec<(String, String)>,
    headers: Vec<(String, String)>,
    body: String,
}

/// A recorded 1Click: answers by path, and keeps every request.
struct Mock {
    calls: Mutex<Vec<Call>>,
    /// Quote answers: built from the request (see [`quote_answer`]).
    quote: Box<dyn Fn(&Value) -> HttpResponse + Send + Sync>,
    /// Status answers, in order; the last one repeats.
    status: Mutex<VecDeque<HttpResponse>>,
    submit: Mutex<VecDeque<HttpResponse>>,
}

impl Mock {
    fn new(quote: impl Fn(&Value) -> HttpResponse + Send + Sync + 'static) -> Self {
        Mock { calls: Mutex::new(Vec::new()), quote: Box::new(quote), status: Mutex::new(VecDeque::new()), submit: Mutex::new(VecDeque::new()) }
    }

    fn calls(&self) -> Vec<Call> {
        self.calls.lock().unwrap().clone()
    }

    fn next(q: &Mutex<VecDeque<HttpResponse>>) -> HttpResponse {
        let mut q = q.lock().unwrap();
        if q.len() > 1 {
            q.pop_front().unwrap()
        } else {
            q.front().cloned().unwrap_or_else(|| HttpResponse { status: 599, body: "{\"message\":\"no answer recorded\"}".into() })
        }
    }

    fn answer(&self, c: &Call) -> HttpResponse {
        if c.url.ends_with("/v0/tokens") {
            ok(fixture("tokens.json"))
        } else if c.url.ends_with("/v0/quote") {
            (self.quote)(&serde_json::from_str(&c.body).unwrap())
        } else if c.url.ends_with("/v0/status") {
            Self::next(&self.status)
        } else if c.url.ends_with("/v0/deposit/submit") {
            Self::next(&self.submit)
        } else {
            HttpResponse { status: 404, body: "{\"message\":\"Cannot GET\"}".into() }
        }
    }
}

impl Transport for &Mock {
    fn get(&self, url: &str, query: &[(&str, &str)], headers: &[(&str, &str)]) -> anyhow::Result<HttpResponse> {
        let c = Call {
            method: "GET",
            url: url.into(),
            query: query.iter().map(|(k, v)| (k.to_string(), v.to_string())).collect(),
            headers: headers.iter().map(|(k, v)| (k.to_string(), v.to_string())).collect(),
            body: String::new(),
        };
        let r = self.answer(&c);
        self.calls.lock().unwrap().push(c);
        Ok(r)
    }

    fn post(&self, url: &str, headers: &[(&str, &str)], body: &str) -> anyhow::Result<HttpResponse> {
        let c = Call {
            method: "POST",
            url: url.into(),
            query: Vec::new(),
            headers: headers.iter().map(|(k, v)| (k.to_string(), v.to_string())).collect(),
            body: body.into(),
        };
        let r = self.answer(&c);
        self.calls.lock().unwrap().push(c);
        Ok(r)
    }
}

/// 1Click's answer to `req`: the recorded quote for its direction, echoing
/// the request, with a deposit address and deadline when it is not dry.
fn quote_answer(req: &Value, deposit: &str) -> HttpResponse {
    let from_zec = req["originAsset"] == ZEC_ASSET;
    let mut r: Value =
        serde_json::from_str(&fixture(if from_zec { "quote_dry_from_zec.json" } else { "quote_dry_to_zec.json" })).unwrap();
    let mut echo = req.clone();
    echo["depositMode"] = json!("SIMPLE");
    echo["insured"] = json!(false);
    r["quoteRequest"] = echo;
    r["quote"]["amountIn"] = req["amount"].clone();
    if req["dry"] == json!(false) {
        r["quote"]["depositAddress"] = json!(deposit);
        r["quote"]["deadline"] = req["deadline"].clone();
        r["quote"]["timeWhenInactive"] = req["deadline"].clone();
    }
    ok(r.to_string())
}

fn tokens(m: &Mock) -> Vec<Token> {
    OneClick::with_base(m, "https://1click.test", None).tokens().unwrap()
}

fn from_zec(amount: &str, dry: bool) -> QuoteParams {
    serde_json::from_value(json!({
        "direction": "fromZec", "asset": USDC_SOL, "amount": amount, "recipient": SOL_WALLET, "dry": dry
    }))
    .unwrap()
}

fn to_zec(amount: &str) -> QuoteParams {
    serde_json::from_value(json!({
        "direction": "toZec", "asset": USDC_SOL, "amount": amount, "refundTo": SOL_WALLET, "slippageBps": 200
    }))
    .unwrap()
}

/// A wallet with `spendable` zatoshis (None: never synced) whose fresh
/// address is [`OWN`].
fn quote_with(m: &Mock, network: Network, spendable: Option<u64>, p: &QuoteParams, key: Option<&str>) -> anyhow::Result<SwapRecord> {
    let toks = tokens(m);
    let own = || -> anyhow::Result<String> { Ok(OWN.to_string()) };
    let bal = move || -> anyhow::Result<Option<u64>> { Ok(spendable) };
    let check = checker(network);
    let ctx = QuoteContext { mainnet: network == MAIN, own_address: &own, spendable: &bal, check_address: &check, now: now() };
    near::request_quote(&OneClick::with_base(m, "https://1click.test", key.map(str::to_string)), &toks, &ctx, p)
}

fn quote_calls(m: &Mock) -> Vec<Value> {
    m.calls().iter().filter(|c| c.url.ends_with("/v0/quote")).map(|c| serde_json::from_str(&c.body).unwrap()).collect()
}

// ------------------------------------------------------------------ client

#[test]
fn tokens_are_read_deduplicated_and_sorted() {
    let m = Mock::new(|_| unreachable!());
    let t = OneClick::with_base(&m, "https://1click.test/", Some("  jwt.token  ".into())).tokens().unwrap();
    let names: Vec<_> = t.iter().map(|t| format!("{}/{}", t.symbol, t.blockchain)).collect();
    assert_eq!(names, ["BTC/btc", "USDC/eth", "USDC/near", "USDC/sol", "ZEC/zec"]);
    // The first of a duplicate is kept, as 1Click lists it.
    assert_eq!(t.iter().find(|t| t.blockchain == "sol").unwrap().asset_id, USDC_SOL);
    let zec = t.iter().find(|t| t.asset_id == ZEC_ASSET).unwrap();
    assert_eq!((zec.decimals, zec.price), (8, Some(1156.99)));
    let c = &m.calls()[0];
    assert_eq!((c.method, c.url.as_str()), ("GET", "https://1click.test/v0/tokens"));
    // The key travels as a bearer token, trimmed.
    assert_eq!(c.headers, vec![("Authorization".to_string(), "Bearer jwt.token".to_string())]);

    // No key, no header.
    let m = Mock::new(|_| unreachable!());
    let _ = tokens(&m);
    assert!(m.calls()[0].headers.is_empty());
}

#[test]
fn amounts_in_base_units() {
    assert_eq!(parse_units("0.01", 8).unwrap(), 1_000_000);
    assert_eq!(parse_units("1", 6).unwrap(), 1_000_000);
    assert_eq!(parse_units(".5", 6).unwrap(), 500_000);
    assert_eq!(parse_units("10.000001", 6).unwrap(), 10_000_001);
    assert_eq!(parse_units("1", 24).unwrap(), 10u128.pow(24));
    for bad in ["", ".", "0", "0.000", "1.0000001", "-1", "1e3", "1,5", "abc"] {
        assert!(parse_units(bad, 6).is_err(), "{bad:?}");
    }
    assert_eq!(format_units("11415125", 6).unwrap(), "11.415125");
    assert_eq!(format_units("132000", 8).unwrap(), "0.00132");
    assert_eq!(format_units("100000000", 8).unwrap(), "1");
    assert_eq!(format_units("0", 8).unwrap(), "0");
    assert!(format_units("1.5", 8).is_err());
    assert_eq!(near::zat_to_zec(1_000_000), "0.01000000");
}

// ------------------------------------------------------------------ quotes

#[test]
fn dry_quote_on_testnet_previews_with_the_placeholder() {
    let m = Mock::new(|r| quote_answer(r, "unused"));
    let rec = quote_with(&m, TEST, None, &from_zec("0.01", true), None).unwrap();
    let req = &quote_calls(&m)[0];
    assert_eq!(req["dry"], json!(true));
    assert_eq!(req["swapType"], "EXACT_INPUT");
    assert_eq!(req["originAsset"], ZEC_ASSET);
    assert_eq!(req["destinationAsset"], USDC_SOL);
    assert_eq!(req["amount"], "1000000");
    assert_eq!(req["slippageTolerance"], 100);
    assert_eq!((req["depositType"].as_str(), req["refundType"].as_str()), (Some("ORIGIN_CHAIN"), Some("ORIGIN_CHAIN")));
    assert_eq!(req["recipientType"], "DESTINATION_CHAIN");
    assert_eq!(req["recipient"], SOL_WALLET);
    // Testnet has no mainnet address: the public placeholder.
    assert_eq!(req["refundTo"], PREVIEW_ADDRESS);
    // Two hours from now; no fee recipients, no confidentiality asked.
    assert_eq!(req["deadline"], "2026-10-17T22:13:00Z");
    assert!(req.get("appFees").is_none() && req.get("confidentiality").is_none());
    assert_eq!(req["quoteWaitingTimeMs"], 3000);

    assert!(rec.dry);
    assert_eq!(rec.id, "dry");
    assert_eq!(rec.deposit_address, None);
    assert_eq!(rec.deadline, None);
    assert_eq!(rec.amount_out_formatted, "11.53043");
    assert_eq!(rec.min_amount_out_formatted, "11.415125");
    assert_eq!(rec.refund_fee_formatted.as_deref(), Some("0.00032"));
    assert_eq!(rec.time_estimate_secs, Some(452.0));
    assert!(!rec.authenticated);
    // Not stored.
    let d = TempDir::new("dry");
    assert!(near::save(&d.0.join("swaps-main.json"), &rec).is_err());
}

#[test]
fn real_quote_from_zec_on_mainnet() {
    let m = Mock::new(|r| quote_answer(r, T_DEPOSIT));
    let rec = quote_with(&m, MAIN, Some(5_000_000), &from_zec("0.01", false), None).unwrap();
    let req = &quote_calls(&m)[0];
    assert_eq!(req["dry"], json!(false));
    // Refunds go to the wallet's fresh shielded address.
    assert_eq!(req["refundTo"], OWN);
    assert_eq!(rec.refund_to, OWN);
    assert_eq!(rec.recipient, SOL_WALLET);
    assert_eq!(rec.id, T_DEPOSIT);
    assert_eq!(rec.deposit_address.as_deref(), Some(T_DEPOSIT));
    assert!(rec.deposit_transparent);
    assert_eq!(rec.state, SwapState::Quoted);
    assert_eq!(rec.amount_in, "1000000");
    assert_eq!(rec.deadline.as_deref(), Some("2026-10-17T22:13:00Z"));
    assert_eq!(rec.direction, Direction::FromZec);
    assert_eq!((rec.origin.symbol.as_str(), rec.destination.blockchain.as_str()), ("ZEC", "sol"));
    assert_eq!(rec.confidentiality, "public");
    // The whole answer is kept, signature included.
    assert!(rec.quote["signature"].as_str().unwrap().starts_with("ed25519:"));
    assert_eq!(check_executable(&rec, MAIN, now()).unwrap(), 1_000_000);

    // The JSON the frontend reads.
    let v = serde_json::to_value(&rec).unwrap();
    assert_eq!(v["direction"], "fromZec");
    assert_eq!(v["state"], "quoted");
    assert_eq!(v["depositAddress"], T_DEPOSIT);
    assert_eq!(v["minAmountOutFormatted"], "11.415125");
}

#[test]
fn real_quote_into_zec_pays_a_shielded_address() {
    let m = Mock::new(|r| quote_answer(r, SOL_WALLET));
    // No ZEC needed: the payer pays from another wallet.
    let rec = quote_with(&m, MAIN, None, &to_zec("10"), None).unwrap();
    let req = &quote_calls(&m)[0];
    assert_eq!(req["swapType"], "FLEX_INPUT");
    assert_eq!(req["originAsset"], USDC_SOL);
    assert_eq!(req["destinationAsset"], ZEC_ASSET);
    assert_eq!(req["amount"], "10000000");
    assert_eq!(req["slippageTolerance"], 200);
    assert_eq!(req["recipient"], OWN);
    assert_eq!(req["refundTo"], SOL_WALLET);
    assert_eq!(rec.deposit_address.as_deref(), Some(SOL_WALLET));
    assert!(!rec.deposit_transparent);
    assert_eq!(rec.min_amount_in_formatted.as_deref(), Some("9.9"));
    assert_eq!(rec.amount_out_formatted, "0.00830782");
    assert_eq!(rec.min_amount_out_formatted, "0.00822474");
    // Nothing to sign here.
    let e = check_executable(&rec, MAIN, now()).unwrap_err().to_string();
    assert!(e.contains("paid from another wallet"), "{e}");

    // The payer's refund address is required.
    let mut p = to_zec("10");
    p.refund_to = None;
    let e = quote_with(&m, MAIN, None, &p, None).unwrap_err().to_string();
    assert!(e.contains("refund address"), "{e}");
}

#[test]
fn quote_refusals_before_1click() {
    let m = Mock::new(|_| panic!("no quote should be asked"));
    // Testnet: dry only.
    let e = quote_with(&m, TEST, Some(10u64.pow(8)), &from_zec("0.01", false), None).unwrap_err().to_string();
    assert!(e.contains("mainnet only"), "{e}");
    // Not enough ZEC for the deposit and a fee.
    let e = quote_with(&m, MAIN, Some(1_005_000), &from_zec("0.01", false), None).unwrap_err().to_string();
    assert!(e.contains("Not enough spendable ZEC") && e.contains("0.01000000") && e.contains("0.01005000"), "{e}");
    // Never synced.
    let e = quote_with(&m, MAIN, None, &from_zec("0.01", false), None).unwrap_err().to_string();
    assert!(e.contains("Sync the wallet first"), "{e}");
    // More decimals than ZEC has.
    assert!(quote_with(&m, MAIN, Some(10u64.pow(8)), &from_zec("0.000000001", false), None).is_err());
    // No recipient; a recipient with spaces.
    let mut p = from_zec("0.01", true);
    p.recipient = Some("  ".into());
    assert!(quote_with(&m, MAIN, None, &p, None).unwrap_err().to_string().contains("recipient"));
    p.recipient = Some("two words".into());
    assert!(quote_with(&m, MAIN, None, &p, None).unwrap_err().to_string().contains("does not look like an address"));
    // ZEC for ZEC, an unknown asset, a silly slippage.
    let mut p = from_zec("0.01", true);
    p.asset = ZEC_ASSET.into();
    assert!(quote_with(&m, MAIN, None, &p, None).is_err());
    p.asset = "nep141:nope.near".into();
    assert!(quote_with(&m, MAIN, None, &p, None).unwrap_err().to_string().contains("does not list"));
    let mut p = from_zec("0.01", true);
    p.slippage_bps = Some(5000);
    assert!(quote_with(&m, MAIN, None, &p, None).unwrap_err().to_string().contains("slippage"));
    // Confidential needs a key.
    let mut p = from_zec("0.01", true);
    p.confidential = true;
    assert!(quote_with(&m, MAIN, None, &p, None).unwrap_err().to_string().contains("API key"));
    assert!(quote_calls(&m).is_empty());
}

#[test]
fn quote_errors_in_words() {
    let m = Mock::new(|_| error("too_low"));
    let e = quote_with(&m, MAIN, None, &from_zec("0.001", true), None).unwrap_err().to_string();
    assert_eq!(e, "The amount is too small for this route: 1Click needs at least 0.00132 ZEC.");

    // Confidential with a key 1Click does not accept for it.
    let m = Mock::new(|_| error("confidential_unauthenticated"));
    let mut p = from_zec("0.01", true);
    p.confidential = true;
    let e = quote_with(&m, MAIN, None, &p, Some("key")).unwrap_err().to_string();
    assert!(e.contains("Confidential swaps need a 1Click API key"), "{e}");
    assert_eq!(quote_calls(&m)[0]["confidentiality"], "basic");

    let m = Mock::new(|_| error("internal"));
    let e = quote_with(&m, MAIN, None, &from_zec("0.01", true), None).unwrap_err().to_string();
    assert!(e.contains("1Click failed (500: Internal server error)"), "{e}");

    let m = Mock::new(|_| ok(json!({"message": "No quotes found"}).to_string()));
    assert!(quote_with(&m, MAIN, None, &from_zec("0.01", true), None).is_err());
    let m = Mock::new(|_| HttpResponse { status: 400, body: json!({"message": "No quotes found"}).to_string() });
    let e = quote_with(&m, MAIN, None, &from_zec("0.01", true), None).unwrap_err().to_string();
    assert!(e.contains("No market maker"), "{e}");
}

#[test]
fn a_quote_for_another_swap_is_not_used() {
    // 1Click echoing another recipient.
    let m = Mock::new(|r| {
        let mut r = r.clone();
        r["recipient"] = json!("SomeoneElse111111111111111111111111111111111");
        quote_answer(&r, T_DEPOSIT)
    });
    let e = quote_with(&m, MAIN, Some(10u64.pow(8)), &from_zec("0.01", false), None).unwrap_err().to_string();
    assert!(e.contains("quoted another swap") && e.contains("recipient"), "{e}");

    // A deposit amount other than the one asked.
    let m = Mock::new(|r| {
        let resp = quote_answer(r, T_DEPOSIT);
        let mut v: Value = serde_json::from_str(&resp.body).unwrap();
        v["quote"]["amountIn"] = json!("2000000");
        ok(v.to_string())
    });
    let e = quote_with(&m, MAIN, Some(10u64.pow(8)), &from_zec("0.01", false), None).unwrap_err().to_string();
    assert!(e.contains("instead of 1000000"), "{e}");

    // A deposit address this wallet cannot pay (another network's).
    let m = Mock::new(|r| quote_answer(r, "tmBsTi2xWTjUdEXnuTceL7fecEQKeWaPDJd"));
    let e = format!("{:#}", quote_with(&m, MAIN, Some(10u64.pow(8)), &from_zec("0.01", false), None).unwrap_err());
    assert!(e.contains("cannot be paid from this wallet"), "{e}");

    // No deposit address on a real quote.
    let m = Mock::new(|r| {
        let mut d = r.clone();
        d["dry"] = json!(true);
        quote_answer(&d, "")
    });
    let e = quote_with(&m, MAIN, Some(10u64.pow(8)), &from_zec("0.01", false), None).unwrap_err().to_string();
    assert!(e.contains("without a deposit address"), "{e}");
}

// ---------------------------------------------------------------- execute

fn stored_quote() -> SwapRecord {
    let m = Mock::new(|r| quote_answer(r, T_DEPOSIT));
    quote_with(&m, MAIN, Some(10u64.pow(8)), &from_zec("0.01", false), None).unwrap()
}

#[test]
fn execute_refusals() {
    let rec = stored_quote();
    assert_eq!(check_executable(&rec, MAIN, now()).unwrap(), 1_000_000);
    // Testnet.
    assert!(check_executable(&rec, TEST, now()).unwrap_err().to_string().contains("mainnet only"));
    // Expired, or too close to the deadline for a deposit to be mined.
    let later = now() + time::Duration::minutes(110);
    let e = check_executable(&rec, MAIN, later).unwrap_err().to_string();
    assert!(e.contains("too soon") && e.contains("new quote"), "{e}");
    assert!(check_executable(&rec, MAIN, now() + time::Duration::hours(3)).is_err());
    assert!(check_executable(&rec, MAIN, now() + time::Duration::minutes(100)).is_ok());
    // Already paid.
    let mut paid = rec.clone();
    paid.deposit_txid = Some("ab".repeat(32));
    assert!(check_executable(&paid, MAIN, now()).unwrap_err().to_string().contains("already sent"));
    // Not waiting for a deposit any more.
    let mut gone = rec.clone();
    gone.state = SwapState::Expired;
    assert!(check_executable(&gone, MAIN, now()).is_err());

    // Funds: the deposit plus the smallest fee.
    assert!(check_funds(Some(1_010_000), 1_000_000).is_ok());
    assert!(check_funds(Some(1_009_999), 1_000_000).is_err());
    assert!(check_funds(None, 1).is_err());
}

// ----------------------------------------------------------------- status

#[test]
fn the_state_machine() {
    use SwapState::*;
    // 1Click's answers.
    assert_eq!(next_state(Quoted, Some("PENDING_DEPOSIT"), false, false), Quoted);
    assert_eq!(next_state(Quoted, None, false, false), Quoted);
    assert_eq!(next_state(Quoted, None, false, true), Expired);
    assert_eq!(next_state(Quoted, Some("PENDING_DEPOSIT"), true, false), Deposited);
    assert_eq!(next_state(Deposited, Some("KNOWN_DEPOSIT_TX"), true, false), DepositSeen);
    assert_eq!(next_state(DepositSeen, Some("PROCESSING"), true, false), Processing);
    assert_eq!(next_state(Processing, Some("SUCCESS"), true, false), Success);
    assert_eq!(next_state(Processing, Some("REFUNDED"), true, false), Refunded);
    assert_eq!(next_state(Deposited, Some("FAILED"), true, false), Failed);
    assert_eq!(next_state(Quoted, Some("INCOMPLETE_DEPOSIT"), false, false), Incomplete);
    // A payment from another wallet seen after the deadline still counts.
    assert_eq!(next_state(Expired, Some("KNOWN_DEPOSIT_TX"), false, true), DepositSeen);
    assert_eq!(next_state(Expired, Some("REFUNDED"), false, true), Refunded);
    // Never backwards: a stale PENDING after PROCESSING, or an unknown status.
    assert_eq!(next_state(Processing, Some("PENDING_DEPOSIT"), true, false), Processing);
    assert_eq!(next_state(DepositSeen, Some("SOMETHING_NEW"), true, false), DepositSeen);
    assert_eq!(next_state(Expired, None, false, true), Expired);
    // Final states stay final.
    for f in [Success, Refunded, Failed] {
        assert!(f.is_final());
        for api in [None, Some("PROCESSING"), Some("PENDING_DEPOSIT"), Some("SUCCESS"), Some("REFUNDED")] {
            assert_eq!(next_state(f, api, true, true), f);
        }
    }
    assert!(!Expired.is_final() && !Quoted.is_final());
}

fn status(s: &str, details: Value) -> HttpResponse {
    ok(json!({
        "correlationId": "c",
        "quoteResponse": serde_json::from_str::<Value>(&fixture("quote_dry_from_zec.json")).unwrap(),
        "status": s,
        "updatedAt": "2026-10-17T20:20:00.000Z",
        "swapDetails": details,
    })
    .to_string())
}

#[test]
fn polling_a_swap_from_zec() {
    let m = Mock::new(|_| unreachable!());
    let client = OneClick::with_base(&m, "https://1click.test", None);
    let mut rec = stored_quote();
    rec.deposit_txid = Some("aa".repeat(32));
    rec.state = SwapState::Deposited;

    let empty = json!({"intentHashes": [], "nearTxHashes": [], "originChainTxHashes": [], "destinationChainTxHashes": []});
    m.submit.lock().unwrap().extend([HttpResponse { status: 502, body: "bad gateway".into() }, ok("{}")]);
    m.status.lock().unwrap().extend([
        error("status_not_found"),
        status("KNOWN_DEPOSIT_TX", empty.clone()),
        status("PROCESSING", empty),
        status(
            "SUCCESS",
            json!({
                "intentHashes": ["h"], "nearTxHashes": ["n"],
                "amountInFormatted": "0.01", "amountOutFormatted": "11.52",
                "originChainTxHashes": [{"hash": "aa", "explorerUrl": "https://explorer/zec/aa"}],
                "destinationChainTxHashes": [{"hash": "5x", "explorerUrl": "https://solscan.io/tx/5x"}],
            }),
        ),
    ]);

    // The txid submit fails once: kept as the error, tried again next time.
    near::refresh_status(&client, &mut rec, now());
    assert!(!rec.deposit_submitted);
    assert!(rec.last_error.as_deref().unwrap().contains("deposit txid"), "{:?}", rec.last_error);
    // 1Click does not know the address yet (404): still Deposited.
    assert_eq!(rec.state, SwapState::Deposited);
    assert_eq!(rec.api_status, None);

    near::refresh_status(&client, &mut rec, now());
    assert!(rec.deposit_submitted);
    assert_eq!(rec.last_error, None);
    assert_eq!((rec.state, rec.api_status.as_deref()), (SwapState::DepositSeen, Some("KNOWN_DEPOSIT_TX")));

    near::refresh_status(&client, &mut rec, now());
    assert_eq!(rec.state, SwapState::Processing);
    near::refresh_status(&client, &mut rec, now());
    assert_eq!(rec.state, SwapState::Success);
    let d = rec.details.as_ref().unwrap();
    assert_eq!(d.amount_out_formatted.as_deref(), Some("11.52"));
    assert_eq!(d.destination_chain_tx_hashes[0].explorer_url, "https://solscan.io/tx/5x");
    assert_eq!(rec.checked_at.as_deref(), Some("2026-10-17T20:13:00Z"));

    // The submit went once with the txid; status asked by deposit address.
    let calls = m.calls();
    let submits: Vec<_> = calls.iter().filter(|c| c.url.ends_with("/deposit/submit")).collect();
    assert_eq!(submits.len(), 2);
    let body: Value = serde_json::from_str(&submits[1].body).unwrap();
    assert_eq!(body, json!({"txHash": "aa".repeat(32), "depositAddress": T_DEPOSIT}));
    let st = calls.iter().find(|c| c.url.ends_with("/v0/status")).unwrap();
    assert_eq!(st.query, vec![("depositAddress".to_string(), T_DEPOSIT.to_string())]);

    // Final: nothing changes it.
    near::refresh_status(&client, &mut rec, now());
    assert_eq!(rec.state, SwapState::Success);
}

#[test]
fn polling_a_quote_nobody_paid() {
    let m = Mock::new(|_| unreachable!());
    let client = OneClick::with_base(&m, "https://1click.test", None);
    let mut rec = stored_quote();
    let empty = json!({"intentHashes": [], "nearTxHashes": [], "originChainTxHashes": [], "destinationChainTxHashes": []});
    m.status.lock().unwrap().push_back(status("PENDING_DEPOSIT", empty));
    near::refresh_status(&client, &mut rec, now());
    assert_eq!(rec.state, SwapState::Quoted);
    // No txid, so no submit.
    assert!(m.calls().iter().all(|c| !c.url.ends_with("/deposit/submit")));
    near::refresh_status(&client, &mut rec, now() + time::Duration::hours(3));
    assert_eq!(rec.state, SwapState::Expired);

    // A refund, with its reason.
    m.status.lock().unwrap().clear();
    m.status.lock().unwrap().push_back(status(
        "REFUNDED",
        json!({"intentHashes": [], "nearTxHashes": [], "originChainTxHashes": [], "destinationChainTxHashes": [],
               "refundedAmountFormatted": "0.00968", "refundReason": "deposit too late"}),
    ));
    near::refresh_status(&client, &mut rec, now() + time::Duration::hours(4));
    assert_eq!(rec.state, SwapState::Refunded);
    assert_eq!(rec.details.as_ref().unwrap().refund_reason.as_deref(), Some("deposit too late"));

    // A network failure is kept, the state is not touched.
    let mut other = stored_quote();
    let m = Mock::new(|_| unreachable!());
    m.status.lock().unwrap().push_back(error("internal"));
    near::refresh_status(&OneClick::with_base(&m, "https://1click.test", None), &mut other, now());
    assert_eq!(other.state, SwapState::Quoted);
    assert!(other.last_error.as_deref().unwrap().contains("500"));
}

// ------------------------------------------------------------------ store

struct TempDir(PathBuf);

impl TempDir {
    fn new(tag: &str) -> Self {
        let p = std::env::temp_dir().join(format!("leekz-near-{tag}-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&p);
        std::fs::create_dir_all(&p).unwrap();
        TempDir(p)
    }
}

impl Drop for TempDir {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(&self.0);
    }
}

#[test]
fn swaps_are_kept_in_their_file() {
    let d = TempDir::new("store");
    let path = d.0.join("swaps-main.json");
    assert!(near::list(&path).unwrap().is_empty());

    let mut a = stored_quote();
    a.proposal_id = Some("p1".into());
    near::save(&path, &a).unwrap();
    let mut b = stored_quote();
    b.id = "t1Z5Ymss1sv4oA7WprAAEr43PyhWFHTeSfc".into();
    b.deposit_address = Some(b.id.clone());
    b.created_at = "2026-10-17T21:00:00Z".into();
    near::save(&path, &b).unwrap();
    // Newest first; saving again replaces.
    near::save(&path, &b).unwrap();
    let all = near::list(&path).unwrap();
    assert_eq!(all.iter().map(|s| s.id.as_str()).collect::<Vec<_>>(), [b.id.as_str(), T_DEPOSIT]);
    assert_eq!(near::get(&path, T_DEPOSIT).unwrap(), a);
    assert!(near::get(&path, "nope").is_err());
    assert!(!path.with_extension("json.tmp").exists());

    // The deposit signed for proposal p1 is recorded with its swap, once.
    let r = near::record_deposit(&path, "p1", "txid1").unwrap().unwrap();
    assert_eq!((r.deposit_txid.as_deref(), r.state), (Some("txid1"), SwapState::Deposited));
    assert!(near::record_deposit(&path, "p1", "txid2").unwrap().is_none());
    assert!(near::record_deposit(&path, "other", "txid3").unwrap().is_none());
    assert_eq!(near::get(&path, T_DEPOSIT).unwrap().deposit_txid.as_deref(), Some("txid1"));

    // A funded swap in flight cannot be forgotten; an unfunded quote can.
    let e = near::forget(&path, T_DEPOSIT).unwrap_err().to_string();
    assert!(e.contains("may still move funds"), "{e}");
    let rest = near::forget(&path, &b.id).unwrap();
    assert_eq!(rest.len(), 1);
    let mut done = near::get(&path, T_DEPOSIT).unwrap();
    done.state = SwapState::Success;
    near::save(&path, &done).unwrap();
    assert!(near::forget(&path, T_DEPOSIT).unwrap().is_empty());

    // Another format version is refused, not overwritten.
    std::fs::write(&path, r#"{"version": 2, "swaps": []}"#).unwrap();
    assert!(near::list(&path).unwrap_err().to_string().contains("format 2"));
}

// ------------------------------------------------------------------- live

/// Reads the live token list and asks for a dry quote of 0.01 ZEC to USDC
/// on Solana: no deposit address is created, nothing is paid.
#[test]
#[ignore = "needs the 1Click API (network); dry quote only, no funds"]
fn live_tokens_and_dry_quote() {
    let client = OneClick::new(near::Http::default(), near::api_key(None));
    let tokens = client.tokens().unwrap();
    let zec = tokens.iter().find(|t| t.asset_id == ZEC_ASSET).expect("ZEC listed");
    assert_eq!(zec.decimals, 8);
    assert!(tokens.iter().any(|t| t.asset_id == USDC_SOL));
    let own = || -> anyhow::Result<String> { Ok(OWN.to_string()) };
    let bal = || -> anyhow::Result<Option<u64>> { Ok(None) };
    let check = checker(MAIN);
    let ctx = QuoteContext { mainnet: true, own_address: &own, spendable: &bal, check_address: &check, now: OffsetDateTime::now_utc() };
    let rec = near::request_quote(&client, &tokens, &ctx, &from_zec("0.01", true)).unwrap();
    assert!(rec.dry && rec.deposit_address.is_none());
    assert_eq!(rec.amount_in, "1000000");
    let out: f64 = rec.amount_out_formatted.parse().unwrap();
    assert!(out > 0.0, "{rec:?}");
    eprintln!("0.01 ZEC -> {} USDC (at least {}), about {:?} s", rec.amount_out_formatted, rec.min_amount_out_formatted, rec.time_estimate_secs);
}
