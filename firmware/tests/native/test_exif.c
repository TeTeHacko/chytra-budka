/* test_exif.c — host unit tests for exif_read.c.
 *
 * exif_read.c is pure libc (no ESP-IDF), so it compiles + runs on the host.
 * We can't easily drive the real writer here (jpeg_stamp.c pulls in
 * esp_camera/battery/diag/mqtt/wifi_mgr), so this test builds a faithful EXIF
 * APP1 segment by hand — same TIFF/IFD layout jpeg_stamp.c emits — and asserts
 * the reader round-trips every field, in BOTH byte orders, and stays graceful
 * on malformed input. The round-trip against the *real* writer is verified on
 * the bench (fetch /photo/exif for a freshly captured photo). */

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "exif_read.h"

static int g_checks = 0;
#define CHECK(cond)                                                          \
    do {                                                                     \
        g_checks++;                                                          \
        if (!(cond)) {                                                       \
            fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            return 1;                                                        \
        }                                                                    \
    } while (0)
#define CHECK_F(a, b) CHECK(fabs((double)(a) - (double)(b)) < 1e-6)

/* ── tiny EXIF builder (mirrors jpeg_stamp.c:build_exif_app1 layout) ──── */

static void wr16(uint8_t *p, uint16_t v, int le) {
    if (le) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
    else    { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
}
static void wr32(uint8_t *p, uint32_t v, int le) {
    if (le) { p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24); }
    else    { p[0]=(uint8_t)(v>>24); p[1]=(uint8_t)(v>>16); p[2]=(uint8_t)(v>>8); p[3]=(uint8_t)v; }
}
static void emit(uint8_t *e, uint16_t tag, uint16_t type, uint32_t count,
                 uint32_t val, int le) {
    wr16(e, tag, le); wr16(e + 2, type, le);
    wr32(e + 4, count, le); wr32(e + 8, val, le);
}

/* Build SOI + APP1(Exif) + EOI into buf. IFD0 (5 entries) ends at 74,
 * ExifSubIFD (4 entries) ends at 128, external data starts at 128.
 * Returns total JPEG length. */
static size_t build_exif_jpeg(uint8_t *buf, int le, const char *dt,
                              const char *user_json) {
    uint8_t *tiff = buf + 12;            /* after SOI + FFE1 + len + "Exif\0\0" */
    tiff[0] = le ? 'I' : 'M';
    tiff[1] = le ? 'I' : 'M';
    wr16(tiff + 2, 0x002A, le);
    wr32(tiff + 4, 8, le);

    const uint32_t sub_off = 74;
    uint32_t ext = 128;

    uint8_t *e = tiff + 8;
    wr16(e, 5, le);                      /* IFD0 entry count */
    e += 2;

    /* 0x010E ImageDescription "pir" — inline (4 bytes incl NUL). */
    wr16(e, 0x010E, le); wr16(e + 2, 2, le); wr32(e + 4, 4, le);
    e[8] = e[9] = e[10] = e[11] = 0; memcpy(e + 8, "pir", 4); e += 12;

    /* 0x0110 Model (offset). */
    { const char *s = "cb-ex01"; uint32_t n = (uint32_t)strlen(s) + 1;
      memcpy(tiff + ext, s, n); emit(e, 0x0110, 2, n, ext, le); e += 12; ext += n; }
    /* 0x0131 Software (offset). */
    { const char *s = "v0.4.0-test"; uint32_t n = (uint32_t)strlen(s) + 1;
      memcpy(tiff + ext, s, n); emit(e, 0x0131, 2, n, ext, le); e += 12; ext += n; }
    /* 0x0132 DateTime (offset, 20). */
    memcpy(tiff + ext, dt, 20); emit(e, 0x0132, 2, 20, ext, le); e += 12; ext += 20;
    /* 0x8769 ExifIFDPointer → SubIFD. */
    emit(e, 0x8769, 4, 1, sub_off, le); e += 12;
    wr32(e, 0, le);                      /* IFD0 next = 0 */

    e = tiff + sub_off;
    wr16(e, 4, le);                      /* ExifSubIFD entry count */
    e += 2;
    /* 0x9003 DateTimeOriginal (offset, 20). */
    memcpy(tiff + ext, dt, 20); emit(e, 0x9003, 2, 20, ext, le); e += 12; ext += 20;
    /* 0x9010 OffsetTime "+02:00" (offset, 7). */
    { const char *s = "+02:00"; uint32_t n = (uint32_t)strlen(s) + 1;
      memcpy(tiff + ext, s, n); emit(e, 0x9010, 2, n, ext, le); e += 12; ext += n; }
    /* 0x9286 UserComment — "ASCII\0\0\0" + JSON. */
    { uint32_t jl = (uint32_t)strlen(user_json); uint32_t tot = 8 + jl;
      memcpy(tiff + ext, "ASCII\0\0\0", 8); memcpy(tiff + ext + 8, user_json, jl);
      emit(e, 0x9286, 7, tot, ext, le); e += 12; ext += tot; }
    /* 0xA434 LensModel "OV3660" (offset). */
    { const char *s = "OV3660"; uint32_t n = (uint32_t)strlen(s) + 1;
      memcpy(tiff + ext, s, n); emit(e, 0xA434, 2, n, ext, le); e += 12; ext += n; }
    wr32(e, 0, le);                      /* SubIFD next = 0 */

    uint32_t tiff_len = ext;
    buf[0] = 0xFF; buf[1] = 0xD8;        /* SOI */
    buf[2] = 0xFF; buf[3] = 0xE1;        /* APP1 */
    uint32_t app1 = 2 + 6 + tiff_len;    /* length field + "Exif\0\0" + tiff */
    buf[4] = (uint8_t)(app1 >> 8); buf[5] = (uint8_t)app1;   /* BE length */
    memcpy(buf + 6, "Exif\0\0", 6);
    size_t total = 12 + tiff_len;
    buf[total] = 0xFF; buf[total + 1] = 0xD9; total += 2;    /* EOI */
    return total;
}

static const char *UJ =
    "{\"seq\":42,\"uptime_s\":1234,\"capture_ms\":88,\"agc\":8,\"ir\":1,"
    "\"framesize\":14,\"quality\":12,\"rssi\":-67,\"heap\":120000,"
    "\"mcu_c\":24.5,\"vbatt\":4.012,\"soc\":92.0}";

static int test_roundtrip(int le) {
    uint8_t buf[1024];
    memset(buf, 0, sizeof(buf));
    size_t n = build_exif_jpeg(buf, le, "2026:06:04 14:30:22", UJ);
    CHECK(n > 0 && n < sizeof(buf));

    exif_meta_t m;
    CHECK(exif_read(buf, n, &m) == true);
    CHECK(m.have_dt == true);
    CHECK(strcmp(m.datetime, "2026:06:04 14:30:22") == 0);
    CHECK(strcmp(m.offset, "+02:00") == 0);
    CHECK(strcmp(m.trigger, "pir") == 0);
    CHECK(strcmp(m.model, "cb-ex01") == 0);
    CHECK(strcmp(m.software, "v0.4.0-test") == 0);
    CHECK(strcmp(m.lens, "OV3660") == 0);
    CHECK(strcmp(m.user_json, UJ) == 0);

    double v;
    CHECK(exif_json_num(m.user_json, "agc", &v) && (int)v == 8);
    CHECK(exif_json_num(m.user_json, "ir", &v) && (int)v == 1);
    CHECK(exif_json_num(m.user_json, "rssi", &v) && (int)v == -67);
    CHECK(exif_json_num(m.user_json, "seq", &v) && (int)v == 42);
    CHECK(exif_json_num(m.user_json, "mcu_c", &v)); CHECK_F(v, 24.5);
    CHECK(exif_json_num(m.user_json, "vbatt", &v)); CHECK_F(v, 4.012);
    CHECK(exif_json_num(m.user_json, "soc", &v));   CHECK_F(v, 92.0);
    /* missing key + substring-of-key must not match */
    CHECK(exif_json_num(m.user_json, "nope", &v) == false);
    CHECK(exif_json_num(m.user_json, "oc", &v) == false);
    return 0;
}

static int test_sentinel(void) {
    uint8_t buf[1024];
    memset(buf, 0, sizeof(buf));
    size_t n = build_exif_jpeg(buf, 1, "1970:01:01 00:00:00", UJ);
    exif_meta_t m;
    CHECK(exif_read(buf, n, &m) == true);
    CHECK(strcmp(m.datetime, "1970:01:01 00:00:00") == 0);
    CHECK(m.have_dt == false);          /* pre-SNTP sentinel → no real clock */
    return 0;
}

static int test_malformed(void) {
    uint8_t buf[1024];
    exif_meta_t m;

    /* NULL / too-short / no SOI. */
    CHECK(exif_read(NULL, 100, &m) == false);
    memset(buf, 0, sizeof(buf));
    CHECK(exif_read(buf, 4, &m) == false);
    CHECK(exif_read(buf, sizeof(buf), &m) == false);    /* all zeros, no SOI */

    /* SOI + EOI only, no APP1. */
    buf[0] = 0xFF; buf[1] = 0xD8; buf[2] = 0xFF; buf[3] = 0xD9;
    CHECK(exif_read(buf, 4, &m) == false);

    /* Good blob, then truncate mid-segment → reader must reject, not crash. */
    memset(buf, 0, sizeof(buf));
    size_t n = build_exif_jpeg(buf, 1, "2026:06:04 14:30:22", UJ);
    CHECK(exif_read(buf, n - 60, &m) == false);

    /* Corrupt magic → false. */
    memset(buf, 0, sizeof(buf));
    n = build_exif_jpeg(buf, 1, "2026:06:04 14:30:22", UJ);
    buf[12 + 2] = 0x00; buf[12 + 3] = 0x00;             /* clobber 0x002A */
    CHECK(exif_read(buf, n, &m) == false);

    /* Out-of-bounds value offset on the Model entry → that field empty, others
     * still parse, no OOB read. Model is IFD0 entry #1; its value field sits at
     * tiff(+12) + 8 + 2 + 12 + 8 = +30. */
    memset(buf, 0, sizeof(buf));
    n = build_exif_jpeg(buf, 1, "2026:06:04 14:30:22", UJ);
    wr32(buf + 12 + 30, 0xFFFFFFFFu, 1);
    CHECK(exif_read(buf, n, &m) == true);
    CHECK(m.model[0] == 0);             /* dropped: offset out of bounds */
    CHECK(strcmp(m.trigger, "pir") == 0);   /* siblings unaffected */
    CHECK(strcmp(m.datetime, "2026:06:04 14:30:22") == 0);

    /* Absurd IFD0 offset → no entries walked, header still valid → true, empty. */
    memset(buf, 0, sizeof(buf));
    n = build_exif_jpeg(buf, 1, "2026:06:04 14:30:22", UJ);
    wr32(buf + 12 + 4, 0x00FFFFFFu, 1);
    CHECK(exif_read(buf, n, &m) == true);
    CHECK(m.datetime[0] == 0 && m.trigger[0] == 0);
    return 0;
}

int main(void) {
    int rc = test_roundtrip(1)   /* little-endian (II) */
           || test_roundtrip(0)  /* big-endian (MM) */
           || test_sentinel()
           || test_malformed();
    if (rc) { fprintf(stderr, "test_exif FAILED\n"); return rc; }
    printf("  test_exif OK (%d checks)\n", g_checks);
    return 0;
}
