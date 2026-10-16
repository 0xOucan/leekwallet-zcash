/**
 * The microSD card. See sdcard.h.
 */
#include "sdcard.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "board.h"
#include "esp_log.h"
#include "sha2.h"

#if LEEK_HAS_SDCARD && !defined(LEEK_HOST_TEST)
#include "driver/sdmmc_host.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#endif

static const char *TAG = "sdcard";

#define TMP_NAME "ZKVWRITE.TMP"

static bool mounted;

#ifdef LEEK_HOST_TEST
static char host_root[256];
static bool fail_next_rename;

void sdcard__set_root_for_test(const char *dir)
{
    mounted = false;
    host_root[0] = '\0';
    if (dir) snprintf(host_root, sizeof host_root, "%s", dir);
}

void sdcard__fail_next_rename_for_test(void) { fail_next_rename = true; }

static const char *root(void) { return host_root; }
#else
static const char *root(void) { return SDCARD_MOUNT; }
#endif

#if LEEK_HAS_SDCARD && !defined(LEEK_HOST_TEST)
static sdmmc_card_t *card;
#endif

static void remove_stray_temp(void);

int sdcard_mount(void)
{
    if (mounted) return SDCARD_OK;
#ifdef LEEK_HOST_TEST
    struct stat st;
    if (!host_root[0] || stat(host_root, &st) != 0 || !S_ISDIR(st.st_mode)) {
        return SDCARD_E_NOCARD;
    }
#elif LEEK_HAS_SDCARD
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = BOARD_SD_BUS_WIDTH;
    slot.clk = PIN_SD_CLK;
    slot.cmd = PIN_SD_CMD;
    slot.d0 = PIN_SD_D0;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    /* Never format: a card that does not mount may hold someone's only copy
     * of a seed under a filesystem this firmware does not read. */
    esp_vfs_fat_sdmmc_mount_config_t cfg = {
        .format_if_mount_failed = false,
        .max_files = 2,
        .allocation_unit_size = 16 * 1024,
    };
    esp_err_t err = esp_vfs_fat_sdmmc_mount(SDCARD_MOUNT, &host, &slot, &cfg, &card);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mount failed: %s", esp_err_to_name(err));
        card = NULL;
        return SDCARD_E_NOCARD;
    }
#else
    return SDCARD_E_NOCARD;
#endif
    mounted = true;
    remove_stray_temp();
    ESP_LOGI(TAG, "card mounted");
    return SDCARD_OK;
}

void sdcard_unmount(void)
{
    if (!mounted) return;
#if LEEK_HAS_SDCARD && !defined(LEEK_HOST_TEST)
    esp_vfs_fat_sdcard_unmount(SDCARD_MOUNT, card);
    card = NULL;
#endif
    mounted = false;
}

bool sdcard_mounted(void) { return mounted; }

/* 8.3, letters digits and _ only, upper case: what FATFS without long names
 * stores, so what is listed is exactly what can be opened. */
static bool name_ok(const char *name)
{
    size_t n = strlen(name), dot = 0, dots = 0;
    if (n == 0 || n > SDCARD_NAME_MAX) return false;
    for (size_t i = 0; i < n; i++) {
        char c = name[i];
        if (c == '.') {
            dot = i;
            dots++;
        } else if (!(isupper((unsigned char)c) || isdigit((unsigned char)c) || c == '_')) {
            return false;
        }
    }
    return dots == 1 && dot >= 1 && dot <= 8 && n - dot - 1 >= 1 && n - dot - 1 <= 3;
}

static void path_of(const char *name, char *out, size_t cap)
{
    snprintf(out, cap, "%s/%s", root(), name);
}

static void remove_stray_temp(void)
{
    char p[300];
    path_of(TMP_NAME, p, sizeof p);
    if (unlink(p) == 0) {
        ESP_LOGW(TAG, "removed an unfinished write");
    }
}

static bool ends_with_ci(const char *s, const char *ext)
{
    size_t n = strlen(s), e = strlen(ext);
    if (n < e) return false;
    for (size_t i = 0; i < e; i++) {
        if (toupper((unsigned char)s[n - e + i]) != toupper((unsigned char)ext[i])) return false;
    }
    return true;
}

static int cmp_names(const void *a, const void *b) { return strcmp(a, b); }

int sdcard_list(const char *ext, char names[][SDCARD_NAME_MAX + 1], int max)
{
    if (!mounted) return SDCARD_E_NOCARD;
    DIR *d = opendir(root());
    if (!d) return SDCARD_E_IO;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && n < max) {
        char up[SDCARD_NAME_MAX + 2];
        size_t len = strlen(e->d_name);
        if (len > SDCARD_NAME_MAX) continue;
        for (size_t i = 0; i <= len; i++) up[i] = (char)toupper((unsigned char)e->d_name[i]);
        if (!name_ok(up) || !ends_with_ci(up, ext)) continue;
        memcpy(names[n++], up, len + 1);   /* len <= SDCARD_NAME_MAX, checked above */
    }
    closedir(d);
    qsort(names, (size_t)n, sizeof names[0], cmp_names);
    return n;
}

int sdcard_read(const char *name, uint8_t *buf, size_t cap, size_t *len)
{
    if (!mounted) return SDCARD_E_NOCARD;
    if (!name_ok(name)) return SDCARD_E_NAME;
    char p[300];
    path_of(name, p, sizeof p);
    FILE *f = fopen(p, "rb");
    if (!f) return errno == ENOENT ? SDCARD_E_NOTFOUND : SDCARD_E_IO;
    size_t n = fread(buf, 1, cap, f);
    int extra = fgetc(f);
    bool err = ferror(f);
    fclose(f);
    if (err) return SDCARD_E_IO;
    if (extra != EOF) return SDCARD_E_TOOBIG;
    *len = n;
    return SDCARD_OK;
}

static bool exists(const char *name)
{
    char p[300];
    struct stat st;
    path_of(name, p, sizeof p);
    return stat(p, &st) == 0;
}

int sdcard_write_new(const char *name, const uint8_t *buf, size_t len)
{
    if (!mounted) return SDCARD_E_NOCARD;
    if (!name_ok(name)) return SDCARD_E_NAME;
    if (exists(name)) return SDCARD_E_EXISTS;

    char tmp[300], dst[300];
    path_of(TMP_NAME, tmp, sizeof tmp);
    path_of(name, dst, sizeof dst);
    FILE *f = fopen(tmp, "wb");
    if (!f) return SDCARD_E_IO;
    bool ok = fwrite(buf, 1, len, f) == len && fflush(f) == 0 && fsync(fileno(f)) == 0;
    ok = (fclose(f) == 0) && ok;
#ifdef LEEK_HOST_TEST
    if (fail_next_rename) {
        /* The power went between the flush and the rename: the temporary is
         * on the card, the name is not. */
        fail_next_rename = false;
        return SDCARD_E_IO;
    }
#endif
    if (!ok || rename(tmp, dst) != 0) {
        unlink(tmp);
        return SDCARD_E_IO;
    }
    return SDCARD_OK;
}

bool sdcard_free_name(const char *prefix, const char *ext, char out[SDCARD_NAME_MAX + 1])
{
    for (int i = 1; i <= 99; i++) {
        char name[32];
        snprintf(name, sizeof name, "%s%d.%s", prefix, i, ext);
        if (!name_ok(name)) return false;   /* also bounds it to SDCARD_NAME_MAX */
        if (!mounted || !exists(name)) {
            memcpy(out, name, strlen(name) + 1);
            return true;
        }
    }
    return false;
}

/* ------------------------------------------------------- relative paths */

static bool path_ok(const char *path)
{
    size_t n = strlen(path), slashes = 0;
    if (n == 0 || n > SDCARD_PATH_MAX || path[0] == '/' || path[n - 1] == '/' ||
        strstr(path, "..")) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        char c = path[i];
        if (c == '/') {
            slashes++;
        } else if (!(isalnum((unsigned char)c) || c == '_' || c == '.' || c == '-')) {
            return false;
        }
    }
    return slashes <= 1;
}

static int full_path(const char *path, char *out, size_t cap)
{
    if (!mounted) return SDCARD_E_NOCARD;
    if (!path_ok(path)) return SDCARD_E_NAME;
    snprintf(out, cap, "%s/%s", root(), path);
    return SDCARD_OK;
}

int sdcard_size(const char *path, size_t *size)
{
    char p[320];
    int rc = full_path(path, p, sizeof p);
    if (rc != SDCARD_OK) return rc;
    struct stat st;
    if (stat(p, &st) != 0) return errno == ENOENT ? SDCARD_E_NOTFOUND : SDCARD_E_IO;
    if (!S_ISREG(st.st_mode)) return SDCARD_E_NOTFOUND;
    *size = (size_t)st.st_size;
    return SDCARD_OK;
}

int sdcard_read_at(const char *path, size_t offset, uint8_t *buf, size_t n, size_t *got)
{
    char p[320];
    int rc = full_path(path, p, sizeof p);
    if (rc != SDCARD_OK) return rc;
    FILE *f = fopen(p, "rb");
    if (!f) return errno == ENOENT ? SDCARD_E_NOTFOUND : SDCARD_E_IO;
    rc = SDCARD_OK;
    if (fseek(f, (long)offset, SEEK_SET) != 0) {
        rc = SDCARD_E_IO;
    } else {
        *got = fread(buf, 1, n, f);
        if (ferror(f)) rc = SDCARD_E_IO;
    }
    fclose(f);
    return rc;
}

int sdcard_sha256(const char *path, uint8_t out[32], size_t *size)
{
    char p[320];
    int rc = full_path(path, p, sizeof p);
    if (rc != SDCARD_OK) return rc;
    FILE *f = fopen(p, "rb");
    if (!f) return errno == ENOENT ? SDCARD_E_NOTFOUND : SDCARD_E_IO;
    SHA256_CTX ctx;
    sha256_Init(&ctx);
    uint8_t buf[512];
    size_t n, total = 0;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) {
        sha256_Update(&ctx, buf, n);
        total += n;
    }
    rc = ferror(f) ? SDCARD_E_IO : SDCARD_OK;
    fclose(f);
    sha256_Final(&ctx, out);
    if (size) *size = total;
    return rc;
}

int sdcard_mkdir(const char *path)
{
    char p[320];
    int rc = full_path(path, p, sizeof p);
    if (rc != SDCARD_OK) return rc;
    if (mkdir(p, 0755) == 0 || errno == EEXIST) return SDCARD_OK;
    return SDCARD_E_IO;
}

int sdcard_remove(const char *path)
{
    char p[320];
    int rc = full_path(path, p, sizeof p);
    if (rc != SDCARD_OK) return rc;
    if (unlink(p) == 0 || errno == ENOENT) return SDCARD_OK;
    return SDCARD_E_IO;
}

int sdcard_write_replace(const char *path, const uint8_t *buf, size_t len)
{
    char dst[320], tmp[330];
    int rc = full_path(path, dst, sizeof dst);
    if (rc != SDCARD_OK) return rc;
    snprintf(tmp, sizeof tmp, "%s.tmp", dst);
    FILE *f = fopen(tmp, "wb");
    if (!f) return SDCARD_E_IO;
    bool ok = fwrite(buf, 1, len, f) == len && fflush(f) == 0 && fsync(fileno(f)) == 0;
    ok = (fclose(f) == 0) && ok;
    /* FAT cannot rename over an existing name, so the old file goes first;
     * a cut between the two leaves the complete .tmp beside nothing. */
    if (ok) {
        unlink(dst);
        ok = rename(tmp, dst) == 0;
    }
    if (!ok) {
        unlink(tmp);
        return SDCARD_E_IO;
    }
    return SDCARD_OK;
}
