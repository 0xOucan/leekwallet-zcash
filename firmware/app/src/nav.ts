/**
 * The post-unlock home accordion (L6, docs/UI-L6-SPEC.md).
 *
 * Replaces L5's push/pop destinations. There is no "current screen" and no
 * Back button any more: every section lives on the one home screen, and its
 * header button expands or collapses it. Collapsing is not unmounting — the
 * body only gets `[hidden]`, so a half-filled Send form or a mini-app
 * mid-flow keeps its state while the user opens another section to sign.
 *
 * Pure and DOM-free on purpose. main.ts owns the actual `.hidden` writes and
 * the side effects (rendering balances, fetching them) that go with opening a
 * section; what belongs here is only the *rule* — which elements are visible
 * for a given (shellOpen, open) pair — because that rule is a safety property
 * (docs/UI-L5-SPEC.md §3: a locked or disconnected device must never leave a
 * Send form or an address on screen) and safety properties are worth pinning
 * with a test that does not need a browser to run.
 */

/** A section of the home accordion. "apps" names the section, not the `#apps`
 * div nested inside `appsdest` — that div keeps deciding its own visibility
 * from mounted content (mountApps(), src/apps/mount.ts), independent of this
 * module. */
export type Section =
  | "zcash" | "balances" | "chain" | "receive" | "send" | "apps" | "connect" | "activity";

/** Display order, top to bottom. */
export const SECTIONS: readonly Section[] =
  ["zcash", "balances", "chain", "receive", "send", "apps", "connect", "activity"];

/**
 * The sections a Zcash-only build has (src/app-mode.ts): the Zcash wallet and
 * nothing else. The EVM sections are never shown there, shell open or not.
 */
export const ZCASH_SECTIONS: readonly Section[] = ["zcash"];

/** The sections of a build: every one in "full", the Zcash one otherwise. */
export function sectionsFor(mode: "zcash" | "full"): readonly Section[] {
  return mode === "full" ? SECTIONS : ZCASH_SECTIONS;
}

/** The body each header expands: the panel that holds the section's content. */
export const SECTION_PANEL_IDS: Record<Section, string> = {
  zcash: "zcashpanel",
  balances: "walletmenu",
  chain: "chainpanel",
  receive: "addrpanel",
  send: "signpanel",
  apps: "appsdest",
  connect: "wcpanel",
  activity: "activitypanel",
};

/** The `<section class="acc">` wrapper: header plus body. Hidden as a whole
 * while the shell is closed, so no bare header strip survives a lock. */
export const SECTION_WRAPPER_IDS: Record<Section, string> = {
  zcash: "seczcash",
  balances: "secbalances",
  chain: "secchain",
  receive: "secreceive",
  send: "secsend",
  apps: "secapps",
  connect: "secconnect",
  activity: "secactivity",
};

/** The header `<button>` that toggles the section. */
export const SECTION_HEADER_IDS: Record<Section, string> = {
  zcash: "hdrzcash",
  balances: "hdrbalances",
  chain: "hdrchain",
  receive: "hdrreceive",
  send: "hdrsend",
  apps: "hdrapps",
  connect: "hdrconnect",
  activity: "hdractivity",
};

/**
 * Which elements of the accordion should be hidden.
 *
 * `shellOpen` is the outer gate: while it is false (locked, disconnected, or
 * never unlocked) every wrapper, every header and every body is hidden, `open`
 * notwithstanding — that is the property that replaces L5's blanket hide, and
 * the reason a collapsed-but-present accordion is not a hole in it. While the
 * shell is open, every wrapper and header shows and exactly the body named by
 * `open` is expanded; `open === null` means everything is collapsed.
 *
 * Single-open by choice (docs/UI-L6-SPEC.md §2): it keeps L5's "only one
 * destination's content on screen" semantics, and it is what the user
 * described — open Send, click Send again and it collapses so Receive or
 * Connect a site is one click away rather than a scroll away.
 */
export function sectionHidden(
  shellOpen: boolean,
  open: Section | null,
  enabled: readonly Section[] = SECTIONS,
): Record<string, boolean> {
  const out: Record<string, boolean> = {};
  for (const s of SECTIONS) {
    /* A section this build does not have is hidden whatever else is true:
     * the Zcash wallet never shows an EVM section, not even collapsed. */
    const off = !shellOpen || !enabled.includes(s);
    out[SECTION_WRAPPER_IDS[s]] = off;
    out[SECTION_HEADER_IDS[s]] = off;
    out[SECTION_PANEL_IDS[s]] = off || open !== s;
  }
  return out;
}

/** Clicking a header: opens it, or collapses it if it was already open. */
export function toggleSection(open: Section | null, clicked: Section): Section | null {
  return open === clicked ? null : clicked;
}
