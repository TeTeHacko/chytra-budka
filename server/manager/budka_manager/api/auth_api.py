"""Login endpoints for both auth modes + auth state for the SPA."""

from __future__ import annotations

import html
import logging

from fastapi import APIRouter, HTTPException, Request, Response
from fastapi.responses import HTMLResponse, JSONResponse, RedirectResponse
from pydantic import BaseModel

from ..auth import (
    STATE_COOKIE,
    Oidc,
    session_subject,
    set_session_cookie,
    verify_password,
)
from ..settings import get_settings

log = logging.getLogger("budka.auth")

router = APIRouter()

_oidc: Oidc | None = None


def _get_oidc() -> Oidc:
    global _oidc
    settings = get_settings()
    if settings.auth_mode != "oidc" or not settings.oidc_issuer:
        raise HTTPException(404, "OIDC not enabled")
    if _oidc is None:
        _oidc = Oidc(settings)
    return _oidc


def _redirect_uri(request: Request) -> str:
    settings = get_settings()
    base = settings.public_base_url.rstrip("/") or str(request.base_url).rstrip("/")
    return f"{base}/auth/oidc/callback"


@router.get("/api/auth/state")
async def auth_state(request: Request):
    settings = get_settings()
    sub = session_subject(request)
    return {
        "mode": settings.auth_mode,
        "logged_in": sub is not None,
        "user": sub,
    }


class LoginBody(BaseModel):
    password: str


@router.post("/api/login")
async def login(body: LoginBody):
    settings = get_settings()
    if settings.auth_mode != "password":
        raise HTTPException(404, f"password login not enabled (mode={settings.auth_mode})")
    if not verify_password(settings, body.password):
        raise HTTPException(401, "bad password")
    response = JSONResponse({"logged_in": True, "user": "operator"})
    set_session_cookie(response, settings, "operator")
    return response


@router.post("/api/logout")
async def logout():
    response = JSONResponse({"logged_in": False})
    response.delete_cookie("budka_session", path="/")
    return response


@router.get("/auth/oidc/login")
async def oidc_login(request: Request):
    oidc = _get_oidc()
    url, state_cookie = await oidc.authorize_url(_redirect_uri(request))
    response = RedirectResponse(url, status_code=302)
    response.set_cookie(
        STATE_COOKIE,
        state_cookie,
        max_age=600,
        httponly=True,
        secure=True,
        samesite="lax",
        path="/auth",
    )
    return response


def _login_error_page(detail: str, status: int) -> HTMLResponse:
    """A friendly login-failure page. The callback is a browser redirect
    target, so a bare HTTPException would render as raw `{"detail":...}` JSON —
    show a small themed page with a way back to the login screen instead."""
    body = f"""<!doctype html><html lang="cs"><head><meta charset="utf-8">
<title>Přihlášení – Chytrá budka</title>
<meta name="viewport" content="width=device-width,initial-scale=1">
<style>
 body{{font-family:system-ui,-apple-system,sans-serif;background:#14161a;color:#e8e8e8;
      display:grid;place-items:center;min-height:100vh;margin:0}}
 .card{{max-width:26rem;padding:2rem;text-align:center;line-height:1.5}}
 h1{{font-size:1.3rem;margin:0 0 .5rem}} .err{{color:#ff8a8a;margin:1rem 0}}
 a{{color:#7fbfff;text-decoration:none}} a:hover{{text-decoration:underline}}
</style></head><body><div class="card">
 <h1>🐦 Přihlášení se nezdařilo</h1>
 <p class="err">{html.escape(detail)}</p>
 <p><a href="/">← Zpět na přihlášení</a></p>
</div></body></html>"""
    return HTMLResponse(body, status_code=status)


@router.get("/auth/oidc/callback")
async def oidc_callback(request: Request, code: str = "", state: str = "") -> Response:
    oidc = _get_oidc()
    try:
        st = oidc.read_state_cookie(request.cookies.get(STATE_COOKIE))
        if not code or st is None or st.get("state") != state:
            raise HTTPException(400, "Neplatný stav přihlášení – zkus to prosím znovu.")
        user = await oidc.exchange(code, str(st["verifier"]), _redirect_uri(request))
    except HTTPException as e:
        return _login_error_page(str(e.detail), e.status_code)
    except Exception:
        log.exception("OIDC callback failed")
        return _login_error_page("Chyba při komunikaci s poskytovatelem přihlášení.", 502)
    log.info("OIDC login: %s", user)
    response = RedirectResponse("/", status_code=302)
    response.delete_cookie(STATE_COOKIE, path="/auth")
    set_session_cookie(response, get_settings(), user)
    return response
