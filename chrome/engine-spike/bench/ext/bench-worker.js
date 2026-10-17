// The bench in a dedicated worker; bench-page.js forwards logs and the result.
import { run } from './run.js';

self.onmessage = async ({ data }) => {
  const result = await run(new URLSearchParams(data), 'worker', (s) => self.postMessage({ log: s }));
  self.postMessage({ result });
};
