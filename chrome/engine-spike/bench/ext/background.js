// Collects results from bench pages; chromium.mjs reads globalThis.results.
globalThis.results = [];
chrome.runtime.onMessage.addListener((msg) => {
  if (msg?.bench) globalThis.results.push(msg.bench);
});

// chromium.mjs calls this to run the bench in an offscreen document, the
// context the extension's device and engine work lives in.
globalThis.runOffscreen = async (query) => {
  if (await chrome.offscreen.hasDocument()) await chrome.offscreen.closeDocument();
  await chrome.offscreen.createDocument({
    url: `bench.html?${query}&where=offscreen`,
    reasons: ['WORKERS'],
    justification: 'engine spike: sync and proving',
  });
};
