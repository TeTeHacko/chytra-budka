#!/usr/bin/env bash
# End-to-end integration test: firmware core (C++) ↔ relay (Python).
#
# Builds firmware/tests/native/stream_to_relay if needed, spins up the relay
# against a fake ffmpeg sink on an isolated port, then drives 4 scenarios
# from the firmware-side runner and asserts on relay state + metrics.
#
# All artifacts live under a self-contained workdir (default: /tmp/relay-e2e).
# No repository state is modified.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RELAY_DIR="$REPO_ROOT/relay"
FW_TESTS_DIR="$REPO_ROOT/firmware/tests/native"
WORKDIR="${WORKDIR:-/tmp/relay-e2e}"
PORT="${PORT:-18765}"
TOKEN="${TOKEN:-e2e-token-$(date +%s)}"
STREAM="chytra-budka-test"
PATH_URL="/audio/${STREAM}"

RUNNER="$FW_TESTS_DIR/build/stream_to_relay"
RELAY_PY="$RELAY_DIR/relay.py"

PASS=0
FAIL=0
log() { printf '\033[1;36m[e2e]\033[0m %s\n' "$*"; }
ok() {
  printf '  \033[1;32mPASS\033[0m %s\n' "$*"
  PASS=$((PASS + 1))
}
bad() {
  printf '  \033[1;31mFAIL\033[0m %s\n' "$*"
  FAIL=$((FAIL + 1))
}
assert_eq() {
  local got="$1" want="$2" msg="$3"
  if [[ "$got" == "$want" ]]; then ok "$msg (=$want)"; else bad "$msg (got=$got want=$want)"; fi
}
assert_ge() {
  local got="$1" want="$2" msg="$3"
  if ((got >= want)); then ok "$msg (got=$got >= $want)"; else bad "$msg (got=$got want>=$want)"; fi
}

# --- 1. workdir + venv ---------------------------------------------------
log "workdir: $WORKDIR"
rm -rf "$WORKDIR"
mkdir -p "$WORKDIR"

if [[ ! -x "$WORKDIR/.venv/bin/python" ]]; then
  log "creating venv"
  python3 -m venv "$WORKDIR/.venv"
  "$WORKDIR/.venv/bin/pip" install -q --upgrade pip
  "$WORKDIR/.venv/bin/pip" install -q aiohttp prometheus-client
fi
PY="$WORKDIR/.venv/bin/python"

# --- 2. fake ffmpeg ------------------------------------------------------
FAKE_FFMPEG="$WORKDIR/fake-ffmpeg.sh"
cat >"$FAKE_FFMPEG" <<'EOF'
#!/usr/bin/env bash
# Drains stdin, logs byte total, exits 0. Pretends to be ffmpeg.
exec >>"${FFMPEG_LOG:-/tmp/relay-e2e/ffmpeg.log}" 2>&1
echo "[$(date +%T)] fake-ffmpeg start args: $*"
total=0
while true; do
  if ! IFS= read -rN 4096 chunk; then
    [[ -n "${chunk:-}" ]] && total=$((total + ${#chunk}))
    break
  fi
  total=$((total + ${#chunk}))
done
echo "[$(date +%T)] fake-ffmpeg drained $total bytes, exit 0"
EOF
chmod +x "$FAKE_FFMPEG"
: >"$WORKDIR/ffmpeg.log"

# --- 3. test config ------------------------------------------------------
CFG="$WORKDIR/relay.toml"
cat >"$CFG" <<EOF
[server]
listen = "127.0.0.1:${PORT}"
auth_token_env = "RELAY_AUTH_TOKEN"

[mediamtx]
rtsp_host = "127.0.0.1"
rtsp_port = 8554

[ffmpeg]
binary = "${FAKE_FFMPEG}"
input_format = "s16le"
sample_rate = 16000
channels = 2
output_codec = "pcm_s16be"
output_sample_rate = 48000
output_channels = 1

[defaults]
allowed_ips = ["127.0.0.0/8"]
gap_silence_seconds = 2
idle_timeout_seconds = 5
total_timeout_seconds = 0

[streams.${STREAM}]
allowed_ips = ["127.0.0.0/8"]
EOF

# --- 4. test PCMs --------------------------------------------------------
log "generating test PCMs (16 kHz stereo)"
"$PY" - <<EOF
import math, struct
rate = 16000
# Stereo interleaved: L=tone, R=tone (same)
with open("$WORKDIR/tone5s.pcm", "wb") as f:
    for n in range(rate * 5):
        sample = int(20000 * math.sin(2 * math.pi * 1000 * n / rate))
        f.write(struct.pack("<hh", sample, sample))  # L, R
with open("$WORKDIR/silence2s.pcm", "wb") as f:
    f.write(b"\\x00\\x00\\x00\\x00" * rate * 2)  # 2s stereo silence
EOF

# --- 5. build firmware runner if needed ----------------------------------
if [[ ! -x "$RUNNER" ]]; then
  log "building stream_to_relay"
  make -C "$FW_TESTS_DIR" stream-to-relay >"$WORKDIR/build.log" 2>&1 || {
    cat "$WORKDIR/build.log"
    exit 1
  }
fi

# --- 6. start relay ------------------------------------------------------
RELAY_LOG="$WORKDIR/relay.log"
log "starting relay on :$PORT"
RELAY_AUTH_TOKEN="$TOKEN" FFMPEG_LOG="$WORKDIR/ffmpeg.log" \
  "$PY" "$RELAY_PY" --config "$CFG" >"$RELAY_LOG" 2>&1 &
RELAY_PID=$!
trap 'kill -TERM $RELAY_PID 2>/dev/null || true; wait $RELAY_PID 2>/dev/null || true' EXIT

for _ in 1 2 3 4 5 6 7 8; do
  if ss -ltn 2>/dev/null | grep -q ":${PORT} "; then break; fi
  sleep 0.25
done
ss -ltn | grep -q ":${PORT} " || {
  bad "relay failed to bind :$PORT"
  cat "$RELAY_LOG"
  exit 1
}
ok "relay listening"

# --- 7. drive scenarios --------------------------------------------------
run_fw() {
  local label="$1" pcm="$2" mode="$3" token="$4" codec="${5:-pcm}"
  log "scenario: $label (codec=$codec)"
  "$RUNNER" \
    --pcm "$pcm" --rate 16000 \
    --host 127.0.0.1 --port "$PORT" --path "$PATH_URL" \
    --token "$token" --mode "$mode" --codec "$codec" --no-realtime \
    >"$WORKDIR/${label}.out" 2>&1 || true
}

run_fw t1_cont_tone "$WORKDIR/tone5s.pcm" continuous "$TOKEN"
run_fw t2_trig_silence "$WORKDIR/silence2s.pcm" triggered "$TOKEN"
run_fw t3_bad_token "$WORKDIR/tone5s.pcm" continuous "wrong-token"
run_fw t4_trig_tone "$WORKDIR/tone5s.pcm" triggered "$TOKEN"
# T5: same input as T1 but FLAC-encoded — exercises the audio/flac
# Content-Type negotiation, ffmpeg `-f flac` input demux path, and
# FLAC-stream linger drop. Sleep between runs so the previous PCM
# session's linger has dropped (codec change forces respawn anyway,
# but better hygiene).
sleep 3
run_fw t5_cont_flac "$WORKDIR/tone5s.pcm" continuous "$TOKEN" flac

# --- 8. assertions on FW runner output -----------------------------------
# stream_to_relay summary lines look like:
#   "  chunks sent:  250 (480000 bytes)"
#   "  status code:  204"
chunks_sent() { awk '/chunks sent:/ {print $3; exit}' "$1"; }
status_code() { awk '/status code:/ {print $NF; exit}' "$1"; }

t1_sent=$(chunks_sent "$WORKDIR/t1_cont_tone.out")
t1_status=$(status_code "$WORKDIR/t1_cont_tone.out")
assert_eq "${t1_status:-none}" "204" "T1 continuous tone HTTP 204"
assert_ge "${t1_sent:-0}" 200 "T1 continuous tone sent ≥200 chunks"

t2_sent=$(chunks_sent "$WORKDIR/t2_trig_silence.out")
t2_status=$(status_code "$WORKDIR/t2_trig_silence.out")
assert_eq "${t2_sent:-?}" "0" "T2 triggered silence: VAD blocked all chunks"
# triggered+silent path may not even open a connection — accept missing or 204
if [[ -z "$t2_status" || "$t2_status" == "204" ]]; then
  ok "T2 no body sent (status=${t2_status:-none})"
else
  bad "T2 unexpected status=$t2_status"
fi

t3_status=$(status_code "$WORKDIR/t3_bad_token.out")
assert_eq "${t3_status:-none}" "401" "T3 bad token HTTP 401"

t4_sent=$(chunks_sent "$WORKDIR/t4_trig_tone.out")
t4_status=$(status_code "$WORKDIR/t4_trig_tone.out")
assert_eq "${t4_status:-none}" "204" "T4 triggered tone HTTP 204"
assert_ge "${t4_sent:-0}" 200 "T4 triggered tone sent ≥200 chunks"

# T5: FLAC-encoded continuous tone. Encoded byte volume is much smaller
# than PCM (1 kHz tone → ~0.1-0.3× ratio) but the chunk *count* (which
# is the same as PCM here, one HTTP chunk per audio frame from the
# encoder write_cb) should match the input frame count broadly. Status
# 204 + non-zero chunks is the must-have; ratio is a nice-to-have.
t5_sent=$(chunks_sent "$WORKDIR/t5_cont_flac.out")
t5_status=$(status_code "$WORKDIR/t5_cont_flac.out")
assert_eq "${t5_status:-none}" "204" "T5 FLAC continuous HTTP 204"
assert_ge "${t5_sent:-0}" 50 "T5 FLAC sent ≥50 frames (encoder may coalesce)"
# Verify relay actually saw the audio/flac Content-Type and started
# ffmpeg with -f flac. fake-ffmpeg.log captures the args list.
if grep -q "fake-ffmpeg start args:.* -f flac" "$WORKDIR/ffmpeg.log"; then
  ok "T5 relay invoked ffmpeg with -f flac"
else
  bad "T5 ffmpeg args missing -f flac in log"
fi
# And that PCM run before still used s16le.
if grep -q "fake-ffmpeg start args:.* -f s16le" "$WORKDIR/ffmpeg.log"; then
  ok "earlier PCM scenarios used -f s16le"
else
  bad "no -f s16le in ffmpeg log (PCM path missing?)"
fi
# Verify output resampling args present (stereo 16kHz → mono 48kHz)
if grep -q "fake-ffmpeg start args:.* -ac 1" "$WORKDIR/ffmpeg.log"; then
  ok "ffmpeg invoked with -ac 1 (downmix to mono)"
else
  bad "ffmpeg args missing -ac 1"
fi
if grep -q "fake-ffmpeg start args:.* -ar 48000" "$WORKDIR/ffmpeg.log"; then
  ok "ffmpeg invoked with -ar 48000 (resample)"
else
  bad "ffmpeg args missing -ar 48000"
fi

# --- 9. wait for linger to expire so ffmpeg drains, then probe ----------
sleep 1
streams=$(curl -s "http://127.0.0.1:${PORT}/streams" || true)
echo "$streams" >"$WORKDIR/streams.txt"
log "/streams: $streams"

metrics=$(curl -s "http://127.0.0.1:${PORT}/metrics" || true)
echo "$metrics" >"$WORKDIR/metrics.txt"

# bytes_total{stream="chytra-budka-test"} should be ≥ T1+T4+T5 contributions.
# 16 kHz stereo: 5s × 16000 × 2ch × 2B = 320000 per PCM tone. T5 is FLAC
# (smaller), so total ≥ ~640000. Use conservative threshold.
# prometheus_client emits values as floats which may use scientific
# notation (e.g. "1.096059e+06") for large counters — bash ${var%.*} would
# split at the first '.' and yield "1" → false-negative. Hand the raw
# value to awk for int truncation.
bytes_total=$(echo "$metrics" \
  | awk -v s="$STREAM" '$0 ~ "^chytra_relay_bytes_total{stream=\""s"\"}" {printf "%d", $2}')
assert_ge "${bytes_total:-0}" 500000 "chytra_relay_bytes_total{$STREAM} ≥ 500000"

# linger reattach: PCM scenarios reuse ffmpeg via linger, then T5 forces a
# codec-change respawn. Expect roughly 2-5 starts overall.
ffmpeg_starts=$(echo "$metrics" \
  | awk -v s="$STREAM" '$0 ~ "^chytra_relay_ffmpeg_starts_total{stream=\""s"\"}" {print $2}')
ffmpeg_starts_int=${ffmpeg_starts%.*}
if [[ -n "$ffmpeg_starts_int" ]] && ((ffmpeg_starts_int >= 1 && ffmpeg_starts_int <= 5)); then
  ok "ffmpeg_starts_total in [1..5] (=$ffmpeg_starts_int) — linger reattach + codec respawn"
else
  bad "ffmpeg_starts_total unexpected (=${ffmpeg_starts_int:-missing})"
fi

# T3 should have bumped unauthorized counter
unauth=$(echo "$metrics" \
  | awk '$0 ~ "^chytra_relay_unauthorized_total{" {sum += $2} END {print sum+0}')
unauth_int=${unauth%.*}
assert_ge "${unauth_int:-0}" 1 "chytra_relay_unauthorized_total ≥ 1 (T3)"

# --- 10. shutdown signal handling ---------------------------------------
log "sending SIGTERM"
kill -TERM "$RELAY_PID"
wait "$RELAY_PID" 2>/dev/null || true
trap - EXIT
if grep -q "shutdown signal received" "$RELAY_LOG"; then
  ok "relay handled SIGTERM"
else
  bad "relay did not log shutdown"
fi

# --- summary -------------------------------------------------------------
echo
log "PASS=$PASS  FAIL=$FAIL"
if ((FAIL > 0)); then
  echo
  log "relay log tail:"
  tail -30 "$RELAY_LOG" | sed 's/^/  /'
  exit 1
fi
log "all good"
