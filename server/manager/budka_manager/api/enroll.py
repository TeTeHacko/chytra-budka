"""Device-facing enrollment endpoint.

POST /api/v1/enroll   body: CSR PEM
  200  leaf PEM + sub-CA PEM    (approved device, key continuity holds)
  202  {"status": "pending"}    (TOFU queue — device re-POSTs the same CSR)
  403  {"detail": ...}          (operator denied)
  422  {"detail": ...}          (CSR validation failure)
  429  {"detail": ...}          (per-CN issuance rate limit)

Unauthenticated by design: the CSR + operator approval + key continuity ARE
the authorization (see enrollment.py).
"""

from __future__ import annotations

from fastapi import APIRouter, Depends, Request, Response
from fastapi.responses import JSONResponse
from sqlalchemy.ext.asyncio import AsyncSession

from ..db import get_session
from ..settings import get_settings

router = APIRouter(prefix="/api/v1")

MAX_CSR_BYTES = 8192


@router.post("/enroll")
async def enroll(request: Request, session: AsyncSession = Depends(get_session)) -> Response:
    service = request.app.state.enroll_service
    if service is None:
        return JSONResponse({"detail": "enrollment disabled"}, status_code=503)

    body = await request.body()
    if len(body) > MAX_CSR_BYTES:
        return JSONResponse({"detail": "CSR too large"}, status_code=413)

    outcome = await service.handle_csr(session, body)

    if outcome.status == "issued":
        return Response(content=outcome.cert_pem, media_type="application/x-pem-file")
    if outcome.status == "pending":
        return JSONResponse(
            {"status": "pending", "detail": outcome.detail},
            status_code=202,
            headers={"Retry-After": str(get_settings().enroll_retry_after_s)},
        )
    if outcome.status == "denied":
        return JSONResponse({"detail": outcome.detail}, status_code=403)
    if outcome.status == "rate_limited":
        return JSONResponse({"detail": outcome.detail}, status_code=429)
    return JSONResponse({"detail": outcome.detail}, status_code=422)
