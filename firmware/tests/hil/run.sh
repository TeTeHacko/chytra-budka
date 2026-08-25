#!/usr/bin/env bash
# Run the HIL suite with the provisioning credentials already in the
# environment.
#
# Why this exists: every HIL session factory-resets the bench (the autouse
# reset_board fixture), so the run MUST be able to re-provision it onto the
# station LAN afterwards. Those creds live in the operator's ~/.bashrc behind a
# keyring, which means they resolve to EMPTY in any non-interactive shell — a
# script, a CI step, an agent. The suite then skips provisioning, leaves the
# board sitting in its SoftAP, and every later test fails with "no route to
# host": a missing variable that reads like dead hardware. That has cost real
# debugging time more than once.
#
# So take them from NetworkManager, which has the network saved anyway. The PSK
# is only ever assigned to a variable, never printed.
#
#   ./run.sh                          # the OTA gate's selection
#   ./run.sh test_wifi_provision.py   # one module
#   CB_SWEEP=1 ./run.sh test_jpeg_sweep.py
#
# CB_PROVISION_SSID may be overridden to point at another saved network; the
# station network the boards belong on is "IoT-Network", NOT the host's own
# "HomeNet" (ota_upload.sh derives the active WLAN as a fallback, which is the
# wrong one).
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")"

: "${CB_PROVISION_SSID:=IoT-Network}"
export CB_PROVISION_SSID

if [[ -z "${CB_PROVISION_PSK:-}" ]]; then
  if ! nmcli -g NAME connection show 2>/dev/null \
    | grep -qxF "$CB_PROVISION_SSID"; then
    echo "run.sh: no NetworkManager connection named '$CB_PROVISION_SSID'." >&2
    echo "        Save it (or set CB_PROVISION_SSID/CB_PROVISION_PSK yourself)" >&2
    echo "        — without it the bench cannot be re-provisioned after the" >&2
    echo "        session's factory reset." >&2
    exit 2
  fi
  CB_PROVISION_PSK="$(nmcli -s -g 802-11-wireless-security.psk \
    connection show "$CB_PROVISION_SSID" 2>/dev/null)"
  if [[ -z "$CB_PROVISION_PSK" ]]; then
    echo "run.sh: NetworkManager returned no PSK for '$CB_PROVISION_SSID'." >&2
    echo "        Reading a saved secret needs an authorised session — run" >&2
    echo "        this from your own desktop session, or export" >&2
    echo "        CB_PROVISION_PSK yourself." >&2
    exit 2
  fi
fi
export CB_PROVISION_PSK

VENV="$PWD/.venv/bin/python"
[[ -x "$VENV" ]] || VENV="python3"

# No arguments → exactly what firmware/tools/ota_upload.sh gates on.
if [[ $# -eq 0 ]]; then
  set -- -m "not manual"
fi

echo "→ HIL: provisioning SSID '$CB_PROVISION_SSID' (PSK from NetworkManager)"
exec "$VENV" -m pytest "$@"
