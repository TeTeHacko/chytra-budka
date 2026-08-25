# Security Policy

Chytrá Budka is a **networked device** (WiFi, an on-device HTTP/HTTPS server,
MQTT, and OTA firmware updates), so security reports are taken seriously.

## Reporting a vulnerability

Please **do not** open a public issue for security problems. Instead use
GitHub's **private vulnerability reporting** ("Report a vulnerability" under the
repository's Security tab), or email the maintainer (see the repo profile).

Include: affected component, firmware version/commit, reproduction steps, and
the impact you observed. We aim to acknowledge within a few days. This is a
hobby/research project maintained on a best-effort basis — there is no paid
bug-bounty.

## Scope

In scope: the firmware (`firmware/`), the on-device HTTP/HTTPS server and its
auth, the MQTT control surface, the per-device TLS enrollment flow, and the OTA
update path.

## Known limitations (by design / not yet done — not vulnerabilities)

These are documented trade-offs, already tracked. Reports about them are
welcome as *hardening* discussion but aren't treated as 0-days:

- **Signed OTA is deployed fleet-wide; hardware Secure Boot v2 is burned on
  the field unit; Flash Encryption is the remaining item.** OTA images are
  RSA-3072 / RSA-PSS signed (YubiKey-vault-held key) and verified on update by
  every board; the field `ex02` additionally enforces the signature in the
  bootloader via hardware Secure Boot v2 (a deliberately *recoverable* config —
  no Flash Encryption, secure-download retained). Flash Encryption + full eFuse
  lockdown are the remaining hardening step. See
  [`firmware/README.md`](firmware/README.md#production-hardening-stage-e).
- **MQTT broker per-client ACLs are load-bearing for the enrollment trust
  model.** The CSR signer (`metrics-bridge/enroll.py`) anchors device identity
  on the MQTT *topic*, trusting the broker to restrict each device to publish
  only under its own `cb-<id>/#` namespace. Deployments MUST configure
  mosquitto per-client write ACLs accordingly; without them, any holder of the
  (shared) broker credential could request a signed leaf for an arbitrary
  in-constraint identity.
- **Debug endpoints (`/debug/*`)** can crash, reboot, or wipe the device.
  They require the same HTTP Basic auth as every other sensitive endpoint
  (`HTTP_AUTH_OR_RETURN`) — but the auth gate is only active over HTTPS (when
  `s_https_active`), so **on a pre-enrollment HTTP-only build the endpoints are
  effectively unauthenticated**. They are compile-gated off by default
  (`CONFIG_CHYTRA_BUDKA_DEBUG_ENDPOINTS=n`) and **must stay off** for any build
  exposed on a routable network. They exist only for bench/HIL testing.
- **Audio-relay traffic is plain HTTP on the LAN** (Bearer-token auth). TLS is
  intended to be terminated upstream (VPN / reverse proxy), not on the device.
- **MQTT** is username/password over plaintext on the LAN by default.

## Hardening guidance for deployers

- Keep `CONFIG_CHYTRA_BUDKA_DEBUG_ENDPOINTS=n` on anything beyond a private
  bench.
- Put the device on an isolated IoT VLAN; don't expose port 80/443 to the
  internet.
- Set strong values in `secrets.h` (WiFi, MQTT, relay token, HTTP Basic auth).
- Once per-device HTTPS enrollment is set up, the local web UI auth runs over
  TLS; the pre-enrollment HTTP fallback skips the auth gate to avoid sending
  credentials in clear text.
