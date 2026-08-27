/* flat_json.c — see flat_json.h. The str/bool helpers are verbatim ports of
 * the (still-private) statics in mqtt.c. */

#include "flat_json.h"

#include <stdio.h>
#include <string.h>

bool fj_str(const char *json, const char *key, char *out, size_t cap) {
    out[0] = 0;
    char pat[24];
    int pn = snprintf(pat, sizeof(pat), "\"%s\"", key);
    if (pn <= 0 || pn >= (int)sizeof(pat)) return false;
    const char *p = strstr(json, pat);
    if (!p) return false;
    p += pn;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p != ':') return false;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p != '"') return false;
    p++;
    size_t o = 0;
    while (*p && *p != '"' && o + 1 < cap) {
        if (*p == '\\' && p[1]) {
            p++;
            switch (*p) {
                case 'n': out[o++] = '\n'; break;
                case 't': out[o++] = '\t'; break;
                case 'r': out[o++] = '\r'; break;
                default:  out[o++] = *p;   break; /* \" \\ \/ → literal */
            }
            p++;
        } else {
            out[o++] = *p++;
        }
    }
    out[o] = 0;
    return true;
}

bool fj_bool_true(const char *json, const char *key) {
    char pat[24];
    int pn = snprintf(pat, sizeof(pat), "\"%s\"", key);
    if (pn <= 0 || pn >= (int)sizeof(pat)) return false;
    const char *p = strstr(json, pat);
    if (!p) return false;
    p += pn;
    while (*p == ' ' || *p == '\t' || *p == ':' || *p == '\n' || *p == '\r') p++;
    return strncmp(p, "true", 4) == 0;
}

const char *fj_object(const char *json, const char *key, size_t *len) {
    if (len) *len = 0;
    char pat[24];
    int pn = snprintf(pat, sizeof(pat), "\"%s\"", key);
    if (pn <= 0 || pn >= (int)sizeof(pat)) return NULL;
    const char *p = strstr(json, pat);
    if (!p) return NULL;
    p += pn;
    while (*p == ' ' || *p == '\t' || *p == ':' || *p == '\n' || *p == '\r') p++;
    if (*p != '{') return NULL;
    p++;
    const char *end = p;
    /* Flat object: scan to the next '}' outside a string literal. */
    bool in_str = false;
    while (*end) {
        if (in_str) {
            if (*end == '\\' && end[1]) end++;
            else if (*end == '"') in_str = false;
        } else if (*end == '"') {
            in_str = true;
        } else if (*end == '}') {
            if (len) *len = (size_t)(end - p);
            return p;
        }
        end++;
    }
    return NULL;
}
