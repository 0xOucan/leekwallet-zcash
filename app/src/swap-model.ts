/**
 * NEAR Intents swaps without the DOM: the records src-tauri/src/swaps.rs
 * returns (app/near-intents's SwapRecord), and what the Swap screen says
 * about them (src/swap-view.ts). Kept apart so the rules that decide what a
 * user is told about money in flight run under plain Node
 * (test/swap.test.ts).
 */

export interface SwapToken {
  assetId: string;
  decimals: number;
  blockchain: string;
  symbol: string;
  price: number | null;
}

export interface SwapInfo {
  network: string;
  authenticated: boolean;
  apiKeyEnv: string;
  explorer: string;
  defaultSlippageBps: number;
  maxSlippageBps: number;
}

export type SwapState =
  | "quoted" | "expired" | "deposited" | "depositSeen" | "incomplete" | "processing"
  | "success" | "refunded" | "failed";

export interface SwapAsset { assetId: string; symbol: string; blockchain: string; decimals: number }
export interface SwapTxLink { hash: string; explorerUrl: string }

export interface Swap {
  id: string;
  dry: boolean;
  createdAt: string;
  direction: "fromZec" | "toZec";
  origin: SwapAsset;
  destination: SwapAsset;
  amountIn: string;
  amountInFormatted: string;
  amountInUsd: string | null;
  minAmountInFormatted: string | null;
  amountOutFormatted: string;
  amountOutUsd: string | null;
  minAmountOutFormatted: string;
  slippageBps: number;
  deadline: string | null;
  timeEstimateSecs: number | null;
  refundFeeFormatted: string | null;
  depositAddress: string | null;
  depositMemo: string | null;
  depositTransparent: boolean;
  recipient: string;
  refundTo: string;
  confidentiality: string;
  authenticated: boolean;
  proposalId: string | null;
  depositTxid: string | null;
  depositSubmitted: boolean;
  state: SwapState;
  apiStatus: string | null;
  updatedAt: string | null;
  checkedAt: string | null;
  details: {
    amountInFormatted: string | null;
    amountOutFormatted: string | null;
    depositedAmountFormatted: string | null;
    refundedAmountFormatted: string | null;
    refundReason: string | null;
    originChainTxHashes: SwapTxLink[];
    destinationChainTxHashes: SwapTxLink[];
  } | null;
  lastError: string | null;
}

/** States after which a swap moves no more funds. */
export const FINAL_SWAP: readonly SwapState[] = ["success", "refunded", "failed"];
/** How often the swaps not yet final are asked about while the screen is open. */
export const POLL_MS = 20_000;

export const isFinal = (s: SwapState): boolean => FINAL_SWAP.includes(s);

export const chainName = (c: string): string => c.toUpperCase();

/** "USDC on SOL", or "ZEC" for ZEC itself. */
export const assetName = (a: { symbol: string; blockchain: string }): string =>
  a.blockchain === "zec" ? a.symbol : `${a.symbol} on ${chainName(a.blockchain)}`;

/** " (≈ $12.34)", or nothing when 1Click gave no USD value. */
export function usd(v: string | null): string {
  if (!v) return "";
  const n = Number(v);
  return Number.isFinite(n) ? ` (≈ $${n.toFixed(2)})` : "";
}

/** The steps a swap goes through, as the timeline draws them. */
export const STEPS: readonly string[] = ["Quoted", "Deposit sent", "Deposit seen", "Swapping", "Done"];

export function stepIndex(s: SwapState): number {
  switch (s) {
    case "quoted":
    case "expired":
      return 0;
    case "deposited":
      return 1;
    case "depositSeen":
    case "incomplete":
      return 2;
    case "processing":
      return 3;
    default:
      return 4;
  }
}

/** One step of the timeline: its label and how it is drawn. */
export interface TimelineStep { label: string; step: "done" | "now" | "todo" | "bad" }

export function timeline(s: SwapState): TimelineStep[] {
  const now = stepIndex(s);
  return STEPS.map((base, i) => {
    let label = base;
    if (i === 4 && s === "refunded") label = "Refunded";
    if (i === 4 && s === "failed") label = "Failed";
    if (i === 0 && s === "expired") label = "Expired";
    const bad = (i === 4 && (s === "refunded" || s === "failed")) || (i === 0 && s === "expired");
    const step = bad ? "bad" : i < now ? "done" : i === now ? (s === "success" ? "done" : "now") : "todo";
    return { label, step };
  });
}

export const STATE_TEXT: Record<SwapState, string> = {
  quoted: "Waiting for the deposit",
  expired: "Expired: no deposit before the deadline (a late one is refunded)",
  deposited: "Deposit broadcast; waiting for 1Click to see it",
  depositSeen: "1Click saw the deposit",
  incomplete: "Less than the quote needs arrived; 1Click refunds it after the deadline unless more comes",
  processing: "Swapping",
  success: "Done",
  refunded: "Refunded",
  failed: "Failed",
};

/**
 * What the swap says about a refund, if anything.
 *
 * 1Click reports `refundedAmountFormatted` as "0" on every swap, refunded or
 * not: a non-empty string is not a refund. Only an amount above zero is; until
 * the swap ends, the line says where a refund would go.
 */
export function refundLine(s: Swap, show: (address: string) => string = (a) => a): string | null {
  const d = s.details;
  if (d?.refundedAmountFormatted && Number(d.refundedAmountFormatted) > 0) {
    return `Refunded ${d.refundedAmountFormatted} ${s.origin.symbol} to ${show(s.refundTo)}${d.refundReason ? ` (${d.refundReason})` : ""}`;
  }
  if (s.refundTo && !isFinal(s.state)) return `A refund, if any, would go to ${show(s.refundTo)}`;
  return null;
}

/** A quote from ZEC whose deposit this wallet can still sign. */
export const payable = (s: Swap): boolean => s.direction === "fromZec" && s.state === "quoted" && !s.depositTxid;

/** A swap that can no longer move funds, so it may be removed. */
export const removable = (s: Swap): boolean => isFinal(s.state) || s.state === "expired" || payable(s);

/** The swaps to ask 1Click about on the next poll: none off mainnet. */
export const toPoll = (swaps: readonly Swap[], mainnet: boolean): Swap[] =>
  mainnet ? swaps.filter((s) => !isFinal(s.state)) : [];

/** The form's checks before a quote is asked for; null when it may go. */
export function checkForm(f: { asset: string; amount: string; address: string; slippage: string; maxSlippageBps: number }): string | null {
  const amount = f.amount.trim();
  if (!f.asset) return "Choose an asset.";
  const badAmount = !/^\d*(\.\d+)?$/.test(amount) || amount === "" || amount === "." || Number(amount) <= 0;
  if (badAmount || f.address.trim() === "") return "Enter an amount and an address.";
  const slip = Number(f.slippage.trim());
  const max = f.maxSlippageBps / 100;
  if (!Number.isFinite(slip) || slip <= 0 || slip > max) return `Slippage between 0.01% and ${max}%.`;
  return null;
}

/** A date from 1Click (RFC 3339), in the user's words. */
export function when(iso: string | null): string {
  if (!iso) return "—";
  const d = new Date(iso);
  return Number.isNaN(d.getTime()) ? iso : d.toLocaleString(undefined, { dateStyle: "medium", timeStyle: "short" });
}

/** The quote's rows for the review: what is paid, received, and at what cost. */
export function quoteRows(q: Swap, show: (address: string) => string = (a) => a): [string, string][] {
  const inName = assetName(q.origin);
  const outName = assetName(q.destination);
  const rate = Number(q.amountOutFormatted) / Number(q.amountInFormatted);
  const rows: [string, string][] = [
    ["You pay", `${q.amountInFormatted} ${inName}${usd(q.amountInUsd)}`],
    ["You receive about", `${q.amountOutFormatted} ${outName}${usd(q.amountOutUsd)}`],
    ["At least", `${q.minAmountOutFormatted} ${outName} (slippage ${q.slippageBps / 100}%)`],
  ];
  if (Number.isFinite(rate) && rate > 0) rows.push(["Rate", `1 ${q.origin.symbol} ≈ ${rate.toPrecision(6)} ${q.destination.symbol}`]);
  if (q.minAmountInFormatted && q.direction === "toZec") rows.push(["Least deposit swapped", `${q.minAmountInFormatted} ${inName}`]);
  if (q.refundFeeFormatted) rows.push(["Refund fee (if refunded)", `${q.refundFeeFormatted} ${q.origin.symbol}`]);
  rows.push(["Fees", q.authenticated ? "1Click's, included in the amount received" : "1Click's plus 0.25% (no API key), included"]);
  if (q.timeEstimateSecs !== null) rows.push(["Takes about", `${Math.max(1, Math.round(q.timeEstimateSecs / 60))} min after the deposit confirms`]);
  if (!q.dry) rows.push(["Deposit by", when(q.deadline)]);
  rows.push([q.direction === "fromZec" ? "Recipient" : "Into (this wallet)", show(q.recipient)]);
  rows.push(["Refunds to", q.direction === "fromZec" ? `${show(q.refundTo)} (this wallet, shielded)` : show(q.refundTo)]);
  if (q.confidentiality !== "public") rows.push(["Confidentiality", q.confidentiality]);
  return rows;
}
