/* flat_json.h — minimal flat-JSON field extraction (no cJSON dependency —
 * not bundled in IDF v6.0.1). Sufficient for the flat {"key":"value",...}
 * command payloads (cmd/wifi, cmd/auth, cmd/endpoint); NOT a general JSON
 * parser. Extracted from the static helpers in mqtt.c so net_store.c can
 * share them (mqtt.c migration to these symbols is a follow-up cleanup). */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Extract a JSON string value: "<key>" : "<value>" → out (un-escapes
 * \" \\ \/ \n \t \r). Returns true if the key+string was found. */
bool fj_str(const char *json, const char *key, char *out, size_t cap);

/* True if "<key>" is present with boolean literal true. */
bool fj_bool_true(const char *json, const char *key);

/* Locate the flat object value of "<key>": returns a pointer to the char
 * after the opening '{' and sets *len to the span up to the matching '}'
 * (no nested objects supported). NULL when absent/malformed. */
const char *fj_object(const char *json, const char *key, size_t *len);

#ifdef __cplusplus
}
#endif
