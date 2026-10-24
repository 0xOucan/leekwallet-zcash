#!/usr/bin/env bash
#
# Assemble one release of every product, after CI, into a local directory, and
# print the commands that sign and publish it. (RELEASES.md)
#
#   scripts/release-collect.sh <tag> --apk <path-to-signed.apk> [options]
#
#   --apk PATH           the release APK built and signed locally with the
#                        release keystore (app/ANDROID.md, "Release APK")
#   --firmware-dir DIR   what firmware/scripts/release.sh wrote
#                        (default: firmware/release/<tag>/esp32s3cam)
#   --out DIR            where to assemble (default: release/<tag>; must not exist)
#
# The order of a release:
#
#   1. git tag -s <tag>; git push origin <tag>
#   2. Actions -> release -> Run workflow (tag): the desktop bundles and the
#      Chrome extension land in a DRAFT release.
#   3. firmware/scripts/release.sh <tag> esp32s3cam
#   4. the APK: cd app && LEEK_KEYSTORE=... pnpm android:release:arm64
#   5. this script: downloads the draft's assets (read only), adds 3 and 4,
#      writes SHA256SUMS over everything, and PRINTS the sign, upload and
#      publish commands.
#
# It never writes to GitHub, never signs and never reads a key: the commands
# that do are printed for a human to run after reading what is in the
# directory. A script that signs is a script that signs whatever it was given.
set -Eeuo pipefail
trap 'echo "release-collect: failed at line $LINENO" >&2' ERR

REPO_SLUG="${LEEK_RELEASE_REPO:-0xOucan/leekwallet-zcash}"
SIGNER_ID="${LEEK_SIGNER_ID:-0xoucanxxx@gmail.com}"
APP_ID="io.leekwallet.zcash.companion"

die() { echo "release-collect: $*" >&2; exit 1; }
note() { printf '   %s\n' "$*"; }

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "${ROOT}"

TAG=""
APK=""
FW_DIR=""
OUT=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --apk) APK="${2:-}"; shift 2 ;;
        --firmware-dir) FW_DIR="${2:-}"; shift 2 ;;
        --out) OUT="${2:-}"; shift 2 ;;
        -h|--help) sed -n '2,26p' "$0"; exit 0 ;;
        -*) die "unknown option $1" ;;
        *) [[ -z "${TAG}" ]] || die "one tag only"; TAG="$1"; shift ;;
    esac
done

[[ -n "${TAG}" ]] || die "usage: $0 <tag> --apk <path> [--firmware-dir DIR] [--out DIR]"
[[ "${TAG}" =~ ^v[0-9]+\.[0-9]+\.[0-9]+$ ]] || die "tag must look like v1.2.3, got '${TAG}'"
[[ -n "${APK}" ]] || die "--apk is required: the release APK signed with the release keystore"
VERSION="${TAG#v}"
FW_DIR="${FW_DIR:-firmware/release/${TAG}/esp32s3cam}"
OUT="${OUT:-release/${TAG}}"

for tool in gh git sha256sum; do
    command -v "${tool}" >/dev/null 2>&1 || die "${tool} not on PATH"
done

COMMIT=$(git rev-parse --verify --quiet "${TAG}^{commit}") || die "no tag ${TAG} in this checkout (git fetch --tags)"

# ---------------------------------------------------------------------------
# What a release carries. One list, so that this script, the workflow and
# RELEASES.md cannot drift silently: anything missing or extra is an error.
# ---------------------------------------------------------------------------
CI_ASSETS=(
    "zcash-leekwallet-${VERSION}-linux-x86_64.AppImage"
    "zcash-leekwallet-${VERSION}-linux-amd64.deb"
    "zcash-leekwallet-${VERSION}-linux-x86_64"
    "zcash-leekwallet-${VERSION}-windows-x64.msi"
    "zcash-leekwallet-${VERSION}-macos-aarch64.dmg"
    "zcash-leekwallet-${VERSION}-chrome-extension.zip"
)
# Names exactly as release.sh writes them, so a verifier who rebuilds the tag
# can run `sha256sum -c --ignore-missing` in release.sh's output directory.
FW_ASSETS=(
    "leekwallet-s3cam-${VERSION}-update.bin"
    "leekwallet-s3cam-${VERSION}-provision.bin"
    "firmware.bin"
    "bootloader.bin"
    "partitions.bin"
    "BUILDINFO"
    "manifest-fragment.json"
)
APK_NAME="zcash-leekwallet-${VERSION}-android-arm64.apk"

[[ ! -e "${OUT}" ]] || die "${OUT} already exists; remove it first (it is only a copy)"

echo "==> ${TAG} (${COMMIT}) into ${OUT}"

# ---------------------------------------------------------------------------
# Firmware, before any download: it is the part most worth refusing early.
# ---------------------------------------------------------------------------
echo "-- firmware from ${FW_DIR}"
[[ -d "${FW_DIR}" ]] || die "no ${FW_DIR}; run: (cd firmware && ./scripts/release.sh ${TAG} esp32s3cam)"
for f in "${FW_ASSETS[@]}"; do
    [[ -f "${FW_DIR}/${f}" ]] || die "${FW_DIR}/${f} missing (is esptool on PATH for release.sh?)"
done
grep -q "^commit: *${COMMIT}$" "${FW_DIR}/BUILDINFO" \
    || die "${FW_DIR}/BUILDINFO is not from ${TAG} (${COMMIT})"
grep -q "^env: *esp32s3cam$" "${FW_DIR}/BUILDINFO" \
    || die "${FW_DIR}/BUILDINFO is not the esp32s3cam (Zcash board) build"
# release.sh's own manifest, checked here so a file edited after the build is
# caught before it is re-hashed into the release's SHA256SUMS.
(cd "${FW_DIR}" && sha256sum --quiet -c SHA256SUMS) || die "${FW_DIR} does not match its own SHA256SUMS"
cmp -s "${FW_DIR}/firmware.bin" "${FW_DIR}/leekwallet-s3cam-${VERSION}-update.bin" \
    || die "the update image is not a copy of firmware.bin"

# ---------------------------------------------------------------------------
# The APK: refuse the debug key, the wrong app and the wrong version.
# ---------------------------------------------------------------------------
echo "-- APK ${APK}"
[[ -f "${APK}" ]] || die "no APK at ${APK}"
BT=""
if [[ -n "${ANDROID_HOME:-}" && -d "${ANDROID_HOME}/build-tools" ]]; then
    BT=$(find "${ANDROID_HOME}/build-tools" -mindepth 1 -maxdepth 1 -type d | sort -V | tail -1)
fi
APKSIGNER="$(command -v apksigner || true)"
[[ -z "${APKSIGNER}" && -n "${BT}" && -x "${BT}/apksigner" ]] && APKSIGNER="${BT}/apksigner"
AAPT2="$(command -v aapt2 || true)"
[[ -z "${AAPT2}" && -n "${BT}" && -x "${BT}/aapt2" ]] && AAPT2="${BT}/aapt2"

if [[ -n "${APKSIGNER}" ]]; then
    CERTS=$("${APKSIGNER}" verify --print-certs "${APK}") || die "apksigner: ${APK} does not verify"
elif command -v keytool >/dev/null 2>&1; then
    CERTS=$(keytool -printcert -jarfile "${APK}") || die "keytool could not read the APK's certificate"
else
    die "neither apksigner (set ANDROID_HOME) nor keytool found: cannot check the APK's signer"
fi
if grep -qi "CN=Android Debug" <<<"${CERTS}"; then
    die "the APK is signed with the Android DEBUG key; build it with LEEK_KEYSTORE set (app/ANDROID.md)"
fi
CERT_SHA=$(grep -i -m1 "SHA-256 digest" <<<"${CERTS}" | sed 's/.*: *//' || true)
[[ -z "${CERT_SHA}" ]] && CERT_SHA=$(grep -i -m1 "SHA256:" <<<"${CERTS}" | sed 's/.*SHA256: *//' || true)
if [[ -n "${AAPT2}" ]]; then
    BADGING=$("${AAPT2}" dump badging "${APK}" 2>/dev/null) || die "aapt2 could not read ${APK}"
    BADGING=$(head -1 <<<"${BADGING}")
    grep -q "name='${APP_ID}'" <<<"${BADGING}" || die "the APK is not ${APP_ID}: ${BADGING}"
    grep -q "versionName='${VERSION}'" <<<"${BADGING}" || die "the APK's versionName is not ${VERSION}: ${BADGING}"
else
    note "aapt2 not found: application id and versionName NOT checked"
fi

# ---------------------------------------------------------------------------
# The draft's CI assets. A read: gh release download writes nothing to GitHub.
# ---------------------------------------------------------------------------
echo "-- downloading the draft ${TAG} from ${REPO_SLUG}"
mkdir -p "${OUT}"
gh release download "${TAG}" --repo "${REPO_SLUG}" --dir "${OUT}" --pattern 'zcash-leekwallet-*'
for f in "${CI_ASSETS[@]}"; do
    [[ -f "${OUT}/${f}" ]] || die "the draft has no ${f}; did every job of the release workflow pass?"
done

for f in "${FW_ASSETS[@]}"; do
    cp -- "${FW_DIR}/${f}" "${OUT}/${f}"
done
cp -- "${APK}" "${OUT}/${APK_NAME}"

# Exactly the expected set, nothing left over from an earlier draft.
EXPECTED=$(printf '%s\n' "${CI_ASSETS[@]}" "${FW_ASSETS[@]}" "${APK_NAME}" | LC_ALL=C sort)
PRESENT=$(cd "${OUT}" && find . -maxdepth 1 -type f -printf '%f\n' | LC_ALL=C sort)
if [[ "${EXPECTED}" != "${PRESENT}" ]]; then
    diff <(echo "${EXPECTED}") <(echo "${PRESENT}") >&2 || true
    die "the assembled set is not the expected one (< expected, > present)"
fi

# ---------------------------------------------------------------------------
# One SHA256SUMS over every file, in sha256sum's own format: the flashers
# parse it (app/src/flasher.ts parseShaSums) and `sha256sum -c` reads it.
# ---------------------------------------------------------------------------
mapfile -t NAMES <<<"${EXPECTED}"
(cd "${OUT}" && sha256sum -- "${NAMES[@]}" > SHA256SUMS)

echo
echo "-- ${OUT}/SHA256SUMS"
cat "${OUT}/SHA256SUMS"
echo
[[ -n "${CERT_SHA}" ]] && echo "   APK signing certificate SHA-256: ${CERT_SHA}"
echo "   compare it with the one in RELEASES.md; a different one means a different signer."

ALLOWED="${ROOT}/allowed_signers"
if ! grep -qv '^[[:space:]]*\(#\|$\)' "${ALLOWED}" 2>/dev/null; then
    echo
    echo "   NOTE: ${ALLOWED} has no key yet. Add the line for your public signing key"
    echo "   (see the comments in that file) and commit it before publishing, or nobody"
    echo "   can verify SHA256SUMS.sig."
fi

UPLOAD=()
for f in "${FW_ASSETS[@]}" "${APK_NAME}" SHA256SUMS SHA256SUMS.sig; do
    UPLOAD+=("${OUT}/${f}")
done

cat <<EOF

Nothing has been signed or uploaded. Read the hashes above, then:

1. Sign (your SSH signing key; never a key stored in this repository):

   ssh-keygen -Y sign -f ~/.ssh/<your-signing-key> -n file ${OUT}/SHA256SUMS

2. Check the signature against the committed allowed_signers:

   ssh-keygen -Y verify -f allowed_signers -I ${SIGNER_ID} -n file \\
       -s ${OUT}/SHA256SUMS.sig < ${OUT}/SHA256SUMS

3. Upload the rest to the draft:

   gh release upload ${TAG} --repo ${REPO_SLUG} --clobber \\
       $(printf '%s ' "${UPLOAD[@]}")

4. Look at the draft on GitHub, then publish it:

   gh release edit ${TAG} --repo ${REPO_SLUG} --draft=false

5. The website's flasher: merge ${OUT}/manifest-fragment.json into
   leekwalletwebsite/assets/firmware/manifest.json (firmware/docs/RELEASE.md).
EOF
