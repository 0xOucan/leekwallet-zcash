# Accessibility — pre-delivery checklist (T31)

What was audited, what was measured, what passes, and what does not. The
subject is the companion app: `app/index.html`, `app/src/styles.css`,
`app/src/tokens.css` and `app/src/main.ts`, and the desktop app shares its
tokens. The board's own OLED is a separate problem and is not in scope here.

One thing shapes every decision below. **Confirming on the board is a security
step, not a convenience.** The app tells you to compare a passkey, to type a PIN
on the board, to read every page of a transaction before approving it. A user
who cannot tell that the board is waiting either misses the confirmation
entirely or walks over and presses the button without having been told what it
is for — and the second one is exactly the behaviour the confirmation exists to
prevent. So "the screen reader never announced it" is filed here as a security
defect, not as a papercut.

## 1. Screen reader

Every moment the board is waiting for its owner is announced, through one
assertive live region (`#announce`), in words that say what to do on the board:
compare the passkey (spoken digit by digit, because "eight hundred and
forty-two thousand" is not a string anyone can compare against a screen), enter
the PIN, check every page and approve, compare the address. `announce()` clears
the region before writing: two identical prompts in a row are not a change, and
without the clear the second one is silent.

Live regions announce state (connection, wallet, results); none wraps content
that redraws as the user types. Panels are `<section aria-labelledby>` on their
own headings; a visually hidden `h1` gives the page one outline. A control's
accessible name matches its visible label (WCAG 2.5.3), and buttons repeated per
row carry an `aria-label` naming what they act on.

## 2. Keyboard

- Every control is a real `<button>`, `<select>`, `<input>`, `<textarea>`,
  `<summary>` or `<a>`, with no positive `tabindex`, so tab order is DOM order
  and DOM order is reading order.
- There are no modal dialogs, so there is no focus trap to escape and no
  "dismiss the thing in the way" reflex trained on a signing screen.
- One focus ring, defined once, on `a, button, select, input, textarea, summary,
  [tabindex]`.
- `[hidden] { display: none !important; }` globally: a panel hidden by the code
  that drew it is also out of the tab order.

## 3. Contrast

Measured, not eyeballed: sRGB relative luminance per WCAG 2.1, computed from
the tokens in `app/src/tokens.css`. Both themes. Thresholds are 4.5:1 for body
text, 3:1 for large text and for the boundary of a user-interface component
(1.4.3 and 1.4.11).

### Light theme

| Foreground | on `--bg` #F4F5F2 | on `--surface` #FFFFFF |
|---|---|---|
| `--text` #1A1D1A | 15.54 | 17.01 |
| `--muted` #5E665F | 5.42 | 5.93 |
| `--accent` / `--ok` #3F6E31 | 5.50 | 6.02 |
| `--danger` #A33329 | 6.27 | 6.86 |
| `--warn` #8A6510 | 4.86 | 5.32 |
| `--border` #D8DCD6 | 1.27 | 1.39 |
| `--control-border` #7E867F **(new)** | 3.42 | 3.75 |

`--accent-fg` on `--accent` (the primary button's own text): **6.02**.

### Dark theme

| Foreground | on `--bg` #0F1210 | on `--surface` #171B18 |
|---|---|---|
| `--text` #E8EDE9 | 15.90 | 14.69 |
| `--muted` #909A91 | 6.48 | 5.98 |
| `--accent` / `--ok` #7ABF6A | 8.51 | 7.86 |
| `--danger` #E5796A | 6.53 | 6.03 |
| `--warn` #D9A441 | 8.38 | 7.74 |
| `--border` #2A302B | 1.40 | 1.29 |
| `--control-border` #6A716B **(new)** | 3.76 | 3.47 |

### What that says

- **All text passes 4.5:1 in both themes.** The tightest is `--warn` on `--bg`
  in light mode at **4.86:1** — a pass with about 8% of headroom, which is worth
  knowing before anyone darkens the page background.
- **The controls failed 1.4.11 and now pass.** A secondary button is
  `--surface` on a `--surface` panel; an input is `--bg` on `--surface`, and
  `--surface` against `--bg` is **1.09:1** (light) / **1.08:1** (dark). The
  border was therefore the only thing saying "this is a control" — and it was
  `--border` at **1.39:1**. Buttons, inputs, selects, textareas and the manual
  copy box now use `--control-border`, which clears 3:1 against both surfaces in
  both themes with the numbers above. `--border` stays where it belongs: panel
  edges, rules, separators.
- **The focus indicator passes.** `--accent` measures 6.02:1 (light, on surface)
  and 7.86:1 (dark, on bg) against what surrounds it, well past 3:1.
- **Placeholders are now themed.** They were the one piece of text in the app
  whose colour nobody had chosen; the UA default is tuned for a white field and
  these fields are `--bg`, which in dark mode is nearly black. They are
  `--muted` now — 5.42:1 and 6.48:1.
- **State colours are never the only channel.** The header dot is a colour, and
  the words beside it say the same thing. The mainnet warning is `--danger`
  *and* the word MAINNET. UNVERIFIED blocks are a warn-coloured dashed edge
  *and* the literal word, injected from CSS so it cannot be forgotten.

## 4. What does not pass, and why it is here anyway

- **Decorative borders stay at 1.3–1.4:1.** Panel edges, the badge chip, row
  separators. These are not user-interface components and no information
  depends on seeing them. Raising
  them would flatten the deliberate difference between "a thing you operate" and
  "a boundary". Recorded as a knowing choice rather than an oversight.
- **Disabled controls sit at `opacity: 0.5`** and are below every threshold.
  WCAG exempts inactive components (1.4.3, 1.4.11) and their state is exposed
  properly through `disabled`, so this is a pass by exemption, not by measurement.
- **No screen reader has actually run against this build.** Everything above is
  read from the markup and the computed colour values. NVDA + Firefox, VoiceOver
  + Safari, and TalkBack in the Android build each still need a pass, and the
  announcement flow — which is the security-relevant part — is what should be
  exercised first.
- **No automated check guards any of this.** `./scripts/check.sh` has no axe or
  markup-linting stage, so this document is the checklist and it is a manual one.
- **Reflow and zoom are unmeasured.** The layout is fluid and has phone rules,
  but 320 CSS px and 200% text zoom (1.4.4, 1.4.10) were not walked through.
- **Scanning a QR code needs sight.** Every QR in the app has a typed or
  pasted alternative, and the panels say so where scanning is unavailable.

## 5. The checklist itself

Before shipping a change to the companion UI:

1. Tab from the top of the page to the bottom. Every stop shows a visible ring;
   nothing is reachable that is meant to be hidden; nothing traps you.
2. Every new control has an accessible name, and it matches its visible label.
3. Any new "the board is waiting" message goes through `deviceAttention()`, not
   `log()`. This is the security-relevant rule in this document.
4. New live regions are `role="status"` unless the device is waiting, in which
   case they are the existing `#announce`. No live region wraps content that
   redraws as the user types.
5. New colours come from `tokens.css` and are measured, not chosen by eye:
   4.5:1 for text, 3:1 for the edge of anything you can operate. Control
   boundaries use `--control-border`, never `--border`.
6. Anything toggled with `hidden` is verified to actually disappear — it will,
   given the global rule, but a new `display` on a class is how that broke the
   first time.
7. Heading levels still form one outline: `h1` (hidden) → panel `h2` → `h3`
   inside a panel.
