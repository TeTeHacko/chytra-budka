"""OTA artifact store.

Preserves the legacy publish contract of firmware/tools/ota_upload.sh
(two-phase: write .new, fsync, rename bin FIRST, manifest SECOND — the
manifest is the commit point) on the shared `ota` volume that nginx serves
read-only with Basic auth. Adds a per-version archive so ota_rollback.sh has
server-side history too.

Layout:  <ota_dir>/<app>/<app>.bin + version.json
         <ota_dir>/<app>/archive/<version>/{<app>.bin,version.json}
"""

from __future__ import annotations

import hashlib
import json
import logging
import os
import re
import shutil
from dataclasses import dataclass
from pathlib import Path

log = logging.getLogger("budka.ota")

APP_RE = re.compile(r"^[a-z0-9][a-z0-9_-]{0,63}$")
VERSION_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$")
REQUIRED_MANIFEST_KEYS = ("app", "version", "sha256", "size")


class OtaError(Exception):
    """Validation failure — maps to HTTP 422."""


@dataclass
class OtaResult:
    app: str
    version: str
    sha256: str
    size: int


def parse_manifest(raw: bytes) -> dict:
    try:
        manifest = json.loads(raw)
    except ValueError as e:
        raise OtaError(f"manifest is not valid JSON: {e}") from e
    if not isinstance(manifest, dict):
        raise OtaError("manifest must be a JSON object")
    missing = [k for k in REQUIRED_MANIFEST_KEYS if k not in manifest]
    if missing:
        raise OtaError(f"manifest missing keys: {', '.join(missing)}")
    if not APP_RE.match(str(manifest["app"])):
        raise OtaError(f"bad app name {manifest['app']!r}")
    if not VERSION_RE.match(str(manifest["version"])):
        raise OtaError(f"bad version {manifest['version']!r}")
    return manifest


def publish(ota_dir: Path, bin_data: bytes, manifest_raw: bytes) -> OtaResult:
    manifest = parse_manifest(manifest_raw)
    app = str(manifest["app"])
    version = str(manifest["version"])

    digest = hashlib.sha256(bin_data).hexdigest()
    if digest != str(manifest["sha256"]).lower():
        raise OtaError(f"sha256 mismatch: manifest says {manifest['sha256']}, bin is {digest}")
    if int(manifest["size"]) != len(bin_data):
        raise OtaError(f"size mismatch: manifest says {manifest['size']}, bin is {len(bin_data)} B")

    app_dir = ota_dir / app
    app_dir.mkdir(parents=True, exist_ok=True)
    bin_path = app_dir / f"{app}.bin"
    manifest_path = app_dir / "version.json"

    # Two-phase atomic publish, bin before manifest (legacy ordering).
    for path, data in ((bin_path, bin_data), (manifest_path, manifest_raw)):
        tmp = path.with_suffix(path.suffix + ".new")
        with tmp.open("wb") as f:
            f.write(data)
            f.flush()
            os.fsync(f.fileno())
        os.replace(tmp, path)
        path.chmod(0o644)

    archive_dir = app_dir / "archive" / version
    archive_dir.mkdir(parents=True, exist_ok=True)
    shutil.copy2(bin_path, archive_dir / bin_path.name)
    shutil.copy2(manifest_path, archive_dir / manifest_path.name)

    log.info("published %s %s (%d B, sha256 %s…)", app, version, len(bin_data), digest[:12])
    return OtaResult(app=app, version=version, sha256=digest, size=len(bin_data))


def current_manifest(ota_dir: Path, app: str) -> dict | None:
    if not APP_RE.match(app):
        raise OtaError(f"bad app name {app!r}")
    path = ota_dir / app / "version.json"
    try:
        return json.loads(path.read_text())
    except OSError:
        return None
