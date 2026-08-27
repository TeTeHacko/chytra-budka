/* sd_storage.h — microSD via SDIO 1-bit on XIAO Sense expansion. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Mount /sdcard as FAT. Idempotent. Returns ESP_OK if mounted,
 * ESP_ERR_NOT_FOUND if no card detected. */
esp_err_t sd_storage_init(void);

bool sd_storage_ready(void);

/* Free / total bytes on the mounted volume (0/0 if not ready). */
void sd_storage_stats(uint64_t *free_bytes, uint64_t *total_bytes);

/* Write a single binary file (overwrites if exists). */
esp_err_t sd_storage_write_file(const char *path,
                                const uint8_t *data, size_t len);

/* Write a capture into the date-tree: /sdcard/YYYY-MM-DD/HHMMSS_<mac>_<trig>.jpg
 * (or /sdcard/boot/boot-<seq>_… before SNTP). Creates the day directory on
 * day rollover (cached, so mkdir runs at most once per day). On success fills
 * out_fullpath (if non-NULL) with the absolute path written and bumps the
 * listing generation. `tm` is the capture local time; `mac_tag`/`trig` may be
 * NULL (fall back to "unknown"/"unk"). See sd_layout.h for the scheme. */
esp_err_t sd_storage_write_capture(const struct tm *tm, const char *mac_tag,
                                   const char *trig, uint32_t seq,
                                   const uint8_t *data, size_t len,
                                   char *out_fullpath, size_t cap);

/* Last stored capture's day bucket ("YYYY-MM-DD"/"boot"/"root") and leaf
 * filename, for deep-linking the web preview to /view?d=&f=. Returns false
 * (buffers untouched) until the first frame is persisted this boot. day_cap
 * should be >= SD_LAYOUT_DAY_LEN, leaf_cap >= SD_LAYOUT_LEAF_MAX. Thread-safe. */
bool sd_storage_last_photo(char *day, size_t day_cap, char *leaf, size_t leaf_cap);

/* Retention: keep the card from filling. Deletes whole oldest day-buckets
 * (legacy root *.jpg first, then "boot", then earliest dated day; never
 * today's) until free space climbs back over min_free_pct + hysteresis, and —
 * if keep_days > 0 — also drops dated days older than keep_days regardless of
 * free space. No-op if the card is not mounted. Counters surface via
 * sd_storage_pruned_stats().
 *
 * max_files caps how many files ONE call deletes — a budget so a routine pass
 * can't saturate a slow card (unlinks are FAT-table writes, ~100 ms each on a
 * tired card) or starve its caller. <=0 = the built-in maximum
 * (PRUNE_MAX_FILES_PER_PASS). The full-card write-retry path passes 0 (free
 * space fast for the retry); the background maintenance pass on the supervisor
 * loop passes a SMALL budget so a deep backlog is drained gently over many
 * passes and never blocks a concurrent capture's SD write. */
void sd_storage_autoprune(int min_free_pct, int keep_days, int max_files);

/* Cumulative count + bytes of files deleted by autoprune since boot. */
void sd_storage_pruned_stats(uint32_t *files, uint64_t *bytes);

/* One-time legacy migration: move loose pre-date-tree *.jpg from the card root
 * into their day buckets (rename — cheap on FAT, same volume). Moves up to
 * max_files per call (bounded + WDT-fed) so it never stalls the capture task;
 * call it repeatedly (lazily after captures, or via /debug/sd_migrate). Returns
 * the number moved this call; once the root has no loose *.jpg left (or none of
 * the remaining are migratable) it self-disables for the session and returns 0.
 * Drains the slow legacy "root" bucket so the gallery is fast on every board —
 * incl. the OTA-only field, whose SD can't be reached to migrate by hand. */
int sd_storage_migrate_step(int max_files);

/* Monotonic counter bumped whenever the on-card photo set changes (a capture
 * is written or autoprune deletes). The HTTP listing cache keys on this to
 * know when to rebuild without re-reading the card on every page load. */
uint32_t sd_storage_listing_gen(void);

/* Reformat the mounted SD card in place. Wipes all data on the card.
 * The card stays mounted after this returns. Used to recover from
 * FATFS chain corruption (orphan clusters, dir entries the FS can't
 * read back via readdir) without pulling the card out of the budka. */
esp_err_t sd_storage_format(void);

/* Mount with format_if_mount_failed=true. Use when sd_storage_init()
 * returned ESP_FAIL (card present, FATFS rejected — typically
 * FR_NO_FILESYSTEM on a brand-new or wiped card). Wipes all data. */
esp_err_t sd_storage_format_unmounted(void);

#ifdef __cplusplus
}
#endif
