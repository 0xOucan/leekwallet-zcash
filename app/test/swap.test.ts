/**
 * The Swap screen's rules (src/swap-model.ts) under plain Node: the
 * timeline, the refund line (1Click's "0" is not a refund), which swaps may
 * be paid, removed or polled, the form's checks and the quote's rows. No
 * network: the records are what src-tauri/src/swaps.rs returns.
 */

import {
  checkForm, payable, quoteRows, refundLine, removable, stepIndex, timeline, toPoll, assetName, usd,
  STATE_TEXT, type Swap, type SwapState,
} from "../src/swap-model.ts";

let failures = 0;
const check = (c: boolean, m: string) => { if (!c) { console.log(`  FAIL: ${m}`); failures++; } };
const group = (n: string) => console.log(`== ${n}`);

const ZEC = { assetId: "nep141:zec.omft.near", symbol: "ZEC", blockchain: "zec", decimals: 8 };
const USDC = { assetId: "nep141:sol-usdc", symbol: "USDC", blockchain: "sol", decimals: 6 };

function swap(over: Partial<Swap> = {}): Swap {
  return {
    id: "t1Deposit", dry: false, createdAt: "2026-10-17T20:13:00Z", direction: "fromZec",
    origin: ZEC, destination: USDC, amountIn: "1000000", amountInFormatted: "0.01", amountInUsd: "11.57",
    minAmountInFormatted: null, amountOutFormatted: "11.5", amountOutUsd: "11.5", minAmountOutFormatted: "11.38",
    slippageBps: 100, deadline: "2026-10-17T22:13:00Z", timeEstimateSecs: 120, refundFeeFormatted: null,
    depositAddress: "t1Deposit", depositMemo: null, depositTransparent: true, recipient: "SoLRecipient",
    refundTo: "u1own", confidentiality: "public", authenticated: false, proposalId: null, depositTxid: null,
    depositSubmitted: false, state: "quoted", apiStatus: null, updatedAt: null, checkedAt: null, details: null,
    lastError: null, ...over,
  };
}

group("timeline: every state has its step and its words");
{
  const all: SwapState[] = ["quoted", "expired", "deposited", "depositSeen", "incomplete", "processing", "success", "refunded", "failed"];
  for (const s of all) {
    check(STATE_TEXT[s].length > 0, `${s} has words`);
    const t = timeline(s);
    check(t.length === 5, `${s}: five steps`);
    check(t.filter((x) => x.step === "now").length <= 1, `${s}: one current step at most`);
  }
  check(stepIndex("deposited") === 1 && stepIndex("processing") === 3, "step order");
  const ok = timeline("success");
  check(ok.every((x) => x.step === "done"), "success: all done");
  const ref = timeline("refunded");
  check(ref[4]!.label === "Refunded" && ref[4]!.step === "bad", "refunded: the last step says so");
  const exp = timeline("expired");
  check(exp[0]!.label === "Expired" && exp[0]!.step === "bad", "expired: the first step says so");
  check(timeline("depositSeen")[2]!.step === "now" && timeline("depositSeen")[1]!.step === "done", "deposit seen is now");
}

group("refunds: 1Click's \"0\" is not a refund");
{
  const details = (refunded: string | null) => ({
    amountInFormatted: null, amountOutFormatted: null, depositedAmountFormatted: null, refundedAmountFormatted: refunded,
    refundReason: null, originChainTxHashes: [], destinationChainTxHashes: [],
  });
  check(refundLine(swap({ state: "success", details: details("0") })) === null, "a success with \"0\" refunded says nothing");
  check(refundLine(swap({ state: "processing", details: details("0") }))?.startsWith("A refund, if any, would go to") ?? false,
    "in flight: where a refund would go");
  const r = refundLine(swap({ state: "refunded", details: { ...details("0.0099"), refundReason: "slippage" } }));
  check(r === "Refunded 0.0099 ZEC to u1own (slippage)", `refunded: ${r}`);
  check(refundLine(swap({ state: "refunded", details: details("0") })) === null, "a \"refunded\" with nothing refunded is not called a refund");
}

group("what a swap allows: pay, remove, poll");
{
  check(payable(swap()), "an unpaid quote from ZEC is payable");
  check(!payable(swap({ depositTxid: "abc" })), "a paid one is not");
  check(!payable(swap({ direction: "toZec" })), "a swap into ZEC is paid elsewhere");
  check(removable(swap()) && removable(swap({ state: "expired" })) && removable(swap({ state: "failed" })), "removable");
  check(!removable(swap({ state: "deposited", depositTxid: "abc" })), "a deposit in flight stays");
  check(!removable(swap({ direction: "toZec" })), "a swap into ZEC waiting for its deposit stays");
  const list = [swap({ id: "a" }), swap({ id: "b", state: "success" }), swap({ id: "c", state: "processing" })];
  check(toPoll(list, true).map((s) => s.id).join() === "a,c", "polls what is not final");
  check(toPoll(list, false).length === 0, "nothing is polled off mainnet");
}

group("form: checked before 1Click is asked");
{
  const ok = { asset: "x", amount: "0.01", address: "addr", slippage: "1", maxSlippageBps: 1000 };
  check(checkForm(ok) === null, "a good form");
  check(checkForm({ ...ok, asset: "" }) === "Choose an asset.", "no asset");
  for (const amount of ["", ".", "0", "-1", "1e3", "abc"]) check(checkForm({ ...ok, amount }) !== null, `amount ${amount}`);
  check(checkForm({ ...ok, address: "  " }) !== null, "no address");
  for (const slippage of ["0", "11", "x"]) check(checkForm({ ...ok, slippage })?.startsWith("Slippage") ?? false, `slippage ${slippage}`);
}

group("quote: the rows a user checks");
{
  const rows = new Map(quoteRows(swap()));
  check(rows.get("You pay") === "0.01 ZEC (≈ $11.57)", `pay: ${rows.get("You pay")}`);
  check(rows.get("You receive about") === "11.5 USDC on SOL (≈ $11.50)", "receive");
  check(rows.get("At least") === "11.38 USDC on SOL (slippage 1%)", "at least");
  check(rows.get("Fees")!.includes("0.25%"), "no API key: the extra fee is said");
  check(rows.get("Refunds to") === "u1own (this wallet, shielded)", "refunds to this wallet");
  check(rows.has("Deposit by"), "a real quote has a deadline");
  check(!new Map(quoteRows(swap({ dry: true }))).has("Deposit by"), "a preview has none");
  check(new Map(quoteRows(swap({ direction: "toZec", origin: USDC, destination: ZEC }))).has("Into (this wallet)"), "into ZEC");
  check(assetName(ZEC) === "ZEC" && assetName(USDC) === "USDC on SOL", "asset names");
  check(usd(null) === "" && usd("x") === "", "no USD value, nothing said");
}

console.log(failures === 0 ? "\nPASSED (0 failures)" : `\nFAILED (${failures} failures)`);
if (failures) process.exit(1);
