# Contributing to LeekWallet Zcash

Thanks for taking the time to contribute. This project is a hardware wallet:
mistakes here can cost people their funds or their privacy, so we hold changes
to a high standard. These guidelines follow the
[librustzcash contribution guide](https://github.com/zcash/librustzcash/blob/main/CONTRIBUTING.md),
which is the primary reference for anything not covered below.

LeekWallet Zcash is experimental and has not been audited. Please keep that
in mind, and in anything you write about it.

## Table of contents

- [Code of conduct](#code-of-conduct)
- [Questions](#questions)
- [Security issues](#security-issues)
- [Reporting bugs](#reporting-bugs)
- [Suggesting enhancements](#suggesting-enhancements)
- [Project structure](#project-structure)
- [Before you open a pull request](#before-you-open-a-pull-request)
- [Git usage](#git-usage)
- [Commit messages](#commit-messages)
- [Changelog entries](#changelog-entries)
- [Coding style](#coding-style)
- [Project rules](#project-rules)

## Code of conduct

Be respectful and constructive, as in the
[Zcash community's code of conduct](https://github.com/zcash/zcash/blob/master/code_of_conduct.md).

## Questions

Read the [docs](https://zcash.leekwallet.xyz/docs) and search the existing
issues first. If that doesn't answer it, open an issue with as much context as
you can (what you tried, the board, firmware and app versions), or ask in the
LeekWallet threads on the [Zcash Community Forum](https://forum.zcashcommunity.com/).

## Security issues

**Do not open a public issue for a vulnerability.** Report it privately
through GitHub's "Report a vulnerability" button on the repository's Security
tab. If that isn't available, open an issue that says only that you have a
security report and asks for a private contact, without any details.

This covers anything that could leak a seed or key, let a host or a nearby
attacker sign without the user's approval, change what the board shows
against what it signs, or weaken the pairing, the vault or the release
signatures.

## Reporting bugs

Before you report, check that you are on the latest release or `main`, and
search the existing issues, including closed ones.

A good bug report includes:

- what you expected and what happened;
- the board (OLED or colour kit), firmware version, app and version, network
  (mainnet or testnet) and link (USB, Bluetooth or SD);
- steps to reproduce, ideally against the simulator (`make -C firmware/sim zdev`);
- logs or screenshots.

**Never include** a seed phrase, a PIN, a private key, a full viewing key you
want to keep private, or a pairing code. If a log contains one, leave it out
or replace it before posting.

## Suggesting enhancements

Open an issue that explains the problem first, then the change you propose,
and why it belongs in this project rather than in a wallet that uses it.
Changes to the wire protocol, the PCZT checks, key derivation or anything
shown on the board's screen need a discussion before code.

## Project structure

| Path | What it is |
|---|---|
| `firmware/` | ESP32-S3 firmware (ESP-IDF via PlatformIO), the C signer (`components/zsign-c`), the Rust signer (`components/zsign-rust`) and the simulator (`firmware/sim`) |
| `host/` | Rust crates: `zecz-core`, `zecz-cli`, `leek-device`, `leek-zcash` (the SDK), `leek-zcash-ffi` (Kotlin and Swift bindings), `leek-android` (JNI for the Zodl fork) |
| `app/` | Zcash LeekWallet, desktop and Android (Tauri 2) |
| `chrome/` | Chrome extension and its WebAssembly engine |
| `vectors/` | Shared test vectors and their generator |
| `docs/` | Design notes, the journal, the ZIP draft |

The Zodl fork lives in
[leekwallet-zcash-android](https://github.com/0xOucan/leekwallet-zcash-android).
Changes there follow upstream Zodl's own contribution guidelines.

## Before you open a pull request

Run the suites that cover what you changed:

```sh
make -C firmware/sim test          # firmware unit tests, both signers, all vectors
pio run -e esp32s3cam              # firmware build (and esp32s3cam-color for the colour kit)
(cd host && cargo test --workspace)
(cd app && pnpm typecheck && pnpm test)
```

Also run `cargo fmt` and `cargo clippy` on Rust you touched. If you change
anything the signers parse or check, regenerate `vectors/` and explain any
byte that changed. Never delete or weaken a test to make a change pass.

Pull requests may be opened as drafts; mark them ready for review once CI
passes.

## Git usage

As in librustzcash:

- **Merge-based workflow.** Pull requests are merged with merge commits. We
  prefer to keep history; squash merges are the exception.
- **Branch from `main`** and target `main`. The `zecathon-submission` tags
  mark what was submitted to ZECATHON and never move.
- **Clean history.** Each commit is one discrete, meaningful change that
  builds and passes its tests. No "work in progress" or "fix review" commits:
  fold fixes into the commit they belong to (for example with
  [git revise](https://github.com/mystor/git-revise)) before review starts.
- **A bug fix starts with a failing test** in its own commit, before the
  fix, where that's practical.
- **During review**, don't rebase or force-push while someone is reviewing
  without asking. After a rebase, post a link to the diff between the old and
  new branch tips.

## Commit messages

- A short summary line, in the imperative ("Refuse a PCZT with duplicate rk"),
  ideally 50 characters or fewer.
- A body that explains why the change is needed, unless it's trivial.
- `Co-Authored-By:` lines for everyone, or every tool, that wrote part of the
  commit. AI-assisted work is welcome, as long as it is disclosed this way and
  a human has reviewed and tested it.

## Changelog entries

`CHANGELOG.md` is how users learn what they must do to upgrade. Following
librustzcash:

- Any user-visible change, bug fix, change to the wire protocol or to what the
  board shows, and any change to the SDK's public API needs an entry under
  `## Unreleased`.
- The entry goes in the same commit as the change.
- Write it for users: what changed and what they need to do, not
  implementation details.
- Entries describe the change since the last release. If you change something
  that is still unreleased, update its existing entry instead of adding
  another.

## Coding style

- **Rust:** follow the librustzcash coding style, especially on type safety:
  make invalid states unrepresentable, keep struct fields private behind
  checked constructors, and prefer newtypes and enums to bare integers,
  strings and booleans. Document every public item. No `unwrap` or `expect`
  on data from the board, the network or the user.
- **C (firmware):** match the surrounding code. Check every length before you
  read, never trust sizes or indices from the host, wipe secrets from memory
  when you're done with them, and keep the C and Rust signers in agreement.
  The test vectors check both.
- **TypeScript and Kotlin:** match the surrounding code and the existing
  linters.

## Project rules

These are part of the design. A pull request that breaks one will not be
merged:

- **Zcash only.** No other chains in the firmware or the apps.
- **Knob and one Cancel button.** The board's input is a rotary encoder
  (turn, push for OK) and a single Cancel button. Don't add code or on-screen
  hints for other inputs.
- **The pairing code on every connection.** The board remembers no hosts.
- **The board asks before it shares.** The viewing key, a storage key and
  every signature need an explicit approval on the board.
- **What the board shows is what it signs.** Don't show the user a field the
  board hasn't checked against the transaction it signs.
- **No secrets in the repository**: no seeds, keys, keystores or passwords,
  even for testnet. Use the public test vectors.
- **No eFuse burning** in scripts or docs without an explicit, documented
  procedure.
- **Licence.** By contributing you agree that your contribution is licensed
  under this repository's licence (Apache-2.0).
