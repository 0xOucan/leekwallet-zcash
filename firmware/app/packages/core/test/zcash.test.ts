/**
 * The Zcash calls without a board: the .zsig codec against the golden file
 * in vectors/zsig, the checks on every reply, and the chunked upload, over a
 * scripted `call`. The same calls against the firmware itself are in
 * test/zcash-zdev.test.ts (zdev).
 */

import { readFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import { sha256 } from "@noble/hashes/sha256";

import type { CborValue } from "../src/cbor.ts";
import { DeviceError } from "../src/transport.ts";
import {
  checkPczt, encodeZsig, explorerTx, formatZec, MEMO_BYTES, memoBytes, networkOf, parseZsig, toHex, zErrorText, zGetAccount, ZPool,
  zSetBirthday, zShowAddress, zSign, zsigName, type ZCall,
} from "../src/zcash.ts";

let failures = 0;
const check = (c: boolean, m: string) => { if (!c) { console.log(`  FAIL: ${m}`); failures++; } };
const group = (n: string) => console.log(`== ${n}`);

const repo = join(dirname(fileURLToPath(import.meta.url)), "../../../../..");
const vec = (p: string) => new Uint8Array(readFileSync(join(repo, "vectors", p)));
interface KeyVector { name: string; ufvk: string; ua: string; transparent_address: string; seed_fingerprint: string }
const keys = JSON.parse(readFileSync(join(repo, "vectors/keys.json"), "utf8")) as KeyVector[];
const key = (n: string) => keys.find((k) => k.name === n)!;
const unhex = (s: string) => new Uint8Array((s.match(/../g) ?? []).map((h) => parseInt(h, 16)));

async function rejects(p: Promise<unknown>, re: RegExp, m: string): Promise<void> {
  try { await p; check(false, `${m}: did not reject`); } catch (e) { check(re.test(String(e)), `${m}: ${String(e)}`); }
}

/** A board that answers from a script and records what it was asked. */
function scripted(answer: (method: string, p: Record<string, CborValue>) => Record<string, CborValue>) {
  const asked: { method: string; params: Record<string, CborValue>; timeoutMs: number | undefined }[] = [];
  const call: ZCall = async (method, params = {}, timeoutMs) => {
    asked.push({ method, params, timeoutMs });
    return answer(method, params);
  };
  return { call, asked };
}

group(".zsig: the golden file reads, and writes back byte for byte");
{
  const golden = vec("zsig/iw_send_shielded.zsig");
  const pczt = vec("pczt/iw_send_shielded.pczt");
  const z = parseZsig(golden);
  check(z.version === 1, "version 1");
  check(toHex(z.pcztSha256) === toHex(sha256(pczt)), "names the PCZT's hash");
  check(z.entries.every((e) => e.pool === ZPool.Ironwood || e.pool === ZPool.Orchard), "shielded entries");
  check(toHex(encodeZsig(z.pcztSha256, z.entries)) === toHex(golden), "re-encodes identically");
}

group(".zsig: transparent entries make version 2; malformed files are refused");
{
  const t = { pool: ZPool.Transparent, index: 3, sig: new Uint8Array(71).fill(0x30), pubkey: new Uint8Array(33).fill(2) };
  const s = { pool: ZPool.Orchard, index: 0, sig: new Uint8Array(64).fill(9) };
  const b = encodeZsig(new Uint8Array(32), [s, t]);
  check(b[4] === 2, "version 2");
  const back = parseZsig(b);
  check(back.entries[1]!.index === 3 && back.entries[1]!.pubkey!.length === 33, "transparent entry");
  let threw = false;
  try { parseZsig(b.slice(0, b.length - 1)); } catch { threw = true; }
  check(threw, "a truncated file");
  threw = false;
  try { encodeZsig(new Uint8Array(32), [{ pool: ZPool.Transparent, index: 0, sig: new Uint8Array(70) }]); } catch { threw = true; }
  check(threw, "a transparent signature without its key");
  check(zsigName("/home/x/send.pczt") === "send.zsig", "send.pczt -> send.zsig");
  check(zsigName("C:\\a\\tx.PCZT") === "tx.zsig", "Windows path, upper case");
  check(zsigName("unsigned") === "unsigned.zsig", "no extension");
}

group("the network of an address or key");
{
  for (const n of ["main", "test"] as const) {
    const k = key(`${n}/abandon-art/pp0/acct0`);
    check(networkOf(k.ufvk) === n && networkOf(k.ua) === n && networkOf(k.transparent_address) === n, n);
  }
  check(networkOf("0xdeadbeef") === null, "not Zcash");
}

group("PCZT files");
{
  check(checkPczt(vec("pczt/iw_send_shielded.pczt")).version >= 1, "a vector is a PCZT");
  let threw = false;
  try { checkPczt(new TextEncoder().encode("ZSIG....")); } catch { threw = true; }
  check(threw, "a .zsig is not a PCZT");
}

group("zGetAccount: the reply must be what was asked");
{
  const k = key("test/abandon-art/pp0/acct0");
  const answer = (network: string, ufvk = k.ufvk) => ({
    account: 0, network, seedFingerprint: unhex(k.seed_fingerprint), ufvk, ua: k.ua,
    tAddress: k.transparent_address, birthday: 4_000_000,
  });
  const ok = scripted(() => answer("test"));
  const a = await zGetAccount(ok.call, 0, "test");
  check(a.ufvk === k.ufvk && a.birthday === 4_000_000 && a.seedFingerprint === k.seed_fingerprint, "fields");
  check(ok.asked[0]!.timeoutMs === 90_000, "a derive timeout");
  await rejects(zGetAccount(scripted(() => answer("main")).call, 0, "test"), /asked for test/, "the other network");
  await rejects(zGetAccount(scripted(() => answer("test", key("main/abandon-art/pp0/acct0").ufvk)).call, 0, "test"),
    /not a testnet/, "a mainnet key under a testnet label");
  await rejects(zSetBirthday(ok.call, 0, "test", 1.5), /block height/, "a fractional birthday");
}

group("zShowAddress: the sentence and the board's words");
{
  const refuse = scripted(() => { throw new DeviceError(0x0200, "rejected"); });
  try {
    await zShowAddress(refuse.call, 0, "test", key("test/abandon-art/pp0/acct0").ua);
    check(false, "did not reject");
  } catch (e) {
    check(e instanceof DeviceError && e.code === 0x0200, "keeps the code");
    check(/do not match/.test(zErrorText(e)) && /rejected/.test(zErrorText(e)), zErrorText(e));
  }
  const other = scripted(() => ({ ua: key("test/abandon-art/pp0/acct1").ua }));
  await rejects(zShowAddress(other.call, 0, "test", key("test/abandon-art/pp0/acct0").ua), /confirmed/, "a different address");
}

group("zSign: chunks, offsets and the reply");
{
  const pczt = vec("pczt/iw_send_transparent.pczt");
  let got = 0;
  const board = scripted((m, p) => {
    if (m === "zSignBegin") return { maxChunk: 1000, backend: "both" };
    if (m === "zSignChunk") {
      check(p["offset"] === got, `offset ${String(p["offset"])} after ${got}`);
      got += (p["data"] as Uint8Array).length;
      return { received: got };
    }
    return { sighash: new Uint8Array(32).fill(1), signatures: [{ pool: 2, index: 0, sig: new Uint8Array(64) }] };
  });
  const r = await zSign(board.call, pczt, 0, "test");
  check(got === pczt.length, "every byte sent");
  check(board.asked.filter((a) => a.method === "zSignChunk").length === Math.ceil(pczt.length / 1000), "chunk count");
  check(toHex(board.asked[0]!.params["sha256"] as Uint8Array) === toHex(sha256(pczt)), "begin names the hash");
  check(board.asked.at(-1)!.timeoutMs === 150_000, "the review gets a person's time");
  check(r.entries.length === 1 && r.entries[0]!.pool === ZPool.Ironwood, "entries");

  const lying = scripted((m) => m === "zSignBegin" ? { maxChunk: 1000 } : { received: 1 });
  await rejects(zSign(lying.call, pczt, 0, "test"), /received count/, "a board that lost bytes");
}

group("an error the companion has never heard of reads as the board wrote it");
{
  const e = new DeviceError(0x0105, "board is set to testnet");
  check(zErrorText(e).includes("board is set to testnet"), zErrorText(e));
  check(/^The board refused: .*parse/.test(zErrorText(new DeviceError(0x050a, "parse"))), "a 0x05xx refusal");
}

group("amounts, memos and explorer links");
{
  check(formatZec(0) === "0" && formatZec(150_000_000) === "1.5" && formatZec(10_000) === "0.0001", "formatZec");
  check(formatZec(-12_345) === "-0.00012345" && formatZec(2_100_000_000_000_000) === "21000000", "sign and size");
  let threw = false;
  try { formatZec(0.5); } catch { threw = true; }
  check(threw, "a fractional zatoshi");
  check(memoBytes("é") === 2 && MEMO_BYTES === 512, "memo bytes are UTF-8");
  check(explorerTx("test", "ab".repeat(32))!.startsWith("https://testnet."), "testnet explorer");
  check(explorerTx("main", "nope") === null, "not a txid");
}

console.log(`\n${failures ? "FAILED" : "PASSED"} (${failures} failure${failures === 1 ? "" : "s"})`);
process.exit(failures ? 1 : 0);
