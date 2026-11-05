/**
 * The firmware's release version and codename: the one place they are set.
 *
 * getFeatures reports LEEK_FW_VERSION to the companion, and the boot splash
 * and the boot screen show it with the codename. scripts/release.sh refuses
 * to build a vX.Y.Z tag whose version.h says anything else, so a release
 * cannot report a version it is not.
 *
 * This is not the build's identity. ESP-IDF still stamps the short commit
 * hash into esp_app_desc_t as PROJECT_VER (CMakeLists.txt, and
 * CONFIG_APP_REPRODUCIBLE_BUILD in sdkconfig.defaults), which is what names
 * the exact source an image came from. These two strings are constants, so
 * they cost the reproducible build nothing.
 *
 * Moving to a new release: change both here and in the same commit as the
 * CHANGELOG entry, then tag.
 */
#ifndef LEEK_VERSION_H
#define LEEK_VERSION_H

#define LEEK_FW_VERSION  "0.1.0"
/* Sacbeh: the Maya sak bej, "white road" (CHANGELOG.md). */
#define LEEK_FW_CODENAME "Sacbeh"

/* What zGetFeatures reports as fwCompat, and what a host must send as
 * hostCompat for the board not to say "Update your app". Raise them only when
 * the other side must change. Guidance, not a security boundary: either side
 * can lie. Firmware before these fields reports no fwCompat: level 1.
 *   2: the NU7 refusal (ZS_E_APP_OUTDATED), fwCompat and hostCompat. */
#define LEEK_FW_COMPAT       2
#define LEEK_HOST_COMPAT_MIN 2

#endif /* LEEK_VERSION_H */
