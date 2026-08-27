"""OIDC allow-list regression tests.

The allow-list used to compare only the first non-null identity claim, so an
operator entry of the e-mail (`alice@example.com`) never matched Keycloak's
`preferred_username` (`alice`) and every login 403'd. Any claim in the token must
be accepted, and non-members must still be rejected (no substring matching).
"""

from __future__ import annotations

import types

import httpx
import jwt as pyjwt
import pytest
from budka_manager import auth as A
from fastapi import HTTPException

# The token shape Keycloak actually mints for the example.com realm.
CLAIMS = {"preferred_username": "alice", "email": "alice@example.com", "sub": "uuid-1234"}


class _Settings:
    """Only the attributes exchange() touches."""

    oidc_client_id = "cb.example.com"
    oidc_issuer = "https://auth.example.com/realms/example.com"

    def __init__(self, allowed: tuple[str, ...]) -> None:
        self._allowed = allowed

    @property
    def oidc_allowed_users_list(self) -> tuple[str, ...]:
        return self._allowed

    @property
    def oidc_client_secret_file(self):
        # Absent file ⇒ public client (PKCE, no secret) — the cb.example.com case.
        def _raise() -> str:
            raise OSError("no secret file")

        return types.SimpleNamespace(read_text=_raise)


@pytest.fixture
def fake_issuer(monkeypatch: pytest.MonkeyPatch):
    """Stub the token endpoint + JWT verification; claims come from CLAIMS."""

    class _Resp:
        def raise_for_status(self) -> None: ...
        def json(self) -> dict:
            return {"id_token": "fake.jwt.token"}

    class _Client:
        async def __aenter__(self):
            return self

        async def __aexit__(self, *exc) -> bool:
            return False

        async def post(self, *a, **kw):
            return _Resp()

    monkeypatch.setattr(httpx, "AsyncClient", lambda *a, **kw: _Client())
    monkeypatch.setattr(pyjwt, "decode", lambda *a, **kw: CLAIMS)


def _oidc(allowed: tuple[str, ...]) -> A.Oidc:
    o = A.Oidc(_Settings(allowed))  # type: ignore[arg-type]
    o._meta = {
        "token_endpoint": "https://auth.example.com/token",
        "jwks_uri": "https://auth.example.com/jwks",
        "issuer": "https://auth.example.com/realms/example.com",
    }
    o._jwks = types.SimpleNamespace(  # type: ignore[assignment]
        get_signing_key_from_jwt=lambda t: types.SimpleNamespace(key="k")
    )
    return o


async def _exchange(allowed: tuple[str, ...]) -> str:
    # Exploded + trailing comma so the line formats identically before and
    # after the export scrub lengthens the domain.
    return await _oidc(allowed).exchange(
        "code",
        "verifier",
        "https://cb.example.com/auth/oidc/callback",
    )


@pytest.mark.parametrize(
    "allowed",
    [
        pytest.param(("alice@example.com",), id="email-only (the 403 regression)"),
        pytest.param(("alice",), id="preferred_username-only"),
        pytest.param(("alice", "alice@example.com"), id="both-forms"),
        pytest.param(("uuid-1234",), id="sub"),
        pytest.param((), id="empty-allow-list-permits-everyone"),
        pytest.param(("someone@example.com", "alice"), id="mixed-list-hit"),
    ],
)
async def test_allowed_forms(fake_issuer, allowed: tuple[str, ...]) -> None:
    assert await _exchange(allowed) == "alice"


@pytest.mark.parametrize(
    "allowed",
    [
        pytest.param(("someone-else@example.com",), id="different-user"),
        pytest.param(("tth2",), id="similar-login-no-substring-match"),
        # NB: deliberately NOT @example.com — the public-export scrub rewrites
        # example.com→example.com, and an @example.com literal here would collapse
        # into the allowed e-mail and flip this rejection case into a match.
        pytest.param(("alice@elsewhere.example",), id="same-login-other-domain"),
    ],
)
async def test_rejected(fake_issuer, allowed: tuple[str, ...]) -> None:
    with pytest.raises(HTTPException) as ei:
        await _exchange(allowed)
    assert ei.value.status_code == 403
