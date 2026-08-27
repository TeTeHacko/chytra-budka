"""OTA upload API — the curl target that replaces ota_upload.sh's rsync leg.

POST /api/v1/ota/upload   Authorization: Bearer <CB_OTA_TOKEN>
  multipart/form-data: bin=@chytra-budka.bin  manifest=@version.json
  200 {app, version, sha256, size}
  401/403 auth, 422 manifest/bin mismatch
"""

from __future__ import annotations

import hmac

from fastapi import APIRouter, Header, HTTPException, UploadFile

from .. import ota
from ..settings import get_settings

router = APIRouter(prefix="/api/v1/ota")


def _check_token(authorization: str | None) -> None:
    settings = get_settings()
    token = settings.read_token(settings.ota_token_file)
    if token is None:
        raise HTTPException(status_code=503, detail="upload token not configured")
    if authorization is None or not authorization.startswith("Bearer "):
        raise HTTPException(status_code=401, detail="missing bearer token")
    if not hmac.compare_digest(authorization.removeprefix("Bearer ").strip(), token):
        raise HTTPException(status_code=403, detail="bad token")


@router.post("/upload")
async def upload(
    bin: UploadFile, manifest: UploadFile, authorization: str | None = Header(default=None)
):
    _check_token(authorization)
    bin_data = await bin.read()
    manifest_raw = await manifest.read()
    try:
        result = ota.publish(get_settings().ota_dir, bin_data, manifest_raw)
    except ota.OtaError as e:
        raise HTTPException(status_code=422, detail=str(e)) from e
    return result


@router.get("/manifest/{app}")
async def manifest(app: str):
    try:
        current = ota.current_manifest(get_settings().ota_dir, app)
    except ota.OtaError as e:
        raise HTTPException(status_code=422, detail=str(e)) from e
    if current is None:
        raise HTTPException(status_code=404, detail="no published manifest")
    return current
