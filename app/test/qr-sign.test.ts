/**
 * Sign with QR (experimental), the state machine in src/qr-sign.ts, over a
 * scripted backend and a hand-moved clock: the defaults and the controls,
 * the frame loop and its batches, the scan and its progress, and the
 * hand-off of the .zsig. The UR itself is host/leek-ur's, tested in Rust
 * against the firmware's vectors (and src-tauri/src/airgap.rs against the
 * shared fixtures); here the backend's frames are stand-ins of the same
 * shape.
 */

import {
  bigQrAvail, benchTable, frameCaption, loadQrSettings, moduleScale, qrVersionFor, saveQrSettings, scanCaption, timingCaption,
  QrSigner, QR_BATCH, QR_BENCH_CONFIGS, QR_FRAGMENTS, QR_PRESETS, QR_INTERVALS, QR_SETTINGS_KEY, type Clock, type ScanProgress,
} from "../src/qr-sign.ts";
import type { Invoke } from "../src/tauri-transport.ts";
import { fullQrAvail } from "../src/qr-sign.ts";
import { accountsCaption, recordRows, resolveBirthday, QrAccountImport, type AccountsProgress, type QrAccountRecord } from "../src/qr-accounts.ts";
import { BIRTHDAY_MARGIN } from "../src/zcash-auto.ts";

let failures = 0;
const check = (c: boolean, m: string) => { if (!c) { console.log(`  FAIL: ${m}`); failures++; } };
const group = (n: string) => console.log(`== ${n}`);
const flush = async () => { for (let i = 0; i < 10; i++) await new Promise((r) => setTimeout(r, 0)); };

function memStore(init: Record<string, string> = {}) {
  const m = new Map(Object.entries(init));
  return { getItem: (k: string) => m.get(k) ?? null, setItem: (k: string, v: string) => void m.set(k, v), m };
}

function fakeClock() {
  let pending: { fn: () => void; ms: number; id: number } | null = null;
  let id = 0;
  const clock: Clock = {
    set(fn, ms) { pending = { fn, ms, id: ++id }; return id; },
    clear(h) { if (pending && pending.id === h) pending = null; },
  };
  return {
    clock,
    get ms() { return pending?.ms ?? null; },
    async fire() { const p = pending; pending = null; p?.fn(); await flush(); },
  };
}

/** The backend: frames like airgap.rs's, a decoder that completes after `need` parts. */
function fakeBackend(view: Uint8Array, need = 3) {
  const calls: { cmd: string; args?: Record<string, unknown> }[] = [];
  let got = new Set<string>();
  const seqLenFor = (frag: number) => (frag >= view.length ? 1 : Math.ceil(view.length / frag));
  const invoke: Invoke = async <T,>(cmd: string, args?: Record<string, unknown>): Promise<T> => {
    calls.push(args ? { cmd, args } : { cmd });
    if (cmd === "qr_pczt_frames") {
      const frag = args!.maxFragment as number;
      const start = args!.start as number;
      const count = args!.count as number;
      const n = seqLenFor(frag);
      const frames = n === 1 ? ["UR:LEEK-PCZT/ABCD"]
        : Array.from({ length: count }, (_, i) => `UR:LEEK-PCZT/${((start + i) % 255) + 1}-${n}/AB`);
      return { seqLen: n, fragmentLen: Math.ceil(view.length / n), longest: 2 * frag + 51, frames } as T;
    }
    if (cmd === "qr_scan_reset") { got = new Set(); return undefined as T; }
    if (cmd === "qr_scan_feed") {
      const t = String(args!.text);
      if (!/^ur:leek-zsig\//i.test(t)) {
        const st = /^ur:/i.test(t) && !/^ur:leek-pczt/i.test(t) ? "rejected" : "ignored";
        return { state: st, received: got.size, total: got.size ? need : 0, error: st === "rejected" ? "a ur:other" : null, zsig: null } as T;
      }
      const before = got.size;
      got.add(t.toUpperCase());
      const p: ScanProgress = got.size >= need
        ? { state: "complete", received: need, total: need, error: null, zsig: [1, 2, 3] }
        : { state: got.size > before ? "accepted" : "redundant", received: got.size, total: need, error: null, zsig: null };
      return p as T;
    }
    throw new Error(`unexpected ${cmd}`);
  };
  return { invoke, calls };
}

const VIEW = new Uint8Array(1373).fill(7);

group("defaults 50 B / 500 ms, and remembered");
{
  const s = memStore();
  check(loadQrSettings(s).fragment === 50 && loadQrSettings(s).interval === 500, "defaults 50 B / 500 ms");
  check((QR_FRAGMENTS as readonly number[]).includes(50) && QR_FRAGMENTS[0] === 30 && (QR_INTERVALS as readonly number[]).includes(500), "offered lists hold the default and 30 B");
  for (const p of Object.values(QR_PRESETS)) {
    check((QR_FRAGMENTS as readonly number[]).includes(p.fragment) && (QR_INTERVALS as readonly number[]).includes(p.interval), `preset ${p.fragment}/${p.interval} is reachable by the buttons`);
  }
  saveQrSettings({ fragment: 120, interval: 250 }, s);
  check(loadQrSettings(s).fragment === 120 && loadQrSettings(s).interval === 250, "saved and read back");
  const bad = memStore({ [QR_SETTINGS_KEY]: JSON.stringify({ fragment: 999, interval: 7 }) });
  check(loadQrSettings(bad).fragment === 50 && loadQrSettings(bad).interval === 500, "unknown values fall back");
  check(loadQrSettings(memStore({ [QR_SETTINGS_KEY]: "{not json" })).fragment === 50, "garbage falls back");
}

group("the large-QR rules");
{
  check(qrVersionFor(25) === 1 && qrVersionFor(26) === 2, "v1 holds 25 alphanumeric characters at L");
  check(qrVersionFor(151) === 5, "a 50-byte fragment part (~151 chars) is version 5");
  check(qrVersionFor(395) === 10 && qrVersionFor(396) === 11, "v10 holds 395");
  let threw = false;
  try { qrVersionFor(5000); } catch { threw = true; }
  check(threw, "too long for any version");
  const m = moduleScale(45, 400);
  check(m.px === 8 && m.side === 360, `whole pixels per module (${m.px}, ${m.side})`);
  check(moduleScale(45, 10).px === 1, "never below one pixel");
  check(bigQrAvail(1000, 800) === 560 && bigQrAvail(400, 2000) === 384, "min(96 % width, 70 % height)");
}

group("showing the loop, the counter and the batches");
{
  const b = fakeBackend(VIEW);
  const c = fakeClock();
  const q = new QrSigner(b.invoke, c.clock, memStore());
  let changes = 0;
  q.onChange = () => { changes++; };
  await q.show(VIEW);
  let f = q.frame();
  check(q.phase === "showing" && f !== null, "showing");
  check(f?.part === "1-28" && f.shown === 1 && f.seqLen === 28 && f.version === 5, `first frame ${JSON.stringify(f)}`);
  check(c.ms === 500, "next frame after 500 ms");
  check(frameCaption(f!).includes("Part 1-28") && frameCaption(f!).includes("500 ms"), frameCaption(f!));
  const first = b.calls.find((x) => x.cmd === "qr_pczt_frames")!;
  check(first.args!.maxFragment === 50 && first.args!.start === 0 && first.args!.count === QR_BATCH, "asked for the first batch at 50 B");
  check((first.args!.view as number[]).length === VIEW.length, "the signer view itself goes to the encoder");
  await c.fire();
  check(q.frame()?.part === "2-28" && q.frame()?.shown === 2, "advanced to part 2");
  for (let i = 0; i < QR_BATCH - 1; i++) await c.fire();
  const fetches = b.calls.filter((x) => x.cmd === "qr_pczt_frames");
  check(fetches.length === 2 && fetches[1]!.args!.start === QR_BATCH, "the next batch carries on the fountain");
  check(q.frame()?.part === `${QR_BATCH + 1}-28`, `fountain part ${q.frame()?.part}`);
  check(changes > QR_BATCH, "the screen was told of each frame");
}

group("speed presets, and bigger / denser / slower / faster");
{
  const s = memStore();
  const b = fakeBackend(VIEW);
  const c = fakeClock();
  const q = new QrSigner(b.invoke, c.clock, s);
  await q.show(VIEW);
  check(q.preset() === "safe" && c.ms === 500, "starts on Safe, 500 ms");
  check(q.frame()?.part === "1-28", `50 B is 28 parts: ${q.frame()?.part}`);
  q.setPreset("fast");
  await flush();
  check(q.preset() === "fast" && q.settings.fragment === 50 && c.ms === 300, "Fast: 50 B at 300 ms");
  q.setPreset("balanced");
  await flush();
  check(q.preset() === "balanced" && c.ms === 400, "Balanced: 50 B at 400 ms");
  q.setPreset("safe");
  await flush();
  q.bigger();
  await flush();
  let last = b.calls.filter((x) => x.cmd === "qr_pczt_frames").at(-1)!;
  check(q.preset() === "custom" && last.args!.maxFragment === 40, "Bigger from Safe: 40 B, Custom");
  q.bigger();
  await flush();
  check(q.settings.fragment === 30 && !q.canBigger(), "then 30 B, the floor");
  const before = b.calls.filter((x) => x.cmd === "qr_pczt_frames").length;
  q.bigger();
  await flush();
  check(b.calls.filter((x) => x.cmd === "qr_pczt_frames").length === before, "bigger at the floor changes nothing");
  q.denser(); q.denser(); q.denser();
  await flush();
  check(q.settings.fragment === 90, "denser steps 30 -> 40 -> 50 -> 90");
  const at = q.frame()?.part;
  q.faster();
  await flush();
  check(q.settings.interval === 400 && c.ms === 400, "faster: 500 -> 400 ms, rescheduled");
  check(q.frame()?.part === at, "speed changes keep the frame");
  for (let i = 0; i < 9; i++) q.slower();
  check(q.settings.interval === 800 && !q.canSlower(), "slowest is 800 ms");
  for (let i = 0; i < 9; i++) q.faster();
  check(q.settings.interval === 200 && !q.canFaster(), "fastest is 200 ms");
  check(loadQrSettings(s).fragment === 90 && loadQrSettings(s).interval === 200, "remembered");
  q.setPreset("custom");
  check(q.settings.interval === 200, "Custom keeps the settings");
}

group("a still code for a short message does not animate");
{
  const b = fakeBackend(new Uint8Array(40));
  const c = fakeClock();
  const q = new QrSigner(b.invoke, c.clock, memStore());
  await q.show(new Uint8Array(40));
  await c.fire(); await c.fire();
  check(q.frame()?.part === "1-1" && q.frame()?.text === "UR:LEEK-PCZT/ABCD", "the same single part");
  check(b.calls.filter((x) => x.cmd === "qr_pczt_frames").length === 1, "no refetch for a still code");
}

group("scanning the board's reply");
{
  const b = fakeBackend(VIEW, 3);
  const c = fakeClock();
  const q = new QrSigner(b.invoke, c.clock, memStore());
  await q.show(VIEW);
  check((await q.feed("UR:LEEK-ZSIG/1-3/AA")) === null && q.phase === "showing", "nothing is read before the scan starts");
  await q.startScan();
  check(q.phase === "scanning" && b.calls.some((x) => x.cmd === "qr_scan_reset"), "the decoder starts clean");
  check(c.ms === null, "the loop stopped while scanning");
  check(q.frame() === null, "no frame while scanning");
  check(scanCaption(q.progress(), "").startsWith("Point the camera"), "waiting for a first part");
  check((await q.feed("zcash:t1notours")) === null && q.progress().total === 0, "another code is ignored");
  check((await q.feed("UR:LEEK-PCZT/1-28/AB")) === null, "our own code in a mirror is ignored");
  await q.feed("UR:CRYPTO-PSBT/AA");
  check(q.scanNote.includes("ur:other") && q.phase === "scanning", "a foreign UR is noted, scanning goes on");
  /* Fed concurrently, as the camera does: answered in order. */
  const r = await Promise.all([q.feed("UR:LEEK-ZSIG/1-3/AA"), q.feed("ur:leek-zsig/1-3/aa"), q.feed("UR:LEEK-ZSIG/2-3/AA")]);
  check(r.every((x) => x === null), "not complete yet");
  check(q.progress().received === 2 && q.progress().total === 3, `progress ${JSON.stringify(q.progress())}`);
  check(scanCaption(q.progress(), "").includes("2 of 3"), scanCaption(q.progress(), ""));
  const z = await q.feed("UR:LEEK-ZSIG/7-3/AA");
  check(z !== null && Array.from(z) .join() === "1,2,3", "the .zsig is handed over once complete");
  check(q.phase === "finishing", "then it is the wallet's to finish");
  check((await q.feed("UR:LEEK-ZSIG/3-3/AA")) === null, "nothing more is read after completion");
  q.finished();
  check(q.phase === "done", "done");
}

group("the bench stopwatch: board read, reply read, total, and a speed change restarts it");
{
  const b = fakeBackend(VIEW, 2);
  const c = fakeClock();
  let t = 1000;
  c.clock.now = () => t;
  const q = new QrSigner(b.invoke, c.clock, memStore());
  await q.show(VIEW);
  t += 5000;
  check(q.timing().showMs === 5000 && q.timing().readMs === null, `live board read ${JSON.stringify(q.timing())}`);
  q.slower();
  await flush();
  t += 42_000;
  check(q.timing().showMs === 42_000 && q.timing().interval === 800, "a speed change times again from then, with its settings");
  await q.startScan();
  t += 3000;
  await q.feed("zcash:t1notours");
  await q.feed("UR:LEEK-ZSIG/1-2/AA");
  t += 1500;
  check((await q.feed("UR:LEEK-ZSIG/2-2/AA")) !== null, "complete");
  t += 99_000;
  const m = q.timing();
  check(m.showMs === 42_000 && m.readMs === 4500 && m.qrReads === 3, `stopped at completion ${JSON.stringify(m)}`);
  const line = timingCaption(m);
  check(line.includes("50 B / 800 ms") && line.includes("board read 42.0 s") && line.includes("reply read 4.5 s (3 QR)")
    && line.includes("total 46.5 s"), line);
}

group("manual: one part per Next, and Slower/Faster go back to automatic");
{
  const b = fakeBackend(VIEW);
  const c = fakeClock();
  const q = new QrSigner(b.invoke, c.clock, memStore());
  await q.show(VIEW);
  q.setPreset("manual");
  await flush();
  check(q.preset() === "manual" && c.ms === null, "no timer in manual mode");
  const p0 = q.frame()!.shown;
  q.next(); await flush();
  q.next(); await flush();
  check(q.frame()!.shown === p0 + 2 && c.ms === null, `two Nexts, two parts (${q.frame()!.shown - p0})`);
  check(frameCaption(q.frame()!).includes("manual"), frameCaption(q.frame()!));
  check(timingCaption(q.timing()).includes("50 B / manual"), timingCaption(q.timing()));
  q.slower(); await flush();
  check(q.preset() !== "manual" && c.ms === 800, `Slower is automatic again (${c.ms})`);
}

group("the bench sweep: each setting timed to the board's review, then the next");
{
  const b = fakeBackend(VIEW);
  const c = fakeClock();
  let t = 0;
  c.clock.now = () => t;
  const q = new QrSigner(b.invoke, c.clock, memStore());
  await q.show(VIEW);
  q.startBench();
  await flush();
  check(q.settings.fragment === 30 && q.settings.interval === 500 && q.frame()?.shown === 1, "starts at the first setting, part 1");
  t += 12_000;
  for (let i = 0; i < 5; i++) await c.fire();
  check(q.benchNext() && q.settings.fragment === 40, "the board had it: next setting");
  await flush();
  check(q.frame()?.shown === 1, "the next setting starts its loop again");
  t += 3000;
  q.benchNext(true);
  await flush();
  const r = q.bench!.rows;
  check(r.length === 2 && r[0]!.fragment === 30 && r[0]!.ms === 12_000 && r[0]!.frames === 6 && r[1]!.ms === null, JSON.stringify(r));
  /* Same fragment, new pace (50/500 -> 50/400): the loop restarts too. */
  for (let i = 0; i < 3; i++) await c.fire();   /* on 50/500 */
  q.benchNext(); await flush();                  /* to 50/400 */
  check(q.settings.fragment === 50 && q.settings.interval === 400 && q.frame()?.shown === 1,
    `a pace change restarts at part 1 (${q.settings.fragment}/${q.settings.interval}, frame ${q.frame()?.shown})`);
  while (q.benchNext()) await flush();
  check(q.bench!.rows.length === QR_BENCH_CONFIGS.length, "one row per setting");
  const md = benchTable(q.bench!.rows);
  check(md.includes("| 30 | 500 |") && md.includes("| 12.0 |") && md.includes("gave up"), md);
}

group("show again, cancel, failure");
{
  const b = fakeBackend(VIEW);
  const c = fakeClock();
  const q = new QrSigner(b.invoke, c.clock, memStore());
  await q.show(VIEW);
  await c.fire();
  await q.startScan();
  await q.showAgain();
  check(q.phase === "showing" && q.frame()?.part === "1-28", "back to the loop from part 1");
  q.cancel();
  check(q.phase === "cancelled" && c.ms === null && q.frame() === null, "cancel stops the loop");
  await c.fire();
  check(q.frame() === null, "a stale tick draws nothing");

  const broken: Invoke = async <T,>(): Promise<T> => { throw new Error("the backend refused the view"); };
  const q2 = new QrSigner(broken, fakeClock().clock, memStore());
  await q2.show(VIEW);
  check(q2.phase === "failed" && q2.error === "the backend refused the view", "a backend error is shown verbatim");
}

group("full screen: the smaller side, less the captions, whole modules");
{
  check(fullQrAvail(1920, 1080, 60) === 1020, "landscape: the height less the captions");
  check(fullQrAvail(1080, 2400, 60) === 1080, "portrait: the width");
  check(fullQrAvail(800.7, 900, 0) === 800, "whole pixels");
  check(fullQrAvail(10, 10, 50) === 1, "never below one pixel");
  /* v6 (41 modules + 8 quiet) in 1020 px: 20 px a module, 980 px drawn. */
  const m = moduleScale(41 + 8, fullQrAvail(1920, 1080, 60));
  check(m.px === 20 && m.side === 980 && m.side <= 1020, JSON.stringify(m));
}

group("import from the board's QR: the account record, then zw_import");
{
  const REC: QrAccountRecord = {
    network: "test", seedFingerprint: "551a1fd4" + "00".repeat(28), zfp: "551A1FD4",
    accounts: [{ ufvk: "uviewtest1example", account: 0, ua: "utest1exampleaddressxxxxxxxxxxxx", name: null, birthday: null }],
  };
  const fake = (script: AccountsProgress[], tip = 3_000_000) => {
    const calls: { cmd: string; args?: Record<string, unknown> }[] = [];
    let i = 0;
    const invoke: Invoke = async <T,>(cmd: string, args?: Record<string, unknown>): Promise<T> => {
      calls.push(args ? { cmd, args } : { cmd });
      if (cmd === "qr_accounts_reset") return undefined as T;
      if (cmd === "qr_accounts_feed") return script[Math.min(i++, script.length - 1)] as T;
      if (cmd === "zw_tip") return { height: tip } as T;
      if (cmd === "zw_import") return { alreadyImported: false, birthday: args!.birthday } as T;
      throw new Error(`unexpected ${cmd}`);
    };
    return { invoke, calls };
  };
  const p = (state: AccountsProgress["state"], received: number, total: number, extra: Partial<AccountsProgress> = {}): AccountsProgress =>
    ({ state, received, total, error: null, record: null, ...extra });

  const b = fake([p("ignored", 0, 0), p("accepted", 1, 3), p("rejected", 1, 3, { error: "a ur:crypto-psbt" }), p("accepted", 2, 3), p("complete", 0, 0, { record: REC })]);
  const q = new QrAccountImport(b.invoke);
  await q.start("test");
  check(q.phase === "scanning" && b.calls[0]!.cmd === "qr_accounts_reset", "a scan starts on a fresh decoder");
  check(accountsCaption(q).includes("Main menu → View key"), accountsCaption(q));
  check(await q.feed("zcash:t1x") === false && q.total === 0, "a stray code changes nothing");
  await q.feed("UR:LEEK-ZCASH-ACCOUNTS/1-3/X");
  check(accountsCaption(q) === "1 of 3 parts read…", accountsCaption(q));
  await q.feed("UR:CRYPTO-PSBT/X");
  check(accountsCaption(q).includes("ignored one code: a ur:crypto-psbt"), accountsCaption(q));
  await q.feed("UR:LEEK-ZCASH-ACCOUNTS/2-3/X");
  check(accountsCaption(q) === "2 of 3 parts read…", "a good part clears the note");
  check(await q.feed("UR:LEEK-ZCASH-ACCOUNTS/3-3/X") === true && q.phase === "found", "complete: found, the camera stops");
  check(b.calls.filter((c) => c.cmd === "qr_accounts_feed").every((c) => c.args!.network === "test"), "the chosen network goes with every code");
  const rows = recordRows(q.record!, (n) => (n === "test" ? "Testnet" : "Mainnet"));
  check(JSON.stringify(rows.slice(0, 3)) === JSON.stringify([["Network", "Testnet"], ["Account", "0"], ["Seed fingerprint", "ZFP 551A1FD4"]]), JSON.stringify(rows));
  check(await q.feed("UR:LEEK-ZCASH-ACCOUNTS/1-3/X") === false, "nothing more is fed once found");

  /* No birthday from the board: the tip less the margin, as the board set-up. */
  const r = await q.importFound("");
  const imp = b.calls.find((c) => c.cmd === "zw_import")!;
  check(JSON.stringify(imp.args) === JSON.stringify({
    network: "test", ufvk: "uviewtest1example", seedFingerprint: REC.seedFingerprint, account: 0, ua: REC.accounts[0]!.ua, birthday: 3_000_000 - BIRTHDAY_MARGIN,
  }), `zw_import as a board account: ${JSON.stringify(imp.args)}`);
  check(!b.calls.some((c) => c.cmd === "zw_import_watch"), "never the watch-only import");
  check(q.phase === "done" && r.key.wallet === REC.seedFingerprint && r.key.account === 0 && r.key.network === "test", "the board wallet's key to open");

  check(await resolveBirthday(" 3100000 ", null, async () => { throw new Error("no tip"); }) === 3_100_000, "a typed birthday, no server asked");
  check(await resolveBirthday("", 2_900_000, async () => ({ height: 1 })) === 2_900_000, "the record's birthday when it has one");
  let bad = "";
  try { await resolveBirthday("12a", null, async () => ({ height: 1 })); } catch (e) { bad = (e as Error).message; }
  check(bad.includes("block height"), "a bad birthday is refused");

  const w = fake([p("accepted", 1, 2), p("refused", 0, 0, { error: "this is a Mainnet viewing key, and the Testnet wallet is chosen here" })]);
  const q2 = new QrAccountImport(w.invoke);
  await q2.start("test");
  await q2.feed("a");
  check(await q2.feed("b") === true && q2.phase === "failed" && q2.error!.includes("Mainnet viewing key"), "another network's key: refused, the camera stops");
  let threw = false;
  try { await q2.importFound(""); } catch { threw = true; }
  check(threw && !w.calls.some((c) => c.cmd === "zw_import"), "nothing to import after a refusal");

  const c = fake([p("accepted", 1, 2)]);
  const q3 = new QrAccountImport(c.invoke);
  await q3.start("main");
  const pending = q3.feed("a");
  q3.cancel();
  check(await pending === false && q3.phase === "idle", "a cancelled scan drops the answer in flight");
}

group("the policy text: experimental, and never for pairing");
{
  const { readFileSync } = await import("node:fs");
  const { join } = await import("node:path");
  const root = join(import.meta.dirname, "..");
  const scan = readFileSync(join(root, "src/qr-scan.ts"), "utf8");
  check(/experimental/i.test(scan) && /pairing/i.test(scan), "qr-scan.ts says QR signing is experimental and not for pairing");
  const html = readFileSync(join(root, "index.html"), "utf8");
  check(html.includes("Sign with QR (experimental)"), "the button says experimental");
  check(html.includes("Import from the board's QR (experimental)"), "the import button says experimental");
  const imp = html.slice(html.indexOf("Import from the board's QR"), html.indexOf("Import a viewing key (watch-only)")).replace(/\s+/g, " ");
  check(/Main menu → View key/.test(imp) && /never spend/.test(imp) && /does not pair/.test(imp) && /6-digit pairing code/.test(imp),
    "the import says where on the board, that it never spends, and that it is not pairing");
  const acc = readFileSync(join(root, "src/qr-accounts.ts"), "utf8");
  check(/EXPERIMENTAL/.test(acc) && /never pairing/i.test(acc) && /6-digit pairing code/.test(acc), "qr-accounts.ts says one-way, never pairing");
}

console.log(failures ? `\nFAILED (${failures})` : "\nPASSED (0 failures)");
process.exit(failures ? 1 : 0);
