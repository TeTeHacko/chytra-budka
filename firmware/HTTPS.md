# HTTPS + per-device certs + auto-enrollment

> **Status note:** this doc is the original design + the MQTT-transport
> enrollment flow. The firmware today supports BOTH transports: the MQTT
> `cmd/enroll` flow described here, and a direct HTTPS enrollment against the
> [`server/`](../server/README.md) manager (CSR POST + TOFU approval) — the
> standalone stack uses the latter.

The board ships HTTP-only out of the box. This doc covers moving
every HTTP listener (`/mic.wav`, `/stream.mjpg`, `/photos`,
`/last.jpg`, `/capture`, the homepage, all `/debug/*`) behind TLS,
with per-device certs auto-enrolled against an internal sub-CA over
the existing MQTT broker. Plain HTTP stays as a fallback only until
the first enrollment lands; once a cert exists, port 80 becomes a
redirect to 443 plus HSTS.

## Why not "just HTTP on a trusted VLAN"

That was the v1 mitigation and it covers ~90 % of the threat. The
remaining 10 % — anyone who joins the budka wifi (guest who got
the password, an unpatched device on the LAN, the operator forgetting
the network is bridged to guest wifi for a weekend) — sees the live
mic + camera in clear. The bar this project sets itself is "properly,
paranoid hobby tier", so HTTPS goes in.

This is not protection against a determined attacker who already has
shell on the broker host. It IS protection against passive sniffing
on the LAN and against opportunistic browser-side capture of the live
camera stream.

## Trust hierarchy

> **Where the CA comes from:** you don't need any pre-existing PKI —
> [`server/scripts/dev-pki.sh`](../server/scripts/dev-pki.sh) generates the
> whole root + sub-CA pair (a plain 10-year hierarchy; keep the root key
> offline and it serves a real deployment fine). The author mints theirs from
> an existing config-managed internal root instead — an implementation
> detail, not something a self-hosted deployment needs.

```
Internal Root CA               (offline-ish, <offline-root location>, 20y validity,
                               only ever signs sub-CAs)
  └── Chytra Budka Sub-CA     (online on server-host, 10y validity,
                               signs device certs at runtime)
        ├── cb-ex01.lan   (90d, ECDSA P-256)
        ├── cb-ex02.lan
        ├── cb-<id>.lan   (future boards)
        └── …
```

**Why a sub-CA at all** — the sub-CA's private key lives on a server
that runs continuous CSR-signing. If that host gets compromised the
attacker can mint a cert for any `cb-*` name. With a
sub-CA, the blast radius is the budka fleet: revoke the sub-CA from
root (a one-line re-issue at the root), reissue from a fresh sub-CA,
field units re-enroll automatically on next renewal cycle. Without a
sub-CA we'd be using the root for live signing, exposing it to the
same compromise risk without any containment.

**Sub-CA constraints** (baked in at issue time):

| Constraint | Value |
| --- | --- |
| `basicConstraints` | `critical, CA:TRUE, pathlen:0` (can't issue further sub-CAs) |
| `keyUsage` | `critical, keyCertSign, cRLSign, digitalSignature` |
| `extendedKeyUsage` | `serverAuth` only (no clientAuth, codeSigning, anything else) |
| `nameConstraints` | `permitted: DNS:.lan, DNS:.lan, DNS:.local; excluded: DNS:.com, DNS:.org, …` |
| Curve | secp256r1 (P-256) — matches devices, avoids mismatch perf hit |
| Validity | 10 years from issue |

The name constraints mean a stolen sub-CA can ONLY issue certs valid
for `*.lan`/`*.lan`/`*.local` — useless for mounting a MITM
against `google.com`. Public WebPKI ignores `nameConstraints` on a
non-public root, but our internal root respects it, and that's
what matters in our trust path.

## Enrollment protocol

```
Device                            Broker                Signer (cbd)
──────                            ──────                ────────────
[boot]
ESP startup
WiFi assoc
DHCP lease (records option 15 = "doma")
MQTT connect (TLS, validates broker cert via embedded
              CA bundle, password auth as mqtt-user@)

[E1: keygen]  (only if no valid cert in NVS, or env mismatch)
psa_generate_key(ECC P-256)    PSA Crypto (mbedTLS 4.0.0); RNG is
mbedtls_pk_copy_from_psa()     HW-backed and internal — no f_rng/DRBG.
                               ~ a few s on first boot, then cached in NVS.

[E2: CSR build]
subject  = CN=cb-ex01.lan
SANs     = DNS:cb-ex01.lan
           DNS:cb-ex01.local
           DNS:cb-ex01
           IP :192.0.2.x               (current static IP)
key alg  = ECDSA P-256
hash     = SHA-256

PEM-encode → ~ 600 B

[E3: publish]
subscribe cb-ex01/state/cert  (qos 1, no retain)
publish   cb-ex01/cmd/enroll  (qos 1, no retain) ───▶
                                                           validate:
                                                             topic.CN == csr.subject.CN
                                                             csr.public_key is EC P-256
                                                             SAN DNS entries match
                                                               cb-<id>.{doma,chata,local}
                                                               and unqualified <id>
                                                             SAN IP optional, must be RFC1918
                                                             csr self-signature ok
                                                           rate limit: max 1 issue / 24h per CN
                                                             (LOGE + reject if 2nd within 1h,
                                                              LOGW + accept if 2nd within 24h
                                                              — supports legitimate
                                                              factory-erase re-enroll)
                                                           sign:
                                                             serial = monotonic counter
                                                             validity = 90 d
                                                             EKU = serverAuth
                                                             copy SAN entries from CSR
                                                           audit:
                                                             append /var/log/cbd/issued.jsonl
                                                               {ts, cn, serial, fingerprint,
                                                                san_dns, san_ip,
                                                                mqtt_broker_client_addr}
                                                           publish to <id>/state/cert ◀───
                                                             cert PEM, qos 1, no retain

[E4: validate response]
parse PEM cert
verify chain against embedded sub-CA cert
  (mbedtls_x509_crt_verify with embedded chain as trust)
verify cert.public_key matches our generated key
  (prevents a forged response containing someone else's pubkey)
verify SAN DNS contains our FQDN

[E5: persist]
NVS namespace "tls":
  key_der    (uint8_t[~100])
  cert_der   (uint8_t[~700])
  expiry_ts  (int64, unix epoch)
  san_fp     (uint8_t[32], sha256 of canonicalised SANs — for fast env-mismatch check)

[E6: install]
reboot.   (much simpler than hot-swap httpd_ssl — and this path only
           runs at first enroll or domain/IP move, not in steady state.
           ~15 s downtime is acceptable in those rare cases.)

[next boot, with cert valid]
tls_store_load_cert_and_key()
esp_https_server_start(cert, key)
on port 80: redirect-only handler emitting 308 + HSTS
```

### Failure modes

| Phase fails | Behavior |
| --- | --- |
| Keygen | Retry indefinitely with backoff (1, 5, 30 min). Boot continues HTTP-only. Logs ESP_LOGE with mbedtls error code. |
| Publish (broker unreachable) | Same as above — retry when MQTT reconnects. |
| Signer doesn't reply within 60 s | Same — retry on next cfg cycle. Audit log on signer should show whether the request even arrived. |
| Response validates but cert chain doesn't | ESP_LOGE with the failing leaf. Don't persist. Retry — assumes intermittent corruption. |
| NVS write fails | ESP_LOGE. Boot continues HTTP-only with cert held only in RAM until next reboot. (We could persist on next attempt; for the corner case where NVS is full, the right answer is an erase via /debug/nvs_erase from bench.) |
| Env mismatch on existing cert | `tls_store_invalidate()` (mark cert as not-for-current-env), then run the enrollment flow from scratch. Existing cert stays usable for the current boot (no service interruption). |

The cardinal rule: **enrollment failure NEVER bricks the device.** It
falls back to HTTP-only, surfaces loud logs + a `state/enroll` MQTT
topic for HA visibility, and keeps retrying. A device that can't
enroll is still operationally identical to a pre-enrollment device —
which is what we're upgrading from anyway.

## NVS storage layout

Namespace `tls`. DER format (binary, ~2× compact vs PEM). Keys:

| Key | Type | Size | Notes |
| --- | --- | --- | --- |
| `key_der` | blob | ~100 B | ECDSA P-256 private key, sec1 DER encoding |
| `cert_der` | blob | ~700 B | leaf cert DER (just the device's cert; chain is built at runtime from embedded sub-CA) |
| `expiry_ts` | int64 | 8 B | unix epoch of `notAfter`. Used by boot-time renewal check (`expiry - now < 30 d` → renew). |
| `san_fp` | blob | 32 B | sha256 over canonicalised SAN entries (sorted, lowercased). On boot, if computed-from-environment SAN sha256 != stored, env mismatch → re-enroll. Saves a full DER parse per boot. |

Total: ~840 B. NVS is 0x4000 = 16384 B, currently using ~2 KB for cfg
+ wifi_mgr. Plenty of headroom.

The schema-driven `app_config.c` is NOT the right home for these —
they aren't operator-settable knobs and they're binary blobs not
typed scalars. Direct `nvs_get_blob`/`nvs_set_blob` in `tls_store.c`.

## HTTPS server

ESP-IDF 6.0's `esp_https_server` is mature — single-handler-list API
identical to `esp_http_server` except for the init:

```c
httpd_ssl_config_t cfg = HTTPD_SSL_CONFIG_DEFAULT();
cfg.servercert     = cert_pem;  // or use cacert_der + cacert_len
cfg.servercert_len = cert_pem_len;
cfg.prvtkey_pem    = key_pem;
cfg.prvtkey_len    = key_pem_len;
cfg.port_secure    = 443;
cfg.port_insecure  = 0;          // disable plain HTTP on this handle
cfg.session_tickets = false;     // saves RAM, we don't reuse sessions much
cfg.user_cb        = NULL;
httpd_ssl_start(&server, &cfg);
```

The existing `http_server.c` handler registration code stays — same
`httpd_register_uri_handler()` calls work against the SSL server
handle. Only the init function and a few `cfg.*` field names change.

### Port 80 redirect handler

A separate `esp_http_server` instance on port 80, single catch-all
handler:

```c
static esp_err_t redirect_get(httpd_req_t *req) {
    char host[64];
    size_t hl = httpd_req_get_hdr_value_len(req, "Host");
    if (hl == 0 || hl >= sizeof(host)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no host");
    }
    httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host));
    // Strip any :80 suffix — the redirect target uses default 443.
    char *colon = strchr(host, ':');
    if (colon) *colon = 0;

    char loc[160];
    snprintf(loc, sizeof(loc), "https://%s%s", host, req->uri);
    httpd_resp_set_status(req, "308 Permanent Redirect");
    httpd_resp_set_hdr(req, "Location", loc);
    // Don't HSTS on the plain-HTTP redirect — HSTS only goes on the
    // HTTPS response (RFC 6797 §7.2).
    return httpd_resp_send(req, NULL, 0);
}
```

Registered to `"/*"` so every URI gets redirected. Started ONLY after
cert + HTTPS handle are up, so during the no-cert fallback window the
plain HTTP server keeps serving everything as today.

### TLS 1.3 stack notes (verified constraints)

Two hard-won, bench-verified constraints on the HTTPS server:

- **`CONFIG_MBEDTLS_DYNAMIC_BUFFER` must stay OFF** (documented OFF in
  [sdkconfig.defaults](sdkconfig.defaults)). On mbedTLS 4.0.0 it crashes the
  TLS 1.3 server — the dynamic input buffer is NULL when
  `ssl_parse_record_header` reads the ClientHello record header →
  `LoadProhibited` on **every** HTTPS handshake (a crash-loop). Do not
  re-enable it without re-testing the handshake path.
- **`httpd.max_open_sockets` is 4 on the HTTPS handle**
  ([main/http_server.c](main/http_server.c)). Each TLS session costs roughly
  40 KB of internal DRAM; 4 concurrent HTTPS clients cover an MJPEG stream +
  a curl + an HA fetch while leaving heap for the rest of the firmware.

Verified on the bench: **15/15 rapid TLS 1.3 handshakes succeed** (10
sequential + 5 parallel), 0 crashes, and a fresh PSA-generated cert survives
reboot and serves on :443. (The older ESP-IDF v5.5 / mbedTLS 3.6.x stack had a
TLS 1.3 per-session OOM under rapid reconnects — one of the reasons this
project pins ESP-IDF v6.0.1 / mbedTLS 4.0.0 as the only supported toolchain.)


### lwIP `struct dhcp` build break on v6.0.1 (upstream fix in flight)

Building on v6.0.1 with `CONFIG_LWIP_HOOK_DHCP_EXTRA_OPTION_CUSTOM=y`
(needed by `wifi_mgr.c` for DHCP domain discovery) fails:
`lwip_default_hooks.h:74: error: 'struct dhcp' declared inside parameter
list ... [-Werror]` — the prototype uses `struct dhcp` before it is
forward-declared. The diagnostic has no named `-W` flag, so it can't be
waived; the only fixes are correcting the header or making the type
visible first. Bug is present on current ESP-IDF master too.

- **Upstream fix:** https://github.com/espressif/esp-idf/pull/18668
  (one-line forward declaration).
- **Local bridge until a fixed IDF release ships:** `target_compile_options(__idf_lwip PRIVATE -include lwip/dhcp.h)`
  in [CMakeLists.txt](CMakeLists.txt) — touches no SDK file
  (survives IDF re-install). Remove once the PR lands in a release we use.

### HSTS

On every HTTPS response, after handler runs:

```c
httpd_resp_set_hdr(req, "Strict-Transport-Security",
                   "max-age=15552000; includeSubDomains");
```

`max-age=15552000` = 180 days. Long enough to be effective, short
enough that if we discover a problem and have to back out to HTTP we
don't lock browsers out for a year.

NOT using `preload` — that's for public domains submitted to the
HSTS preload list, doesn't apply here.

Conditional: only set HSTS when we know the connection is TLS. With
`esp_https_server` that's always true on the secure handle, so no
runtime check needed — just don't set HSTS on the port-80 server's
responses.

## HTTP Basic auth

Server-cert + TLS alone proves "this is the budka". It does NOT
prove "I'm allowed to look at the mic/camera". Anyone who can resolve
the budka's name still has full access to live AV streams once they
trust the chain.

Add Basic auth gating the sensitive endpoints:

| Endpoint | Gate |
| --- | --- |
| `/mic.wav` | required |
| `/stream.mjpg` | required |
| `/last.jpg`, `/capture` | required |
| `/photo`, `/photos` | required |
| `/`, `/selftest`, `/i2c`, `/sht41/bus1` | open (no audio/video) |
| `/debug/*` | open (already CONFIG-gated; not in production builds) |

Credentials live in `secrets.h`:

```c
#define HTTP_BASIC_USER "budka"
#define HTTP_BASIC_PASS "<random-per-deployment>"
```

Single user/password shared across all devices. HA and the operator
both use it; mobile_app notifications that link back to the device
URL will need it embedded in the URL (`https://user:pass@cb-<id>.lan/last.jpg`)
or HA's `media_source` config provides it.

Implementation: a `auth_check(req)` helper at the top of each
gated handler, called before any work. Returns ESP_OK or sends 401
with a `WWW-Authenticate: Basic realm="budka"` header and returns
ESP_FAIL. Pattern matches the existing path-traversal guard in
`/photo` (early return + send_err + clean log line).

## Domain discovery

Compile-time `CB_DOMAIN_FALLBACK "doma"` in `config.h` is the LAST
resort. Preferred source is **DHCP option 15** (domain name),
captured via the lwIP parse hook:

```c
// firmware/main/wifi_mgr.c
#include "lwip/dhcp.h"        /* DHCP_OPTION_DOMAIN_NAME */
#include "lwip/pbuf.h"

static char s_dhcp_domain[64];

// IDF 6.0's name for upstream LWIP_HOOK_DHCP_PARSE_OPTION. Payload is
// in pbuf `p` at `offset`. Called on the lwIP tcpip task — no locking
// needed for the static buffer because that task is the sole writer
// and accessors below read it (with the worst-case being a transient
// mid-write read, which a length-0 NUL terminator at write end makes
// safe to truncate).
void lwip_dhcp_on_extra_option(struct dhcp *dhcp, uint8_t state,
                                uint8_t option, uint8_t len,
                                struct pbuf *p, uint16_t offset) {
    if (option == DHCP_OPTION_DOMAIN_NAME && len > 0) {
        size_t n = len < sizeof(s_dhcp_domain) - 1
                 ? len : sizeof(s_dhcp_domain) - 1;
        pbuf_copy_partial(p, s_dhcp_domain, n, offset);
        s_dhcp_domain[n] = 0;
    }
}

const char *wifi_mgr_get_domain(void) {
    return s_dhcp_domain[0] ? s_dhcp_domain : CB_DOMAIN_FALLBACK;
}
```

Enabled via `sdkconfig.defaults`:

```
CONFIG_LWIP_HOOK_DHCP_EXTRA_OPTION_CUSTOM=y
```

**Caveat: DHCP parameter request list.** Upstream lwIP only requests
options 1 (subnet mask), 3 (router), 28 (broadcast), 6 (DNS) in its
DISCOVER. Option 15 (domain name) is NOT in the list, so a strict
DHCP server that only sends requested options will skip it. Most
real-world servers (dnsmasq, ISC dhcpd) send option 15 unconditionally
when configured. If yours doesn't, the fallback to `CB_DOMAIN_FALLBACK`
kicks in (see below). The proper fix — extending lwIP's parameter
request list — needs either a component override or upstream
LWIP_HOOK_DHCP_APPEND_OPTIONS (not currently wrapped by IDF 6.0).

Why DHCP and not a per-device build-time `#define`:

- Moving a budka between deployments (doma → chata) becomes
  zero-config: DHCP gives the new domain, env-staleness check
  detects the SAN mismatch, device re-enrolls automatically.
- No per-deployment firmware build artifact to keep track of.
- No NVS knob = no MQTT cfg topic to mis-set.

Trade: depends on DHCP server having option 15 configured. If it
doesn't, fallback to compile-time default kicks in, which is
graceful.

## Environment-staleness check

```c
// tls_check.c — called once after WiFi+DHCP up, before httpd starts
bool tls_cert_valid_for_environment(void) {
    if (!tls_store_has_cert()) return false;

    int64_t now = time(NULL);
    int64_t expiry = tls_store_get_expiry();
    if (expiry - now < 30 * 86400) {
        ESP_LOGI(TAG, "cert expires in <30d (at %lld) — renewing", expiry);
        return false;
    }

    char ip[16];
    if (wifi_mgr_get_ip_str(ip, sizeof(ip))) {
        // SAN canonical = SANs sorted, lowercased, joined with '\n'
        char canon[256];
        tls_make_canonical_san(canon, sizeof(canon),
                               device_id(), wifi_mgr_get_domain(), ip);
        uint8_t want_fp[32];
        tls_sha256(canon, strlen(canon), want_fp);

        uint8_t have_fp[32];
        if (tls_store_get_san_fp(have_fp) != ESP_OK ||
            memcmp(want_fp, have_fp, 32) != 0) {
            ESP_LOGW(TAG, "cert SAN doesn't match current "
                          "(domain='%s' ip=%s) — re-enrolling",
                     wifi_mgr_get_domain(), ip);
            return false;
        }
    }

    return true;
}
```

Three reasons to re-enroll, in order of likelihood:

1. **Near expiry** (`expiry - now < 30 d`) — normal 60-day rolling
   renewal (cert valid 90d, renew at 60d remaining → effectively
   one renewal every 30d).
2. **Domain changed** — device moved between `.lan` and `.lan`.
3. **IP changed** — static IP swapped, or device on DHCP got a
   different lease.

All three trigger the same enrollment flow. The signer's rate-limit
(1/24h per CN) absorbs all three legitimate cases without complaint.


## Signer daemon

The original MQTT-transport signer lived in the author's metrics-bridge
daemon; the standalone stack's manager ([`server/`](../server/README.md))
supersedes it — enrollment lands there over HTTPS with TOFU approval.

## Threat model — what we're defending against

| Threat | Defense | Residual risk |
| --- | --- | --- |
| Passive sniff on LAN of live mic/cam | TLS 1.2/1.3 encryption | None for traffic in flight |
| Active MITM presenting forged cert | Sub-CA pinned in embedded chain on every consumer | Sub-CA key compromise (handled by rotation) |
| Unauthorized LAN client viewing streams | HTTP Basic auth on AV endpoints | Credential theft from `secrets.h` leak |
| Rogue board claiming someone else's CN | Signer rate-limit + audit log alerts | First-claim-wins until the legit device boots — narrow window |
| Stolen sub-CA key | Name constraints, short cert validity (90d) | Attacker can MITM `*.lan` for 90d max from cert issue |
| Stolen root CA key | Offline storage of the root key | Catastrophic; rebuild everything |

## What we're explicitly NOT doing

- **mTLS** (client cert on device for HTTPS) — HA/browser config 10× harder, marginal threat reduction for our model.
- **OAuth / Keycloak** — single-user shared password is what we need.
- **OCSP / CRL** — 90d cert validity makes revocation moot; just wait it out or rotate the sub-CA.
- **Per-device MQTT credentials** — shared `mqtt-user@` plus signer-side rate limit + audit log is good enough.
- **Public CA (Let's Encrypt)** — devices have no public DNS or public reachability; LE doesn't apply.
- **Post-quantum** — mbedTLS 4.0.0 (ESP-IDF v6.0.1) doesn't ship production PQ. Store-now-decrypt-later isn't a hobby-tier concern.
- **HSM-backed key storage on device** — ESP32-S3 has eFuse + Flash Encryption + Secure Boot but enrolling those would add ~2 weeks of work for marginal benefit on private LAN.

## Future work (parking lot)

- **Domain change without re-enroll**: extend cert SANs to cover
  multiple domains upfront if a device is known to move. Probably
  never worth it.
- **mTLS for the MQTT broker** — current broker uses password auth.
  Could move to per-device cert auth using the same enrollment
  infra. Saves shared password rotation pain.
- **Web UI for cert status** — `/cert-info` page showing fingerprint,
  expiry, SAN list. Useful for ops debugging.
- **Push cert rotation** — if a device's cert needs rotating
  out-of-band, MQTT cmd topic `cmd/cert_renew` triggers an immediate
  re-enrollment. Today the trigger is boot-time only.
