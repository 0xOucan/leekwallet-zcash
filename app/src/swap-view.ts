/**
 * The Swap screen (index.html #zpane-swap): NEAR Intents swaps through
 * 1Click, over src-tauri/src/swaps.rs, on desktop and Android alike.
 *
 * The backend asks for quotes, keeps every swap with a deposit address in
 * its wallet's swaps file, builds the ZEC deposit as an ordinary send and
 * asks 1Click where each swap stands; this screen draws, polls, and has the
 * board sign a deposit exactly as the Send screen has it sign a payment
 * (the wallet's `sign`, src/zcash-wallet.ts). What it says about a swap is
 * src/swap-model.ts, tested under Node.
 */

import { formatZec, zErrorText, type ZNetwork } from "../packages/core/src/zcash.ts";
import type { Invoke } from "./tauri-transport.ts";
import {
  assetName, chainName, checkForm, isFinal, payable, quoteRows, refundLine, removable, timeline, toPoll, when,
  POLL_MS, STATE_TEXT, type Swap, type SwapInfo, type SwapToken,
} from "./swap-model.ts";

/** A built send, as zw_propose_send and swap_execute return it. */
export interface Proposal {
  id: string; kind: "send" | "shield"; targetHeight: number;
  payments: { recipient: string; amountZat: number; pool: string; hasMemo: boolean }[];
  change: { valueZat: number; pool: string }[]; feeZat: number; totalZat: number;
  signerViewBytes: number; compact: boolean;
}

/** Which wallet the commands are about (as src/zcash-wallet.ts names it). */
export interface WalletKey { network: ZNetwork; wallet: string; account: number }

/** What the Swap screen needs of the wallet around it. */
export interface SwapHost {
  invoke: Invoke;
  /** The wallet on screen, or null. */
  key(): WalletKey | null;
  /** It can sign (a board account, not a viewing key). */
  canSign(): boolean;
  /**
   * Have the board sign `view` and finish it (prove, broadcast), with the
   * progress said on `say` and drawn on `bar`. Resolves to the txid.
   */
  sign(key: WalletKey, view: Uint8Array, mainnetConfirmed: boolean, say: (t: string) => void, bar: HTMLProgressElement): Promise<string>;
  /** A transaction was broadcast, or ZEC arrived: sync. */
  changed(): void;
  qr(text: string): SVGSVGElement;
  copy(text: string): Promise<string>;
  log(line: string): void;
}

export interface SwapView {
  /** The wallet on screen changed (another one, or none). */
  reset(): void;
  /** The Swap tab was opened. */
  opened(): void;
}

const $ = <T extends HTMLElement>(id: string): T => {
  const el = document.getElementById(id);
  if (!el) throw new Error(`missing element #${id}`);
  return el as T;
};

function el<K extends keyof HTMLElementTagNameMap>(tag: K, cls: string, text?: string): HTMLElementTagNameMap[K] {
  const e = document.createElement(tag);
  if (cls) e.className = cls;
  if (text !== undefined) e.textContent = text;
  return e;
}

function kvRows(kv: HTMLElement, rows: [string, string][]): void {
  kv.replaceChildren();
  for (const [k, v] of rows) kv.append(el("dt", "", k), el("dd", "", v));
}

const sameKey = (a: WalletKey | null, b: WalletKey | null) =>
  a !== null && b !== null && a.network === b.network && a.wallet === b.wallet && a.account === b.account;

export function initSwapView(host: SwapHost): SwapView {
  let info: SwapInfo | null = null;
  let tokens: SwapToken[] = [];
  let swaps: Swap[] = [];
  /** The quote on screen (dry or real). */
  let quote: Swap | null = null;
  /** The deposit under review: the swap it pays and its signer view. */
  let paying: { key: WalletKey; swap: Swap; proposal: Proposal; view: Uint8Array } | null = null;
  let pollTimer: ReturnType<typeof setTimeout> | null = null;

  const say = (t: string) => { $("swstatus").textContent = t; };
  const sayList = (t: string) => { $("swliststatus").textContent = t; };
  const mainnet = () => host.key()?.network === "main";
  const open = () => !$("zpane-swap").hidden && !$("seczcash").hidden;

  const direction = (): "fromZec" | "toZec" => ($("swto") as HTMLInputElement).checked ? "toZec" : "fromZec";
  const selected = (): SwapToken | null => tokens.find((t) => t.assetId === $<HTMLSelectElement>("swasset").value) ?? null;

  function call<T>(cmd: string, args: Record<string, unknown> = {}): Promise<T> {
    const k = host.key();
    if (!k) return Promise.reject(new Error("Open a wallet first: a swap pays to and from it."));
    return host.invoke<T>(cmd, { ...k, ...args });
  }

  function stage(s: "form" | "quote" | "review"): void {
    $("swform").hidden = s !== "form";
    $("swquote").hidden = s !== "quote";
    $("swreview").hidden = s !== "review";
  }

  /** The labels and notes of the form for the direction and asset chosen. */
  function renderForm(): void {
    const dir = direction();
    const t = selected();
    const other = t ? assetName(t) : "the other asset";
    $("swassetlabel").textContent = dir === "fromZec" ? "Receive" : "Pay with";
    $("swamountlabel").textContent = dir === "fromZec" ? "Amount to send (ZEC)" : `Amount to pay (${other})`;
    $("swaddrlabel").textContent = dir === "fromZec"
      ? `Recipient's address on ${t ? chainName(t.blockchain) : "the destination chain"}`
      : `Your refund address on ${t ? chainName(t.blockchain) : "the paying chain"} (if the swap fails, 1Click refunds there)`;
    const auth = info?.authenticated ?? false;
    const conf = $<HTMLInputElement>("swconfidential");
    conf.disabled = !auth;
    if (!auth) conf.checked = false;
    let privacy = dir === "fromZec"
      ? "Refunds come back to a fresh shielded (Orchard) address of this wallet. The deposit address 1Click gives is "
        + "usually transparent: the deposit and its amount are public on the Zcash chain (the rest of your wallet is not)."
      : "The ZEC arrives at a fresh shielded (Orchard) address of this wallet, so the payout is private on the Zcash chain.";
    if (!auth) privacy += ` No 1Click API key is set (${info?.apiKeyEnv ?? "LEEKZ_1CLICK_API_KEY"}): 1Click adds a 0.25% fee.`;
    if (dir === "fromZec" && !host.canSign()) privacy += " This wallet is watch-only: it can swap into ZEC, not from it.";
    $("swprivacy").textContent = privacy;
    $("swtestnote").hidden = mainnet();
    $("swquotebtn").hidden = !mainnet();
  }

  async function loadTokens(): Promise<void> {
    const sel = $<HTMLSelectElement>("swasset");
    try {
      tokens = (await host.invoke<SwapToken[]>("swap_tokens")).filter((t) => t.blockchain !== "zec");
    } catch (e) {
      sel.replaceChildren(new Option("The asset list could not be loaded", ""));
      say(zErrorText(e));
      return;
    }
    const keep = sel.value;
    sel.replaceChildren(...tokens.map((t) => new Option(`${t.symbol} · ${chainName(t.blockchain)}`, t.assetId)));
    const usdcSol = tokens.find((t) => t.symbol === "USDC" && t.blockchain === "sol");
    sel.value = tokens.some((t) => t.assetId === keep) ? keep : (usdcSol?.assetId ?? tokens[0]?.assetId ?? "");
    renderForm();
  }

  async function loadSwaps(): Promise<void> {
    const k = host.key();
    try {
      const v = await call<Swap[]>("swap_list");
      if (!sameKey(k, host.key())) return;
      swaps = v;
      sayList("");
    } catch (e) {
      swaps = [];
      sayList(zErrorText(e));
    }
    renderSwaps();
    schedulePoll();
  }

  async function load(): Promise<void> {
    const k = host.key();
    if (!k) return;
    try {
      info = await host.invoke<SwapInfo>("swap_info", { network: k.network });
      const slip = $<HTMLInputElement>("swslippage");
      if (slip.value === "" || slip.dataset["touched"] !== "1") slip.value = String(info.defaultSlippageBps / 100);
    } catch (e) {
      say(zErrorText(e));
    }
    renderForm();
    await Promise.all([loadTokens(), loadSwaps()]);
  }

  async function requestQuote(dry: boolean): Promise<void> {
    const t = selected();
    const amount = $<HTMLInputElement>("swamount").value.trim();
    const addr = $<HTMLTextAreaElement>("swaddr").value.trim();
    const slippage = $<HTMLInputElement>("swslippage").value;
    const bad = checkForm({ asset: t?.assetId ?? "", amount, address: addr, slippage, maxSlippageBps: info?.maxSlippageBps ?? 1000 });
    if (bad || !t) { say(bad ?? "Choose an asset."); return; }
    const dir = direction();
    const params = {
      direction: dir, asset: t.assetId, amount,
      recipient: dir === "fromZec" ? addr : null,
      refundTo: dir === "toZec" ? addr : null,
      slippageBps: Math.round(Number(slippage.trim()) * 100),
      dry,
      confidential: $<HTMLInputElement>("swconfidential").checked,
    };
    const k = host.key();
    const btns = [$<HTMLButtonElement>("swquotebtn"), $<HTMLButtonElement>("swpreviewbtn")];
    btns.forEach((b) => { b.disabled = true; });
    say(dry ? "Asking 1Click for a price…" : "Asking 1Click for a quote…");
    try {
      const q = await call<Swap>("swap_quote", { params });
      if (!sameKey(k, host.key())) return;
      quote = q;
      renderQuote(q);
      stage("quote");
      say("");
      if (!q.dry) {
        host.log(`zcash: swap quote ${q.amountInFormatted} ${q.origin.symbol} -> ${q.destination.symbol}`);
        await loadSwaps();
      }
    } catch (e) {
      say(zErrorText(e));
    } finally {
      btns.forEach((b) => { b.disabled = false; });
    }
  }

  function renderQuote(q: Swap): void {
    kvRows($("swquotekv"), quoteRows(q));
    $("swquotehead").textContent = q.dry ? "Price preview (dry run)" : "Quote";
    const dep = q.depositAddress;
    $("swdepositbox").hidden = dep === null;
    const qr = $("swdepositqr");
    qr.hidden = true;
    qr.replaceChildren();
    if (dep !== null) {
      const inName = assetName(q.origin);
      $("swdepositlabel").textContent = q.direction === "fromZec"
        ? `Deposit address (Zcash${q.depositTransparent ? ", transparent: the deposit is public" : ""}). "Review the deposit" builds the send; the board shows this address.`
        : `Pay ${q.amountInFormatted} ${inName} from your ${chainName(q.origin.blockchain)} wallet to this address, before ${when(q.deadline)}. Only ${inName}: anything else is lost.`;
      $("swdeposit").textContent = dep;
      /* Another wallet pays a swap into ZEC: its deposit address as a QR
       * code, for a phone wallet to scan. */
      if (q.direction === "toZec") {
        qr.replaceChildren(host.qr(dep));
        qr.hidden = false;
      }
    }
    $("swreviewbtn").hidden = q.dry || !payable(q) || !host.canSign();
    $("swquotenote").textContent = q.dry
      ? "A preview only: no deposit address was made. " + (mainnet() ? "Get a quote to swap." : "Swaps need Mainnet.")
      : q.direction === "fromZec"
        ? "1Click's quote, not checked by the board. Nothing is sent until you sign the deposit on the board."
        : "The swap appears below; this app follows it until the ZEC arrives (it shows in the balance after a sync).";
  }

  /** Builds the ZEC deposit of `s` and shows it for review. */
  async function reviewDeposit(s: Swap): Promise<void> {
    const k = host.key();
    if (!k) return;
    say("Building the deposit…");
    try {
      const r = await call<{ proposal: Proposal; view: number[] }>("swap_execute", { id: s.id });
      if (!sameKey(k, host.key())) return;
      const p = r.proposal;
      paying = { key: k, swap: s, proposal: p, view: Uint8Array.from(r.view) };
      const pay = p.payments[0];
      const rows: [string, string][] = [
        ["Swap", `${s.amountInFormatted} ZEC → about ${s.amountOutFormatted} ${assetName(s.destination)}`],
        ["Deposit to", pay ? pay.recipient : (s.depositAddress ?? "")],
        ["Amount", `${formatZec(pay ? pay.amountZat : 0)} ZEC (${pay ? pay.pool : ""})`],
      ];
      for (const c of p.change) rows.push(["Change back to you", `${formatZec(c.valueZat)} ZEC (${c.pool})`]);
      rows.push(["Fee", `${formatZec(p.feeZat)} ZEC`]);
      rows.push(["Leaves the wallet", `${formatZec(p.totalZat)} ZEC`]);
      kvRows($("swreviewkv"), rows);
      $("swreviewnote").textContent =
        `The board shows the deposit address${s.depositTransparent ? " (transparent, with its warning)" : ""} and the amount: `
        + `compare them with the ones here. Deposit before ${when(s.deadline)}; `
        + `a built transaction also expires about 40 blocks after ${p.targetHeight}.`;
      ($("swmainconfirm") as HTMLInputElement).checked = false;
      $("swmainconfirmrow").hidden = k.network !== "main";
      stage("review");
      say("Check this, then sign: the board shows the same address and amount, and its screen is the one to trust.");
      $("zpane-swap").scrollIntoView?.({ behavior: "smooth", block: "start" });
    } catch (e) {
      say(zErrorText(e));
      await loadSwaps();
    }
  }

  async function signDeposit(): Promise<void> {
    const pay = paying;
    if (!pay || !sameKey(pay.key, host.key())) { paying = null; stage("form"); return; }
    const mainOk = ($("swmainconfirm") as HTMLInputElement).checked;
    if (pay.key.network === "main" && !mainOk) { say("This is mainnet: tick the confirmation above first."); return; }
    const btn = $<HTMLButtonElement>("swsignbtn");
    btn.disabled = true;
    try {
      const txid = await host.sign(pay.key, pay.view, mainOk, say, $<HTMLProgressElement>("swbar"));
      host.log(`zcash: swap deposit ${txid} broadcast`);
      paying = null;
      quote = null;
      $<HTMLInputElement>("swamount").value = "";
      $<HTMLTextAreaElement>("swaddr").value = "";
      stage("form");
      say(`Deposit sent: ${txid}. 1Click is told of it now; the swap below follows it.`);
      /* zw_finish recorded the txid with the swap; tell 1Click at once. */
      await loadSwaps();
      await checkSwap(pay.swap.id);
      host.changed();
    } catch (e) {
      say(zErrorText(e));
    } finally {
      btn.disabled = false;
    }
  }

  function copyLine(label: string, value: string): HTMLElement {
    const row = el("div", "swap__id");
    const code = el("code", "", `${label} ${value}`);
    const b = el("button", "secondary", "Copy");
    b.type = "button";
    b.addEventListener("click", () => void host.copy(value).then(() => sayList(`Copied the ${label.toLowerCase()}.`)));
    row.append(code, b);
    return row;
  }

  function renderSwaps(): void {
    const list = $("swlist");
    $("swempty").hidden = swaps.length > 0;
    list.replaceChildren(...swaps.map((s) => {
      const li = document.createElement("li");
      const head = el("div", "zhist__head");
      const kind = s.direction === "fromZec" ? `ZEC → ${assetName(s.destination)}` : `${assetName(s.origin)} → ZEC`;
      head.append(
        document.createTextNode(`${kind} · ${when(s.createdAt)}`),
        el("span", "zhist__amt", `${s.amountInFormatted} ${s.origin.symbol} → ${s.details?.amountOutFormatted ?? `≈ ${s.amountOutFormatted}`} ${s.destination.symbol}`),
      );
      const ol = el("ol", "timeline");
      timeline(s.state).forEach((t, i) => {
        const item = el("li", "", `${i > 0 ? "→ " : ""}${t.label}`);
        item.dataset["step"] = t.step;
        ol.append(item);
      });
      li.append(head, ol, el("div", "swap__line", STATE_TEXT[s.state] ?? s.state));
      if (s.state === "quoted" || s.state === "expired") li.append(el("div", "swap__line", `Deposit by ${when(s.deadline)}`));
      if (s.direction === "toZec" && (s.state === "quoted" || s.state === "incomplete")) {
        li.append(el("div", "swap__line", `Pay ${s.amountInFormatted} ${assetName(s.origin)} to the deposit address below, from your wallet on ${chainName(s.origin.blockchain)}.`));
      }
      const refund = refundLine(s);
      if (refund) li.append(el("div", "swap__line", refund));
      if (s.depositAddress) li.append(copyLine("Deposit", s.depositAddress));
      if (s.depositTxid) li.append(copyLine("Deposit tx", s.depositTxid));
      for (const t of s.details?.destinationChainTxHashes ?? []) li.append(copyLine("Payout", t.explorerUrl || t.hash));
      if (s.lastError) li.append(el("div", "swap__line", `Last check: ${s.lastError}`));
      else if (s.checkedAt) li.append(el("div", "swap__line", `Checked ${when(s.checkedAt)}${s.apiStatus ? ` · 1Click: ${s.apiStatus}` : ""}`));

      const actions = el("div", "swap__actions");
      if (payable(s) && host.canSign()) {
        const b = el("button", "primary", "Review the deposit");
        b.type = "button";
        b.addEventListener("click", () => void reviewDeposit(s));
        actions.append(b);
      }
      if (!isFinal(s.state)) {
        const b = el("button", "secondary", "Check now");
        b.type = "button";
        b.addEventListener("click", () => void checkSwap(s.id));
        actions.append(b);
      }
      if (removable(s)) {
        const b = el("button", "ghost", "Remove");
        b.type = "button";
        b.title = "Forget this swap on this device (it can no longer move funds)";
        b.addEventListener("click", () => {
          void call<Swap[]>("swap_forget", { id: s.id }).then((v) => { swaps = v; renderSwaps(); }, (e) => sayList(zErrorText(e)));
        });
        actions.append(b);
      }
      li.append(actions);
      return li;
    }));
  }

  async function checkSwap(id: string): Promise<void> {
    const k = host.key();
    try {
      const s = await call<Swap>("swap_status", { id });
      if (!sameKey(k, host.key())) return;
      const i = swaps.findIndex((x) => x.id === id);
      const was = i >= 0 ? swaps[i] : undefined;
      if (i >= 0) swaps[i] = s;
      else swaps.unshift(s);
      renderSwaps();
      /* ZEC arrived or came back: sync, so the balance shows it. */
      if (was && was.state !== s.state && (s.state === "success" || s.state === "refunded")) host.changed();
    } catch (e) {
      sayList(zErrorText(e));
    }
  }

  function stopPoll(): void {
    if (pollTimer !== null) clearTimeout(pollTimer);
    pollTimer = null;
  }

  /** Follows every swap that is not final while the Swap screen is open on mainnet. */
  function schedulePoll(): void {
    stopPoll();
    if (toPoll(swaps, mainnet()).length === 0) return;
    pollTimer = setTimeout(() => {
      void (async () => {
        if (open() && !document.hidden) {
          for (const s of toPoll(swaps, mainnet())) await checkSwap(s.id);
        }
        schedulePoll();
      })();
    }, POLL_MS);
  }

  /* ----------------------------------------------------------- wiring */

  for (const id of ["swfrom", "swto", "swasset"]) $(id).addEventListener("change", renderForm);
  $("swslippage").addEventListener("input", () => { $("swslippage").dataset["touched"] = "1"; });
  $("swquotebtn").addEventListener("click", () => { if (mainnet()) void requestQuote(false); });
  $("swpreviewbtn").addEventListener("click", () => void requestQuote(true));
  $("swbackbtn").addEventListener("click", () => { quote = null; stage("form"); say(""); });
  $("swreviewbtn").addEventListener("click", () => { if (quote) void reviewDeposit(quote); });
  $("swcancelbtn").addEventListener("click", () => {
    paying = null;
    stage(quote ? "quote" : "form");
    say("Cancelled; nothing was signed.");
  });
  $("swsignbtn").addEventListener("click", () => void signDeposit());
  $("swdepositcopy").addEventListener("click", () => {
    const dep = quote?.depositAddress;
    if (dep) void host.copy(dep).then(() => say("Copied the deposit address."));
  });
  $("swrefresh").addEventListener("click", () => {
    const btn = $<HTMLButtonElement>("swrefresh");
    btn.disabled = true;
    sayList("");
    void (async () => {
      for (const s of swaps.filter((x) => !isFinal(x.state))) await checkSwap(s.id);
      schedulePoll();
    })().finally(() => { btn.disabled = false; });
  });

  return {
    reset() {
      stopPoll();
      swaps = [];
      quote = null;
      paying = null;
      info = null;
      say("");
      sayList("");
      stage("form");
      renderSwaps();
    },
    opened() {
      void load();
    },
  };
}
