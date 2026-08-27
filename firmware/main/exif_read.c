/* exif_read.c — see exif_read.h.
 *
 * Mirrors the layout written by jpeg_stamp.c:build_exif_app1():
 *   JPEG SOI (FF D8) → APP1 (FF E1) → 2-byte BE length → "Exif\0\0" →
 *   TIFF header (II/MM + 0x002A magic + 4-byte IFD0 offset) →
 *   IFD0 (entries + ExifIFDPointer 0x8769) → ExifSubIFD.
 * All IFD offsets are relative to the start of the TIFF header. Every read is
 * bounds-checked against the end of the APP1 payload — the JPEG comes off an
 * SD card and may be truncated or corrupt. */

#include "exif_read.h"

#include <stdlib.h>
#include <string.h>

/* ── byte readers (endianness picked from the TIFF header) ─────────── */

static uint16_t rd16(const uint8_t *p, bool le) {
    return le ? (uint16_t)(p[0] | ((uint16_t)p[1] << 8))
              : (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}
static uint32_t rd32(const uint8_t *p, bool le) {
    return le ? ((uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                 ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24))
              : ((uint32_t)p[3] | ((uint32_t)p[2] << 8) |
                 ((uint32_t)p[1] << 16) | ((uint32_t)p[0] << 24));
}

/* Copy an ASCII/UNDEFINED field, stopping at NUL or `count`, dropping control
 * bytes, NUL-terminating, never overflowing `cap`. */
static void copy_str(const uint8_t *data, uint32_t count, char *out, size_t cap) {
    size_t o = 0;
    for (uint32_t i = 0; i < count && o + 1 < cap; i++) {
        uint8_t c = data[i];
        if (c == 0) break;
        if (c >= 0x20 && c != 0x7f) out[o++] = (char)c;
    }
    out[o] = 0;
}

/* TIFF field type → element size in bytes (0 for types we don't size). */
static uint32_t type_size(uint16_t type) {
    switch (type) {
        case 1: case 2: case 6: case 7: return 1;  /* BYTE/ASCII/SBYTE/UNDEFINED */
        case 3: case 8:                 return 2;  /* SHORT/SSHORT */
        case 4: case 9: case 11:        return 4;  /* LONG/SLONG/FLOAT */
        case 5: case 10: case 12:       return 8;  /* RATIONAL/SRATIONAL/DOUBLE */
        default:                        return 1;
    }
}

/* Resolve a directory entry's value bytes. For payloads ≤4 bytes the value is
 * stored inline in the entry; otherwise the entry holds an offset (from TIFF
 * start) to the data. Returns NULL (with *bytes=0) if the data would fall
 * outside the segment. */
static const uint8_t *entry_data(const uint8_t *entry, const uint8_t *tiff,
                                 size_t avail, bool le, uint32_t *bytes) {
    uint16_t type  = rd16(entry + 2, le);
    uint32_t count = rd32(entry + 4, le);
    *bytes = 0;
    if (count > 0x10000u) return NULL;        /* sanity cap on absurd counts */
    uint32_t total = count * type_size(type);
    if (total <= 4) {
        *bytes = total;
        return entry + 8;                     /* inline value */
    }
    uint32_t off = rd32(entry + 8, le);
    if (off > avail || total > avail - off) return NULL;
    *bytes = total;
    return tiff + off;
}

/* Walk one IFD. For IFD0 pass exif_sub_off non-NULL to capture the
 * ExifIFDPointer (0x8769); for the ExifSubIFD pass NULL. */
static void walk_ifd(const uint8_t *tiff, size_t avail, bool le,
                     uint32_t ifd_off, exif_meta_t *out,
                     uint32_t *exif_sub_off) {
    if (ifd_off > avail || avail - ifd_off < 2) return;
    const uint8_t *ifd = tiff + ifd_off;
    uint16_t nent = rd16(ifd, le);
    size_t room = (avail - ifd_off - 2) / 12;   /* entries that actually fit */
    if (nent > room) nent = (uint16_t)room;

    const uint8_t *e = ifd + 2;
    for (uint16_t i = 0; i < nent; i++, e += 12) {
        uint16_t tag = rd16(e, le);
        if (exif_sub_off && tag == 0x8769) {     /* ExifIFDPointer (LONG) */
            *exif_sub_off = rd32(e + 8, le);
            continue;
        }
        uint32_t bytes = 0;
        const uint8_t *d;
        switch (tag) {
            case 0x010E: /* ImageDescription → trigger */
                d = entry_data(e, tiff, avail, le, &bytes);
                if (d) copy_str(d, bytes, out->trigger, sizeof(out->trigger));
                break;
            case 0x0110: /* Model → device id */
                d = entry_data(e, tiff, avail, le, &bytes);
                if (d) copy_str(d, bytes, out->model, sizeof(out->model));
                break;
            case 0x0131: /* Software → fw version */
                d = entry_data(e, tiff, avail, le, &bytes);
                if (d) copy_str(d, bytes, out->software, sizeof(out->software));
                break;
            case 0x0132: /* DateTime — fallback only if no DateTimeOriginal yet */
                if (out->datetime[0] == 0) {
                    d = entry_data(e, tiff, avail, le, &bytes);
                    if (d) copy_str(d, bytes, out->datetime, sizeof(out->datetime));
                }
                break;
            case 0x9003: /* DateTimeOriginal — preferred (overwrites 0x0132) */
                d = entry_data(e, tiff, avail, le, &bytes);
                if (d) copy_str(d, bytes, out->datetime, sizeof(out->datetime));
                break;
            case 0x9010: /* OffsetTime */
                d = entry_data(e, tiff, avail, le, &bytes);
                if (d) copy_str(d, bytes, out->offset, sizeof(out->offset));
                break;
            case 0x9286: /* UserComment — 8-byte charset prefix + JSON */
                d = entry_data(e, tiff, avail, le, &bytes);
                if (d && bytes > 8)
                    copy_str(d + 8, bytes - 8, out->user_json, sizeof(out->user_json));
                break;
            case 0xA434: /* LensModel → sensor */
                d = entry_data(e, tiff, avail, le, &bytes);
                if (d) copy_str(d, bytes, out->lens, sizeof(out->lens));
                break;
            default:
                break;
        }
    }
}

bool exif_read(const uint8_t *jpeg, size_t len, exif_meta_t *out) {
    if (out) memset(out, 0, sizeof(*out));
    if (!jpeg || !out || len < 12) return false;
    if (jpeg[0] != 0xFF || jpeg[1] != 0xD8) return false;   /* SOI */

    /* Walk JPEG marker segments looking for APP1 "Exif". */
    const uint8_t *tiff = NULL;
    size_t avail = 0;
    size_t p = 2;
    while (p + 4 <= len) {
        if (jpeg[p] != 0xFF) break;                         /* not a marker */
        uint8_t marker = jpeg[p + 1];
        if (marker == 0xD9 || marker == 0xDA) break;        /* EOI / start of scan */
        if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) {
            p += 2;                                         /* standalone marker */
            continue;
        }
        uint16_t seglen = (uint16_t)(((uint16_t)jpeg[p + 2] << 8) | jpeg[p + 3]);
        if (seglen < 2) break;
        size_t seg_payload = p + 4;                         /* after the length field */
        size_t seg_end = p + 2 + seglen;                    /* length counts itself */
        if (seg_end > len) break;                           /* truncated segment */
        if (marker == 0xE1 && seglen >= 8 &&
            memcmp(jpeg + seg_payload, "Exif\0\0", 6) == 0) {
            tiff  = jpeg + seg_payload + 6;
            avail = seg_end - (seg_payload + 6);
            break;
        }
        p = seg_end;
    }
    if (!tiff || avail < 8) return false;

    /* TIFF header: byte order, magic, IFD0 offset. */
    bool le;
    if (tiff[0] == 'I' && tiff[1] == 'I') le = true;
    else if (tiff[0] == 'M' && tiff[1] == 'M') le = false;
    else return false;
    if (rd16(tiff + 2, le) != 0x002A) return false;
    uint32_t ifd0_off = rd32(tiff + 4, le);

    uint32_t sub_off = 0;
    walk_ifd(tiff, avail, le, ifd0_off, out, &sub_off);
    if (sub_off) walk_ifd(tiff, avail, le, sub_off, out, NULL);

    out->have_dt = (out->datetime[0] != 0 &&
                    strncmp(out->datetime, "1970", 4) != 0);
    return true;
}

bool exif_json_num(const char *json, const char *key, double *out) {
    if (!json || !key || !out) return false;
    size_t klen = strlen(key);
    if (klen == 0) return false;
    for (const char *p = json; (p = strchr(p, '"')) != NULL; p++) {
        if (strncmp(p + 1, key, klen) == 0 && p[1 + klen] == '"') {
            const char *q = p + 1 + klen + 1;       /* past the closing quote */
            while (*q == ' ' || *q == '\t') q++;
            if (*q != ':') { p = q - 1; continue; } /* "key" used as a value, skip */
            q++;
            while (*q == ' ' || *q == '\t') q++;
            char *end = NULL;
            double v = strtod(q, &end);
            if (end == q) return false;             /* not a number */
            *out = v;
            return true;
        }
    }
    return false;
}
