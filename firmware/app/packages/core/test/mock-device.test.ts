/**
 * Mock device tests.
 *
 * These check the mock behaves like the protocol says the firmware must, so UI
 * built against it does not acquire habits real hardware will break.
 */

import { encodeCbor, decodeCbor, type CborValue } from "../src/cbor.ts";
import { encodeFrame, FrameDecoder, FrameType } from "../src/framing.ts";
import { MockDevice } from "../src/mock-device.ts";
import { ErrorCode } from "../src/transport.ts";
import { PROTOCOL_VERSION } from "../src/session.ts";

/* The session- and unlock-gated request these tests use: selectWallet is in
 * the keys tier (refused without a session) and needs an unlocked device,
 * like every z* method the Zcash firmware adds. */
const GATED = { index: 1 } as Record<string, CborValue>;

let failures = 0;
const check = (c: boolean, m: string) => { if (!c) { console.log(`  FAIL: ${m}`); failures++; } };
const group = (n: string) => console.log(`== ${n}`);

type Reply = { result?: Record<string, CborValue>; error?: { code: number; message: string } };

/** Send one request, await one reply. */
async function call(
  dev: MockDevice,
  method: string,
  params: Record<string, CborValue> = {},
): Promise<Reply> {
  return new Promise((resolve, reject) => {
    const decoder = new FrameDecoder();
    dev.onFrame((frame) => {
      const frames = decoder.push(frame);
      const f = frames[0];
      if (!f) return;
      const body = decodeCbor(f.payload) as Record<string, CborValue>;
      /* Both error frame types, or an encrypted error reads as an empty
       * success - which is the exact client bug the firmware's send_error
       * comment describes, and which this helper had. */
      if (f.type === FrameType.Error || f.type === FrameType.EncryptedError) {
        resolve({ error: { code: Number(body["code"]), message: String(body["message"]) } });
      } else {
        resolve({ result: body["result"] as Record<string, CborValue> });
      }
    });
    dev.send(encodeFrame(FrameType.Request, encodeCbor({ method, ...params }))).catch(reject);
  });
}

/** The frame TYPE of the reply, for tests that care how an error is carried. */
async function rawFrame(dev: MockDevice, method: string): Promise<number> {
  return new Promise((resolve, reject) => {
    const decoder = new FrameDecoder();
    dev.onFrame((frame) => {
      const f = decoder.push(frame)[0];
      if (f) resolve(f.type);
    });
    dev.send(encodeFrame(FrameType.Request, encodeCbor({ method }))).catch(reject);
  });
}

/* Both handshake legs, in order. There is no shortcut on the mock any more
 * than on the device: `hello` only commits, and nothing is established until
 * `helloReveal`. */
async function pair(dev: MockDevice): Promise<Reply> {
  await call(dev, "hello", { version: PROTOCOL_VERSION });
  return call(dev, "helloReveal");
}

async function connected(opts = {}): Promise<MockDevice> {
  const dev = new MockDevice(opts);
  await dev.open();
  await pair(dev);
  return dev;
}

async function main(): Promise<void> {
  group("session must be established first");
  {
    const dev = new MockDevice();
    await dev.open();

    /* getStatus is in the "always" tier - PROTOCOL.md section 5 tells the app
     * to poll it, and the firmware answers it in plaintext with no session. It
     * is the KEY operations that must be refused, so assert on one of those. */
    const early = await call(dev, "selectWallet", GATED);
    check(early.error?.code === ErrorCode.SessionRequired,
      `expected SessionRequired, got ${JSON.stringify(early)}`);

    /* One leg is not a session. A reveal is what derives, and a mock that
     * established on `hello` alone would let host code skip the commitment
     * check that makes the passkey worth comparing. */
    const half = await call(dev, "hello", { version: PROTOCOL_VERSION });
    check(half.result?.["version"] === PROTOCOL_VERSION, "hello should name the version");
    check(dev.session === "awaitingReveal",
      `after hello the mock is ${dev.session}, not awaitingReveal`);
    const stillEarly = await call(dev, "selectWallet", GATED);
    check(stillEarly.error?.code === ErrorCode.SessionRequired,
      "a committed-but-unrevealed handshake served a key operation");

    const hello = await call(dev, "helloReveal");
    check(hello.result?.["passkey"] === "314159", "the reveal should return a passkey to compare");

    const ok = await call(dev, "getStatus");
    check(ok.result !== undefined, "getStatus should work after the handshake");
  }

  group("a version the device does not speak is named as such");
  {
    const dev = new MockDevice();
    await dev.open();

    const old = await call(dev, "hello");
    check(old.error?.code === ErrorCode.UnsupportedVersion,
      `a v1 host (no version field) got ${JSON.stringify(old)}`);
    const future = await call(dev, "hello", { version: 99 });
    check(future.error?.code === ErrorCode.UnsupportedVersion,
      `a host from the future got ${JSON.stringify(future)}`);
    check(dev.session === "none", "a refused hello still moved the session");

    /* And a reveal with nothing behind it is a session error, not a pairing. */
    const stray = await call(dev, "helloReveal");
    check(stray.error?.code === ErrorCode.SessionRequired,
      `a stray helloReveal got ${JSON.stringify(stray)}`);
  }

  group("a session is pending until the passkey is compared");
  {
    /* The mock used to mark the session established the moment `hello` was
     * answered, so the passkey comparison - the entire defence against a
     * machine in the middle - could be skipped and app code still passed.
     * The firmware goes to PENDING and refuses everything until the user
     * confirms. Found by running the real protocol.c on the host. */
    const dev = new MockDevice({ autoConfirmSession: false, startUnlocked: true });
    await dev.open();

    const hello = await pair(dev);
    check(hello.result?.["passkey"] === "314159", "the reveal should offer a passkey");
    check(dev.session === "pending", `session went to ${dev.session}, not pending`);
    check(dev.confirmations.some((c) => c.includes("passkey")),
      "the passkey comparison was never shown");

    const early = await call(dev, "selectWallet", GATED);
    check(early.error?.code === ErrorCode.SessionRequired,
      `pending session served a key operation: ${JSON.stringify(early)}`);

    dev.confirmSession();
    const ok = await call(dev, "selectWallet", GATED);
    check(ok.result?.["activeWallet"] === 1, "confirming the passkey did not open the session");
  }

  group("an in-session error is encrypted, not plaintext");
  {
    /* Counters, not secrecy. The device advances its receive counter whenever
     * a frame decrypts, error or not; the host advances on opening a reply. A
     * plaintext error leaves them one apart and every later frame fails to
     * decrypt. The mock answered in plaintext, so a client that mishandled the
     * encrypted form passed here and desynced against real hardware. */
    const dev = await connected({ startUnlocked: true });
    const raw = await rawFrame(dev, "definitelyNotAMethod");
    check(raw === FrameType.EncryptedError,
      `error came back as frame type 0x${raw.toString(16)}, expected 0x7e`);

    /* And the channel still works afterwards. */
    const after = await call(dev, "getStatus");
    check(after.result !== undefined, "the session did not survive an error");
  }

  group("locked device refuses key operations");
  {
    const dev = await connected();
    const r = await call(dev, "selectWallet", GATED);
    check(r.error?.code === ErrorCode.NotUnlocked,
      `locked selectWallet should fail, got ${JSON.stringify(r)}`);

    await call(dev, "unlock");
    dev.enterPin();
    const ok = await call(dev, "selectWallet", GATED);
    check(ok.result?.["activeWallet"] === 1, "unlocked selectWallet should answer");
  }

  group("the PIN never crosses the wire");
  {
    const dev = await connected({ autoPin: false });
    const r = await call(dev, "unlock", { pin: "1234" } as Record<string, CborValue>);
    check(r.result?.["prompted"] === 1, "unlock should prompt");
    check(dev.confirmations.some((c) => c.includes("Enter PIN on device")),
      "unlock must prompt on the device rather than accept a PIN parameter");
  }

  group("unlock only prompts; the host learns the outcome by polling");
  {
    /* The device calls ui_request_unlock() and answers {prompted:1,unlocked:0}
     * immediately - the user has not touched the keypad yet. The mock used to
     * unlock synchronously and answer {unlocked:1}, so anything built against
     * it believed unlocking was instantaneous and had no polling path at all. */
    const dev = await connected({ autoPin: false });

    const r = await call(dev, "unlock");
    check(r.result?.["prompted"] === 1, `unlock did not report a prompt: ${JSON.stringify(r)}`);
    check(r.result?.["unlocked"] === 0,
      `unlock claimed success before the user typed anything: ${JSON.stringify(r)}`);

    // Still locked, and key operations still refused, while the pad is up.
    check((await call(dev, "getStatus")).result?.["unlocked"] === 0,
      "the device reported itself unlocked before the PIN was entered");
    const early = await call(dev, "selectWallet", GATED);
    check(early.error?.code === ErrorCode.NotUnlocked,
      `a prompting device served a key operation: ${JSON.stringify(early)}`);

    // The PIN is typed on the device. Nothing crosses the wire; the host only
    // finds out by asking again.
    dev.enterPin();
    check((await call(dev, "getStatus")).result?.["unlocked"] === 1,
      "the status poll never reported the unlock");

    // Already unlocked is the one case with an immediate answer, and no prompt.
    const again = await call(dev, "unlock");
    check(again.result?.["unlocked"] === 1 && again.result?.["prompted"] === undefined,
      `unlock on an unlocked device should answer {unlocked:1}: ${JSON.stringify(again)}`);

    // lock says what it did rather than answering an empty map.
    check((await call(dev, "lock")).result?.["unlocked"] === 0,
      "lock must report the new state");
  }

  group("a simulated user eventually types the PIN");
  {
    const dev = await connected({ autoPin: true, pinEntryMs: 20 });
    await call(dev, "unlock");
    check((await call(dev, "getStatus")).result?.["unlocked"] === 0,
      "autoPin unlocked the device synchronously, which is the bug being fixed");
    await new Promise((r) => setTimeout(r, 60));
    check((await call(dev, "getStatus")).result?.["unlocked"] === 1,
      "the simulated user never finished typing");
  }

  group("getFeatures invents nothing the device does not send");
  {
    const dev = await connected();
    const r = await call(dev, "getFeatures");
    check(r.result?.["blindSigning"] === 0, "the inherited blind-signing flag must read off");
    check(r.result?.["chain"] === "zcash", "getFeatures must name the Zcash firmware");
    check(typeof r.result?.["model"] === "string", "getFeatures names no model");
    /* `initialized` existed only here, so app code could branch on a field real
     * hardware never sends. protocol.c writes exactly four keys. */
    check(!("initialized" in (r.result ?? {})),
      `getFeatures invented a field: ${JSON.stringify(r.result)}`);
  }

  group("a passphrase answers with the seed fingerprint, confirmed on the device");
  {
    const dev = await connected({ startUnlocked: true, walletCount: 2 });
    const r = await call(dev, "setPassphrase", { passphrase: "x" });
    const fp = r.result?.["fingerprint"];
    check(typeof fp === "number" && Number.isInteger(fp) && fp >= 0 && fp <= 0xffffffff,
      `setPassphrase must answer a uint32 fingerprint: ${JSON.stringify(r)}`);
    check(r.result?.["passphrase"] === 1, "setPassphrase must report the passphrase on");
    check(dev.confirmations.some((c) => c.includes("passphrase")),
      "the passphrase was applied without a confirmation on the device");

    const refused = await connected({ startUnlocked: true, autoApprove: false });
    const no = await call(refused, "setPassphrase", { passphrase: "x" });
    check(no.error?.code === ErrorCode.UserRejected,
      `expected UserRejected, got ${JSON.stringify(no)}`);
    check((await call(refused, "getStatus")).result?.["passphrase"] === 0,
      "a refused passphrase was left applied");
  }

  group("the removed EVM commands are unknown, as on the Zcash firmware");
  {
    const dev = await connected({ startUnlocked: true });
    for (const m of ["getAddress", "signTransaction", "signMessage", "signTypedData"]) {
      const r = await call(dev, m, { index: 0 });
      check(r.error?.code === ErrorCode.MalformedFrame,
        `${m} should be an unknown method, got ${JSON.stringify(r)}`);
    }
  }

  group("switching wallets drops the passphrase");
  {
    const dev = await connected({ startUnlocked: true, walletCount: 2 });
    await call(dev, "setPassphrase", { passphrase: "x" });
    check((await call(dev, "getStatus")).result?.["passphrase"] === 1, "passphrase should be active");

    await call(dev, "selectWallet", { index: 2 });
    check((await call(dev, "getStatus")).result?.["passphrase"] === 0,
      "passphrase must not survive a wallet switch");
  }

  group("malformed input is rejected, not guessed at");
  {
    const dev = await connected({ startUnlocked: true });
    check((await call(dev, "notARealMethod")).error !== undefined, "unknown method should error");

    const r = await call(dev, "selectWallet", { index: 99 });
    check(r.error?.code === ErrorCode.NoWallet, "out-of-range wallet should error");
  }

  group("disconnect clears session state");
  {
    const dev = await connected({ startUnlocked: true });
    await call(dev, "setPassphrase", { passphrase: "x" });
    await dev.close();
    await dev.open();
    check(dev.session === "none", "reopening left a session behind");
    const r = await call(dev, "selectWallet", GATED);
    check(r.error?.code === ErrorCode.SessionRequired, "a new connection needs a new session");
  }

  group("latency is modelled");
  {
    const dev = await connected({ startUnlocked: true, latencyMs: 40 });
    const t0 = Date.now();
    await call(dev, "selectWallet", GATED);
    check(Date.now() - t0 >= 35, "the mock should be slow enough to reveal missing spinners");
  }

  console.log(`\n${failures ? "FAILED" : "PASSED"} (${failures} failure${failures === 1 ? "" : "s"})`);
  process.exit(failures ? 1 : 0);
}

main();
