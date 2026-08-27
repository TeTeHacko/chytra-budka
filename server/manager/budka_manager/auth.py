"""Operator authentication for the web UI/API.

Two modes (CB_AUTH_MODE):

  password  (default) single operator password from a file mount + an
            HMAC-signed session cookie. Zero external dependencies — the
            self-hoster path.
  oidc      standard OpenID Connect authorization-code flow (+PKCE) against
            any issuer (Keycloak, Authentik, ...). The manager is a
            confidential client; ID tokens are verified against the issuer's
            JWKS. Optional allow-list of usernames.
  off       no auth (development only; logs a warning).

Device-facing endpoints are NOT session-authed and must stay exempt:
/api/v1/enroll (CSR + TOFU), /api/v1/ota/* (bearer token), /ingest/*
(bearer token), /healthz.
"""

from __future__ import annotations

import base64
import hashlib
import hmac
import json
import logging
import secrets
import time
from typing import Any

import httpx
import jwt as pyjwt
from fastapi import HTTPException, Request, Response

from .settings import Settings, get_settings

log = logging.getLogger("budka.auth")

COOKIE = "budka_session"
STATE_COOKIE = "budka_oidc_state"


# --- signed session cookie ----------------------------------------------------


def _b64(data: bytes) -> str:
    return base64.urlsafe_b64encode(data).rstrip(b"=").decode()


def _unb64(s: str) -> bytes:
    return base64.urlsafe_b64decode(s + "=" * (-len(s) % 4))


def _secret(settings: Settings) -> bytes:
    return settings.session_secret_file.read_bytes().strip()


def make_session(settings: Settings, sub: str) -> str:
    payload = _b64(
        json.dumps({"sub": sub, "exp": int(time.time()) + settings.session_ttl_s}).encode()
    )
    sig = _b64(hmac.new(_secret(settings), payload.encode(), hashlib.sha256).digest())
    return f"{payload}.{sig}"


def check_session(settings: Settings, token: str | None) -> str | None:
    """Returns the session subject, or None."""
    if not token or "." not in token:
        return None
    payload, sig = token.rsplit(".", 1)
    want = _b64(hmac.new(_secret(settings), payload.encode(), hashlib.sha256).digest())
    if not hmac.compare_digest(sig, want):
        return None
    try:
        data = json.loads(_unb64(payload))
    except ValueError:
        return None
    if int(data.get("exp", 0)) < time.time():
        return None
    return str(data.get("sub") or "") or None


def set_session_cookie(response: Response, settings: Settings, sub: str) -> None:
    response.set_cookie(
        COOKIE,
        make_session(settings, sub),
        max_age=settings.session_ttl_s,
        httponly=True,
        secure=True,
        samesite="lax",
        path="/",
    )


def session_subject(request: Request) -> str | None:
    settings = get_settings()
    if settings.auth_mode == "off":
        return "anonymous"
    return check_session(settings, request.cookies.get(COOKIE))


async def require_auth(request: Request) -> str:
    """FastAPI dependency for operator-only routes."""
    sub = session_subject(request)
    if sub is None:
        raise HTTPException(status_code=401, detail="not authenticated")
    return sub


# --- password mode --------------------------------------------------------------


def verify_password(settings: Settings, password: str) -> bool:
    try:
        want = settings.operator_password_file.read_text().strip()
    except OSError:
        log.error("operator password file %s unreadable", settings.operator_password_file)
        return False
    return bool(want) and hmac.compare_digest(password, want)


# --- OIDC mode ------------------------------------------------------------------


class Oidc:
    """Minimal, dependency-light OIDC code flow (+PKCE) for a confidential
    client. Discovery + JWKS cached for the process lifetime."""

    def __init__(self, settings: Settings) -> None:
        self.settings = settings
        self._meta: dict[str, Any] | None = None
        self._jwks: pyjwt.PyJWKClient | None = None

    async def metadata(self) -> dict[str, Any]:
        if self._meta is None:
            url = self.settings.oidc_issuer.rstrip("/") + "/.well-known/openid-configuration"
            async with httpx.AsyncClient(timeout=10) as client:
                r = await client.get(url)
                r.raise_for_status()
                self._meta = r.json()
        assert self._meta is not None
        return self._meta

    def _client_secret(self) -> str | None:
        """The confidential-client secret, or None for a PUBLIC client (PKCE,
        no secret — e.g. the Keycloak `cb.example.com` public client). Absent/empty
        file ⇒ public client."""
        try:
            return self.settings.oidc_client_secret_file.read_text().strip() or None
        except OSError:
            return None

    async def authorize_url(self, redirect_uri: str) -> tuple[str, str]:
        """Returns (url, state_cookie_value). PKCE verifier + CSRF state ride
        in one signed, short-lived cookie."""
        meta = await self.metadata()
        state = secrets.token_urlsafe(24)
        verifier = secrets.token_urlsafe(48)
        challenge = _b64(hashlib.sha256(verifier.encode()).digest())
        params = httpx.QueryParams(
            {
                "response_type": "code",
                "client_id": self.settings.oidc_client_id,
                "redirect_uri": redirect_uri,
                "scope": self.settings.oidc_scopes,
                "state": state,
                "code_challenge": challenge,
                "code_challenge_method": "S256",
            }
        )
        payload = _b64(
            json.dumps(
                {
                    "state": state,
                    "verifier": verifier,
                    "exp": int(time.time()) + 600,
                }
            ).encode()
        )
        sig = _b64(hmac.new(_secret(self.settings), payload.encode(), hashlib.sha256).digest())
        return f"{meta['authorization_endpoint']}?{params}", f"{payload}.{sig}"

    def read_state_cookie(self, cookie: str | None) -> dict[str, Any] | None:
        if not cookie or "." not in cookie:
            return None
        payload, sig = cookie.rsplit(".", 1)
        want = _b64(hmac.new(_secret(self.settings), payload.encode(), hashlib.sha256).digest())
        if not hmac.compare_digest(sig, want):
            return None
        data = json.loads(_unb64(payload))
        if int(data.get("exp", 0)) < time.time():
            return None
        return data

    async def exchange(self, code: str, verifier: str, redirect_uri: str) -> str:
        """Code → tokens → verified ID token. Returns the username."""
        meta = await self.metadata()
        data = {
            "grant_type": "authorization_code",
            "code": code,
            "redirect_uri": redirect_uri,
            "client_id": self.settings.oidc_client_id,
            "code_verifier": verifier,
        }
        # Confidential client → send the secret; public client (PKCE) → omit it
        # entirely (Keycloak rejects a secret on a public client).
        secret = self._client_secret()
        if secret:
            data["client_secret"] = secret
        async with httpx.AsyncClient(timeout=10) as client:
            r = await client.post(meta["token_endpoint"], data=data)
            r.raise_for_status()
            tokens = r.json()

        id_token = tokens.get("id_token")
        if not id_token:
            raise HTTPException(502, "issuer returned no id_token")
        if self._jwks is None:
            self._jwks = pyjwt.PyJWKClient(meta["jwks_uri"])
        key = self._jwks.get_signing_key_from_jwt(id_token)
        claims = pyjwt.decode(
            id_token,
            key.key,
            algorithms=["RS256", "ES256"],
            audience=self.settings.oidc_client_id,
            issuer=meta["issuer"],
        )
        # Match the allow-list against ANY identity claim, not just the first
        # non-null one: Keycloak sends preferred_username=`alice` while operators
        # naturally write the e-mail `alice@example.com` — accept either so nobody has
        # to guess which claim the issuer minted the token with.
        candidates = {
            str(claims[c]) for c in ("preferred_username", "email", "sub") if claims.get(c)
        }
        user = str(claims.get("preferred_username") or claims.get("email") or claims.get("sub"))
        allowed = self.settings.oidc_allowed_users_list
        if allowed and not (candidates & set(allowed)):
            log.warning("OIDC user %r (claims %s) not in allow-list", user, sorted(candidates))
            raise HTTPException(403, "Uživatel nemá přístup (není na allow-listu).")
        return user
