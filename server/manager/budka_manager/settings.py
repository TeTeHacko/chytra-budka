"""Runtime configuration — env-first (12-factor), secrets via *_FILE mounts.

Every knob is a CB_* environment variable so the same image runs under
docker compose, Portainer and k8s without a config file. Secret material is
never passed as an env value: pass a path in CB_*_FILE and the value is read
from the mounted file.
"""

from __future__ import annotations

from functools import lru_cache
from pathlib import Path

from pydantic_settings import BaseSettings, SettingsConfigDict


class Settings(BaseSettings):
    model_config = SettingsConfigDict(env_prefix="CB_", extra="ignore")

    db_url: str = "sqlite+aiosqlite:////data/budka.db"

    # --- MQTT (service account on the internal listener) ---
    mqtt_host: str = "mosquitto"
    mqtt_port: int = 1883
    mqtt_username: str = "svc-manager"
    mqtt_password_file: Path = Path("/secrets/svc_manager_pass")

    # --- photo archive ---
    archive_root: Path = Path("/photos")
    retention_days: int = 90

    # --- config write echo-wait ---
    echo_timeout_s: float = 6.0

    # --- enrollment CA ---
    enroll_mode: str = "https"  # off | https
    ca_cert_file: Path = Path("/secrets/sub_ca_budka.pem")
    ca_key_file: Path = Path("/secrets/sub_ca_budka.key")
    enroll_validity_days: int = 90
    enroll_suffixes: str = ".lan,.lan,.local"
    enroll_retry_after_s: int = 60  # Retry-After for 202 pending responses
    enroll_max_per_hour: int = 3  # per-CN issuance rate limit
    # Device IDs allowed to re-key without a fresh operator approval. Meant for
    # bench boards: the HIL suite factory-resets them on every run, which wipes
    # the NVS key, and they are physically in reach. Empty = every re-key waits
    # for an operator (the safe default for anything deployed).
    enroll_trusted_devices: str = ""

    # --- OTA artifact store ---
    ota_dir: Path = Path("/ota")
    ota_token_file: Path = Path("/secrets/ota_token")

    # --- device push ingest (phase 6) ---
    ingest_token_file: Path = Path("/secrets/ingest_token")

    # --- operator auth ---
    auth_mode: str = "password"  # password | oidc | off
    operator_password_file: Path = Path("/secrets/operator_password")
    session_secret_file: Path = Path("/secrets/session_secret")
    session_ttl_s: int = 7 * 86400
    public_base_url: str = ""  # e.g. https://budka.example.com (OIDC redirect)
    oidc_issuer: str = ""  # e.g. https://kc.example.com/realms/main
    oidc_client_id: str = "budka"
    oidc_client_secret_file: Path = Path("/secrets/oidc_client_secret")
    oidc_scopes: str = "openid profile email"
    oidc_allowed_users: str = ""  # comma-separated allow-list; empty = all

    @property
    def oidc_allowed_users_list(self) -> tuple[str, ...]:
        return tuple(u.strip() for u in self.oidc_allowed_users.split(",") if u.strip())

    @property
    def enroll_suffix_tuple(self) -> tuple[str, ...]:
        return tuple(s.strip() for s in self.enroll_suffixes.split(",") if s.strip())

    @property
    def enroll_trusted_tuple(self) -> tuple[str, ...]:
        return tuple(d.strip() for d in self.enroll_trusted_devices.split(",") if d.strip())

    def read_token(self, path: Path) -> str | None:
        try:
            return path.read_text().strip() or None
        except OSError:
            return None


@lru_cache
def get_settings() -> Settings:
    return Settings()
