// Collects results from bench pages into chrome.storage.session, where
// chromium.mjs reads them from an extension page. Not in a global: MV3
// stops an idle service worker after ~30 s, which loses globals (it did,
// during the sync runs).
// Log lines go there too, so chromium.mjs can show an offscreen run's progress.
let queue = Promise.resolve();
const append = (key, item) =>
  (queue = queue.then(async () => {
    const got = await chrome.storage.session.get({ [key]: [] });
    await chrome.storage.session.set({ [key]: [...got[key], item] });
  }));
chrome.runtime.onMessage.addListener((msg) => {
  if (msg?.bench) append('results', msg.bench);
  if (msg?.log !== undefined) append('log', msg.log);
});
