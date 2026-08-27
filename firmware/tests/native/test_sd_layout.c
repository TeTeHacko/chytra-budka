/* test_sd_layout.c — host unit tests for the pure SD layout/retention logic.
 * No FATFS, no ESP-IDF; sd_layout.c is plain libc. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "sd_layout.h"

static int g_checks = 0;
#define CHECK(cond)                                                      \
    do {                                                                 \
        g_checks++;                                                      \
        if (!(cond)) {                                                   \
            fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            return 1;                                                    \
        }                                                                \
    } while (0)

/* A struct tm with year>=2024 → "valid clock"; else pre-SNTP. */
static struct tm mk_tm(int y, int mo, int d, int h, int mi, int s) {
    struct tm t;
    memset(&t, 0, sizeof(t));
    t.tm_year = y - 1900;
    t.tm_mon = mo - 1;
    t.tm_mday = d;
    t.tm_hour = h;
    t.tm_min = mi;
    t.tm_sec = s;
    return t;
}

static int test_paths(void) {
    char buf[SD_LAYOUT_LEAF_MAX];

    struct tm good = mk_tm(2026, 6, 4, 14, 30, 22);
    CHECK(sd_layout_time_valid(&good));
    char day[SD_LAYOUT_DAY_LEN];
    CHECK(sd_layout_day_for_time(&good, day, sizeof(day)) == true);
    CHECK(strcmp(day, "2026-06-04") == 0);
    sd_layout_leaf_name(&good, "aa_bb", "pir", 42, buf, sizeof(buf));
    CHECK(strcmp(buf, "143022_0000000042_aa_bb_pir.jpg") == 0);

    /* Same wall-clock second, different boot seq → distinct names that still
     * sort chronologically (newer seq sorts later/greater). */
    char a[SD_LAYOUT_LEAF_MAX], b[SD_LAYOUT_LEAF_MAX];
    sd_layout_leaf_name(&good, "aa_bb", "pir", 7, a, sizeof(a));
    sd_layout_leaf_name(&good, "aa_bb", "pir", 8, b, sizeof(b));
    CHECK(strcmp(a, b) != 0);
    CHECK(strcmp(a, b) < 0);  /* seq 7 < seq 8 lexically */

    struct tm pre = mk_tm(1970, 1, 1, 0, 0, 5);
    CHECK(!sd_layout_time_valid(&pre));
    CHECK(sd_layout_day_for_time(&pre, day, sizeof(day)) == false);
    CHECK(strcmp(day, "boot") == 0);
    sd_layout_leaf_name(&pre, "aa_bb", "mqtt", 7, buf, sizeof(buf));
    CHECK(strcmp(buf, "boot-0000000007_aa_bb_mqtt.jpg") == 0);

    /* NULL mac/trig fall back, don't crash. */
    sd_layout_leaf_name(&good, NULL, NULL, 0, buf, sizeof(buf));
    CHECK(strcmp(buf, "143022_0000000000_unknown_unk.jpg") == 0);

    /* The trigger tag is still recoverable from the seq-bearing leaf. */
    char trig[16];
    sd_layout_trigger_of("143022_0000000042_aa_bb_pir.jpg", trig, sizeof(trig));
    CHECK(strcmp(trig, "pir") == 0);
    return 0;
}

static int test_classify(void) {
    CHECK(sd_layout_is_date_dir("2026-06-04"));
    CHECK(!sd_layout_is_date_dir("2026-6-04"));   /* not zero-padded */
    CHECK(!sd_layout_is_date_dir("2026/06/04"));  /* wrong separator */
    CHECK(!sd_layout_is_date_dir("2026-06-040")); /* too long */
    CHECK(!sd_layout_is_date_dir("boot"));

    CHECK(sd_layout_classify("2026-06-04") == SD_BUCKET_DATE);
    CHECK(sd_layout_classify("boot") == SD_BUCKET_BOOT);
    CHECK(sd_layout_classify("20240615-143022_aa_pir.jpg") == SD_BUCKET_ROOT_JPG);
    CHECK(sd_layout_classify("photo.JPG") == SD_BUCKET_ROOT_JPG);  /* case-insens */
    CHECK(sd_layout_classify(".") == SD_BUCKET_OTHER);
    CHECK(sd_layout_classify("..") == SD_BUCKET_OTHER);
    CHECK(sd_layout_classify("System Volume Information") == SD_BUCKET_OTHER);
    CHECK(sd_layout_classify("notes.txt") == SD_BUCKET_OTHER);
    return 0;
}

static int test_ordering(void) {
    /* root < boot < earliest date < later date. */
    CHECK(sd_layout_bucket_cmp_oldest_first("root", "boot") < 0);
    CHECK(sd_layout_bucket_cmp_oldest_first("boot", "2024-01-01") < 0);
    CHECK(sd_layout_bucket_cmp_oldest_first("root", "2024-01-01") < 0);
    CHECK(sd_layout_bucket_cmp_oldest_first("2024-01-01", "2026-06-04") < 0);
    CHECK(sd_layout_bucket_cmp_oldest_first("2026-06-04", "2024-01-01") > 0);
    CHECK(sd_layout_bucket_cmp_oldest_first("2026-06-04", "2026-06-04") == 0);
    CHECK(sd_layout_bucket_cmp_oldest_first("boot", "root") > 0);

    /* descending leaf comparator: newer (lexically larger) first. */
    const char *a = "143000_x_pir.jpg";
    const char *b = "090000_x_pir.jpg";
    const char *pa = a, *pb = b;
    CHECK(sd_layout_name_cmp_desc(&pa, &pb) < 0);  /* a before b (a newer) */
    CHECK(sd_layout_name_cmp_desc(&pb, &pa) > 0);
    return 0;
}

static int test_age(void) {
    struct tm now = mk_tm(2026, 6, 4, 12, 0, 0);
    /* keep_days = 30 */
    CHECK(sd_layout_day_older_than("2026-05-01", &now, 30) == true);   /* 34 d */
    CHECK(sd_layout_day_older_than("2026-05-20", &now, 30) == false);  /* 15 d */
    CHECK(sd_layout_day_older_than("2026-06-04", &now, 30) == false);  /* today */
    /* Exact boundary: keep_days days old is KEPT, keep_days+1 is pruned. */
    CHECK(sd_layout_day_older_than("2026-05-05", &now, 30) == false);  /* exactly 30 d */
    CHECK(sd_layout_day_older_than("2026-05-04", &now, 30) == true);   /* 31 d */
    /* keep_days <= 0 → age prune off. */
    CHECK(sd_layout_day_older_than("2020-01-01", &now, 0) == false);
    /* non-date buckets handled by the space pass, never aged. */
    CHECK(sd_layout_day_older_than("root", &now, 30) == false);
    CHECK(sd_layout_day_older_than("boot", &now, 30) == false);
    /* year boundary sanity */
    struct tm ny = mk_tm(2026, 1, 5, 0, 0, 0);
    CHECK(sd_layout_day_older_than("2025-12-01", &ny, 30) == true);   /* 35 d */
    CHECK(sd_layout_day_older_than("2025-12-20", &ny, 30) == false);  /* 16 d */
    return 0;
}

static int test_validation(void) {
    CHECK(sd_layout_valid_day_param("2026-06-04"));
    CHECK(sd_layout_valid_day_param("root"));
    CHECK(sd_layout_valid_day_param("boot"));
    CHECK(!sd_layout_valid_day_param("../etc"));
    CHECK(!sd_layout_valid_day_param("2026-6-4"));
    CHECK(!sd_layout_valid_day_param(""));

    CHECK(sd_layout_valid_leaf("143022_aa_bb_pir.jpg"));
    CHECK(!sd_layout_valid_leaf("../secret"));
    CHECK(!sd_layout_valid_leaf("a/b.jpg"));
    CHECK(!sd_layout_valid_leaf("a b.jpg"));
    CHECK(!sd_layout_valid_leaf(""));
    return 0;
}

static int test_pagination(void) {
    size_t s, e;
    sd_layout_page_slice(250, 0, 100, &s, &e);
    CHECK(s == 0 && e == 100);
    sd_layout_page_slice(250, 2, 100, &s, &e);
    CHECK(s == 200 && e == 250);   /* last page partial */
    sd_layout_page_slice(250, 9, 100, &s, &e);
    CHECK(s == 250 && e == 250);   /* out of range → empty */
    sd_layout_page_slice(250, -3, 100, &s, &e);
    CHECK(s == 0 && e == 100);     /* negative clamps to 0 */
    sd_layout_page_slice(0, 0, 100, &s, &e);
    CHECK(s == 0 && e == 0);       /* empty set */
    /* Hostile huge page must not wrap the size_t product into a bogus offset. */
    sd_layout_page_slice(250, 2000000000, 100, &s, &e);
    CHECK(s == 250 && e == 250);   /* clamped to end → empty, not wrapped */
    return 0;
}

static int test_trigger(void) {
    char t[16];
    sd_layout_trigger_of("143022_aa_bb_pir.jpg", t, sizeof(t));
    CHECK(strcmp(t, "pir") == 0);
    sd_layout_trigger_of("boot-0000000007_aa_mqtt.jpg", t, sizeof(t));
    CHECK(strcmp(t, "mqtt") == 0);
    sd_layout_trigger_of("noextension", t, sizeof(t));
    CHECK(t[0] == '\0');
    sd_layout_trigger_of("nounderscore.jpg", t, sizeof(t));
    CHECK(t[0] == '\0');
    return 0;
}

static int test_legacy_target(void) {
    char d[SD_LAYOUT_DAY_LEN];
    /* Dated legacy → YYYY-MM-DD */
    CHECK(sd_layout_legacy_target("20240615-143022_aa_bb_pir.jpg", d, sizeof(d)) == true);
    CHECK(strcmp(d, "2024-06-15") == 0);
    CHECK(sd_layout_legacy_target("20261231-000000_x_reed.jpg", d, sizeof(d)) == true);
    CHECK(strcmp(d, "2026-12-31") == 0);
    /* Pre-sync legacy → boot */
    CHECK(sd_layout_legacy_target("boot-0000000007_aa_mqtt.jpg", d, sizeof(d)) == true);
    CHECK(strcmp(d, "boot") == 0);
    /* New date-tree leaf (would never be in root) — NOT a legacy flat name. */
    CHECK(sd_layout_legacy_target("143022_0000000042_aa_pir.jpg", d, sizeof(d)) == false);
    /* Junk / non-migratable */
    CHECK(sd_layout_legacy_target("notes.txt", d, sizeof(d)) == false);
    CHECK(sd_layout_legacy_target("2024-06-15_bad.jpg", d, sizeof(d)) == false);
    CHECK(sd_layout_legacy_target("", d, sizeof(d)) == false);
    return 0;
}

int main(void) {
    printf("test_sd_layout:\n");
    if (test_paths()) return 1;
    if (test_classify()) return 1;
    if (test_legacy_target()) return 1;
    if (test_ordering()) return 1;
    if (test_age()) return 1;
    if (test_validation()) return 1;
    if (test_pagination()) return 1;
    if (test_trigger()) return 1;
    printf("  ok: %d checks\n", g_checks);
    printf("test_sd_layout: PASS\n");
    return 0;
}
