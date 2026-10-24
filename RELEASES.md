# Releases

Each version of Zcash LeekWallet is one GitHub release, tagged `vX.Y.Z`, at
[0xOucan/leekwallet-zcash/releases](https://github.com/0xOucan/leekwallet-zcash/releases).
It carries every product built from that tag: the board's firmware, the
desktop app, the Android app and the Chrome extension, with one `SHA256SUMS`
over all of them and one signature over that.

## Build it yourself if you can

A prebuilt binary from a central place is a trust risk, whoever runs that
place. This is a hardware wallet: the firmware decides what your board signs,
and the apps decide what they ask it to sign. A release page that serves a
tampered file can serve it to everyone at once, and a checksum on the same page
was written by the same person.

So the recommended way to use this project is to **build it from source**,
from a tag you have looked at. Everything here can be built with free tools,
and the commands are below. The releases exist for people who cannot build,
and so that people who can build have something to compare against: if your
build matches the published file, both of you have learned something.

What the checks below prove, from least to most:

| Check | Proves |
|---|---|
| `sha256sum -c SHA256SUMS` | your download is the file that was published |
| `ssh-keygen -Y verify` on `SHA256SUMS.sig` | the list of hashes was signed by the key in `allowed_signers` |
| your own rebuild matches | the published binary came from the published source |
| secure boot on the board | which firmware the board will run. **Not enabled yet** on this project's boards ([README](README.md)) |

## What a release contains

`<version>` is the tag without its `v` (tag `v0.1.0`, version `0.1.0`).

| File | Product | What it is | Built by |
|---|---|---|---|
| `leekwallet-s3cam-<version>-update.bin` | firmware | The application alone, written at `0x10000`. **Keeps your wallet.** Use this to update | maintainer, `firmware/scripts/release.sh` |
| `leekwallet-s3cam-<version>-provision.bin` | firmware | Bootloader, partition table and application in one image, written at `0x0`. **Erases every wallet on the board.** For a new or unbootable board only | maintainer |
| `firmware.bin`, `bootloader.bin`, `partitions.bin` | firmware | The three parts, as the build leaves them, for anyone rebuilding the tag to compare. `firmware.bin` is the same file as the update image | maintainer |
| `BUILDINFO` | firmware | Tag, commit, board environment, PlatformIO platform pin, the PlatformIO, `rustc +esp` and esptool versions used | maintainer |
| `manifest-fragment.json` | firmware | The entry the website's flasher reads, hashes included | maintainer |
| `zcash-leekwallet-<version>-linux-x86_64.AppImage` | desktop app | Linux, runs without installing | CI (`release.yml`) |
| `zcash-leekwallet-<version>-linux-amd64.deb` | desktop app | Debian, Ubuntu and derivatives | CI |
| `zcash-leekwallet-<version>-linux-x86_64` | desktop app | The bare Linux executable, the file to compare against your own build (below) | CI |
| `zcash-leekwallet-<version>-windows-x64.msi` | desktop app | Windows installer, **not code-signed** | CI |
| `zcash-leekwallet-<version>-macos-aarch64.dmg` | desktop app | macOS on Apple silicon, **not notarised** | CI |
| `zcash-leekwallet-<version>-android-arm64.apk` | Android app | `io.leekwallet.zcash.companion`, arm64, signed with the project's release key | maintainer, `app/scripts/android-release.sh` |
| `zcash-leekwallet-<version>-chrome-extension.zip` | Chrome extension | The unpacked extension (`chrome/extension/dist`), for *Load unpacked* | CI |
| `SHA256SUMS` | all | SHA-256 of every file above, in `sha256sum` format | maintainer |
| `SHA256SUMS.sig` | all | SSH signature over `SHA256SUMS` | maintainer, by hand |

The desktop and Android packages are the same app, Zcash LeekWallet. The
other Android app, LeekWallet Zcash (the Zodl fork), lives in
[its own repository](https://github.com/0xOucan/leekwallet-zcash-android)
and is not part of these releases.

The firmware is for the ESP32-S3 CAM board this project is built on
([wiring](firmware/docs/WIRING-S3CAM.md)). Flashing the provision image onto a
board that holds a seed **destroys the seed**; the flashers in the app and on
the website ask which of the two you are doing for that reason.

## Verifying a download

### 1. The hashes

Download `SHA256SUMS` and `SHA256SUMS.sig` with the files you want, into one
folder.

```bash
sha256sum -c --ignore-missing SHA256SUMS          # Linux
shasum -a 256 -c --ignore-missing SHA256SUMS      # macOS
```

On Windows, in PowerShell, compare the output with the file's line in
`SHA256SUMS`:

```powershell
Get-FileHash .\zcash-leekwallet-0.1.0-windows-x64.msi -Algorithm SHA256
```

### 2. The signature

`allowed_signers` holds the public key that signs releases. Take it from the
git repository **at the release's tag**, not from the release page (a key
published next to the signature it vouches for proves nothing), and if you
can, compare it with a second channel such as the maintainer's
[SSH signing keys on GitHub](https://api.github.com/users/0xOucan/ssh_signing_keys).

```bash
git clone https://github.com/0xOucan/leekwallet-zcash
git -C leekwallet-zcash show v0.1.0:allowed_signers > allowed_signers

ssh-keygen -Y verify -f allowed_signers -I 0xoucanxxx@gmail.com \
    -n file -s SHA256SUMS.sig < SHA256SUMS
```

It should print `Good "file" signature for 0xoucanxxx@gmail.com`. Needs
OpenSSH 8.1 or newer (`ssh -V`; on Windows, Git for Windows ships one).
Anything else, including an error about the key, means: do not use the
files.

A good signature says the maintainer's key signed this list of hashes. It
does not say what is in the files. For that, rebuild.

### 3. The Android signing certificate

Android installs an update only if it is signed by the same key as the app
already installed, so the certificate is what to check across versions:

```bash
apksigner verify --print-certs zcash-leekwallet-0.1.0-android-arm64.apk
```

Its `SHA-256 digest` must be the one written here, which does not change from
release to release:

```
Release certificate SHA-256: (published with the first release)
```

### Unsigned desktop installers

The `.msi` and `.dmg` are not code-signed: that needs paid certificates, and
the reasons are in
[firmware/docs/RELEASE.md](firmware/docs/RELEASE.md#macos-and-windows-code-signing-not-done-and-why).
Windows SmartScreen will say "Windows protected your PC" (*More info*, *Run
anyway*) and macOS Gatekeeper that the developer cannot be verified
(Control-click the app, *Open*). Check the hash and the signature above first:
those warnings are true, nobody vouched for the author, and the checks are
what you can do about it.

## Rebuilding and comparing

Start from the tag, in a fresh clone, so nothing local is in the build:

```bash
git clone https://github.com/0xOucan/leekwallet-zcash
cd leekwallet-zcash
git checkout v0.1.0
```

Toolchains: [docs/TOOLCHAIN.md](docs/TOOLCHAIN.md). The full reasoning
behind each of the claims below, and what to do when a comparison fails, is in
[firmware/docs/RELEASE.md](firmware/docs/RELEASE.md).

### Firmware: byte for byte

```bash
cd firmware
./scripts/release.sh v0.1.0 esp32s3cam
cd release/v0.1.0/esp32s3cam
sha256sum -c --ignore-missing /path/to/downloaded/SHA256SUMS
```

`release.sh` clones the tag into a temporary directory and builds there with
the pinned PlatformIO platform (`espressif32@6.12.0`, ESP-IDF 5.5.0).
`firmware.bin`, `bootloader.bin`, `partitions.bin` and the update image should
match exactly, wherever you build: the build does not depend on its path
(measured on 2026-10-24 for this board, Rust signer included). Before concluding anything from a mismatch, compare your
`BUILDINFO` with the published one: the Zcash signer is Rust, built with the
`esp` toolchain from `espup` (`rustc +esp`), which nothing pins but
docs/TOOLCHAIN.md, and a different version gives different bytes. The
provision image is also rewritten by esptool, whose version `BUILDINFO`
records.

`./scripts/repro-check.sh` builds one commit twice at two different paths and
compares, which is the test behind "byte for byte".

### Desktop app: the Linux executable, at a fixed path

The Rust executable depends on the directory it is built in (cargo derives
hashes from the absolute path; the compiled code is the same, the unwind
tables come out in another order). So the recipe fixes the path, and the
published executable was built on Ubuntu 24.04 at that path:

```bash
rm -rf /tmp/leekwallet-repro-build
git clone https://github.com/0xOucan/leekwallet-zcash /tmp/leekwallet-repro-build
cd /tmp/leekwallet-repro-build && git checkout v0.1.0
cd app
pnpm install --frozen-lockfile
pnpm tauri build --ci --no-bundle
sha256sum src-tauri/target/release/leekwallet-companion
```

Compare with `zcash-leekwallet-<version>-linux-x86_64` in `SHA256SUMS`.
`app/rust-toolchain.toml` pins rustc (build through rustup; a
distribution's `cargo` ignores that file), and `--frozen-lockfile` pins the
JavaScript dependencies. The C parts of the dependency tree are compiled by
the system's C compiler, so the closest match is on Ubuntu 24.04 (a container
will do) with the packages `.github/workflows/release.yml` installs.

What is **not** reproducible, and not claimed: the `.deb`, `.AppImage`,
`.msi` and `.dmg` themselves. They are made by `dpkg-deb`, the AppImage tools,
WiX and `hdiutil`, which add their own timestamps. Windows and macOS builds
have not been compared at all. Compare the executable inside a package with
the one you built, not the package.

### Android APK: compare the contents, not the file

An APK carries its signature, which only the key holder can make, so your
build can never be byte-identical to the published file. What can match is
everything else. The realistic way, the one F-Droid uses:

1. Build the APK unsigned: `cd app && pnpm android:release:arm64` (with a debug
   or throwaway key; [app/ANDROID.md](app/ANDROID.md) has the setup), and keep
   `src-tauri/gen/android/app/build/outputs/apk/universal/release/app-universal-release-unsigned.apk`.
2. Copy the published signature onto your build with
   [`apksigcopier`](https://github.com/obfusk/apksigcopier), which then checks
   that it verifies:
   ```bash
   apksigcopier compare zcash-leekwallet-0.1.0-android-arm64.apk app-universal-release-unsigned.apk
   ```
   If the published signature verifies over your bytes, your build is the
   published build.
3. Short of that, `diffoscope` on the two APKs, or `unzip` both and
   `diff -r` with `META-INF/` excluded, shows what differs.

Be realistic: this has **not been measured** for the Android build. It goes
through Gradle, the NDK and a project that `tauri android init` regenerates,
and the native library has the same build-path dependence as the desktop
executable. Expect differences, and treat a matching `lib/arm64-v8a/*.so`
(built at `/tmp/leekwallet-repro-build`) as the most meaningful partial
result.

### Chrome extension: compare the unpacked files

```bash
chrome/engine/build.sh                    # needs clang, wasm-bindgen-cli 0.2.129
cd chrome/extension && npm ci && node build.mjs
```

Then unzip the published zip and compare it with `chrome/extension/dist`:

```bash
mkdir published && unzip -q zcash-leekwallet-0.1.0-chrome-extension.zip -d published
diff -r published chrome/extension/dist
```

The zip is written with fixed timestamps and sorted entries, so it is a
function of `dist/`. Whether `dist/` itself is reproducible has **not been
measured**: the engine is built with whatever stable Rust CI installed that
day (no `rust-toolchain.toml` in `chrome/engine` yet) and Ubuntu 24.04's clang,
both printed in the workflow's log. A difference in the `.wasm` alone most
likely means a different compiler.

## For the maintainer: cutting a release

1. Set the version in `app/src-tauri/tauri.conf.json`,
   `app/src-tauri/Cargo.toml`, `app/package.json` and
   `chrome/extension/manifest.json`; commit.
2. `git tag -s v0.1.0 -m v0.1.0 && git push origin v0.1.0`.
3. GitHub, Actions, **release**, *Run workflow* with the tag. It builds the
   desktop bundles and the Chrome zip and attaches them to a **draft**
   release. It needs no secret beyond the workflow's own token.
4. `cd firmware && ./scripts/release.sh v0.1.0 esp32s3cam`, and
   `./scripts/repro-check.sh v0.1.0` before trusting it.
5. The APK, with the release keystore (never in this repository):
   `cd app && LEEK_KEYSTORE=... LEEK_KEY_ALIAS=... LEEK_KEYSTORE_PASS=... pnpm android:release:arm64`.
6. `scripts/release-collect.sh v0.1.0 --apk <the signed APK>`. It downloads
   the draft's files, adds the firmware and the APK, refuses a debug-signed APK
   or a firmware build from another commit, writes `SHA256SUMS`, and prints
   the commands for the rest: sign with `ssh-keygen -Y sign -f <key> -n file
   SHA256SUMS`, verify, `gh release upload`, and publish the draft. It runs
   none of them.
7. The first time: put the public signing key in `allowed_signers` and the
   APK certificate digest above, before step 2.
