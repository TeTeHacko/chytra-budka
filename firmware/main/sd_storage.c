/* sd_storage.c — microSD on XIAO ESP32-S3 Sense expansion.
 *
 * SDIO 1-bit mode using SDMMC peripheral on:
 *   CLK   = GPIO7
 *   CMD   = GPIO9
 *   DAT0  = GPIO8
 *
 * 1-bit (not 4-bit) because XIAO Sense only routes DAT0; DAT1-3 are
 * not bonded to expansion connector. Throughput ~3-4 MB/s, plenty
 * for trail-cam JPEGs.
 */

#include "sd_storage.h"

#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "config.h"
#include "driver/sdmmc_host.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_task_wdt.h"
#include "esp_vfs_fat.h"
#include "sd_layout.h"
#include "sdmmc_cmd.h"

static const char *TAG = "sd";

#define MOUNT_POINT "/sdcard"

/* Retention pass bounds — keep a single autoprune call cheap so it never
 * stalls the capture task even when the legacy root bucket holds thousands of
 * loose files. Whatever's left is cleared on the next capture. */
#define PRUNE_HYSTERESIS         5    /* free target = min_free_pct + this */
#define PRUNE_MAX_FILES_PER_PASS 300
#define PRUNE_MAX_DIRS_PER_PASS  16
#define PRUNE_BATCH              96   /* names gathered per opendir scan */
#define PRUNE_NAME_MAX           64

static sdmmc_card_t *s_card = NULL;
static bool s_mounted = false;

static atomic_uint_least32_t s_pruned_files = 0;
static atomic_ullong        s_pruned_bytes = 0;
static atomic_uint_least32_t s_listing_gen  = 0;
/* Last day directory we mkdir'd, so we only hit the FS on day rollover. */
static char s_ensured_day[SD_LAYOUT_DAY_LEN] = {0};
/* Last successfully-stored capture (day bucket + leaf filename) so the web UI
 * can deep-link the home preview to /view?d=&f=. Written by the capture task,
 * read by the HTTP task — both short strings copied as a unit under a spinlock
 * (sub-µs hold). Empty until the first stored frame. */
static portMUX_TYPE s_last_photo_lock = portMUX_INITIALIZER_UNLOCKED;
static char s_last_day[SD_LAYOUT_DAY_LEN] = {0};
static char s_last_leaf[SD_LAYOUT_LEAF_MAX] = {0};
/* Legacy migration self-disables once the root has no loose *.jpg left. */
static bool s_migrate_done = false;

/* Provably-bounded string copy. dirent.d_name is char[256], so the compiler
 * (correctly, in the abstract) flags strncpy/snprintf of it into our small
 * bucket-name buffers as a possible truncation — even though classification
 * guarantees these are ≤10-char date dirs / "boot" / "root". An explicit
 * length-clamped memcpy is provably in-bounds and silences both
 * -Wstringop-truncation and -Wformat-truncation. */
static void copy_bounded(char *dst, size_t dstsize, const char *src) {
    if (!dst || dstsize == 0) return;
    size_t n = strlen(src);
    if (n >= dstsize) n = dstsize - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static esp_err_t mount_internal(bool format_on_fail) {
    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = format_on_fail,
        .max_files              = 5,
        .allocation_unit_size   = 16 * 1024,
    };

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.flags = SDMMC_HOST_FLAG_1BIT;
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;

    sdmmc_slot_config_t slot_cfg = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_cfg.width = 1;
    slot_cfg.clk = SD_CLK_PIN;
    slot_cfg.cmd = SD_CMD_PIN;
    slot_cfg.d0  = SD_DAT0_PIN;
    /* d1..d3 unused in 1-bit mode */
    slot_cfg.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    return esp_vfs_fat_sdmmc_mount(MOUNT_POINT, &host, &slot_cfg,
                                   &mount_cfg, &s_card);
}

esp_err_t sd_storage_init(void) {
    if (s_mounted) return ESP_OK;

    esp_err_t err = mount_internal(false);
    if (err != ESP_OK) {
        if (err == ESP_FAIL) {
            ESP_LOGW(TAG, "FATFS mount failed (corrupt or unformatted) — "
                          "run /debug/sd_format?confirm=yes to wipe + format");
        } else if (err == ESP_ERR_TIMEOUT) {
            ESP_LOGW(TAG, "no card detected");
        } else {
            ESP_LOGW(TAG, "sdmmc mount: 0x%x", err);
        }
        return err;
    }

    s_mounted = true;
    ESP_LOGI(TAG, "mounted %s — %s, %lluMB",
             MOUNT_POINT, s_card->cid.name,
             ((uint64_t)s_card->csd.capacity * s_card->csd.sector_size) /
                 (1024ULL * 1024ULL));
    return ESP_OK;
}

esp_err_t sd_storage_format_unmounted(void) {
    if (s_mounted) return ESP_ERR_INVALID_STATE;
    ESP_LOGW(TAG, "formatting unmounted card — all data will be lost");
    esp_err_t err = mount_internal(true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mount-with-format failed: %s", esp_err_to_name(err));
        return err;
    }
    s_mounted = true;
    ESP_LOGI(TAG, "format+mount done — %s is fresh FAT32 (%s, %lluMB)",
             MOUNT_POINT, s_card->cid.name,
             ((uint64_t)s_card->csd.capacity * s_card->csd.sector_size) /
                 (1024ULL * 1024ULL));
    return ESP_OK;
}

bool sd_storage_ready(void) { return s_mounted; }

void sd_storage_stats(uint64_t *free_bytes, uint64_t *total_bytes) {
    if (free_bytes) *free_bytes = 0;
    if (total_bytes) *total_bytes = 0;
    if (!s_mounted) return;

    FATFS *fs = NULL;
    DWORD free_clusters = 0;
    if (f_getfree("0:", &free_clusters, &fs) != FR_OK || !fs) return;

    uint64_t cluster_bytes = (uint64_t)fs->csize * 512;
    if (total_bytes) *total_bytes = ((uint64_t)fs->n_fatent - 2) * cluster_bytes;
    if (free_bytes)  *free_bytes  = (uint64_t)free_clusters * cluster_bytes;
}

esp_err_t sd_storage_format(void) {
    if (!s_mounted || !s_card) return ESP_ERR_INVALID_STATE;
    ESP_LOGW(TAG, "reformatting %s — all data will be lost", MOUNT_POINT);
    esp_err_t e = esp_vfs_fat_sdcard_format(MOUNT_POINT, s_card);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "format failed: %s", esp_err_to_name(e));
        return e;
    }
    ESP_LOGI(TAG, "format done — %s is fresh FAT32", MOUNT_POINT);
    return ESP_OK;
}

esp_err_t sd_storage_write_file(const char *path, const uint8_t *data,
                                size_t len) {
    if (!s_mounted) return ESP_ERR_INVALID_STATE;
    FILE *f = fopen(path, "wb");
    if (!f) return ESP_FAIL;
    size_t wr = fwrite(data, 1, len, f);
    /* ESP-IDF's FATFS port doesn't always commit the directory entry on
     * fclose alone — observed symptom: write returns OK, log says
     * "saved", but readdir() doesn't see the file even seconds later
     * (only after a subsequent unrelated FS operation forces a sync).
     * fflush + fsync forces both the file data and the directory inode
     * out to the SD card before we return. */
    fflush(f);
    int fd = fileno(f);
    if (fd >= 0) fsync(fd);
    fclose(f);
    if (wr != len) {
        /* Card filled mid-write — drop the truncated file so it isn't later
         * listed and served by /photo as a corrupt image (or counted in the
         * /photos.json byte totals). The caller retries after a prune. */
        unlink(path);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* ── date-tree capture write ─────────────────────────────────────────────── */

/* mkdir /sdcard/<day> unless we already did this day. Idempotent; an
 * already-existing dir (EEXIST) counts as ensured. On any other failure we
 * leave s_ensured_day untouched so the next write retries the mkdir. */
static void ensure_day_dir(const char *day) {
    if (strcmp(day, s_ensured_day) == 0) return;
    char p[32];
    snprintf(p, sizeof(p), "%s/%s", MOUNT_POINT, day);
    if (mkdir(p, 0775) == 0 || errno == EEXIST) {
        strncpy(s_ensured_day, day, sizeof(s_ensured_day) - 1);
        s_ensured_day[sizeof(s_ensured_day) - 1] = '\0';
    } else {
        ESP_LOGW(TAG, "mkdir %s failed (errno=%d)", p, errno);
    }
}

esp_err_t sd_storage_write_capture(const struct tm *tm, const char *mac_tag,
                                   const char *trig, uint32_t seq,
                                   const uint8_t *data, size_t len,
                                   char *out_fullpath, size_t cap) {
    if (!s_mounted) return ESP_ERR_INVALID_STATE;
    char day[SD_LAYOUT_DAY_LEN];
    sd_layout_day_for_time(tm, day, sizeof(day));
    ensure_day_dir(day);
    char leaf[SD_LAYOUT_LEAF_MAX];
    sd_layout_leaf_name(tm, mac_tag, trig, seq, leaf, sizeof(leaf));
    char path[128];
    snprintf(path, sizeof(path), "%s/%s/%s", MOUNT_POINT, day, leaf);
    esp_err_t e = sd_storage_write_file(path, data, len);
    if (e == ESP_OK) {
        atomic_fetch_add(&s_listing_gen, 1);
        taskENTER_CRITICAL(&s_last_photo_lock);
        strncpy(s_last_day, day, sizeof(s_last_day) - 1);
        s_last_day[sizeof(s_last_day) - 1] = '\0';
        strncpy(s_last_leaf, leaf, sizeof(s_last_leaf) - 1);
        s_last_leaf[sizeof(s_last_leaf) - 1] = '\0';
        taskEXIT_CRITICAL(&s_last_photo_lock);
        if (out_fullpath && cap) {
            strncpy(out_fullpath, path, cap - 1);
            out_fullpath[cap - 1] = '\0';
        }
    }
    return e;
}

/* Last stored capture's day bucket + leaf filename for a /view?d=&f= deep
 * link. Returns false (and leaves the buffers untouched) until the first
 * frame has been persisted this boot. Thread-safe vs the capture task. */
bool sd_storage_last_photo(char *day, size_t day_cap, char *leaf, size_t leaf_cap) {
    bool have;
    taskENTER_CRITICAL(&s_last_photo_lock);
    have = (s_last_leaf[0] != '\0');
    if (have) {
        if (day && day_cap) {
            strncpy(day, s_last_day, day_cap - 1);
            day[day_cap - 1] = '\0';
        }
        if (leaf && leaf_cap) {
            strncpy(leaf, s_last_leaf, leaf_cap - 1);
            leaf[leaf_cap - 1] = '\0';
        }
    }
    taskEXIT_CRITICAL(&s_last_photo_lock);
    return have;
}

/* ── retention (autoprune) ───────────────────────────────────────────────── */

void sd_storage_pruned_stats(uint32_t *files, uint64_t *bytes) {
    if (files) *files = atomic_load(&s_pruned_files);
    if (bytes) *bytes = atomic_load(&s_pruned_bytes);
}

uint32_t sd_storage_listing_gen(void) { return atomic_load(&s_listing_gen); }

/* Delete files from one bucket. For "root" (legacy loose files) only *.jpg at
 * the card root are removed and the root dir is never rmdir'd; for a dated day
 * or "boot" every file is removed and the now-empty dir is rmdir'd. Gathers a
 * batch of names first (rather than unlinking mid-readdir) to avoid the FATFS
 * readdir-vs-mutate quirk. Returns true if the bucket was fully drained. */
static bool prune_drain_bucket(const char *bucket, int *budget) {
    bool is_root = (strcmp(bucket, SD_LAYOUT_ROOT) == 0);
    char dirpath[32];
    if (is_root)
        snprintf(dirpath, sizeof(dirpath), "%s", MOUNT_POINT);
    else
        snprintf(dirpath, sizeof(dirpath), "%s/%s", MOUNT_POINT, bucket);

    char (*names)[PRUNE_NAME_MAX] = malloc((size_t)PRUNE_BATCH * PRUNE_NAME_MAX);
    if (!names) return false;
    DIR *d = opendir(dirpath);
    if (!d) {
        free(names);
        return false;
    }
    int n = 0;
    bool more = false;
    int scanned = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        /* readdir itself can stall per-entry on a near-EOL card — the exact
         * failure mode autoprune exists for — so feed the WDT during the scan
         * too, not just the unlink loop. */
        if (++scanned % 32 == 0) (void)esp_task_wdt_reset();
        const char *nm = e->d_name;
        if (is_root) {
            if (sd_layout_classify(nm) != SD_BUCKET_ROOT_JPG) continue;
        } else {
            if (nm[0] == '.' && (nm[1] == '\0' || (nm[1] == '.' && nm[2] == '\0')))
                continue;
        }
        if (n >= PRUNE_BATCH) {
            more = true;
            break;
        }
        copy_bounded(names[n], PRUNE_NAME_MAX, nm);
        n++;
    }
    closedir(d);

    int deleted = 0, since_wdt = 0;
    for (int i = 0; i < n && *budget > 0; i++) {
        char fp[160];
        snprintf(fp, sizeof(fp), "%s/%s", dirpath, names[i]);
        struct stat st;
        uint64_t sz = (stat(fp, &st) == 0) ? (uint64_t)st.st_size : 0;
        if (unlink(fp) == 0) {
            atomic_fetch_add(&s_pruned_files, 1);
            atomic_fetch_add(&s_pruned_bytes, sz);
            (*budget)--;
            deleted++;
        }
        /* Reset every 8 stat+unlink pairs: on a degraded card each pair can
         * take >100 ms, and the committed task-WDT timeout is as low as 5 s. */
        if (++since_wdt >= 8) {
            since_wdt = 0;
            (void)esp_task_wdt_reset();
        }
    }
    free(names);

    bool drained = (!more && deleted == n);
    if (drained && !is_root) rmdir(dirpath);  /* drop the now-empty day/boot dir */
    return drained;
}

/* Scan the card root and return the oldest prunable bucket name into `out`
 * (never `today`). false if only today's bucket remains. */
static bool find_oldest_bucket(const char *today, char *out, size_t cap) {
    DIR *d = opendir(MOUNT_POINT);
    if (!d) return false;
    char best[SD_LAYOUT_DAY_LEN];
    best[0] = '\0';
    bool have = false;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        sd_bucket_kind_t k = sd_layout_classify(e->d_name);
        const char *cand = NULL;
        if (k == SD_BUCKET_DATE) {
            if (strcmp(e->d_name, today) == 0) continue;  /* never today */
            cand = e->d_name;
        } else if (k == SD_BUCKET_BOOT) {
            if (strcmp(today, SD_LAYOUT_BOOT_DIR) == 0) continue;  /* pre-sync run */
            cand = SD_LAYOUT_BOOT_DIR;
        } else if (k == SD_BUCKET_ROOT_JPG) {
            cand = SD_LAYOUT_ROOT;
        } else {
            continue;
        }
        if (!have || sd_layout_bucket_cmp_oldest_first(cand, best) < 0) {
            copy_bounded(best, sizeof(best), cand);
            have = true;
        }
    }
    closedir(d);
    if (have && out && cap)
        copy_bounded(out, cap, best);
    return have;
}

void sd_storage_autoprune(int min_free_pct, int keep_days, int max_files) {
    if (!s_mounted) return;
    if (min_free_pct < 0) min_free_pct = 0;
    if (min_free_pct > 90) min_free_pct = 90;

    char today[SD_LAYOUT_DAY_LEN];
    struct tm tm_now;
    {
        time_t now = time(NULL);
        localtime_r(&now, &tm_now);
        sd_layout_day_for_time(&tm_now, today, sizeof(today));
    }

    int file_budget = (max_files > 0 && max_files < PRUNE_MAX_FILES_PER_PASS)
                          ? max_files : PRUNE_MAX_FILES_PER_PASS;
    int dirs_done = 0;
    uint32_t files_before = atomic_load(&s_pruned_files);

    /* Age-based pass: gather old dated days, then drain them. Collect names
     * first so we don't rmdir entries out from under the root readdir. */
    if (keep_days > 0) {
        char old[PRUNE_MAX_DIRS_PER_PASS][SD_LAYOUT_DAY_LEN];
        int no = 0;
        DIR *d = opendir(MOUNT_POINT);
        if (d) {
            struct dirent *e;
            while ((e = readdir(d)) != NULL && no < PRUNE_MAX_DIRS_PER_PASS) {
                if (sd_layout_classify(e->d_name) == SD_BUCKET_DATE &&
                    strcmp(e->d_name, today) != 0 &&
                    sd_layout_day_older_than(e->d_name, &tm_now, keep_days)) {
                    copy_bounded(old[no], SD_LAYOUT_DAY_LEN, e->d_name);
                    no++;
                }
            }
            closedir(d);
        }
        for (int i = 0; i < no && file_budget > 0; i++) {
            ESP_LOGW(TAG, "autoprune: dropping aged day %s (keep_days=%d)",
                     old[i], keep_days);
            prune_drain_bucket(old[i], &file_budget);
            dirs_done++;
        }
    }

    /* Space-based pass: drop oldest buckets until free climbs over target. */
    int target = min_free_pct + PRUNE_HYSTERESIS;
    if (target > 95) target = 95;
    while (dirs_done < PRUNE_MAX_DIRS_PER_PASS && file_budget > 0) {
        /* find_oldest_bucket does a full root readdir each iteration; on a huge
         * legacy root that's non-trivial, so feed the WDT between buckets. */
        (void)esp_task_wdt_reset();
        uint64_t fb = 0, tb = 0;
        sd_storage_stats(&fb, &tb);
        if (tb == 0) break;
        int free_pct = (int)((fb * 100ULL) / tb);
        if (free_pct >= target) break;
        char oldest[SD_LAYOUT_DAY_LEN];
        if (!find_oldest_bucket(today, oldest, sizeof(oldest))) break;  /* only today */
        ESP_LOGW(TAG, "autoprune: free %d%% < target %d%% — dropping oldest bucket %s",
                 free_pct, target, oldest);
        prune_drain_bucket(oldest, &file_budget);
        dirs_done++;
    }

    uint32_t freed = atomic_load(&s_pruned_files) - files_before;
    if (freed) {
        atomic_fetch_add(&s_listing_gen, 1);  /* listing changed */
        ESP_LOGI(TAG, "autoprune: removed %" PRIu32 " file(s) this pass "
                      "(%" PRIu32 " total)", freed, atomic_load(&s_pruned_files));
    }
}

/* ── legacy migration (flat root → day buckets) ──────────────────────────── */

/* mkdir /sdcard/<reldir>, ignoring EEXIST. Unlike ensure_day_dir this doesn't
 * touch the capture day-cache (migration walks many days). */
static void mkdir_quiet(const char *reldir) {
    char p[32];
    snprintf(p, sizeof(p), "%s/%s", MOUNT_POINT, reldir);
    if (mkdir(p, 0775) != 0 && errno != EEXIST)
        ESP_LOGW(TAG, "migrate: mkdir %s failed (errno=%d)", p, errno);
}

int sd_storage_migrate_step(int max_files) {
    if (!s_mounted || s_migrate_done || max_files <= 0) return 0;

    int batch = (max_files < PRUNE_BATCH) ? max_files : PRUNE_BATCH;
    char (*names)[PRUNE_NAME_MAX] = malloc((size_t)batch * PRUNE_NAME_MAX);
    if (!names) return 0;

    DIR *d = opendir(MOUNT_POINT);
    if (!d) {
        free(names);
        return 0;
    }
    int n = 0, scanned = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && n < batch) {
        if (++scanned % 32 == 0) (void)esp_task_wdt_reset();
        if (sd_layout_classify(e->d_name) != SD_BUCKET_ROOT_JPG) continue;
        copy_bounded(names[n], PRUNE_NAME_MAX, e->d_name);
        n++;
    }
    closedir(d);

    int moved = 0, since_wdt = 0;
    for (int i = 0; i < n; i++) {
        char tgt[SD_LAYOUT_DAY_LEN];
        if (!sd_layout_legacy_target(names[i], tgt, sizeof(tgt)))
            continue;  /* unrecognized loose file — leave it untouched */
        mkdir_quiet(tgt);
        char src[160], dst[200];
        snprintf(src, sizeof(src), "%s/%s", MOUNT_POINT, names[i]);
        snprintf(dst, sizeof(dst), "%s/%s/%s", MOUNT_POINT, tgt, names[i]);
        if (rename(src, dst) == 0)
            moved++;
        else
            ESP_LOGW(TAG, "migrate: rename %s → %s/ failed (errno=%d)",
                     names[i], tgt, errno);
        if (++since_wdt >= 16) {
            since_wdt = 0;
            (void)esp_task_wdt_reset();
        }
    }
    free(names);

    /* Self-disable when there's nothing left to do: either the root has no loose
     * *.jpg (n==0), or this pass couldn't move any of what it found (all
     * unrecognized / rename-failing) — no point rescanning the root every
     * capture forever. */
    if (n == 0 || moved == 0) {
        s_migrate_done = true;
        if (n == 0)
            ESP_LOGI(TAG, "migrate: root clean — legacy migration complete");
    }
    if (moved) {
        atomic_fetch_add(&s_listing_gen, 1);
        ESP_LOGI(TAG, "migrate: moved %d legacy file(s) into day buckets", moved);
    }
    return moved;
}
