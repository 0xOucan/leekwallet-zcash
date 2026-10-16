/**
 * The microSD card: mount, list, read, and write a new file safely.
 *
 * One implementation of the file operations for the board and the host:
 * ESP-IDF mounts FATFS under SDCARD_MOUNT and from then on it is POSIX
 * stdio and dirent, which a host directory also is. Only mounting differs.
 * The sim points the root at a directory (sdcard__set_root_for_test), so the
 * same code that runs on the card is what the suites exercise.
 *
 * Names are 8.3 (FATFS without long names): "SEED1.ZKV".
 *
 * Writes never overwrite. A new file is written as a temporary, flushed,
 * then renamed to a name that does not exist yet, so a power cut leaves
 * either the old directory or the new file whole, never half a seed file
 * under a real name. A stray temporary is removed at the next mount.
 */
#ifndef LEEK_SDCARD_H
#define LEEK_SDCARD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SDCARD_MOUNT     "/sdcard"
#define SDCARD_NAME_MAX  12          /* 8.3 */
#define SDCARD_FILES_MAX 16

#define SDCARD_OK          0
#define SDCARD_E_NOCARD  (-1)   /* no card, or it would not mount */
#define SDCARD_E_IO      (-2)
#define SDCARD_E_NAME    (-3)   /* not an 8.3 name */
#define SDCARD_E_EXISTS  (-4)
#define SDCARD_E_TOOBIG  (-5)
#define SDCARD_E_NOTFOUND (-6)

/** Mount the card if it is not mounted. */
int sdcard_mount(void);

/** Unmount; a no-op if not mounted. */
void sdcard_unmount(void);

bool sdcard_mounted(void);

/**
 * Names ending in `ext` (".ZKV", case-insensitive), sorted, at most `max`.
 * Returns the count, or a negative SDCARD_E_*.
 */
int sdcard_list(const char *ext, char names[][SDCARD_NAME_MAX + 1], int max);

/** Read a whole file of at most `cap` bytes. */
int sdcard_read(const char *name, uint8_t *buf, size_t cap, size_t *len);

/** Write a new file; SDCARD_E_EXISTS if the name is taken. */
int sdcard_write_new(const char *name, const uint8_t *buf, size_t len);

/**
 * The first free name of the form PREFIXn.EXT, n from 1 to 99 ("SEED", "ZKV"
 * gives SEED1.ZKV, SEED2.ZKV, ...). False if all are taken.
 */
bool sdcard_free_name(const char *prefix, const char *ext, char out[SDCARD_NAME_MAX + 1]);

/*
 * Files by relative path, for the airgap folder (FORMATS.md, "SD-card airgap
 * layout"): "LEEKZ/unsigned.pczt" and the like. Long names, so the firmware
 * needs FATFS long-name support (CONFIG_FATFS_LFN_HEAP). A path is at most
 * one folder deep, of [A-Za-z0-9_.-], with no "..".
 */
#define SDCARD_PATH_MAX 48

/** Size of the file at `path`. */
int sdcard_size(const char *path, size_t *size);

/** Read up to `n` bytes at `offset`; `*got` is how many. */
int sdcard_read_at(const char *path, size_t offset, uint8_t *buf, size_t n, size_t *got);

/** SHA-256 of the whole file, streamed; `*size` (optional) its length. */
int sdcard_sha256(const char *path, uint8_t out[32], size_t *size);

/** Create a folder if it does not exist. */
int sdcard_mkdir(const char *path);

/**
 * Write `path`, replacing any file there, through a temporary and a rename,
 * so a power cut leaves the old file or the new one. For files that are not
 * secrets and may be overwritten (a .zsig); seed files use sdcard_write_new.
 */
int sdcard_write_replace(const char *path, const uint8_t *buf, size_t len);

/** Remove `path`; SDCARD_OK if it was not there. */
int sdcard_remove(const char *path);

#ifdef LEEK_HOST_TEST
/** Use a host directory as the card; NULL means "no card inserted". */
void sdcard__set_root_for_test(const char *dir);
/** Fail the rename step of the next write, as a power cut there would. */
void sdcard__fail_next_rename_for_test(void);
#endif

#endif /* LEEK_SDCARD_H */
