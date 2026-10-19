/**
 * Which app this build is: the Zcash wallet, or the full Companion.
 *
 * The LeekWallet Zcash firmware is Zcash-only (it dropped getAddress,
 * signTransaction, signMessage and signTypedData, docs/zcash/ROADMAP.md
 * 1.1), so the default build is a Zcash wallet and shows nothing of the EVM
 * side: no chain selector, no balances, no EVM receive or send, no mini
 * apps, no "Connect a site" (WalletConnect) and no "Pair by QR" (the
 * ur:crypto-hdkey / ur:eth-sign-request air gap). The EVM code stays in the
 * tree for the "master" firmware and app, which build with
 *
 *     VITE_LEEK_APP_MODE=full pnpm build      (or pnpm dev, pnpm tauri ...)
 *
 * Vite replaces `import.meta.env.VITE_LEEK_APP_MODE` at build time. Under
 * plain Node (the tests) there is no `import.meta.env`, and the mode is the
 * default.
 *
 * Pure on purpose, like nav.ts: the rule is testable without a browser.
 */

export type AppMode = "zcash" | "full";

/** Anything but "full" is the Zcash wallet: the safe reading of a typo. */
export function parseAppMode(value: unknown): AppMode {
  return typeof value === "string" && value.trim().toLowerCase() === "full" ? "full" : "zcash";
}

export const APP_MODE: AppMode = parseAppMode(import.meta.env?.VITE_LEEK_APP_MODE);

/** Whether this build shows the EVM side at all. */
export const EVM_ENABLED: boolean = APP_MODE === "full";
