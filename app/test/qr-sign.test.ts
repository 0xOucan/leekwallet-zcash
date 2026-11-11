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
  bigQrAvail, frameCaption, loadQrSettings, moduleScale, qrVersionFor, saveQrSettings, scanCaption, timingCaption,
  QrSigner, QR_BATCH, QR_FRAGMENTS, QR_PRESETS, QR_INTERVALS, QR_SETTINGS_KEY, type Clock, type ScanProgress,
} from "../src/qr-sign.ts";
import type { Invoke } from "../src/tauri-transport.ts";

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

group("defaults 90 B / 500 ms, and remembered");
{
  const s = memStore();
  check(loadQrSettings(s).fragment === 90 && loadQrSettings(s).interval === 500, "defaults 90 B / 500 ms");
  check((QR_FRAGMENTS as readonly number[]).includes(90) && QR_FRAGMENTS[0] === 50 && (QR_INTERVALS as readonly number[]).includes(500), "offered lists hold the defaults and 50 B");
  for (const p of Object.values(QR_PRESETS)) {
    check((QR_FRAGMENTS as readonly number[]).includes(p.fragment) && (QR_INTERVALS as readonly number[]).includes(p.interval), `preset ${p.fragment}/${p.interval} is reachable by the buttons`);
  }
  saveQrSettings({ fragment: 120, interval: 250 }, s);
  check(loadQrSettings(s).fragment === 120 && loadQrSettings(s).interval === 250, "saved and read back");
  const bad = memStore({ [QR_SETTINGS_KEY]: JSON.stringify({ fragment: 999, interval: 7 }) });
  check(loadQrSettings(bad).fragment === 90 && loadQrSettings(bad).interval === 500, "unknown values fall back");
  check(loadQrSettings(memStore({ [QR_SETTINGS_KEY]: "{not json" })).fragment === 90, "garbage falls back");
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
  check(f?.part === "1-16" && f.shown === 1 && f.seqLen === 16 && f.version === 8, `first frame ${JSON.stringify(f)}`);
  check(c.ms === 500, "next frame after 500 ms");
  check(frameCaption(f!).includes("Part 1-16") && frameCaption(f!).includes("500 ms"), frameCaption(f!));
  const first = b.calls.find((x) => x.cmd === "qr_pczt_frames")!;
  check(first.args!.maxFragment === 90 && first.args!.start === 0 && first.args!.count === QR_BATCH, "asked for the first batch at 50 B");
  check((first.args!.view as number[]).length === VIEW.length, "the signer view itself goes to the encoder");
  await c.fire();
  check(q.frame()?.part === "2-16" && q.frame()?.shown === 2, "advanced to part 2");
  for (let i = 0; i < QR_BATCH - 1; i++) await c.fire();
  const fetches = b.calls.filter((x) => x.cmd === "qr_pczt_frames");
  check(fetches.length === 2 && fetches[1]!.args!.start === QR_BATCH, "the next batch carries on the fountain");
  check(q.frame()?.part === `${QR_BATCH + 1}-16`, `fountain part ${q.frame()?.part}`);
  check(changes > QR_BATCH, "the screen was told of each frame");
}

group("speed presets, and bigger / denser / slower / faster");
{
  const s = memStore();
  const b = fakeBackend(VIEW);
  const c = fakeClock();
  const q = new QrSigner(b.invoke, c.clock, s);
  await q.show(VIEW);
  check(q.preset() === "safe", "starts on Safe");
  q.setPreset("fast");
  await flush();
  let last = b.calls.filter((x) => x.cmd === "qr_pczt_frames").at(-1)!;
  check(q.preset() === "fast" && last.args!.maxFragment === 150 && c.ms === 250, "Fast: 150 B at 250 ms, a new encoder");
  check(q.frame()?.part === "1-10" && q.frame()?.shown === 1, `restarted at part 1: ${q.frame()?.part}`);
  q.setPreset("balanced");
  await flush();
  check(q.preset() === "balanced" && q.settings.fragment === 120 && c.ms === 400, "Balanced: 120 B at 400 ms");
  q.setPreset("safe");
  await flush();
  q.bigger();
  await flush();
  last = b.calls.filter((x) => x.cmd === "qr_pczt_frames").at(-1)!;
  check(q.preset() === "custom" && last.args!.maxFragment === 50 && !q.canBigger(), "Bigger from Safe: 50 B, Custom");
  check(q.frame()?.part === "1-28", `50 B is 28 parts: ${q.frame()?.part}`);
  const before = b.calls.filter((x) => x.cmd === "qr_pczt_frames").length;
  q.bigger();
  await flush();
  check(b.calls.filter((x) => x.cmd === "qr_pczt_frames").length === before, "bigger at the floor changes nothing");
  q.denser(); q.denser();
  await flush();
  check(q.settings.fragment === 120, "denser steps 50 -> 90 -> 120");
  const at = q.frame()?.part;
  q.faster();
  await flush();
  check(q.settings.interval === 400 && c.ms === 400, "faster: 500 -> 400 ms, rescheduled");
  check(q.frame()?.part === at, "speed changes keep the frame");
  check(q.preset() === "balanced", "120/400 is Balanced again");
  for (let i = 0; i < 9; i++) q.slower();
  check(q.settings.interval === 800 && !q.canSlower(), "slowest is 800 ms");
  for (let i = 0; i < 9; i++) q.faster();
  check(q.settings.interval === 200 && !q.canFaster(), "fastest is 200 ms");
  check(loadQrSettings(s).fragment === 120 && loadQrSettings(s).interval === 200, "remembered");
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
  check(line.includes("90 B / 800 ms") && line.includes("board read 42.0 s") && line.includes("reply read 4.5 s (3 QR)")
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
  check(timingCaption(q.timing()).includes("90 B / manual"), timingCaption(q.timing()));
  q.slower(); await flush();
  check(q.preset() !== "manual" && c.ms === 800, `Slower is automatic again (${c.ms})`);
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
  check(q.phase === "showing" && q.frame()?.part === "1-16", "back to the loop from part 1");
  q.cancel();
  check(q.phase === "cancelled" && c.ms === null && q.frame() === null, "cancel stops the loop");
  await c.fire();
  check(q.frame() === null, "a stale tick draws nothing");

  const broken: Invoke = async <T,>(): Promise<T> => { throw new Error("the backend refused the view"); };
  const q2 = new QrSigner(broken, fakeClock().clock, memStore());
  await q2.show(VIEW);
  check(q2.phase === "failed" && q2.error === "the backend refused the view", "a backend error is shown verbatim");
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
}

console.log(failures ? `\nFAILED (${failures})` : "\nPASSED (0 failures)");
process.exit(failures ? 1 : 0);
