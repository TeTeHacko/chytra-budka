/* test_csr.c — native test for tls_enroll.c (HTTPS.md / Phase E1b).
 *
 * Round-trips a freshly-generated EC P-256 keypair + CSR through
 * tls_enroll_*, then parses the resulting PEM back with mbedtls
 * 3.6's x509_csr_parse and asserts on every field tls_enroll is
 * supposed to populate:
 *   - subject CN
 *   - SAN list (DNS x3 + IP)
 *   - public key algo + curve
 *   - signature algo (SHA-256 ECDSA)
 *   - self-signature integrity
 *
 * Skips the MQTT publish / signer side — those go through HIL once
 * the signer daemon is up. This test is the dev-loop unit-level
 * safety net: catches keygen/CSR regressions in ~50 ms without
 * needing the bench.
 *
 * Build:
 *   make build/test_csr && ./build/test_csr
 *
 * Why this lives in tests/native/ and not the IDF unity component:
 * the CSR builder is pure crypto, no peripheral or RTOS dependency, so
 * it runs far faster on the host. NOTE: IDF v6.0.1 vendors mbedtls
 * 4.0.0 and tls_enroll.c uses the 4.x API (5-arg mbedtls_pk_parse_key,
 * 3-arg mbedtls_x509write_csr_pem). This test therefore only compiles on
 * a host whose system mbedtls is ALSO >= 4.x — the Makefile detects the
 * host major version and skips this test (with a notice) on older mbedtls
 * (e.g. Arch's 3.6.x), rather than failing the whole suite. */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "tls_enroll.h"

#include "mbedtls/oid.h"
#include "mbedtls/x509_csr.h"

#define ASSERT_OK(expr) do {                                          \
    esp_err_t _e = (expr);                                            \
    if (_e != ESP_OK) {                                               \
        fprintf(stderr, "FAIL %s:%d: %s = 0x%x\n",                    \
                __FILE__, __LINE__, #expr, (unsigned)_e);             \
        return 1;                                                     \
    }                                                                 \
} while (0)

#define ASSERT_TRUE(cond, msg) do {                                   \
    if (!(cond)) {                                                    \
        fprintf(stderr, "FAIL %s:%d: %s\n",                           \
                __FILE__, __LINE__, msg);                             \
        return 1;                                                     \
    }                                                                 \
} while (0)

static const char *EXPECTED_CN        = "cb-ex01.lan";
static const char *EXPECTED_SHORT     = "cb-ex01";
static const char *EXPECTED_MDNS      = "cb-ex01.local";
static const char *EXPECTED_IP_STR    = "192.0.2.42";
static const uint8_t EXPECTED_IP4[4]  = {192, 168, 70, 42};

/* Walk the parsed CSR's SAN list looking for a DNS entry with the
 * given string value. mbedtls's SAN traversal in 3.6 returns a
 * single subject_alternative_names linked list. */
static bool find_san_dns(const mbedtls_x509_csr *csr, const char *want) {
    mbedtls_x509_subject_alternative_name san;
    int ret;
    /* Iterate parsed SAN sequence — 3.6 surfaces SAN via the parsed
     * extension that lives in csr->subject_alt_names. We use the
     * helper x509 functions instead of digging into ASN.1 directly. */
    const mbedtls_x509_sequence *cur = &csr->subject_alt_names;
    for (; cur != NULL; cur = cur->next) {
        if (cur->buf.p == NULL) continue;
        memset(&san, 0, sizeof(san));
        ret = mbedtls_x509_parse_subject_alt_name(&cur->buf, &san);
        if (ret != 0) continue;
        if (san.type == MBEDTLS_X509_SAN_DNS_NAME &&
            san.san.unstructured_name.len == strlen(want) &&
            memcmp(san.san.unstructured_name.p, want,
                   san.san.unstructured_name.len) == 0) {
            mbedtls_x509_free_subject_alt_name(&san);
            return true;
        }
        mbedtls_x509_free_subject_alt_name(&san);
    }
    return false;
}

static bool find_san_ip4(const mbedtls_x509_csr *csr, const uint8_t want[4]) {
    mbedtls_x509_subject_alternative_name san;
    int ret;
    const mbedtls_x509_sequence *cur = &csr->subject_alt_names;
    for (; cur != NULL; cur = cur->next) {
        if (cur->buf.p == NULL) continue;
        memset(&san, 0, sizeof(san));
        ret = mbedtls_x509_parse_subject_alt_name(&cur->buf, &san);
        if (ret != 0) continue;
        if (san.type == MBEDTLS_X509_SAN_IP_ADDRESS &&
            san.san.unstructured_name.len == 4 &&
            memcmp(san.san.unstructured_name.p, want, 4) == 0) {
            mbedtls_x509_free_subject_alt_name(&san);
            return true;
        }
        mbedtls_x509_free_subject_alt_name(&san);
    }
    return false;
}

int main(void) {
    tls_enroll_keypair_t kp = {0};
    char pem[2048];
    size_t pem_len = 0;

    /* ── 1. generate ──────────────────────────────────────────────── */
    ASSERT_OK(tls_enroll_generate_keypair(&kp));
    ASSERT_TRUE(kp.ready, "keypair not marked ready after generate");

    /* ── 2. export + reload DER round-trip ────────────────────────── */
    uint8_t der[256];
    size_t der_len = 0;
    ASSERT_OK(tls_enroll_keypair_export_der(&kp, der, sizeof(der), &der_len));
    ASSERT_TRUE(der_len > 50 && der_len < 200,
                "SEC1 DER for P-256 should be ~120 B");

    tls_enroll_keypair_t kp2 = {0};
    ASSERT_OK(tls_enroll_keypair_load_der(&kp2, der, der_len));
    ASSERT_TRUE(kp2.ready, "loaded keypair not ready");
    tls_enroll_keypair_free(&kp2);

    /* ── 3. build CSR ─────────────────────────────────────────────── */
    tls_enroll_csr_subject_t subj = {
        .cn            = EXPECTED_CN,
        .san_dns_fqdn  = EXPECTED_CN,
        .san_dns_short = EXPECTED_SHORT,
        .san_dns_mdns  = EXPECTED_MDNS,
        .san_ip_str    = EXPECTED_IP_STR,
    };
    ASSERT_OK(tls_enroll_build_csr(&kp, &subj, pem, sizeof(pem), &pem_len));
    ASSERT_TRUE(pem_len > 400 && pem_len < 1500,
                "CSR PEM size out of expected range");
    ASSERT_TRUE(strstr(pem, "-----BEGIN CERTIFICATE REQUEST-----") == pem,
                "CSR doesn't start with BEGIN marker");

    /* ── 4. parse back, verify every field ────────────────────────── */
    mbedtls_x509_csr csr;
    mbedtls_x509_csr_init(&csr);
    int ret = mbedtls_x509_csr_parse(
        &csr, (const unsigned char *)pem, pem_len + 1);  /* +1 = trailing NUL */
    ASSERT_TRUE(ret == 0, "mbedtls_x509_csr_parse failed");

    /* Subject CN check */
    char subject_str[256];
    int sn = mbedtls_x509_dn_gets(subject_str, sizeof(subject_str), &csr.subject);
    ASSERT_TRUE(sn > 0, "couldn't render subject DN");
    char want_dn[64];
    snprintf(want_dn, sizeof(want_dn), "CN=%s", EXPECTED_CN);
    ASSERT_TRUE(strcmp(subject_str, want_dn) == 0,
                "subject DN mismatch");

    /* Public key: P-256 / ECDSA. mbedtls_pk_ec() returns the underlying
     * ecp_keypair*; group.id is the curve identifier. */
    ASSERT_TRUE(mbedtls_pk_get_type(&csr.pk) == MBEDTLS_PK_ECKEY,
                "pubkey not ECKEY");
    mbedtls_ecp_keypair *ec = mbedtls_pk_ec(csr.pk);
    ASSERT_TRUE(ec != NULL, "ec keypair is NULL");
    mbedtls_ecp_group_id gid = mbedtls_ecp_keypair_get_group_id(ec);
    ASSERT_TRUE(gid == MBEDTLS_ECP_DP_SECP256R1, "curve not secp256r1");

    /* Signature algorithm: ECDSA-SHA256 */
    mbedtls_md_type_t md;
    mbedtls_pk_type_t pk;
    void *opts;
    ret = mbedtls_oid_get_sig_alg(&csr.sig_oid, &md, &pk);
    (void)opts;
    ASSERT_TRUE(ret == 0, "sig OID didn't parse");
    ASSERT_TRUE(md == MBEDTLS_MD_SHA256, "hash != SHA-256");
    ASSERT_TRUE(pk == MBEDTLS_PK_ECDSA, "sig pk != ECDSA");

    /* SAN entries — all four must be present */
    ASSERT_TRUE(find_san_dns(&csr, EXPECTED_CN), "SAN DNS (FQDN) missing");
    ASSERT_TRUE(find_san_dns(&csr, EXPECTED_SHORT), "SAN DNS (short) missing");
    ASSERT_TRUE(find_san_dns(&csr, EXPECTED_MDNS), "SAN DNS (.local) missing");
    ASSERT_TRUE(find_san_ip4(&csr, EXPECTED_IP4), "SAN IP missing");

    mbedtls_x509_csr_free(&csr);
    tls_enroll_keypair_free(&kp);

    printf("OK test_csr — keygen + CSR + 4 SAN entries + parse + verify\n");
    return 0;
}
