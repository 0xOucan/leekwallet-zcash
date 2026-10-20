/**
 * ZIP 321 payment requests and the network of a scanned address
 * (src/zip321.ts): what a QR code in the Send form may fill in, and what it
 * must refuse. Addresses come from vectors/keys.json (public test seeds).
 */

import { readFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

import { addressKind, parsePaymentRequest, parseZecAmount, Zip321Error } from "../src/zip321.ts";

let failures = 0;
const check = (c: boolean, m: string) => { if (!c) { console.log(`  FAIL: ${m}`); failures++; } };
const group = (n: string) => console.log(`== ${n}`);

const repo = join(dirname(fileURLToPath(import.meta.url)), "../../../../..");
interface KeyVector { name: string; ua: string; transparent_address: string; ua_orchard_only: string }
const keys = JSON.parse(readFileSync(join(repo, "vectors/keys.json"), "utf8")) as KeyVector[];
const key = (n: string) => keys.find((k) => k.name === n)!;
const tUa = key("test/abandon-art/pp0/acct0").ua;
const tT = key("test/abandon-art/pp0/acct0").transparent_address;
const tUa1 = key("test/abandon-art/pp0/acct1").ua_orchard_only;
const mUa = key("main/abandon-art/pp0/acct0").ua;
const mT = key("main/abandon-art/pp0/acct0").transparent_address;

const b64url = (s: string) => Buffer.from(s, "utf8").toString("base64url");

function refuses(raw: string, re: RegExp, m: string): void {
  try {
    parsePaymentRequest(raw);
    check(false, `${m}: accepted`);
  } catch (e) {
    check(e instanceof Zip321Error && re.test((e as Error).message), `${m}: ${String(e)}`);
  }
}

group("the network and kind of an address");
{
  check(addressKind(tUa)?.network === "test" && addressKind(tUa)?.kind === "unified", "testnet UA");
  check(addressKind(mUa)?.network === "main" && addressKind(mUa)?.kind === "unified", "mainnet UA");
  check(addressKind(tT)?.network === "test" && addressKind(tT)?.kind === "transparent", "testnet t-address");
  check(addressKind(mT)?.network === "main" && addressKind(mT)?.kind === "transparent", "mainnet t-address");
  check(addressKind("textest1qqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqq")?.kind === "tex", "testnet TEX");
  check(addressKind("tex1qqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqq")?.network === "main", "mainnet TEX");
  check(addressKind("0x52908400098527886E0F7030069857D2E4169EE7") === null, "an EVM address is not Zcash");
  check(addressKind("bc1qar0srrr7xfkvy5l643lydnw9re59gtzzwf5mdq") === null, "a Bitcoin address is not Zcash");
  check(addressKind("uview1abc") === null, "a viewing key is not an address");
}

group("a bare address");
{
  const r = parsePaymentRequest(`  ${tUa}\n`);
  check(r.bare && r.network === "test" && r.payments.length === 1 && r.payments[0]!.address === tUa, "trimmed, testnet");
  check(parsePaymentRequest(mT).network === "main", "mainnet t-address");
  refuses("hello world", /not a Zcash address/, "text");
}

group("amounts");
{
  check(parseZecAmount("1").zat === 100_000_000 && parseZecAmount("1").text === "1", "1 ZEC");
  check(parseZecAmount("0.00000001").zat === 1, "one zatoshi");
  check(parseZecAmount("1.50000000").text === "1.5", "trailing zeros trimmed");
  check(parseZecAmount("007.25").text === "7.25", "leading zeros trimmed");
  check(parseZecAmount("21000000").zat === 21_000_000 * 100_000_000, "the whole supply");
  for (const bad of ["1.000000001", "-1", "1e3", "1,5", ".5", "21000000.00000001", "", "0x10"]) {
    let threw = false;
    try { parseZecAmount(bad); } catch { threw = true; }
    check(threw, `amount ${JSON.stringify(bad)} refused`);
  }
}

group("zcash: URIs");
{
  const memo = "Thank you for your purchase";
  const r = parsePaymentRequest(`zcash:${tUa}?amount=0.0123&memo=${b64url(memo)}&label=Coffee%20shop&message=Order%20%2342`);
  const p = r.payments[0]!;
  check(!r.bare && r.network === "test" && r.payments.length === 1, "one testnet payment");
  check(p.address === tUa && p.amount === "0.0123" && p.amountZat === 1_230_000, "address and amount");
  check(p.memo === memo && p.label === "Coffee shop" && p.message === "Order #42", "memo, label, message");

  const q = parsePaymentRequest(`ZCASH:?address=${tT}&amount=1`);
  check(q.payments[0]!.address === tT && q.payments[0]!.memo === null, "address as a parameter, scheme in capitals");

  const multi = parsePaymentRequest(`zcash:?address=${tUa}&amount=1&address.1=${tT}&amount.1=0.5&memo=${b64url("hi")}&label.1=Rent`);
  check(multi.payments.length === 2, "two payments");
  check(multi.payments[1]!.address === tT && multi.payments[1]!.amount === "0.5" && multi.payments[1]!.label === "Rent", "payment 1");
  check(multi.payments[0]!.memo === "hi", "payment 0 memo");

  check(parsePaymentRequest(`zcash:${tUa}?amount=1&unknown=x`).payments.length === 1, "an unknown optional parameter is ignored");
  const emoji = "Pizza 🍕";
  check(parsePaymentRequest(`zcash:${tUa}?memo=${b64url(emoji)}`).payments[0]!.memo === emoji, "a UTF-8 memo");
}

group("requests that must be refused");
{
  refuses(`zcash:${tUa}?amount=1&amount=2`, /twice/, "a parameter twice");
  refuses(`zcash:${tUa}?address=${tUa1}`, /twice/, "address in the path and as a parameter");
  refuses(`zcash:${tUa}?amount=1.123456789`, /8 decimals/, "nine decimals");
  refuses(`zcash:${tUa}?amount=22000000`, /more than there will ever be/, "above the supply");
  refuses(`zcash:${tT}?memo=${b64url("hi")}`, /transparent/, "a memo to a transparent address");
  refuses(`zcash:${tUa}?req-futurething=1`, /requires "req-futurething"/, "an unknown required parameter");
  refuses(`zcash:?address=${tUa}&address.1=${mUa}`, /mixes mainnet and testnet/, "two networks");
  refuses(`zcash:?amount=1`, /without an address/, "no address");
  refuses(`zcash:?address=${tUa}&amount.1=1`, /without an address/, "an index without an address");
  refuses(`zcash:?address=${tUa}&address.01=${tT}`, /not a valid parameter name/, "an index with a leading zero");
  refuses(`zcash:?address=${tUa}&address.10000=${tT}`, /not a valid parameter name/, "an index above 9999");
  refuses(`zcash:${tUa}?memo=!!!`, /base64url/, "a memo that is not base64url");
  refuses(`zcash:${tUa}?memo=${Buffer.from([0xf6, 0, 1]).toString("base64url")}`, /binary/, "a binary memo");
  refuses(`zcash:${tUa}?memo=${Buffer.alloc(513, 0x41).toString("base64url")}`, /at most 512/, "a memo over 512 bytes");
  refuses(`zcash:${tUa}?memo=${Buffer.from([0xc3, 0x28]).toString("base64url")}`, /UTF-8/, "a memo that is not UTF-8");
  refuses(`zcash:${tUa}?label=%E0%A4%A`, /percent-encoding/, "bad percent-encoding");
  refuses(`zcash:bitcoinaddress?amount=1`, /not a Zcash address/, "not a Zcash address");
  refuses(`zcash:`, /no payment/, "nothing at all");
  refuses(`zcash:${tUa}?amount`, /has no value/, "a parameter without a value");
}

console.log(`\n${failures ? "FAILED" : "PASSED"} (${failures} failure${failures === 1 ? "" : "s"})`);
process.exit(failures ? 1 : 0);
