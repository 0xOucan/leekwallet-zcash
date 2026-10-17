// The service worker only routes: it makes sure the offscreen document,
// which hosts the engine worker, exists. Chrome stops an idle service
// worker after about 30 s, so it holds no state (CHROME-ENGINE.md, risk 5).

const OFFSCREEN_PATH = 'offscreen.html';
let creating = null;

async function ensureOffscreen() {
  const existing = await chrome.runtime.getContexts({
    contextTypes: [chrome.runtime.ContextType.OFFSCREEN_DOCUMENT],
  });
  if (existing.length > 0) return;
  // Two callers can race into createDocument, which throws for the second.
  creating ??= chrome.offscreen
    .createDocument({
      url: OFFSCREEN_PATH,
      reasons: [chrome.offscreen.Reason.WORKERS],
      justification:
        'Runs the Zcash wallet engine (sync, proving) in a dedicated worker. ' +
        'A service worker cannot host it: it is stopped when idle, and the ' +
        'engine must block in Atomics.wait, which only a worker may do.',
    })
    .finally(() => {
      creating = null;
    });
  await creating;
}

chrome.runtime.onInstalled.addListener(() => void ensureOffscreen());
chrome.runtime.onStartup.addListener(() => void ensureOffscreen());

chrome.runtime.onMessage.addListener((msg, _sender, reply) => {
  if (msg?.to !== 'background') return false;
  if (msg.type === 'ensureEngine') {
    ensureOffscreen().then(
      () => reply({ ok: true }),
      (e) => reply({ ok: false, error: String(e?.message ?? e) }),
    );
    return true;
  }
  return false;
});
