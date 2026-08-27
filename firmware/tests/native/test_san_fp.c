/* test_san_fp.c — exercises tls_store_compute_san_fp.
 *
 * The SAN fingerprint is what the boot-time env-staleness check
 * compares to detect IP / domain changes. The exact byte format
 * also has to match the signer-side Python computation (enroll.py)
 * — keep this test in lock-step with signer-side test_san_fp.py. */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "tls_store.h"

/* Pull in just the canonicalisation; nvs/esp_log/etc. live behind
 * the shim. tls_store.c also pulls in nvs.h which doesn't exist on
 * host — but we only test compute_san_fp here, and that function
 * doesn't touch NVS. The Makefile compiles tls_store.c with a small
 * stub for nvs.h. */

#define ASSERT_TRUE(cond, msg) do {                                   \
    if (!(cond)) {                                                    \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg); \
        return 1;                                                     \
    }                                                                 \
} while (0)

static void print_hex(const uint8_t *b, size_t n) {
    for (size_t i = 0; i < n; i++) printf("%02x", b[i]);
}

int main(void) {
    uint8_t fp1[32], fp2[32], fp3[32];

    /* Same identity → same fingerprint. */
    tls_store_compute_san_fp("cb-ex01", "doma", "192.0.2.42", fp1);
    tls_store_compute_san_fp("cb-ex01", "doma", "192.0.2.42", fp2);
    ASSERT_TRUE(memcmp(fp1, fp2, 32) == 0,
                "identical inputs should produce identical fingerprint");

    /* IP change → different fingerprint (the test that drives the
     * env-staleness re-enroll). */
    tls_store_compute_san_fp("cb-ex01", "doma", "192.0.2.99", fp3);
    ASSERT_TRUE(memcmp(fp1, fp3, 32) != 0,
                "IP change should change fingerprint");

    /* Domain change → different fingerprint. */
    tls_store_compute_san_fp("cb-ex01", "chata", "192.0.2.42", fp3);
    ASSERT_TRUE(memcmp(fp1, fp3, 32) != 0,
                "domain change should change fingerprint");

    /* Device id change → different fingerprint. */
    tls_store_compute_san_fp("cb-ex02", "doma", "192.0.2.42", fp3);
    ASSERT_TRUE(memcmp(fp1, fp3, 32) != 0,
                "device id change should change fingerprint");

    /* No IP yet (pre-WiFi-up) — empty / NULL should be tolerated and
     * produce a deterministic fingerprint different from the with-IP
     * case. */
    tls_store_compute_san_fp("cb-ex01", "doma", NULL, fp3);
    ASSERT_TRUE(memcmp(fp1, fp3, 32) != 0,
                "removing IP should change fingerprint");

    tls_store_compute_san_fp("cb-ex01", "doma", "", fp2);
    ASSERT_TRUE(memcmp(fp2, fp3, 32) == 0,
                "empty ip_str and NULL ip_str should produce same fingerprint");

    /* No domain (fallback empty) — should still hash without crashing
     * and produce a fingerprint distinct from the with-domain case. */
    tls_store_compute_san_fp("cb-ex01", "", NULL, fp3);
    tls_store_compute_san_fp("cb-ex01", NULL, NULL, fp2);
    ASSERT_TRUE(memcmp(fp2, fp3, 32) == 0,
                "empty and NULL domain should produce same fingerprint");
    ASSERT_TRUE(memcmp(fp1, fp3, 32) != 0,
                "removing domain should change fingerprint");

    /* Empty device id → still safe (no entries hashed, sha256 of
     * empty input). Not a realistic scenario but the helper shouldn't
     * dereference NULL. */
    tls_store_compute_san_fp(NULL, NULL, NULL, fp1);
    /* SHA-256("") = e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855 */
    const uint8_t sha256_empty[32] = {
        0xe3,0xb0,0xc4,0x42,0x98,0xfc,0x1c,0x14,0x9a,0xfb,0xf4,0xc8,
        0x99,0x6f,0xb9,0x24,0x27,0xae,0x41,0xe4,0x64,0x9b,0x93,0x4c,
        0xa4,0x95,0x99,0x1b,0x78,0x52,0xb8,0x55,
    };
    ASSERT_TRUE(memcmp(fp1, sha256_empty, 32) == 0,
                "no-inputs fingerprint should be sha256 of empty string");

    /* Print one fingerprint for human eyeball (helps debug signer-side
     * Python implementation if it diverges). */
    printf("fp(ex01, doma, 192.0.2.42) = ");
    tls_store_compute_san_fp("cb-ex01", "doma",
                              "192.0.2.42", fp1);
    print_hex(fp1, 32);
    printf("\n");

    printf("OK test_san_fp — canonicalisation stable, edges handled\n");
    return 0;
}
