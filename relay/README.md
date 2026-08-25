# Audio Relay — chytra-budka

Receives chunked HTTP audio stream from the ESP32 firmware (`POST
/audio/<name>`) and republishes it as an RTSP source consumable by
BirdNET-Go. Bridges between the ESP32's HTTP-only push stack and
BirdNET-Go's RTSP-pull model.

## Pipeline

```
ESP32 (HTTP POST chunked, audio/L16 s16le — rate/channels per relay.toml,
       default 16 kHz stereo; the audio/L16 rate=/channels= params override)
  ↓
relay.py (aiohttp listener on :8765)
  ↓ stdin pipe
ffmpeg -f s16le -ar 16000 -ac 2 -i -  \     # input = source format
       -ar 48000 -ac 1 \                    # resample → BirdNET (output_* in toml)
       -c:a pcm_s16le -f rtsp -rtsp_transport tcp \
       rtsp://localhost:8554/<name>
  ↓
mediamtx on :8554
  ↓
BirdNET-Go (rtsp://localhost:8554/<name>)
```
> `-re` is intentionally NOT used — the stdin pipe is already rate-limited by
> the ESP32's audio clock, and `-re` made ffmpeg drift behind (see relay.py).
> Exact rates/channels come from `relay.toml` (`sample_rate`/`channels` in,
> `output_sample_rate`/`output_channels` out); the values above are the defaults.

## Why this design

ESP32 has a small TCP/IP stack — running an RTSP server on the device is
fragile and burns RAM. HTTP POST is dead simple, supports keep-alive, and
reconnects cleanly on WiFi flap. The relay handles all the hard parts
(RTSP, codec, buffer management, gap-fill) on a beefy server.

## Endpoints

| Path                   | Method | Auth                    | Purpose                                                   |
| ---------------------- | ------ | ----------------------- | --------------------------------------------------------- |
| `/audio/<stream_name>` | POST   | `Authorization: Bearer` | Audio chunks. Body is raw 16-bit PCM LE.                  |
| `/health`              | GET    | none                    | Liveness check                                            |
| `/streams`             | GET    | none                    | Live stream table: writer state, linger state, bytes, age |
| `/metrics`             | GET    | none                    | Prometheus exposition (active_streams, bytes_total, …)    |

A POST to `/audio/<name>` either spawns a fresh ffmpeg subprocess or
re-attaches to an existing one that is currently lingering with silence
(see _Linger / gap silence_ below). On writer disconnect ffmpeg keeps the
RTSP source alive for `gap_silence_seconds` to absorb short reconnects.

## Auth and access control

- Bearer token compared in constant time (`hmac.compare_digest`)
- Optional CIDR allowlist per stream via `[streams.<name>] allowed_ips`
  (or fallback `[defaults]`)
- TLS is **not** terminated by the relay — front it with nginx/caddy or
  put it on a private network / VPN

## Linger / gap silence

When the writer disconnects cleanly, the relay continues feeding silence
into ffmpeg for `gap_silence_seconds` (default 30 s) so mediamtx keeps the
RTSP path published. If a new POST for the same stream arrives within that
window, the linger is cancelled and the new client takes over the existing
ffmpeg — no RTSP teardown, no BirdNET-Go reattach, no detection gap.

## Single-writer guarantee

If a second concurrent POST hits an active stream, it gets `409 Conflict`
immediately. Two ESP32s sharing the same stream name would otherwise mix
into the same ffmpeg stdin and corrupt the RTSP audio. Use distinct
`<stream_name>` per device.

## Watchdogs

Per-stream knobs in `relay.toml`:

- `idle_timeout_seconds` (default 10) — drop request if no chunk arrives
  for this long
- `total_timeout_seconds` (default 0 = unlimited) — hard cap on a single
  POST duration

Both reset to defaults from `[defaults]` if a stream has no per-stream
section.

## Config

`relay.toml`:

```toml
[server]
listen = "0.0.0.0:8765"
auth_token_env = "RELAY_AUTH_TOKEN"

[mediamtx]
rtsp_host = "127.0.0.1"
rtsp_port = 8554

[ffmpeg]
binary = "/usr/bin/ffmpeg"
input_format = "s16le"
sample_rate = 48000
channels = 1
output_codec = "pcm_s16le"

[defaults]
allowed_ips = ["10.0.0.0/8", "172.16.0.0/12", "192.168.0.0/16", "127.0.0.0/8", "::1/128"]
gap_silence_seconds = 30
idle_timeout_seconds = 10

[streams.chytra-budka]
allowed_ips = ["192.168.0.0/16"]
gap_silence_seconds = 30
idle_timeout_seconds = 10
```

## Deployment (server-host)

```bash
sudo ./install.sh
```

The script:

1. creates the `birdnet` system user if missing
2. installs to `/opt/chytra-budka-relay/` with a private venv
3. writes `/etc/chytra-budka/relay.env` (0640, root:birdnet) with a freshly
   generated 64-hex-char token on first run; rerun is idempotent and
   keeps an existing token
4. drops `relay.toml` to `/etc/chytra-budka/relay.toml`
5. installs and starts `chytra-budka-relay.service`

The token written to `relay.env` must be copied into
`firmware/main/secrets.h` as `RELAY_AUTH` before the next firmware build.

## Testing without the ESP32

### Pure Python (file → POST at realtime)

`test-stream.py` is a streaming HTTP/1.1 chunked client that writes raw
chunks straight to the socket so the input is paced at audio realtime
without buffering:

```bash
ffmpeg -f lavfi -i "sine=frequency=440:duration=60" \
       -ar 48000 -ac 1 -c:a pcm_s16le -f s16le /tmp/sine60.pcm

RELAY_AUTH_TOKEN="$(sudo grep RELAY_AUTH_TOKEN /etc/chytra-budka/relay.env | cut -d= -f2)" \
  ./test-stream.py \
    --url http://127.0.0.1:8765/audio/chytra-budka \
    --input /tmp/sine60.pcm
```

### Live mic (ALSA → curl)

`test-client.sh` pipes a real mic through curl. ffmpeg's ALSA input is
inherently realtime so no extra pacing is needed:

```bash
RELAY_AUTH_TOKEN="$(cat ~/.config/chytra-budka/relay-token)" \
  ./test-client.sh chytra-budka http://localhost:8765
```

### Verifying the RTSP side

```bash
ffprobe -v error -rtsp_transport tcp -show_streams \
  rtsp://127.0.0.1:8554/chytra-budka
# → codec_name=pcm_s16le sample_rate=48000 channels=1
```

### Inspecting the relay state

```bash
curl -s http://127.0.0.1:8765/health
curl -s http://127.0.0.1:8765/streams
curl -s http://127.0.0.1:8765/metrics | grep -v '^#' | head
```

### End-to-end (firmware core ↔ relay)

`test-e2e.sh` exercises the full path on the host: it builds the
`stream_to_relay` runner from `firmware/tests/native/` (which uses the
exact same `cb_core` C++ classes the ESP32 firmware uses — `Vad`,
`ChunkedPoster`, plus a POSIX transport), spins up the relay against a
fake `ffmpeg` sink on an isolated port, drives four scenarios
(continuous + tone, triggered + silence, bad token, triggered + tone),
and asserts on relay state and Prometheus metrics.

```bash
./test-e2e.sh
# 12/12 PASS — no ESP32 required.
```

Workdir defaults to `/tmp/relay-e2e`. Override with `WORKDIR=…`,
`PORT=…`, `TOKEN=…`.

## Wiring into BirdNET-Go

Add an RTSP source to `~/birdnet-go/config/config.yaml` under
`realtime.rtsp.streams`:

```yaml
- name: Chytra Budka
  url: rtsp://127.0.0.1:8554/chytra-budka
  enabled: true
  type: rtsp
  transport: tcp
```

Restart the container: `docker compose restart birdnet-go`. With linger
enabled, BirdNET-Go's RTSP probe will keep the path attached across short
ESP32 reconnects.

## Open items

- Opus on the wire (saves WiFi airtime at cost of ESP32 CPU). Requires
  encoder on FW and matching `-f` input format on relay; deferred until
  audio bandwidth becomes a real problem.
