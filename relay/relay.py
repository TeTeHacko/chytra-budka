"""relay.py — HTTP audio chunks → ffmpeg → RTSP via mediamtx.

Receives chunked HTTP POST from ESP32 (or any compliant client), pipes raw
PCM into a per-stream ffmpeg subprocess that publishes RTSP into a local
mediamtx instance. BirdNET-Go (or any RTSP consumer) pulls from mediamtx.

TLS is intentionally NOT handled here — front the relay with an HTTPS
reverse proxy or run it on a private network / VPN.
"""

from __future__ import annotations

import argparse
import asyncio
import hmac
import ipaddress
import logging
import os
import shutil
import signal
import sys
import time
import tomllib
from dataclasses import dataclass, field
from pathlib import Path
from urllib.parse import urlsplit

from aiohttp import web
from prometheus_client import (
    CONTENT_TYPE_LATEST,
    CollectorRegistry,
    Counter,
    Gauge,
    generate_latest,
)

log = logging.getLogger("chytra-budka-relay")


# ─── Config ─────────────────────────────────────────────────────────────────


@dataclass
class StreamConfig:
    name: str
    allowed_nets: list[ipaddress._BaseNetwork] = field(default_factory=list)
    gap_silence_seconds: float = 30.0
    idle_timeout_seconds: float = 10.0
    total_timeout_seconds: float = 0.0  # 0 = unlimited

    def ip_allowed(self, remote: str | None) -> bool:
        if not self.allowed_nets:
            return True
        if not remote:
            return False
        try:
            addr = ipaddress.ip_address(remote.split("%", 1)[0])
        except ValueError:
            return False
        return any(addr in net for net in self.allowed_nets)


@dataclass
class Config:
    listen_host: str
    listen_port: int
    auth_token: str
    rtsp_host: str
    rtsp_port: int
    ffmpeg_bin: str
    input_format: str
    sample_rate: int
    channels: int
    output_codec: str
    output_sample_rate: int  # 0 = same as input
    output_channels: int  # 0 = same as input
    streams: dict[str, StreamConfig]
    default_stream: StreamConfig

    @classmethod
    def from_file(cls, path: Path) -> Config:
        data = tomllib.loads(path.read_text())
        env_var = data["server"].get("auth_token_env", "RELAY_AUTH_TOKEN")
        token = os.environ.get(env_var)
        if not token:
            raise SystemExit(f"missing env {env_var}")

        listen = data["server"]["listen"]
        host, port = _parse_host_port(listen)

        # Defaults block first — per-stream sections inherit any keys they
        # do not override. Without this fallback, a [streams.foo] section
        # that only sets allowed_ips would silently revert gap_silence,
        # idle_timeout, etc. to hardcoded class defaults rather than the
        # operator's [defaults] block.
        defaults_cfg = data.get("defaults", {}) or {}
        default_allowed = [
            ipaddress.ip_network(c, strict=False) for c in (defaults_cfg.get("allowed_ips") or [])
        ]
        default_gap = float(defaults_cfg.get("gap_silence_seconds", 30.0))
        default_idle = float(defaults_cfg.get("idle_timeout_seconds", 10.0))
        default_total = float(defaults_cfg.get("total_timeout_seconds", 0.0))

        default_stream = StreamConfig(
            name="<default>",
            allowed_nets=default_allowed,
            gap_silence_seconds=default_gap,
            idle_timeout_seconds=default_idle,
            total_timeout_seconds=default_total,
        )

        streams: dict[str, StreamConfig] = {}
        for name, scfg in (data.get("streams") or {}).items():
            allowed = scfg.get("allowed_ips")
            if allowed is None:
                nets = default_allowed
            else:
                nets = [ipaddress.ip_network(c, strict=False) for c in allowed]
            streams[name] = StreamConfig(
                name=name,
                allowed_nets=nets,
                gap_silence_seconds=float(scfg.get("gap_silence_seconds", default_gap)),
                idle_timeout_seconds=float(scfg.get("idle_timeout_seconds", default_idle)),
                total_timeout_seconds=float(scfg.get("total_timeout_seconds", default_total)),
            )

        ffmpeg_cfg = data["ffmpeg"]
        return cls(
            listen_host=host,
            listen_port=port,
            auth_token=token,
            rtsp_host=data["mediamtx"]["rtsp_host"],
            rtsp_port=int(data["mediamtx"]["rtsp_port"]),
            ffmpeg_bin=ffmpeg_cfg["binary"],
            input_format=ffmpeg_cfg.get("input_format", "s16le"),
            sample_rate=int(ffmpeg_cfg["sample_rate"]),
            channels=int(ffmpeg_cfg["channels"]),
            output_codec=ffmpeg_cfg["output_codec"],
            output_sample_rate=int(ffmpeg_cfg.get("output_sample_rate", 0)),
            output_channels=int(ffmpeg_cfg.get("output_channels", 0)),
            streams=streams,
            default_stream=default_stream,
        )

    def stream_for(self, name: str) -> StreamConfig:
        return self.streams.get(name, self.default_stream)


def _parse_host_port(listen: str) -> tuple[str, int]:
    # urlsplit needs a scheme; "//host:port" parses bracketed IPv6 correctly.
    parts = urlsplit("//" + listen)
    if parts.port is None:
        raise SystemExit(f"invalid listen address (no port): {listen!r}")
    host = parts.hostname or "0.0.0.0"
    return host, parts.port


# ─── Prometheus ─────────────────────────────────────────────────────────────


class Metrics:
    def __init__(self, registry: CollectorRegistry):
        self.registry = registry
        self.active_streams = Gauge(
            "chytra_relay_active_streams",
            "Active stream sessions (writer connected or lingering)",
            registry=registry,
        )
        self.bytes_total = Counter(
            "chytra_relay_bytes_total",
            "Bytes received from clients",
            ["stream"],
            registry=registry,
        )
        self.ffmpeg_starts = Counter(
            "chytra_relay_ffmpeg_starts_total",
            "ffmpeg subprocess starts",
            ["stream"],
            registry=registry,
        )
        self.ffmpeg_exits = Counter(
            "chytra_relay_ffmpeg_exits_total",
            "ffmpeg subprocess exits",
            ["stream", "status"],
            registry=registry,
        )
        self.unauthorized = Counter(
            "chytra_relay_unauthorized_total",
            "Requests rejected for bad bearer or forbidden IP",
            ["reason"],
            registry=registry,
        )
        self.concurrent_rejected = Counter(
            "chytra_relay_concurrent_rejected_total",
            "Concurrent writer attempts rejected with 409",
            ["stream"],
            registry=registry,
        )
        self.last_chunk_age = Gauge(
            "chytra_relay_last_chunk_age_seconds",
            "Seconds since last chunk received from active writer",
            ["stream"],
            registry=registry,
        )
        self.idle_timeouts = Counter(
            "chytra_relay_idle_timeouts_total",
            "Streams cut due to idle timeout",
            ["stream"],
            registry=registry,
        )

    def prime(self, stream_names: list[str]) -> None:
        """Initialize all known label combinations to 0 so dashboards see
        the time series before the first real event. Without this, a
        counter that has never been incremented is absent from /metrics
        and rate() expressions return no data."""
        for reason in ("bearer", "ip"):
            self.unauthorized.labels(reason=reason).inc(0)
        for name in stream_names:
            self.bytes_total.labels(stream=name).inc(0)
            self.ffmpeg_starts.labels(stream=name).inc(0)
            self.concurrent_rejected.labels(stream=name).inc(0)
            self.idle_timeouts.labels(stream=name).inc(0)


# ─── Stream session ─────────────────────────────────────────────────────────


class StreamSession:
    """One ffmpeg subprocess per stream name. Stdin = raw PCM, RTSP out.

    Lifecycle:
      - created on first POST / re-creation after ffmpeg exit
      - feed() pumps client chunks into ffmpeg stdin
      - on writer disconnect, linger() task feeds silence for gap_silence_seconds
        keeping the RTSP path alive across short reconnects (WiFi flap)
      - second POST to same name while session is alive returns 409
        unless the previous request has finished and only the linger is running
    """

    def __init__(self, cfg: Config, scfg: StreamConfig, metrics: Metrics):
        self.cfg = cfg
        self.scfg = scfg
        self.name = scfg.name if scfg.name != "<default>" else "default"
        self.metrics = metrics
        self.proc: asyncio.subprocess.Process | None = None
        self.stderr_task: asyncio.Task | None = None
        self.linger_task: asyncio.Task | None = None
        # writer_active: there is currently a POST request feeding this session
        self.writer_active: bool = False
        self.bytes_in: int = 0
        self.last_chunk_mono: float = time.monotonic()
        self.created_mono: float = time.monotonic()
        # codec negotiated at start(): 'pcm' or 'flac'. Used to decide
        # whether linger silence keeps the FLAC frame stream alive — for
        # FLAC we just stop feeding (ffmpeg holds the last decoded buffer
        # under -re) since synthetic PCM zeros would corrupt the stream.
        self.codec: str = "pcm"
        # Per-session input params (may differ from cfg defaults when
        # the client advertises rate/channels in Content-Type header).
        self.input_rate: int = cfg.sample_rate
        self.input_channels: int = cfg.channels

    # ─ lifecycle ─

    async def start(self, name: str, codec: str = "pcm", rate: int = 0, channels: int = 0):
        """Start ffmpeg subprocess for this stream.

        codec: 'pcm' for raw audio/L16 input (legacy default), 'flac' for
        audio/flac input. The output codec/rate/channels are operator-set
        in [ffmpeg] config; ffmpeg auto-decodes the FLAC container so the
        downstream RTSP stream stays identical between codecs.

        rate/channels: if >0, override the config defaults for this session
        (typically parsed from the client's Content-Type header).
        """
        self.name = name
        self.input_rate = rate if rate > 0 else self.cfg.sample_rate
        self.input_channels = channels if channels > 0 else self.cfg.channels
        rtsp_url = f"rtsp://{self.cfg.rtsp_host}:{self.cfg.rtsp_port}/{name}"
        # Input args differ by codec; everything else is shared.
        if codec == "flac":
            in_args = ["-f", "flac"]  # ffmpeg autodetects sample_rate / channels
        else:
            in_args = [
                "-f",
                self.cfg.input_format,
                "-ar",
                str(self.input_rate),
                "-ac",
                str(self.input_channels),
            ]
        # Output filter / resample args — applied when output differs from input.
        out_args: list[str] = []
        out_rate = self.cfg.output_sample_rate
        out_ch = self.cfg.output_channels
        if out_ch > 0 and out_ch != self.input_channels:
            out_args += ["-ac", str(out_ch)]
        if out_rate > 0 and out_rate != self.input_rate:
            out_args += ["-ar", str(out_rate)]
        cmd = [
            self.cfg.ffmpeg_bin,
            "-hide_banner",
            "-loglevel",
            "warning",
            # NOTE: -re intentionally REMOVED. The relay feeds ffmpeg via
            # stdin pipe which is already naturally rate-limited by the
            # ESP32's real-time audio clock. Adding -re caused ffmpeg to
            # drift behind when the source clock is slightly faster than
            # nominal (typical for cheap I2S oscillators), accumulating
            # unbounded lag (observed 14→27s/hour growth).
            *in_args,
            "-i",
            "-",
            *out_args,
            "-c:a",
            self.cfg.output_codec,
            "-f",
            "rtsp",
            "-rtsp_transport",
            "tcp",
            rtsp_url,
        ]
        log.info(
            "[%s] starting ffmpeg (codec=%s, in=%dHz/%dch, out=%dHz/%dch) → %s",
            name,
            codec,
            self.input_rate,
            self.input_channels,
            out_rate or self.input_rate,
            out_ch or self.input_channels,
            rtsp_url,
        )
        self.proc = await asyncio.create_subprocess_exec(
            *cmd,
            stdin=asyncio.subprocess.PIPE,
            stdout=asyncio.subprocess.DEVNULL,
            stderr=asyncio.subprocess.PIPE,
        )
        self.metrics.ffmpeg_starts.labels(stream=name).inc()
        self.codec = codec
        self.stderr_task = asyncio.create_task(self._drain_stderr(), name=f"stderr-{name}")

    def is_proc_alive(self) -> bool:
        return self.proc is not None and self.proc.returncode is None

    async def _drain_stderr(self):
        proc = self.proc
        if not proc or not proc.stderr:
            return
        try:
            async for line in proc.stderr:
                log.warning(
                    "[%s] ffmpeg: %s",
                    self.name,
                    line.decode(errors="replace").rstrip(),
                )
        except asyncio.CancelledError:
            pass

    # ─ data path ─

    async def feed(self, chunk: bytes):
        if not self.proc or not self.proc.stdin:
            return
        try:
            self.proc.stdin.write(chunk)
            await self.proc.stdin.drain()
        except (BrokenPipeError, ConnectionResetError):
            log.warning("[%s] ffmpeg stdin closed mid-stream", self.name)
            raise
        self.bytes_in += len(chunk)
        self.last_chunk_mono = time.monotonic()
        self.metrics.bytes_total.labels(stream=self.name).inc(len(chunk))

    async def feed_silence_chunk(self, chunk: bytes):
        """Feed silence without bumping bytes_in counter (linger keepalive)."""
        if not self.proc or not self.proc.stdin:
            return
        try:
            self.proc.stdin.write(chunk)
            await self.proc.stdin.drain()
        except (BrokenPipeError, ConnectionResetError):
            raise

    async def stop(self, reason: str = "stop"):
        await self._cancel_linger()
        proc = self.proc
        self.proc = None
        if proc and proc.returncode is None:
            try:
                if proc.stdin:
                    proc.stdin.close()
            except Exception:
                pass
            try:
                await asyncio.wait_for(proc.wait(), timeout=5)
            except TimeoutError:
                proc.kill()
                await proc.wait()
        rc = proc.returncode if proc else None
        log.info(
            "[%s] ffmpeg exited rc=%s bytes=%d age=%.1fs reason=%s",
            self.name,
            rc,
            self.bytes_in,
            time.monotonic() - self.created_mono,
            reason,
        )
        if proc:
            self.metrics.ffmpeg_exits.labels(
                stream=self.name, status="ok" if rc == 0 else "error"
            ).inc()
        if self.stderr_task and not self.stderr_task.done():
            self.stderr_task.cancel()
            try:
                await self.stderr_task
            except (asyncio.CancelledError, Exception):
                pass
        self.stderr_task = None

    async def _cancel_linger(self):
        if self.linger_task and not self.linger_task.done():
            self.linger_task.cancel()
            try:
                await self.linger_task
            except (asyncio.CancelledError, Exception):
                pass
        self.linger_task = None


# ─── App state ──────────────────────────────────────────────────────────────


class AppState:
    def __init__(self, cfg: Config, metrics: Metrics):
        self.cfg = cfg
        self.metrics = metrics
        self.sessions: dict[str, StreamSession] = {}
        self.locks: dict[str, asyncio.Lock] = {}

    def lock_for(self, name: str) -> asyncio.Lock:
        lk = self.locks.get(name)
        if lk is None:
            lk = asyncio.Lock()
            self.locks[name] = lk
        return lk

    async def shutdown(self):
        log.info("shutting down %d session(s)", len(self.sessions))
        await asyncio.gather(
            *(s.stop("shutdown") for s in list(self.sessions.values())),
            return_exceptions=True,
        )
        self.sessions.clear()


# ─── HTTP handlers ──────────────────────────────────────────────────────────


def _check_auth(cfg: Config, request: web.Request) -> bool:
    auth = request.headers.get("Authorization", "")
    expected = f"Bearer {cfg.auth_token}"
    return hmac.compare_digest(auth.encode(), expected.encode())


def _valid_stream_name(name: str) -> bool:
    if not (1 <= len(name) <= 64):
        return False
    return all(c.isalnum() or c in "-_" for c in name)


def _detect_codec(request: web.Request) -> str:
    """Map incoming Content-Type to the codec ffmpeg should expect.

    Anything matching audio/flac → 'flac' (libFLAC stream). Everything else
    (audio/L16, audio/x-l16, missing header) defaults to 'pcm' so legacy
    clients keep working unchanged.
    """
    ct = (request.headers.get("Content-Type") or "").lower()
    if "audio/flac" in ct or ct.startswith("audio/x-flac"):
        return "flac"
    return "pcm"


def _parse_audio_params(request: web.Request) -> tuple[int, int]:
    """Extract rate and channels from Content-Type header (RFC 3190 audio/L16).

    Format: audio/L16; rate=16000; channels=2
    Returns (rate, channels) — 0 means "use config default".
    """
    ct = request.headers.get("Content-Type") or ""
    rate = 0
    channels = 0
    for part in ct.split(";"):
        part = part.strip().lower()
        if part.startswith("rate="):
            try:
                rate = int(part[5:])
            except ValueError:
                pass
        elif part.startswith("channels="):
            try:
                channels = int(part[9:])
            except ValueError:
                pass
    return rate, channels


async def handle_audio(request: web.Request) -> web.Response:
    state: AppState = request.app["state"]
    cfg = state.cfg
    metrics = state.metrics

    if not _check_auth(cfg, request):
        metrics.unauthorized.labels(reason="bearer").inc()
        return web.Response(status=401, text="unauthorized")

    name = request.match_info["name"]
    if not _valid_stream_name(name):
        return web.Response(status=400, text="bad stream name")

    scfg = cfg.stream_for(name)
    if not scfg.ip_allowed(request.remote):
        metrics.unauthorized.labels(reason="ip").inc()
        log.warning("[%s] forbidden IP %s", name, request.remote)
        return web.Response(status=403, text="forbidden")

    codec = _detect_codec(request)
    ct_rate, ct_channels = _parse_audio_params(request)

    # Acquire stream lock for the takeover/create decision
    session: StreamSession
    async with state.lock_for(name):
        existing = state.sessions.get(name)
        if existing and existing.writer_active:
            metrics.concurrent_rejected.labels(stream=name).inc()
            log.warning("[%s] rejecting concurrent writer from %s", name, request.remote)
            return web.Response(status=409, text="stream busy")

        # Reuse existing ffmpeg only if it's alive AND was started for the
        # same codec — switching audio/L16 ↔ audio/flac mid-pipeline would
        # confuse ffmpeg's input demuxer (it sniffs once at start). Force
        # a fresh subprocess on codec change.
        if existing and existing.is_proc_alive() and existing.codec == codec:
            await existing._cancel_linger()
            session = existing
            log.info("[%s] writer reattached from %s (codec=%s)", name, request.remote, codec)
        else:
            if existing:
                await existing.stop("respawn" if existing.codec == codec else "codec_change")
            session = StreamSession(cfg, scfg, metrics)
            await session.start(name, codec=codec, rate=ct_rate, channels=ct_channels)
            state.sessions[name] = session
            metrics.active_streams.set(len(state.sessions))
            log.info("[%s] writer connected from %s (codec=%s)", name, request.remote, codec)
        session.writer_active = True
        session.last_chunk_mono = time.monotonic()

    # Idle / total watchdog runs alongside body read
    stop_event = asyncio.Event()
    started_mono = time.monotonic()

    async def watchdog():
        while not stop_event.is_set():
            try:
                await asyncio.wait_for(stop_event.wait(), timeout=1.0)
                return
            except TimeoutError:
                pass
            now = time.monotonic()
            age = now - session.last_chunk_mono
            metrics.last_chunk_age.labels(stream=name).set(age)
            if age > scfg.idle_timeout_seconds:
                log.warning("[%s] idle timeout (%.1fs), cutting", name, age)
                metrics.idle_timeouts.labels(stream=name).inc()
                # Trigger client read failure by closing ffmpeg stdin? No —
                # we want to leave session alive for linger. Just abort the
                # request task by transport close.
                tr = request.transport
                if tr is not None:
                    tr.close()
                return
            if scfg.total_timeout_seconds > 0 and now - started_mono > scfg.total_timeout_seconds:
                log.warning(
                    "[%s] total timeout (%.0fs), cutting",
                    name,
                    scfg.total_timeout_seconds,
                )
                tr = request.transport
                if tr is not None:
                    tr.close()
                return

    wd_task = asyncio.create_task(watchdog(), name=f"watchdog-{name}")

    feed_error: BaseException | None = None
    try:
        async for chunk in request.content.iter_chunked(8192):
            await session.feed(chunk)
    except asyncio.CancelledError:
        raise
    except (BrokenPipeError, ConnectionResetError) as exc:
        feed_error = exc
        log.warning("[%s] ffmpeg pipe broken: %s", name, exc)
    except Exception as exc:
        feed_error = exc
        log.warning("[%s] read error: %s", name, exc)
    finally:
        stop_event.set()
        try:
            await wd_task
        except Exception:
            pass

    log.info(
        "[%s] writer disconnected (bytes=%d, dur=%.1fs)",
        name,
        session.bytes_in,
        time.monotonic() - started_mono,
    )

    # Schedule linger or stop. FLAC streams can't be lingered with
    # synthetic silence — we'd have to run a libFLAC encoder here to keep
    # frame boundaries valid. Drop the session immediately on disconnect;
    # the next reconnect just opens a fresh stream (mediamtx tolerates
    # short gaps via ffmpeg's `-re` buffering).
    async with state.lock_for(name):
        session.writer_active = False
        if feed_error is not None or not session.is_proc_alive():
            await _drop_session(state, name, session, "writer_error")
        elif session.codec == "flac":
            await _drop_session(state, name, session, "flac_no_linger")
        elif scfg.gap_silence_seconds > 0:
            session.linger_task = asyncio.create_task(
                _linger_silence(state, name, session, scfg.gap_silence_seconds),
                name=f"linger-{name}",
            )
        else:
            await _drop_session(state, name, session, "no_linger")

    return web.Response(status=204)


async def _drop_session(state: AppState, name: str, session: StreamSession, reason: str):
    await session.stop(reason)
    if state.sessions.get(name) is session:
        state.sessions.pop(name, None)
    state.metrics.active_streams.set(len(state.sessions))


async def _linger_silence(state: AppState, name: str, session: StreamSession, seconds: float):
    """Feed silence into ffmpeg for `seconds` to keep RTSP path alive.

    Without -re, ffmpeg processes stdin as fast as possible, so we must
    rate-limit silence feeding ourselves at ~real-time pace (100 ms chunks
    with 100 ms sleeps between them).
    """
    # Use session's actual input params (ffmpeg stdin expects the input format).
    bytes_per_sec = session.input_rate * session.input_channels * 2
    chunk_duration = 0.1  # 100 ms
    chunk = bytes(int(bytes_per_sec * chunk_duration))  # 100 ms of silence
    log.info("[%s] lingering with silence for %.1fs", name, seconds)
    deadline = time.monotonic() + seconds
    natural_exit = False
    try:
        while time.monotonic() < deadline:
            if not session.is_proc_alive():
                break
            try:
                await session.feed_silence_chunk(chunk)
            except (BrokenPipeError, ConnectionResetError):
                break
            await asyncio.sleep(chunk_duration)
        natural_exit = True
        log.info("[%s] linger expired, dropping", name)
    except asyncio.CancelledError:
        log.info("[%s] linger cancelled (writer reattached)", name)
        raise

    if natural_exit:
        async with state.lock_for(name):
            if state.sessions.get(name) is session and not session.writer_active:
                await _drop_session(state, name, session, "linger_expired")


async def handle_health(_request: web.Request) -> web.Response:
    return web.Response(text="ok\n")


async def handle_streams(request: web.Request) -> web.Response:
    state: AppState = request.app["state"]
    now = time.monotonic()
    if not state.sessions:
        return web.Response(text="(no active streams)\n")
    lines = []
    for name, s in state.sessions.items():
        rc = s.proc.returncode if s.proc else "none"
        age = now - s.last_chunk_mono
        lines.append(
            f"{name}\twriter={'yes' if s.writer_active else 'no'}\t"
            f"linger={'yes' if s.linger_task and not s.linger_task.done() else 'no'}\t"
            f"bytes={s.bytes_in}\trc={rc}\tlast_chunk_age={age:.1f}s"
        )
    return web.Response(text="\n".join(lines) + "\n")


async def handle_metrics(request: web.Request) -> web.Response:
    state: AppState = request.app["state"]
    body = generate_latest(state.metrics.registry)
    return web.Response(body=body, content_type=CONTENT_TYPE_LATEST.split(";")[0])


# ─── App wiring ─────────────────────────────────────────────────────────────


def build_app(cfg: Config) -> web.Application:
    app = web.Application(client_max_size=1024 * 1024)  # streaming endpoint reads body manually
    metrics = Metrics(CollectorRegistry())
    metrics.prime(list(cfg.streams.keys()))
    state = AppState(cfg, metrics)
    app["state"] = state
    app.router.add_post("/audio/{name}", handle_audio)
    app.router.add_get("/health", handle_health)
    app.router.add_get("/streams", handle_streams)
    app.router.add_get("/metrics", handle_metrics)
    return app


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser()
    p.add_argument("--config", type=Path, default=Path("relay.toml"))
    p.add_argument("--log-level", default="INFO")
    return p.parse_args()


async def run(cfg: Config) -> None:
    app = build_app(cfg)
    runner = web.AppRunner(app, access_log=None)
    await runner.setup()
    site = web.TCPSite(runner, host=cfg.listen_host, port=cfg.listen_port)
    await site.start()
    log.info("listening on %s:%d", cfg.listen_host, cfg.listen_port)

    stop_event = asyncio.Event()
    loop = asyncio.get_running_loop()
    for sig in (signal.SIGINT, signal.SIGTERM):
        loop.add_signal_handler(sig, stop_event.set)

    try:
        await stop_event.wait()
    finally:
        log.info("shutdown signal received")
        await app["state"].shutdown()
        await runner.cleanup()


def main() -> int:
    args = parse_args()
    logging.basicConfig(
        level=args.log_level,
        format="%(asctime)s %(levelname)s %(name)s: %(message)s",
    )
    # A malformed config (bad TOML, missing key, unreadable file) must not
    # crash-loop the daemon every RestartSec with an opaque traceback —
    # log a clean error and exit non-zero. SystemExit (e.g. an explicit
    # config-validation abort inside from_file) is allowed to propagate.
    try:
        cfg = Config.from_file(args.config)
    except SystemExit:
        raise
    except Exception as e:
        logging.getLogger("relay").error("config load failed (%s): %s", type(e).__name__, e)
        return 1
    # Fail fast on a missing ffmpeg: otherwise the daemon starts "healthy" and
    # only errors when the FIRST stream tries to spawn ffmpeg, logged as an
    # opaque subprocess failure long after deploy. Assert it at startup.
    if shutil.which(cfg.ffmpeg_bin) is None:
        logging.getLogger("relay").error(
            "ffmpeg not found at %r — install ffmpeg or fix [ffmpeg] binary in %s",
            cfg.ffmpeg_bin,
            args.config,
        )
        return 1
    asyncio.run(run(cfg))
    return 0


if __name__ == "__main__":
    sys.exit(main())
