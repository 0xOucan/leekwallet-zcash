import { defineConfig } from "vite";
import { readFileSync } from "node:fs";

/**
 * Serve the shipping Content-Security-Policy during development too, read
 * from tauri.conf.json so there is one copy of it (the same reasoning as
 * firmware/app/vite.config.js: a dev server with a weaker policy than the
 * bundle hides exactly the bugs a policy causes).
 */
function shippingCsp() {
  const config = JSON.parse(readFileSync("./src-tauri/tauri.conf.json", "utf8"));
  const csp = config?.app?.security?.csp;
  if (typeof csp !== "string" || csp === "") {
    throw new Error("no app.security.csp in tauri.conf.json to mirror in dev");
  }
  return {
    name: "shipping-csp",
    configureServer(server) {
      server.middlewares.use((_req, res, next) => {
        res.setHeader("Content-Security-Policy", csp);
        next();
      });
    },
  };
}

// 1421, not 1420: the EVM companion in firmware/app owns 1420, and both
// may well run side by side on a developer's machine.
export default defineConfig({
  root: ".",
  plugins: [shippingCsp()],
  build: { outDir: "dist", emptyOutDir: true, target: "es2022" },
  server: { port: 1421, strictPort: true },
  clearScreen: false,
});
