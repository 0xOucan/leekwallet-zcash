/**
 * zdev (firmware/sim) for the companion's tests: the firmware's own
 * protocol, session, both Zcash signers and the review pages on a
 * pseudo-terminal, holding the public abandon-art test seed.
 *
 * The link below is the desktop USB path minus the Rust process in between:
 * the same "LK" frames transport-serial/src/wire.rs writes, and above them
 * the companion's own framing, CBOR and session (packages/core). The client
 * runs the handshake the way main.ts's Client does: hello, helloReveal,
 * commitment check, then encrypted calls once the "board" has approved.
 *
 * Development only: zdev never holds a seed with real funds.
 */

import { spawn, spawnSync, type ChildProcess } from "node:child_process";
import { closeSync, constants, existsSync, mkdtempSync, openSync, rmSync, writeSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join } from "node:path";
import tty from "node:tty";
import { fileURLToPath } from "node:url";

import { decodeCbor, encodeCbor, type CborValue } from "../packages/core/src/cbor.ts";
import { encodeFrame, FrameDecoder, FrameType } from "../packages/core/src/framing.ts";
import {
  COMMIT_BYTES, deriveSession, generateKeypair, generateNonce, NONCE_BYTES,
  PROTOCOL_VERSION, Session, verifyCommitment,
} from "../packages/core/src/session.ts";
import { DeviceError } from "../packages/core/src/transport.ts";

const here = dirname(fileURLToPath(import.meta.url));
export const repo = join(here, "..", "..");
const sim = join(repo, "firmware/sim");

/** make -C firmware/sim build/zdev (incremental); null if it cannot be built. */
export function zdevBinary(): string | null {
  const r = spawnSync("make", ["-C", sim, "build/zdev"], {
    stdio: process.env["VERBOSE"] ? "inherit" : "pipe",
    env: { CARGO_BUILD_JOBS: "4", ...process.env },
  });
  if (r.status !== 0) {
    console.error(`zdev did not build:\n${r.stderr?.toString().slice(-2000)}`);
    return null;
  }
  return join(sim, "build/zdev");
}

export interface Zdev {
  child: ChildProcess;
  link: string;
  out: string;
  passkeys: string[];
  answer(a: string): void;
  stop(): void;
}

export async function startZdev(bin: string, { yes = true, args = [] as string[] } = {}): Promise<Zdev> {
  const dir = mkdtempSync(join(tmpdir(), "leek-zdev-"));
  const link = join(dir, "port");
  const child = spawn(bin, ["--link", link, ...args, ...(yes ? ["--yes"] : [])], { stdio: ["pipe", "pipe", "pipe"] });
  const z: Zdev = {
    child, link, out: "", passkeys: [],
    answer: (a) => { child.stdin?.write(`${a}\n`); },
    stop: () => { child.kill(); rmSync(dir, { recursive: true, force: true }); },
  };
  child.stdout?.on("data", (d: Buffer) => {
    const s = d.toString();
    z.out += s;
    if (process.env["VERBOSE"]) process.stdout.write(`  [zdev] ${s}`);
    for (const m of s.matchAll(/Passkey on this device: (\d{6})/g)) z.passkeys.push(m[1]!);
  });
  child.stderr?.on("data", (d: Buffer) => { z.out += d.toString(); });
  const t0 = Date.now();
  while (!existsSync(link)) {
    if (Date.now() - t0 > 10_000) throw new Error(`zdev did not start: ${z.out}`);
    await new Promise((r) => setTimeout(r, 50));
  }
  return z;
}

const SYNC = [0x4c, 0x4b]; // "LK", wire.rs SYNC

/** The PTY as the board's USB port: LK frames out, LK frames found in console text. */
export class PtyLink {
  private readonly fd: number;
  private readonly stream: tty.ReadStream;
  private buf = new Uint8Array(0);
  private waiting: ((f: { type: number; payload: Uint8Array }) => void) | null = null;
  private queue: { type: number; payload: Uint8Array }[] = [];

  constructor(path: string) {
    this.fd = openSync(path, constants.O_RDWR | constants.O_NOCTTY);
    this.stream = new tty.ReadStream(this.fd);
    this.stream.on("data", (b: Buffer) => this.feed(new Uint8Array(b)));
  }

  private feed(bytes: Uint8Array): void {
    const merged = new Uint8Array(this.buf.length + bytes.length);
    merged.set(this.buf);
    merged.set(bytes, this.buf.length);
    this.buf = merged;
    for (;;) {
      let at = -1;
      for (let i = 0; i + 1 < this.buf.length; i++) {
        if (this.buf[i] === SYNC[0] && this.buf[i + 1] === SYNC[1]) { at = i; break; }
      }
      if (at < 0 || this.buf.length < at + 5) break;
      /* The companion's own decoder from here on: len, type, payload. */
      const frames = new FrameDecoder().push(this.buf.slice(at + 2));
      if (frames.length === 0) break;
      const f = frames[0]!;
      this.buf = this.buf.slice(at + 2 + 3 + f.payload.length);
      if (this.waiting) { const w = this.waiting; this.waiting = null; w(f); } else this.queue.push(f);
    }
  }

  send(type: number, payload: Uint8Array): void {
    const frame = encodeFrame(type as FrameType, payload);
    writeSync(this.fd, Uint8Array.from([...SYNC, ...frame]));
  }

  recv(ms: number): Promise<{ type: number; payload: Uint8Array }> {
    const q = this.queue.shift();
    if (q) return Promise.resolve(q);
    return new Promise((resolve, reject) => {
      const t = setTimeout(() => { this.waiting = null; reject(new Error("zdev did not answer")); }, ms);
      this.waiting = (f) => { clearTimeout(t); resolve(f); };
    });
  }

  close(): void {
    this.stream.destroy();
    try { closeSync(this.fd); } catch { /* already closed */ }
  }
}

/** The companion's client, as main.ts runs it, over the PTY. */
export class ZdevClient {
  private readonly link: PtyLink;
  private session: Session | null = null;

  constructor(path: string) {
    this.link = new PtyLink(path);
  }

  close(): void { this.link.close(); }

  /** One request and its reply; encrypted once the session is up. */
  readonly call = async (
    method: string, params: Record<string, CborValue> = {}, timeoutMs = 5000,
  ): Promise<Record<string, CborValue>> => {
    const body = encodeCbor({ method, ...params });
    const encrypted = this.session?.isActive ?? false;
    this.link.send(encrypted ? FrameType.EncryptedRequest : FrameType.Request,
      encrypted ? this.session!.encrypt(body) : body);
    const f = await this.link.recv(timeoutMs);
    let payload = f.payload;
    if (f.type === FrameType.EncryptedResponse || f.type === FrameType.EncryptedError) {
      payload = this.session!.decrypt(payload);
    }
    const v = decodeCbor(payload) as Record<string, CborValue>;
    if (f.type === FrameType.Error || f.type === FrameType.EncryptedError) {
      throw new DeviceError(Number(v["code"]), String(v["message"]));
    }
    return v["result"] as Record<string, CborValue>;
  };

  /** Handshake, commitment check, then wait for ALLOW on the "board". */
  async pair(timeoutMs = 20_000): Promise<string> {
    const { privateKey, publicKey } = generateKeypair();
    const ack = await this.call("hello", { version: PROTOCOL_VERSION, hostPubkey: publicKey });
    if (ack["version"] !== PROTOCOL_VERSION) throw new Error("protocol version mismatch");
    const devicePubkey = ack["devicePubkey"] as Uint8Array;
    const deviceCommit = ack["deviceCommit"] as Uint8Array;
    if (devicePubkey?.length !== 32 || deviceCommit?.length !== COMMIT_BYTES) throw new Error("bad helloAck");
    const hostNonce = generateNonce();
    const r = await this.call("helloReveal", { hostNonce });
    const deviceNonce = r["deviceNonce"] as Uint8Array;
    if (deviceNonce?.length !== NONCE_BYTES) throw new Error("no device nonce");
    if (!verifyCommitment(deviceCommit, devicePubkey, publicKey, deviceNonce)) {
      throw new Error("the commitment does not open");
    }
    this.session = new Session(deriveSession(privateKey, devicePubkey, {
      hostPublic: publicKey, devicePublic: devicePubkey, hostNonce, deviceNonce,
    }), "host");
    this.session.confirm();
    const deadline = Date.now() + timeoutMs;
    for (;;) {
      try {
        await this.call("getStatus");
        return this.session.passkey;
      } catch (e) {
        if (Date.now() > deadline) throw e;
        await new Promise((res) => setTimeout(res, 300));
      }
    }
  }
}
