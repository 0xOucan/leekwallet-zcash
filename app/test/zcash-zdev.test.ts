/**
 * The Zcash calls (packages/core/src/zcash.ts) against zdev: the firmware's
 * protocol, signers and review pages on a PTY (firmware/sim). Pairs, reads
 * the account on both networks, sets a birthday per network, has the
 * address checked, and signs the PCZT vectors (vectors/pczt.json), checking
 * the sighash the "board" approved and the .zsig written from it.
 *
 *   pnpm test:zdev       (builds zdev with make; skips loudly if it cannot)
 */

import { createHash } from "node:crypto";
import { readFileSync } from "node:fs";
import { join } from "node:path";

import { DeviceError } from "../packages/core/src/transport.ts";
import { parseStatus } from "../packages/core/src/device-state.ts";
import {
  encodeZsig, parseZsig, toHex, zErrorText, zGetAccount, zGetFeatures, ZErr, ZPool,
  zSetBirthday, zShowAddress, zSign, type ZNetwork,
} from "../packages/core/src/zcash.ts";
import { repo, startZdev, zdevBinary, ZdevClient } from "./zdev.ts";
import { fakeWallet } from "./fake-wallet.ts";
import { BIRTHDAY_MARGIN, ensureWallet, readAccountRetrying } from "../src/zcash-auto.ts";
import { isWrongNetwork, networkFromStatus, wrongNetwork } from "../src/network-guard.ts";
import { ReceiveModel } from "../src/receive-model.ts";

let failures = 0;
const check = (c: boolean, m: string) => { if (!c) { console.log(`  FAIL: ${m}`); failures++; } };
const group = (n: string) => console.log(`== ${n}`);

interface KeyVector { name: string; ufvk: string; ua: string; transparent_address: string; seed_fingerprint: string }
interface PcztVector { name: string; file: string; sighash: string }
const keys = JSON.parse(readFileSync(join(repo, "vectors/keys.json"), "utf8")) as KeyVector[];
const key = (n: string) => keys.find((k) => k.name === n)!;
const pczts = JSON.parse(readFileSync(join(repo, "vectors/pczt.json"), "utf8")) as PcztVector[];
const vector = (n: string) => {
  const v = pczts.find((e) => e.name === n)!;
  return { v, bytes: new Uint8Array(readFileSync(join(repo, "vectors", v.file))) };
};

async function rejects(p: Promise<unknown>, ok: (e: unknown) => boolean, m: string): Promise<void> {
  try { await p; check(false, `${m}: did not reject`); } catch (e) { check(ok(e), `${m}: ${String(e)}`); }
}

/**
 * The board refuses a request for the network it is not set to (the
 * network guard, firmware branch board-network-guard). zdev with it
 * refuses mainnet; zdev without it answers. Either is right here, as
 * long as a refusal reaches the user as the board's own words.
 */
async function onMainnet<T>(what: string, f: () => Promise<T>): Promise<T | null> {
  try {
    return await f();
  } catch (e) {
    if (e instanceof DeviceError && (e.code & 0xff00) !== 0x0200) {
      check(zErrorText(e).includes(e.message.replace(/^device error 0x[0-9a-f]{4}: /, "")),
        `${what}: the board's message is not shown verbatim`);
      console.log(`  (${what}: the board refused mainnet: ${zErrorText(e)})`);
      return null;
    }
    throw e;
  }
}

const bin = zdevBinary();
if (!bin) {
  console.log("SKIPPED: zdev could not be built (see above); nothing was tested against the firmware");
  process.exit(0);
}

group("an approving board");
{
  const z = await startZdev(bin, { yes: true });
  const dev = new ZdevClient(z.link);
  try {
    const passkey = await dev.pair();
    await new Promise((r) => setTimeout(r, 50));
    check(z.passkeys.at(-1) === passkey, `passkey ${passkey} is not the one zdev shows`);

    const f = await zGetFeatures(dev.call);
    check(f.showAddress && f.maxChunk > 0 && f.maxPczt > 0, `features ${JSON.stringify(f)}`);

    for (const network of ["test", "main"] as ZNetwork[]) {
      const k = key(`${network}/abandon-art/pp0/acct0`);
      const get = () => zGetAccount(dev.call, 0, network);
      const a = network === "main" ? await onMainnet("zGetAccount", get) : await get();
      if (!a) continue;
      check(a.ufvk === k.ufvk, `${network} ufvk`);
      check(a.ua === k.ua, `${network} ua`);
      check(a.tAddress === k.transparent_address, `${network} t-address`);
      check(a.seedFingerprint === k.seed_fingerprint, `${network} fingerprint`);
      check(a.birthday === null, `${network} has no birthday yet`);
    }
    check((await zGetAccount(dev.call, 1, "test")).ufvk === key("test/abandon-art/pp0/acct1").ufvk, "account 1");

    group("one birthday per network");
    check(await zSetBirthday(dev.call, 0, "test", 4_501_000), "stored in flash");
    check((await zGetAccount(dev.call, 0, "test")).birthday === 4_501_000, "testnet birthday");
    if (await onMainnet("zSetAccountMeta", () => zSetBirthday(dev.call, 0, "main", 3_520_600))) {
      check((await zGetAccount(dev.call, 0, "main")).birthday === 3_520_600, "mainnet birthday");
      check((await zGetAccount(dev.call, 0, "test")).birthday === 4_501_000, "testnet birthday kept");
    }
    await rejects(zSetBirthday(dev.call, 0, "test", 0), (e) => /block height/.test(String(e)), "birthday 0");

    group("the address check");
    const ua = key("test/abandon-art/pp0/acct0").ua;
    check((await zShowAddress(dev.call, 0, "test", ua)).ua === ua, "confirmed address");
    await rejects(zShowAddress(dev.call, 0, "test", key("test/abandon-art/pp0/acct1").ua),
      (e) => e instanceof DeviceError && e.code === ZErr.ForeignAddress && /NOT from its wallet/.test(zErrorText(e)),
      "another account's address");

    group("signing PCZT files");
    {
      const { v, bytes } = vector("iw_send_shielded_compact");
      const phases: string[] = [];
      const r = await zSign(dev.call, bytes, 0, "test", (p) => phases.push(p.phase));
      check(toHex(r.sighash) === v.sighash, "compact send: sighash");
      check(toHex(r.sha256) === createHash("sha256").update(bytes).digest("hex"), "compact send: sha256");
      check(phases.includes("upload") && phases.at(-1) === "review", `phases ${phases}`);
      const zsig = parseZsig(encodeZsig(r.sha256, r.entries));
      check(zsig.version === 1 && zsig.entries.length === 1 && zsig.entries[0]!.pool === ZPool.Ironwood, "compact send: .zsig");
    }
    {
      const { v, bytes } = vector("iw_send_transparent");
      const r = await zSign(dev.call, bytes, 0, "test");
      check(toHex(r.sighash) === v.sighash, "full PCZT in several chunks");
    }
    {
      const { v, bytes } = vector("t_shield_compact");
      const r = await zSign(dev.call, bytes, 0, "test");
      check(toHex(r.sighash) === v.sighash, "shield: sighash");
      check(r.entries.some((e) => e.pool === ZPool.Transparent), "shield: transparent signatures");
      check(encodeZsig(r.sha256, r.entries)[4] === 2, "shield: .zsig version 2");
    }
    {
      const { v, bytes } = vector("main_iw_send_shielded_compact");
      const r = await onMainnet("zSign", () => zSign(dev.call, bytes, 0, "main"));
      if (r) check(toHex(r.sighash) === v.sighash, "mainnet send");
    }

    group("refusals carry the board's words, and the session goes on");
    await rejects(zSign(dev.call, vector("r_output_value_tampered").bytes, 0, "test"),
      (e) => e instanceof DeviceError && e.code === (ZErr.SignBase | 13) && /^The board refused: /.test(zErrorText(e)),
      "tampered output");
    await rejects(zSign(dev.call, vector("main_iw_send_shielded_compact").bytes, 0, "test"),
      (e) => e instanceof DeviceError && (e.code & 0xff00) === ZErr.SignBase, "mainnet PCZT signed as testnet");
    await rejects(zSign(dev.call, new TextEncoder().encode("not a pczt"), 0, "test"),
      (e) => /not a PCZT/.test(String(e)), "a file that is not a PCZT");
    check((await zGetAccount(dev.call, 0, "test")).ua === ua, "session still usable");
  } finally {
    dev.close();
    z.stop();
  }
}

group("a board that says no");
{
  const z = await startZdev(bin, { yes: false });
  const dev = new ZdevClient(z.link);
  try {
    z.answer("y"); // the pairing
    await dev.pair();
    z.answer("n"); // the review
    await rejects(zSign(dev.call, vector("iw_send_shielded_compact").bytes, 0, "test"),
      (e) => e instanceof DeviceError && e.code === ZErr.UserRejected, "rejected review");
    z.answer("n"); // the address check
    await rejects(zShowAddress(dev.call, 0, "test", key("test/abandon-art/pp0/acct0").ua),
      (e) => e instanceof DeviceError && /do not match/.test(zErrorText(e)), "rejected address");
    z.answer("y");
    const { v, bytes } = vector("iw_send_shielded_compact");
    check(toHex((await zSign(dev.call, bytes, 0, "test")).sighash) === v.sighash, "signs after a rejection");
  } finally {
    dev.close();
    z.stop();
  }
}

/* librustzcash's address of test/abandon-art/pp0/acct0 at diversifier
 * index 1 and its transparent receiver (firmware/sim/test_zcash_account.c,
 * host/zecz-core/tests/diversified.rs): what the wallet's rotation hands out. */
const DIVERSIFIED_1 = {
  ua: "utest1nggn4c6e0gmlr6fjlt4sel2wnph2r5ytca36uffhetvjpshjm3zy4xzeedhu56sj5qkyaafm6xq5xc75st8nj959mzwvz6qca2gzllzgfuxapwdxmcrafvpj5vm7tmed83mku2umc9g",
  t: "tmVNYZhAQMp61GkauCYV5R4eVKpxzzMZBJF",
};

/**
 * The automatic wallet (src/zcash-auto.ts) on a board: read the account,
 * create the wallet at the tip minus the margin and record the birthday on
 * the board, open it on the next connection; a passphrase is another
 * wallet. The wallet backend is the in-memory one (test/fake-wallet.ts);
 * the board is zdev.
 */
async function autoFlow(network: ZNetwork): Promise<void> {
  group(`the automatic wallet on a ${network === "main" ? "Mainnet" : "Testnet"} board`);
  const z = await startZdev(bin!, { yes: true, args: ["--network", network] });
  const dev = new ZdevClient(z.link);
  const w = fakeWallet({ tip: network === "main" ? 3_100_000 : 3_700_000 });
  try {
    await dev.pair();
    check(parseStatus(await dev.call("getStatus")).network === network,
      `getStatus does not report the board on ${network}`);
    const k = key(`${network}/abandon-art/pp0/acct0`);
    const a = await readAccountRetrying(dev.call, 0, network);
    check(a.seedFingerprint === k.seed_fingerprint && a.birthday === null, "the account, no birthday yet");
    const r = await ensureWallet(a, { call: dev.call, invoke: w.invoke });
    const expected = (network === "main" ? 3_100_000 : 3_700_000) - BIRTHDAY_MARGIN;
    check(r.created && r.birthday === expected && r.birthdaySource === "tip" && r.storedOnBoard, `created at ${expected}`);
    check((await zGetAccount(dev.call, 0, network)).birthday === expected, "the board now has the birthday");
    check(w.wallets.has(`${network}-${k.seed_fingerprint}-0`), "the wallet is keyed by network, fingerprint and account");
    const again = await ensureWallet(await readAccountRetrying(dev.call, 0, network), { call: dev.call, invoke: w.invoke });
    check(!again.created && again.birthdaySource === "existing" && w.calls.filter((c) => c === "zw_import").length === 1, "the next connection opens it");
    const other: ZNetwork = network === "main" ? "test" : "main";
    await rejects(readAccountRetrying(dev.call, 0, other, { retryMs: 1 }),
      (e) => e instanceof DeviceError && e.code === 0x0528, "the other network is refused by the board");
    /* The Companion on the other network: the refusal names the board's
     * network, and the one-tap switch reads the account there. */
    let note: ReturnType<typeof wrongNetwork> | null = null;
    try { await zGetAccount(dev.call, 0, other); } catch (e) { if (isWrongNetwork(e)) note = wrongNetwork(other, (e as Error).message); }
    check(note !== null && note.board === network, `the notice names the board's network (${note?.board})`);
    const switched = await readAccountRetrying(dev.call, 0, note!.board);
    check(switched.network === network && switched.seedFingerprint === k.seed_fingerprint, "after the switch the account reads");
    /* getStatus names the board's network on firmware that reports it;
     * older firmware has no field, and that is fine too. */
    const st = await dev.call("getStatus");
    const reported = networkFromStatus(st["network"]);
    check(reported === null || reported === network, `getStatus network ${String(st["network"])}`);
    if (reported !== null) console.log(`  (getStatus reports ${reported})`);
  } finally {
    dev.close();
    z.stop();
  }

  /* The same seed with a passphrase: another fingerprint, another wallet. */
  const zp = await startZdev(bin!, { yes: true, args: ["--network", network, "--passphrase", "hunter2"] });
  const devp = new ZdevClient(zp.link);
  try {
    await devp.pair();
    const a = await readAccountRetrying(devp.call, 0, network);
    check(a.seedFingerprint !== key(`${network}/abandon-art/pp0/acct0`).seed_fingerprint, "a passphrase is another seed fingerprint");
    const r = await ensureWallet(a, { call: devp.call, invoke: w.invoke });
    check(r.created && w.wallets.size === 2, "...and gets its own wallet");
  } finally {
    devp.close();
    zp.stop();
  }
}

await autoFlow("test");
await autoFlow("main");

group("rotation and the board address, checked on the board");
{
  const z = await startZdev(bin, { yes: true });
  const dev = new ZdevClient(z.link);
  try {
    await dev.pair();
    const a = await readAccountRetrying(dev.call, 0, "test");
    const m = new ReceiveModel(async () => ({ unified: DIVERSIFIED_1.ua, transparent: DIVERSIFIED_1.t }));
    m.board = { ua: a.ua, tAddress: a.tAddress };
    m.defaults = { unified: a.ua, transparent: a.tAddress };
    await m.rotate();
    const fresh = m.shielded();
    check(fresh.ua === DIVERSIFIED_1.ua && fresh.ua !== a.ua, "the fresh address is not the board's index 0");
    const shown = await zShowAddress(dev.call, 0, "test", fresh.verifyUa!);
    check(shown.ua === DIVERSIFIED_1.ua, "the board confirms the rotated address is its own");
    check(shown.tAddress === DIVERSIFIED_1.t, `the board derives the same transparent receiver (${shown.tAddress})`);
    m.mode = "board";
    check((await zShowAddress(dev.call, 0, "test", m.shielded().verifyUa!)).ua === a.ua, "the board address (index 0) is confirmed as the board's");
    m.mode = "fresh";
    const t = await m.newTransparent();
    const tv = m.transparent();
    check(t === DIVERSIFIED_1.t && tv.verifyUa === DIVERSIFIED_1.ua, "a new transparent address and its unified address");
    check((await zShowAddress(dev.call, 0, "test", tv.verifyUa!)).tAddress === t, "the board checks the transparent receiver through it");
    await rejects(zShowAddress(dev.call, 0, "test", key("test/abandon-art/pp0/acct1").ua),
      (e) => e instanceof DeviceError && e.code === ZErr.ForeignAddress, "another account's rotated address is refused");
  } finally {
    dev.close();
    z.stop();
  }
}

console.log(`\n${failures ? "FAILED" : "PASSED"} (${failures} failure${failures === 1 ? "" : "s"})`);
process.exit(failures ? 1 : 0);
