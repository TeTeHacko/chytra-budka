/* sd_layout.c — pure path + retention policy logic. See sd_layout.h.
 *
 * Deliberately free of ESP-IDF / FATFS so firmware/tests/native can build
 * and exercise it with host gcc. Only the C standard library is used. */
#include "sd_layout.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

bool sd_layout_time_valid(const struct tm *tm) {
    /* Same gate camera.c used: a clock that hasn't seen SNTP sits in 1970,
     * so anything from 2024 on is "real". */
    return tm && tm->tm_year >= (2024 - 1900);
}

bool sd_layout_day_for_time(const struct tm *tm, char *out, size_t cap) {
    if (!out || cap == 0) return false;
    if (sd_layout_time_valid(tm)) {
        snprintf(out, cap, "%04d-%02d-%02d",
                 tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday);
        return true;
    }
    snprintf(out, cap, "%s", SD_LAYOUT_BOOT_DIR);
    return false;
}

void sd_layout_leaf_name(const struct tm *tm, const char *mac_tag,
                         const char *trig, uint32_t seq, char *out, size_t cap) {
    if (!out || cap == 0) return;
    if (!mac_tag) mac_tag = "unknown";
    if (!trig) trig = "unk";
    if (sd_layout_time_valid(tm)) {
        /* Day is already in the directory name, so the leaf needs only the
         * time-of-day — plus the boot sequence, so two captures in the SAME
         * wall-clock second (a PIR/reed burst) don't collide and silently
         * overwrite each other. seq is zero-padded after HHMMSS so the
         * descending name sort stays chronological within a second. */
        snprintf(out, cap, "%02d%02d%02d_%010" PRIu32 "_%s_%s.jpg",
                 tm->tm_hour, tm->tm_min, tm->tm_sec, seq, mac_tag, trig);
    } else {
        snprintf(out, cap, "boot-%010" PRIu32 "_%s_%s.jpg", seq, mac_tag, trig);
    }
}

bool sd_layout_is_date_dir(const char *name) {
    if (!name) return false;
    /* strict "YYYY-MM-DD" */
    if (strlen(name) != 10) return false;
    for (int i = 0; i < 10; i++) {
        char c = name[i];
        if (i == 4 || i == 7) {
            if (c != '-') return false;
        } else if (c < '0' || c > '9') {
            return false;
        }
    }
    return true;
}

static bool ends_with_jpg(const char *name) {
    size_t n = strlen(name);
    if (n < 4) return false;
    const char *e = name + n - 4;
    return e[0] == '.' && tolower((unsigned char)e[1]) == 'j' &&
           tolower((unsigned char)e[2]) == 'p' && tolower((unsigned char)e[3]) == 'g';
}

sd_bucket_kind_t sd_layout_classify(const char *name) {
    if (!name || name[0] == '\0') return SD_BUCKET_OTHER;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return SD_BUCKET_OTHER;
    if (sd_layout_is_date_dir(name)) return SD_BUCKET_DATE;
    if (strcmp(name, SD_LAYOUT_BOOT_DIR) == 0) return SD_BUCKET_BOOT;
    if (ends_with_jpg(name)) return SD_BUCKET_ROOT_JPG;
    return SD_BUCKET_OTHER;
}

/* root (legacy, pre-tree) is the oldest; then boot (pre-clock); then dated
 * days chronologically. */
static int bucket_rank(const char *n) {
    if (strcmp(n, SD_LAYOUT_ROOT) == 0) return 0;
    if (strcmp(n, SD_LAYOUT_BOOT_DIR) == 0) return 1;
    return 2;  /* a dated day */
}

int sd_layout_bucket_cmp_oldest_first(const char *a, const char *b) {
    int ra = bucket_rank(a), rb = bucket_rank(b);
    if (ra != rb) return ra - rb;
    if (ra == 2) return strcmp(a, b);  /* both dated → lexical = chronological */
    return 0;
}

int sd_layout_name_cmp_desc(const void *a, const void *b) {
    const char *sa = *(const char *const *)a;
    const char *sb = *(const char *const *)b;
    return strcmp(sb, sa);  /* descending */
}

/* days since 1970-01-01 for a proleptic-Gregorian civil date (Hinnant). */
static long days_from_civil(int y, int m, int d) {
    y -= (m <= 2);
    long era = (y >= 0 ? y : y - 399) / 400;
    int yoe = (int)(y - era * 400);
    int doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097L + doe - 719468L;
}

bool sd_layout_day_older_than(const char *day, const struct tm *now, int keep_days) {
    if (keep_days <= 0 || !now) return false;
    if (!sd_layout_is_date_dir(day)) return false;  /* root/boot → space pass */
    int y = (day[0] - '0') * 1000 + (day[1] - '0') * 100 +
            (day[2] - '0') * 10 + (day[3] - '0');
    int mo = (day[5] - '0') * 10 + (day[6] - '0');
    int d = (day[8] - '0') * 10 + (day[9] - '0');
    long day_no = days_from_civil(y, mo, d);
    long now_no = days_from_civil(now->tm_year + 1900, now->tm_mon + 1, now->tm_mday);
    return (now_no - day_no) > keep_days;
}

bool sd_layout_valid_day_param(const char *d) {
    if (!d || d[0] == '\0') return false;
    if (strcmp(d, SD_LAYOUT_ROOT) == 0) return true;
    if (strcmp(d, SD_LAYOUT_BOOT_DIR) == 0) return true;
    return sd_layout_is_date_dir(d);
}

bool sd_layout_valid_leaf(const char *f) {
    if (!f || f[0] == '\0') return false;
    if (strstr(f, "..") != NULL) return false;
    for (const char *c = f; *c; c++) {
        char ch = *c;
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
              (ch >= '0' && ch <= '9') || ch == '.' || ch == '_' || ch == '-'))
            return false;
    }
    return true;
}

void sd_layout_page_slice(size_t count, int page, int per_page,
                          size_t *start, size_t *end) {
    if (page < 0) page = 0;
    if (per_page < 1) per_page = 1;
    size_t pp = (size_t)per_page;
    /* Clamp before multiplying so a hostile ?page= (e.g. 2e9) can't wrap the
     * size_t product into an in-range-but-wrong offset. pp >= 1, so count/pp
     * is safe; page beyond that lands the whole slice past the end → empty. */
    size_t s = ((size_t)page > count / pp) ? count : (size_t)page * pp;
    if (s > count) s = count;
    size_t e = s + pp;
    if (e > count) e = count;
    if (start) *start = s;
    if (end) *end = e;
}

void sd_layout_trigger_of(const char *leaf, char *out, size_t cap) {
    if (!out || cap == 0) return;
    out[0] = '\0';
    if (!leaf) return;
    size_t nlen = strlen(leaf);
    if (nlen < 5 || !ends_with_jpg(leaf)) return;  /* need at least "_x.jpg" */
    const char *last_us = strrchr(leaf, '_');
    if (!last_us) return;
    const char *tag = last_us + 1;
    const char *dot = leaf + nlen - 4;  /* start of ".jpg" */
    if (tag >= dot) return;
    size_t tlen = (size_t)(dot - tag);
    if (tlen >= cap) tlen = cap - 1;
    memcpy(out, tag, tlen);
    out[tlen] = '\0';
}

static bool all_digits(const char *s, int n) {
    for (int i = 0; i < n; i++)
        if (s[i] < '0' || s[i] > '9') return false;
    return true;
}

bool sd_layout_legacy_target(const char *name, char *out, size_t cap) {
    if (!name || !out || cap < SD_LAYOUT_DAY_LEN) return false;
    /* Pre-sync legacy (no real clock at capture): "boot-<seq>_…". */
    if (strncmp(name, "boot-", 5) == 0) {
        snprintf(out, cap, "%s", SD_LAYOUT_BOOT_DIR);
        return true;
    }
    /* Dated legacy (old camera.c format): "YYYYMMDD-HHMMSS_…" — 8 digits, '-',
     * 6 digits, '_'. Map the date prefix to the "YYYY-MM-DD" day bucket. */
    if (strlen(name) >= 16 && all_digits(name, 8) && name[8] == '-' &&
        all_digits(name + 9, 6) && name[15] == '_') {
        out[0] = name[0]; out[1] = name[1]; out[2] = name[2]; out[3] = name[3];
        out[4] = '-';     out[5] = name[4]; out[6] = name[5];
        out[7] = '-';     out[8] = name[6]; out[9] = name[7];
        out[10] = '\0';
        return true;
    }
    return false;
}
