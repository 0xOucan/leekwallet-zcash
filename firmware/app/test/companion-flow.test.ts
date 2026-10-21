/**
 * The Companion's automatic flow without a board: the sync schedule
 * (src/sync-scheduler.ts), the remembered board (src/board-memory.ts), the
 * Receive screen's rotation and board-address view (src/receive-model.ts),
 * and the wallet set-up (src/zcash-auto.ts) over a scripted board and a
 * scripted wallet backend. The same set-up against the firmware is in
 * test/zcash-zdev.test.ts.
 */

import { readFileSync } from "node:fs";
import { join } from "node:path";

import type { CborValue } from "../packages/core/src/cbor.ts";
import { DeviceError } from "../packages/core/src/transport.ts";
import type { ZCall, ZNetwork } from "../packages/core/src/zcash.ts";
import type { Invoke } from "../src/tauri-transport.ts";
import { boardLabel, forgetBoard, loadBoard, saveBoard, BOARD_KEY } from "../src/board-memory.ts";
import { ReceiveModel } from "../src/receive-model.ts";
import { SyncScheduler, type Timers } from "../src/sync-scheduler.ts";
import { BIRTHDAY_MARGIN, ensureWallet, readAccountRetrying } from "../src/zcash-auto.ts";
import { fakeWallet } from "./fake-wallet.ts";
import { txHead, txKindLabel } from "../src/tx-labels.ts";
import { isWrongNetwork, networkFromStatus, otherNetwork, wrongNetwork, WRONG_NETWORK } from "../src/network-guard.ts";
import { closeLeftoverLink, releasedMessage } from "../src/link-events.ts";
import {
  capabilityRange, centerCrop, clampToRange, decodePasses, enhance, loadCamera, nextCamera, saveCamera,
  videoConstraints, CAMERA_KEY,
} from "../src/qr-tuning.ts";
import { repo } from "./zdev.ts";

let failures = 0;
const check = (c: boolean, m: string) => { if (!c) { console.log(`  FAIL: ${m}`); failures++; } };
const group = (n: string) => console.log(`== ${n}`);
const tick = () => new Promise((r) => setTimeout(r, 0));

/* ---------------------------------------------------------- scheduler */

/** A clock the test moves by hand. */
function fakeTimers() {
  let now = 0;
  let next = 1;
  const pending = new Map<number, { at: number; f: () => void }>();
  const t: Timers & { advance(ms: number): Promise<void>; count(): number } = {
    set: (f, ms) => { const id = next++; pending.set(id, { at: now + ms, f }); return id; },
    clear: (id) => { pending.delete(id as number); },
    now: () => now,
    async advance(ms) {
      const until = now + ms;
      for (;;) {
        const due = [...pending.entries()].filter(([, p]) => p.at <= until).sort((a, b) => a[1].at - b[1].at)[0];
        if (!due) break;
        pending.delete(due[0]);
        now = due[1].at;
        due[1].f();
        await tick();
      }
      now = until;
    },
    count: () => pending.size,
  };
  return t;
}

group("sync: on open, on the interval, never twice at once");
{
  const timers = fakeTimers();
  let runs = 0;
  let release: (() => void) | null = null;
  let concurrent = 0;
  let maxConcurrent = 0;
  const s = new SyncScheduler(async () => {
    runs++;
    concurrent++;
    maxConcurrent = Math.max(maxConcurrent, concurrent);
    await new Promise<void>((r) => { release = r; });
    concurrent--;
  }, { intervalMs: 75_000, timers });
  s.start();
  await tick();
  check(runs === 1 && s.running, "syncs as soon as the wallet opens");
  s.start();
  await tick();
  check(runs === 1, "start twice is one sync");
  void s.trigger();
  void s.trigger();
  await tick();
  check(runs === 1, "a request during a sync does not start a second one");
  release!();
  await tick();
  await tick();
  check(runs === 2, "...it runs once right after (a send just made shows up)");
  release!();
  await tick();
  check(runs === 2 && !s.running && timers.count() === 1, "then waits for the interval");
  await timers.advance(74_999);
  check(runs === 2, "not before the interval");
  await timers.advance(1);
  check(runs === 3, "every 75 s");
  release!();
  await tick();
  check(maxConcurrent === 1, "never two syncs at once");

  group("sync: paused in the background");
  s.pause();
  check(timers.count() === 0, "no timer while in the background");
  await timers.advance(300_000);
  check(runs === 3, "nothing runs in the background");
  void s.trigger();
  await tick();
  check(runs === 3, "not even on request");
  s.resume();
  await tick();
  check(runs === 4, "back in the foreground with a stale sync: syncs at once");
  release!();
  await tick();
  s.pause();
  await timers.advance(10_000);
  s.resume();
  await tick();
  check(runs === 4 && timers.count() === 1, "back soon after a sync: waits for the rest of the interval");
  await timers.advance(65_000);
  check(runs === 5, "...then syncs");
  release!();
  await tick();

  group("sync: stopped when the wallet leaves");
  s.stop();
  check(timers.count() === 0 && !s.started, "no timer after stop");
  void s.trigger();
  await tick();
  check(runs === 5, "a request after stop does nothing");
}
{
  const timers = fakeTimers();
  let runs = 0;
  const s = new SyncScheduler(async () => { runs++; throw new Error("server down"); }, { intervalMs: 1000, timers });
  s.start();
  await tick();
  await timers.advance(1000);
  check(runs === 2, "a failed sync is tried again on the next interval");
  s.stop();
}

group("sync: held off during a send, then caught up");
{
  const timers = fakeTimers();
  let runs = 0;
  let release: (() => void) | null = null;
  const s = new SyncScheduler(async () => { runs++; await new Promise<void>((r) => { release = r; }); }, { intervalMs: 1000, timers });
  s.start();
  await tick();
  let idle = false;
  void s.whenIdle().then(() => { idle = true; });
  await tick();
  check(!idle, "whenIdle waits for the running sync");
  release!();
  await tick();
  check(idle && runs === 1, "...and resolves when it ends");
  const done = s.hold();
  check(timers.count() === 0, "no timer while held");
  await timers.advance(5000);
  void s.trigger();
  await tick();
  check(runs === 1, "no sync while a send holds it (the finish would be refused)");
  done();
  await tick();
  check(runs === 2, "released: the sync asked for meanwhile runs");
  release!();
  await tick();
  done();
  check(timers.count() === 1, "a second release changes nothing");
  s.stop();
}

/* ------------------------------------------------------- board memory */

group("the remembered board");
{
  const m = new Map<string, string>();
  const store = { getItem: (k: string) => m.get(k) ?? null, setItem: (k: string, v: string) => { m.set(k, v); }, removeItem: (k: string) => { m.delete(k); } };
  check(loadBoard(store) === null, "nothing at first");
  saveBoard({ link: "ble", bleId: "AA:BB:CC:DD:EE:FF", bleName: "LeekWallet-1A2B", at: 5 }, store);
  const b = loadBoard(store);
  check(b?.link === "ble" && b.bleId === "AA:BB:CC:DD:EE:FF" && b.bleName === "LeekWallet-1A2B", "Bluetooth id and name");
  check(boardLabel(b!) === "Bluetooth · LeekWallet-1A2B", "label");
  const stored = m.get(BOARD_KEY)!;
  check(!/passkey|session|key"/i.test(stored), "no session secret is stored");
  saveBoard({ link: "usb", at: 6 }, store);
  check(loadBoard(store)?.link === "usb" && boardLabel(loadBoard(store)!) === "USB cable", "USB");
  m.set(BOARD_KEY, "{not json");
  check(loadBoard(store) === null, "garbage reads as nothing");
  m.set(BOARD_KEY, JSON.stringify({ link: "ble" }));
  check(loadBoard(store) === null, "Bluetooth without an id reads as nothing");
  forgetBoard(store);
  check(!m.has(BOARD_KEY), "forgotten");
}

/* ------------------------------------------------------ receive model */

interface KeyVector { name: string; ufvk: string; ua: string; transparent_address: string; seed_fingerprint: string; ua_orchard_only: string }
const keys = JSON.parse(readFileSync(join(repo, "vectors/keys.json"), "utf8")) as KeyVector[];
const key = (n: string) => keys.find((k) => k.name === n)!;
const k0 = key("test/abandon-art/pp0/acct0");

group("receive: a fresh address each time, and the board's own");
{
  let i = 0;
  const handed: string[] = [];
  let fail = false;
  const m = new ReceiveModel(async () => {
    if (fail) throw new Error("this network's wallet is busy");
    i++;
    const a = { unified: `utest1fresh${i}`, transparent: i <= 3 ? `tmFresh${i}` : null };
    handed.push(a.unified);
    return a;
  });
  m.defaults = { unified: k0.ua, transparent: k0.transparent_address };
  m.board = { ua: k0.ua, tAddress: k0.transparent_address };
  check(m.shielded().ua === k0.ua && /default/.test(m.shielded().note), "before rotating: the default address");
  await m.rotate();
  const first = m.shielded();
  await m.rotate();
  const second = m.shielded();
  check(first.ua === "utest1fresh1" && second.ua === "utest1fresh2", "each opening shows a new address");
  check(second.verifyUa === second.ua, "verify on the board checks the address shown");
  m.mode = "board";
  const b = m.shielded();
  check(b.ua === k0.ua && b.verifyUa === k0.ua && /index 0/.test(b.title), "the board address (index 0) is the board's own ua");
  check(m.transparent().address === k0.transparent_address, "board view: the board's t-address");
  m.mode = "fresh";
  check(m.shielded().ua === "utest1fresh2", "back to the fresh one, unchanged by the toggle");
  fail = true;
  check(!(await m.rotate()) && m.shielded().ua === "utest1fresh2" && /busy/.test(m.lastError ?? ""), "a failed rotation keeps the last address");
  fail = false;

  group("receive: transparent, default and new");
  check(m.transparent().address === k0.transparent_address && m.transparent().verifyUa === k0.ua, "default: index 0, checked through the board's ua");
  const t = await m.newTransparent();
  check(t === "tmFresh3", "a new transparent address: the next diversified index's receiver");
  const tv = m.transparent();
  check(tv.address === t && tv.verifyUa === m.fresh?.unified, "checked on the board through the unified address that carries it");
  check(/Public/.test(tv.note) && /Shield/.test(tv.note), "says it is public and to shield");
  check((await m.newTransparent()) === null && /shielded-only/.test(m.lastError ?? ""), "no transparent receiver left: says so");
  m.reset();
  check(m.shielded().ua === null && m.transparent().address === null && m.mode === "fresh", "reset forgets everything");
}

/* --------------------------------------------------------- auto set-up */

function scriptedBoard(answer: (method: string, p: Record<string, CborValue>) => Record<string, CborValue>) {
  const asked: { method: string; params: Record<string, CborValue> }[] = [];
  const call: ZCall = async (method, params = {}) => { asked.push({ method, params }); return answer(method, params); };
  return { call, asked };
}
const unhex = (s: string) => new Uint8Array((s.match(/../g) ?? []).map((h) => parseInt(h, 16)));
const accountReply = (birthday: number | null) => ({
  account: 0, network: "test", seedFingerprint: unhex(k0.seed_fingerprint), ufvk: k0.ufvk, ua: k0.ua,
  tAddress: k0.transparent_address, ...(birthday === null ? {} : { birthday }),
});

group("auto: 0x0300 while the vault opens is asked once more");
{
  let n = 0;
  const b = scriptedBoard((m) => {
    if (m === "zGetAccount" && n++ === 0) throw new DeviceError(0x0300, "derivation failed");
    return accountReply(null);
  });
  let retried = false;
  const a = await readAccountRetrying(b.call, 0, "test", { retryMs: 1, onRetry: () => { retried = true; } });
  check(retried && a.ua === k0.ua && b.asked.length === 2, "read on the second try");
  let m = 0;
  const twice = scriptedBoard(() => { m++; throw new DeviceError(0x0300, "derivation failed"); });
  try { await readAccountRetrying(twice.call, 0, "test", { retryMs: 1 }); check(false, "two 0x0300 rejected"); }
  catch (e) { check((e as DeviceError).code === 0x0300 && m === 2, "only once more"); }
  const other = scriptedBoard(() => { throw new DeviceError(0x0528, "the board is set to Mainnet"); });
  try { await readAccountRetrying(other.call, 0, "test", { retryMs: 1 }); check(false, "0x0528 rejected"); }
  catch (e) { check((e as DeviceError).code === 0x0528 && other.asked.length === 1, "other errors are not retried"); }
}

group("auto: no birthday on the board -> tip minus a margin, recorded on the board");
{
  let boardBirthday: number | null = null;
  const b = scriptedBoard((m, p) => {
    if (m === "zGetAccount") return accountReply(boardBirthday);
    if (m === "zSetAccountMeta") { boardBirthday = Number(p["birthday"]); return { stored: 1 }; }
    throw new Error(`unexpected ${m}`);
  });
  const w = fakeWallet({ tip: 3_700_000 });
  const a = await readAccountRetrying(b.call, 0, "test");
  const r = await ensureWallet(a, { call: b.call, invoke: w.invoke });
  check(r.created && r.birthdaySource === "tip" && r.birthday === 3_700_000 - BIRTHDAY_MARGIN, `created at the tip minus ${BIRTHDAY_MARGIN}`);
  check(boardBirthday === 3_700_000 - BIRTHDAY_MARGIN && r.storedOnBoard, "the birthday is now on the board");
  check(w.calls.filter((c) => c === "zw_import").length === 1, "one import");
  const again = await ensureWallet(await readAccountRetrying(b.call, 0, "test"), { call: b.call, invoke: w.invoke });
  check(!again.created && again.birthdaySource === "existing", "the next connection opens it");
  check(w.calls.filter((c) => c === "zw_import").length === 1 && b.asked.filter((x) => x.method === "zSetAccountMeta").length === 1, "...without importing or writing again");
}

group("auto: the board's birthday is used as it is");
{
  const b = scriptedBoard((m) => {
    if (m === "zGetAccount") return accountReply(3_456_789);
    throw new Error(`the board was asked ${m}`);
  });
  const w = fakeWallet({ tip: 3_700_000 });
  const r = await ensureWallet(await readAccountRetrying(b.call, 0, "test"), { call: b.call, invoke: w.invoke });
  check(r.created && r.birthday === 3_456_789 && r.birthdaySource === "board" && !r.storedOnBoard, "born at the board's birthday");
  check(!w.calls.includes("zw_tip"), "the tip is not asked for");
}

group("auto: a board that cannot record the birthday still gets its wallet");
{
  const b = scriptedBoard((m) => {
    if (m === "zGetAccount") return accountReply(null);
    throw new DeviceError(0x0300, "no wallet on the device");
  });
  const w = fakeWallet({ tip: 3_700_000 });
  const logs: string[] = [];
  const r = await ensureWallet(await readAccountRetrying(b.call, 0, "test", { retryMs: 1 }), { call: b.call, invoke: w.invoke, log: (l) => logs.push(l) });
  check(r.created && !r.storedOnBoard && logs.some((l) => /not recorded on the board/.test(l)), "created, and the log says why the board has no birthday");
}

group("activity: every kind has a label, a send to yourself shows its fee");
{
  check(txKindLabel("received") === "Received" && txKindLabel("sent") === "Sent", "received and sent");
  check(txKindLabel("self") === "Sent to yourself", "self is \"Sent to yourself\"");
  check(txKindLabel("shielded") === "Shielded" && txKindLabel("internal") === "Moved between pools", "shielded and internal");
  check(txKindLabel("swapOut") === "Swap out" && txKindLabel("refund_pending") === "Refund pending", "a newer kind reads as words");
  for (const k of [undefined, null, 7, "", "<b>", "x".repeat(80)]) {
    const l = txKindLabel(k);
    check(l === "Transaction" && !/undefined/.test(l), `kind ${JSON.stringify(k)} -> ${l}`);
  }
  const self = txHead({ kind: "self", deltaZat: -10_000, amountZat: 10_000, feeZat: 10_000 });
  check(self.label === "Sent to yourself" && self.amount === "−0.0001 ZEC fee" && self.dir === "out", `self: ${JSON.stringify(self)}`);
  const selfNoFee = txHead({ kind: "self", deltaZat: -15_000, amountZat: 15_000, feeZat: null });
  check(selfNoFee.amount === "−0.00015 ZEC fee", "without a known fee, the balance change is the fee");
  check(txHead({ kind: "sent", deltaZat: -150_010_000, amountZat: 150_000_000, feeZat: 10_000 }).amount === "−1.5001 ZEC", "a send shows what left");
  check(txHead({ kind: "received", deltaZat: 100_000, amountZat: 100_000, feeZat: null }).amount === "+0.001 ZEC", "a receive");
  const hidden = txHead({ kind: "self", deltaZat: -10_000, amountZat: 10_000, feeZat: 10_000 }, true);
  check(hidden.amount === "••••" && hidden.label === "Sent to yourself", "hidden balances stay hidden");
  const fresh = txHead({ kind: "mystery", deltaZat: -1, amountZat: 1, feeZat: null });
  check(`${fresh.label} · date`.startsWith("Mystery · "), "never \"undefined · date\"");
}

group("wrong network: the board's 0x0528, and the one-tap switch");
{
  check(WRONG_NETWORK === 0x0528, "the code");
  check(isWrongNetwork(new DeviceError(0x0528, "the board is set to Mainnet")), "0x0528 is the wrong network");
  check(!isWrongNetwork(new DeviceError(0x0300, "no wallet")) && !isWrongNetwork(new Error("x")) && !isWrongNetwork(null), "nothing else is");
  check(otherNetwork("test") === "main" && otherNetwork("main") === "test", "two networks");
  const w = wrongNetwork("test", "the board is set to Mainnet; switch it to Testnet in Settings > Network, or use the Mainnet app.");
  check(w.board === "main" && w.button === "Switch the app to Mainnet", `button: ${w.button}`);
  check(/set to Mainnet and this app to Testnet/.test(w.text) && /Settings → Network/.test(w.text), "says both networks and the board setting");
  check(/\(The board says: the board is set to Mainnet;.*Mainnet app\.\)$/.test(w.text), `keeps the board's words once: ${w.text}`);
  check(/\(The board says: the board is set to Testnet\.\)$/.test(wrongNetwork("main", "device error 0x0528: the board is set to Testnet").text), "without the code prefix");
  check(wrongNetwork("main").button === "Switch the app to Testnet" && !/board says/.test(wrongNetwork("main").text), "and the other way, without a message");

  /* A board on Mainnet, the app on Testnet: refused, switched, read. */
  const board: ZNetwork = "main";
  const b = scriptedBoard((m, p) => {
    if (m !== "zGetAccount") throw new Error(`asked ${m}`);
    if (p["network"] !== board) throw new DeviceError(WRONG_NETWORK, "the board is set to Mainnet");
    const km = keys.find((k) => k.name === "main/abandon-art/pp0/acct0")!;
    return { account: 0, network: board, seedFingerprint: unhex(km.seed_fingerprint), ufvk: km.ufvk, ua: km.ua, tAddress: km.transparent_address };
  });
  let app: ZNetwork = "test";
  let note: ReturnType<typeof wrongNetwork> | null = null;
  try { await readAccountRetrying(b.call, 0, app, { retryMs: 1 }); } catch (e) { if (isWrongNetwork(e)) note = wrongNetwork(app, (e as Error).message); }
  check(note !== null && note.board === "main", "the refusal becomes the notice");
  app = note!.board; // the tap
  const a = await readAccountRetrying(b.call, 0, app, { retryMs: 1 });
  check(a.network === "main" && b.asked.length === 2, "after the switch the account reads, once more and no more");

  check(networkFromStatus("main") === "main" && networkFromStatus("Testnet") === "test" && networkFromStatus(" mainnet ") === "main", "getStatus network, in its spellings");
  for (const v of [undefined, null, 1, "", "regtest", {}]) check(networkFromStatus(v) === null, `no network from ${JSON.stringify(v)}`);
}

group("link: closed on a new start and in the background, said in words");
{
  const asked: string[] = [];
  const invoke = (async <T,>(cmd: string): Promise<T> => { asked.push(cmd); return true as T; }) as Invoke;
  const line = await closeLeftoverLink(invoke);
  check(asked[0] === "link_reset" && line !== null && /left open by the previous run/.test(line), `a leftover link: ${line}`);
  const none = await closeLeftoverLink((async <T,>() => false as T) as Invoke);
  check(none === null, "nothing left over, nothing said");
  const old = await closeLeftoverLink((async () => { throw new Error("unknown command link_reset"); }) as Invoke);
  check(old !== null && /could not close/.test(old), "an older backend does not stop the start");
  check(await closeLeftoverLink(null) === null, "a browser tab has no link");
  check(/background for more than 30 seconds/.test(releasedMessage("background")) && /Reconnect/.test(releasedMessage("background")), "background");
  check(!/undefined/.test(releasedMessage("")) && /no reason given/.test(releasedMessage("")), "an empty reason still reads");
}

group("scanner: camera memory, sliders, crops, passes, contrast");
{
  const mem = new Map<string, string>();
  const st = { getItem: (k: string) => mem.get(k) ?? null, setItem: (k: string, v: string) => { mem.set(k, v); } };
  check(JSON.stringify(loadCamera(st)) === JSON.stringify({ facing: "environment" }), "the rear camera by default");
  saveCamera({ deviceId: "abc" }, st);
  check(mem.get(CAMERA_KEY) === "id:abc" && JSON.stringify(loadCamera(st)) === JSON.stringify({ deviceId: "abc" }), "a chosen camera is remembered");
  saveCamera({ facing: "user" }, st);
  check(JSON.stringify(loadCamera(st)) === JSON.stringify({ facing: "user" }), "the front camera is remembered");
  mem.set(CAMERA_KEY, "garbage");
  check("facing" in loadCamera(st), "garbage reads as the rear camera");
  const vc = videoConstraints({ deviceId: "abc" }) as { deviceId: { exact: string }; width: { ideal: number }; height: { ideal: number } };
  check(vc.deviceId.exact === "abc" && vc.width.ideal === 1920 && vc.height.ideal === 1080, "full HD asked of the chosen camera");
  const devs = [{ deviceId: "rear" }, { deviceId: "front" }];
  check(JSON.stringify(nextCamera(devs, { facing: "environment" }, "rear")) === JSON.stringify({ deviceId: "front" }), "rear -> front");
  check(JSON.stringify(nextCamera(devs, { deviceId: "front" })) === JSON.stringify({ deviceId: "rear" }), "front -> rear");
  check(JSON.stringify(nextCamera([{ deviceId: "" }], { facing: "environment" })) === JSON.stringify({ facing: "user" }), "ids unknown: the other facing mode");
  const ev = capabilityRange({ min: -2, max: 2, step: 0.5 }, 0);
  check(ev !== null && ev.min === -2 && ev.max === 2 && ev.value === 0, "exposure compensation as a slider");
  check(capabilityRange(undefined, 0) === null && capabilityRange({ min: 1, max: 1 }, 1) === null, "no slider without a range");
  check(clampToRange(-7, { min: -2, max: 2, step: 0.5 }) === -2 && clampToRange(0.74, { min: -2, max: 2, step: 0.5 }) === 0.5, "clamped onto the step");
  const z = capabilityRange({ min: 1, max: 8 }, 3);
  check(z !== null && z.step === 0.35 && Math.abs(z.value - 3.1) < 1e-9, `zoom without a step gets one: ${JSON.stringify(z)}`);
  const c = centerCrop(1920, 1080);
  check(c.size === 648 && c.sx === 636 && c.sy === 216, `the middle square: ${JSON.stringify(c)}`);
  const seen = new Set<string>();
  for (let n = 0; n < 6; n++) {
    const ps = decodePasses(n);
    check(ps.length === 2 && ps[0]!.region === "center" && !ps[0]!.contrast, "the middle at full resolution every frame");
    seen.add(JSON.stringify(ps[1]));
  }
  check(seen.size === 3, "three extra passes in rotation: the whole frame, contrast, the other binarizer");
  check(decodePasses(0, -2).every((p) => p.contrast), "brightness lowered in software: every pass gets the contrast");
  /* A washed-out code: dark modules at 200, light at 250 (a glowing screen). */
  const px = new Uint8ClampedArray(4 * 100);
  for (let i = 0; i < 100; i++) { const v = i % 2 ? 250 : 200; px.set([v, v, v, 255], i * 4); }
  enhance(px);
  check(px[0] === 0 && px[4] === 255 && px[3] === 255, `stretched to black and white: ${px[0]}, ${px[4]}`);
  const grey = new Uint8ClampedArray(4 * 3);
  [60, 128, 200].forEach((v, i) => grey.set([v, v, v, 255], i * 4));
  const plain = grey.slice();
  enhance(plain, 0);
  enhance(grey, -3);
  check(grey[4]! < plain[4]!, `darker in the middle with the brightness lowered: ${grey[4]} < ${plain[4]}`);
}

console.log(`\n${failures ? "FAILED" : "PASSED"} (${failures} failure${failures === 1 ? "" : "s"})`);
process.exit(failures ? 1 : 0);
