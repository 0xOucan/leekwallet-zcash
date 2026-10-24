#!/usr/bin/env bash
# A release APK for arm64, signed with a LOCAL key (ANDROID.md, "Release APK").
#
#   pnpm android:release:arm64
#
# Tauri's `android build` (without --debug) uses Cargo's release profile
# (src-tauri/Cargo.toml: opt-level 3, LTO, one codegen unit, stripped) and
# leaves an UNSIGNED APK, because signing config would live in
# src-tauri/gen/android, which is generated and not committed. So the APK is
# aligned and signed here, after the build, with:
#
#   LEEK_KEYSTORE      a keystore path   (default: ~/.android/debug.keystore,
#                                          the Android SDK's own debug key,
#                                          created here if missing)
#   LEEK_KEY_ALIAS     its key alias     (default: androiddebugkey)
#   LEEK_KEYSTORE_PASS its password      (default: android, the SDK's; with
#                                          LEEK_KEYSTORE set and this unset, it
#                                          is asked for, not echoed)
#
# Only the default debug keystore is ever created here. A LEEK_KEYSTORE path
# that does not exist is an error: a typo must not quietly sign a release with
# a brand-new key. Passwords reach apksigner through the environment, not its
# command line, so they do not show in `ps`.
#
# No key, keystore or password is ever committed. A debug key is fine for
# installing on your own phone; a build for other people needs a real key,
# kept outside this tree, passed through the three variables above.
set -euo pipefail

here="$(cd "$(dirname "$0")/.." && pwd)"
: "${ANDROID_HOME:?set ANDROID_HOME (see ANDROID.md, Environment)}"
: "${NDK_HOME:?set NDK_HOME (see ANDROID.md, Environment)}"

bt="$(ls -d "$ANDROID_HOME"/build-tools/* | sort -V | tail -1)"
debug_ks="$HOME/.android/debug.keystore"
keystore="${LEEK_KEYSTORE:-$debug_ks}"
alias="${LEEK_KEY_ALIAS:-androiddebugkey}"
if [ -n "${LEEK_KEYSTORE:-}" ] && [ -z "${LEEK_KEYSTORE_PASS:-}" ]; then
  read -r -s -p "password for $keystore: " LEEK_KEYSTORE_PASS; echo
fi
pass="${LEEK_KEYSTORE_PASS:-android}"

if [ ! -f "$keystore" ]; then
  [ "$keystore" = "$debug_ks" ] || { echo "keystore not found: $keystore" >&2; exit 1; }
  echo "creating a local debug keystore at $keystore"
  mkdir -p "$(dirname "$keystore")"
  keytool -genkeypair -v -keystore "$keystore" -alias "$alias" \
    -storepass "$pass" -keypass "$pass" -keyalg RSA -keysize 2048 -validity 10000 \
    -dname "CN=Android Debug,O=Android,C=US"
fi

# The hand edits gen/ loses on every `tauri android init` (the USB host
# feature, the Espressif device filter): refuse to build an APK without them.
bash "$here/scripts/check-android-manifest.sh"

toolchain="$NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin"
cd "$here"
CC_aarch64_linux_android="$toolchain/aarch64-linux-android24-clang" \
AR_aarch64_linux_android="$toolchain/llvm-ar" \
  pnpm tauri android build --apk --target aarch64 "$@"

out="$here/src-tauri/gen/android/app/build/outputs/apk/universal/release"
unsigned="$out/app-universal-release-unsigned.apk"
aligned="$out/app-universal-release-aligned.apk"
signed="$out/leekwallet-companion-arm64-release.apk"
[ -f "$unsigned" ] || { echo "no unsigned APK at $unsigned" >&2; exit 1; }

"$bt/zipalign" -f -p 4 "$unsigned" "$aligned"
LEEK_SIGN_PASS="$pass" "$bt/apksigner" sign --ks "$keystore" --ks-key-alias "$alias" \
  --ks-pass env:LEEK_SIGN_PASS --key-pass env:LEEK_SIGN_PASS --out "$signed" "$aligned"
"$bt/apksigner" verify "$signed"
rm -f "$aligned"
ls -l "$signed"
echo "signed with $keystore ($alias)"
