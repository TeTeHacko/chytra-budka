/* test_flat_json.c — host unit test for the flat-JSON helpers (flat_json.c)
 * used by net_store.c to parse cmd/endpoint payloads. Worth pinning: string
 * extraction with escapes, bool detection, and the flat-object slicer that
 * feeds {"set":{...}} into net_store (incl. braces inside string values —
 * the slicer must not stop early there).
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "flat_json.h"

int main(void) {
    char out[64];

    /* string extraction, whitespace tolerance */
    assert(fj_str("{\"a\":\"x\", \"mqtt_uri\" : \"mqtt://h:1883\"}",
                  "mqtt_uri", out, sizeof(out)));
    assert(strcmp(out, "mqtt://h:1883") == 0);

    /* escapes */
    assert(fj_str("{\"k\":\"a\\\"b\\\\c\\nd\"}", "k", out, sizeof(out)));
    assert(strcmp(out, "a\"b\\c\nd") == 0);

    /* absent key / non-string value */
    assert(!fj_str("{\"k\":123}", "k", out, sizeof(out)));
    assert(!fj_str("{\"other\":\"v\"}", "k", out, sizeof(out)));

    /* truncation is bounded + NUL-terminated */
    char tiny[4];
    assert(fj_str("{\"k\":\"abcdef\"}", "k", tiny, sizeof(tiny)));
    assert(strcmp(tiny, "abc") == 0);

    /* bools */
    assert(fj_bool_true("{\"clear\": true}", "clear"));
    assert(!fj_bool_true("{\"clear\": false}", "clear"));
    assert(!fj_bool_true("{\"clear\":\"true\"}", "clear")); /* string ≠ bool */

    /* flat-object slicer */
    size_t len = 0;
    const char *obj = fj_object(
        "{\"set\":{\"mqtt_uri\":\"mqtt://new:1883\",\"ota_url\":\"https://o/x\"}}",
        "set", &len);
    assert(obj != NULL);
    char body[128];
    snprintf(body, sizeof(body), "{%.*s}", (int)len, obj);
    assert(fj_str(body, "mqtt_uri", out, sizeof(out)));
    assert(strcmp(out, "mqtt://new:1883") == 0);
    assert(fj_str(body, "ota_url", out, sizeof(out)));
    assert(strcmp(out, "https://o/x") == 0);

    /* braces inside string values must not terminate the slice */
    obj = fj_object("{\"set\":{\"relay_tok\":\"a}b\",\"x\":\"y\"}}", "set", &len);
    assert(obj != NULL);
    snprintf(body, sizeof(body), "{%.*s}", (int)len, obj);
    assert(fj_str(body, "x", out, sizeof(out)) && strcmp(out, "y") == 0);
    assert(fj_str(body, "relay_tok", out, sizeof(out)) && strcmp(out, "a}b") == 0);

    /* missing / malformed object */
    assert(fj_object("{\"set\":\"notobj\"}", "set", &len) == NULL);
    assert(fj_object("{}", "set", &len) == NULL);

    printf("test_flat_json: all OK\n");
    return 0;
}
