/* sd_layout.h — pure (FATFS-free) path + retention policy logic for the
 * microSD photo store. Split out from sd_storage.c so the layout rules
 * (date-dir paths, oldest-bucket selection, age cutoff, pagination math,
 * filename validation) can be unit-tested on the host with plain gcc —
 * no ESP-IDF, no FATFS, no SD card. sd_storage.c owns the actual VFS
 * calls and calls into here for every decision about *where* a file goes
 * and *which* bucket to drop.
 *
 * Layout on the card:
 *   /sdcard/YYYY-MM-DD/HHMMSS_<mac>_<trig>.jpg   captures with a real clock
 *   /sdcard/boot/boot-<seq>_<mac>_<trig>.jpg     captures before SNTP sync
 *   /sdcard/<legacy>.jpg                          flat files from the pre-tree
 *                                                 firmware (the "root" bucket)
 *
 * A "bucket" is a prunable/listable unit at the card root: a dated day dir,
 * the "boot" dir, or the loose legacy *.jpg files (named "root"). Ordering
 * oldest→newest is: root (legacy) < boot < earliest dated day < … < today.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SD_MOUNT_POINT     "/sdcard"
#define SD_LAYOUT_DAY_LEN  11   /* "YYYY-MM-DD" + NUL (also fits "boot"/"root") */
#define SD_LAYOUT_LEAF_MAX 64   /* "HHMMSS_<mac>_<trig>.jpg" / "boot-<seq>_…" */
#define SD_LAYOUT_BOOT_DIR "boot"
#define SD_LAYOUT_ROOT     "root"

/* A real wall-clock time (post-SNTP)? Mirrors camera.c's tm_year >= 2024
 * gate that decides dated vs. boot-sequence filenames. */
bool sd_layout_time_valid(const struct tm *tm);

/* Day-directory name for a capture time → "YYYY-MM-DD" (valid clock) or
 * "boot" (pre-sync). Returns true for a dated day, false for "boot". */
bool sd_layout_day_for_time(const struct tm *tm, char *out, size_t cap);

/* Leaf filename for a capture → "HHMMSS_<mac>_<trig>.jpg" (valid clock) or
 * "boot-<seq>_<mac>_<trig>.jpg" (pre-sync). */
void sd_layout_leaf_name(const struct tm *tm, const char *mac_tag,
                         const char *trig, uint32_t seq, char *out, size_t cap);

/* Classification of a name read from the /sdcard root via readdir. */
typedef enum {
    SD_BUCKET_OTHER = 0,  /* "." ".." or anything we don't manage */
    SD_BUCKET_DATE,       /* a "YYYY-MM-DD" day directory */
    SD_BUCKET_BOOT,       /* the "boot" directory */
    SD_BUCKET_ROOT_JPG,   /* a loose legacy *.jpg at the root */
} sd_bucket_kind_t;

sd_bucket_kind_t sd_layout_classify(const char *name);
bool sd_layout_is_date_dir(const char *name);  /* strict "YYYY-MM-DD" */

/* Oldest-first ordering of bucket NAMES ("root" | "boot" | "YYYY-MM-DD").
 * <0 if a is older than b, >0 if newer, 0 equal. */
int sd_layout_bucket_cmp_oldest_first(const char *a, const char *b);

/* qsort comparator over (char *) leaf names, DESCENDING (newest first within
 * a day, since names start HHMMSS / are fixed-width timestamps). a,b point at
 * char* elements. */
int sd_layout_name_cmp_desc(const void *a, const void *b);

/* Is dated day `day` strictly older than `keep_days` before `now`? Non-dated
 * buckets ("root"/"boot") return false — they are handled by the space-based
 * pass, not the age pass. keep_days<=0 always returns false (age prune off). */
bool sd_layout_day_older_than(const char *day, const struct tm *now, int keep_days);

/* Validate the /photo ?d= bucket param: "root" | "boot" | "YYYY-MM-DD". */
bool sd_layout_valid_day_param(const char *d);

/* Validate a leaf filename: non-empty, only [A-Za-z0-9._-], no "..". Mirrors
 * the anti-traversal gate the /photo handler used to inline. */
bool sd_layout_valid_leaf(const char *f);

/* Half-open page slice [*start,*end) over `count` items. page clamped >=0,
 * per_page forced >=1; an out-of-range page yields an empty slice at count. */
void sd_layout_page_slice(size_t count, int page, int per_page,
                          size_t *start, size_t *end);

/* Extract the trigger tag (text between the last '_' and ".jpg") from a leaf
 * into `out`; empty string if the name has no recognizable tag. */
void sd_layout_trigger_of(const char *leaf, char *out, size_t cap);

/* Migration: where a LEGACY flat root file belongs in the date-tree.
 *   "YYYYMMDD-HHMMSS_<mac>_<trig>.jpg"  → out = "YYYY-MM-DD"   (dated)
 *   "boot-<seq>_<mac>_<trig>.jpg"       → out = "boot"          (pre-sync)
 *   anything else                       → returns false (leave it alone)
 * `out` must hold >= SD_LAYOUT_DAY_LEN. Used by the one-time on-device
 * migration that drains the legacy flat root into day buckets. */
bool sd_layout_legacy_target(const char *name, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
