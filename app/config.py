"""
Centralized application configuration.

All environment-dependent values are loaded here ONCE via pydantic-settings.
Never read os.environ directly elsewhere in the codebase — import `settings`
from this module instead. This keeps configuration testable and typed.
"""
from functools import lru_cache
from pathlib import Path

from pydantic_settings import BaseSettings, SettingsConfigDict


class Settings(BaseSettings):
    # --- Application ---
    app_name: str = "Smart IoT Parcel Box"
    environment: str = "development"
    debug: bool = True
    api_v1_prefix: str = "/api"

    # --- Security ---
    hardware_api_key: str
    cors_origins: str = "http://localhost:3000"

    # Shared secret for the operator dashboard's WebSocket (`?token=` query
    # param on /ws/dashboard). Separate from hardware_api_key since that one
    # is meant for ESP32 devices, not browsers. Leave blank only for local
    # dev — the dashboard feed is unauthenticated if this is unset.
    dashboard_token: str = ""

    # Requests allowed per client IP per minute against /api/* (see
    # app/rate_limit.py). Protects the DB/MQTT bridge from a malfunctioning
    # or compromised device flooding the server.
    rate_limit_per_minute: int = 120

    # --- Database ---
    # MySQL (async, via the pure-Python aiomysql driver — no C/Rust compiler
    # needed, so it installs cleanly on any Python version/OS). Matches the
    # project's design doc, which specifies MySQL as the DBMS. utf8mb4 is
    # required (not plain utf8) to correctly store Thai text and emoji.
    database_url: str = "mysql+aiomysql://parcelbox:parcelbox@localhost:3306/parcel_box?charset=utf8mb4"

    # --- MQTT ---
    mqtt_host: str = "localhost"
    mqtt_port: int = 1883
    mqtt_username: str | None = None
    mqtt_password: str | None = None
    mqtt_client_id: str = "parcel-box-server"
    mqtt_keepalive: int = 60
    mqtt_topic_status: str = "parcelbox/+/status"
    mqtt_topic_event: str = "parcelbox/+/event"
    mqtt_topic_command: str = "parcelbox/{device_id}/command"

    # --- LINE Messaging API ---
    # LINE Notify (the old approach) was permanently shut down by LINE on
    # March 31, 2025. This project uses a LINE Official Account's Messaging
    # API "push message" endpoint instead — see app/services/line_messaging.py.
    line_channel_access_token: str = ""
    line_user_id: str = ""  # the recipient's LINE user ID (not a phone number)
    line_messaging_api_url: str = "https://api.line.me/v2/bot/message/push"

    # Public HTTPS base URL for this server (e.g. an ngrok/Cloudflare Tunnel
    # URL during dev, or your real domain in production). Required only for
    # LINE image messages, since LINE's servers fetch images from a public
    # URL rather than accepting an upload. Leave blank to send text-only
    # notifications.
    public_base_url: str = ""

    # --- File storage ---
    upload_dir: str = "./uploads"
    max_upload_size_mb: int = 8

    model_config = SettingsConfigDict(env_file=".env", env_file_encoding="utf-8", extra="ignore")

    @property
    def cors_origin_list(self) -> list[str]:
        return [o.strip() for o in self.cors_origins.split(",") if o.strip()]

    @property
    def upload_path(self) -> Path:
        p = Path(self.upload_dir)
        p.mkdir(parents=True, exist_ok=True)
        return p

    @property
    def max_upload_size_bytes(self) -> int:
        return self.max_upload_size_mb * 1024 * 1024


@lru_cache
def get_settings() -> Settings:
    """Cached settings singleton — safe to call repeatedly/import anywhere."""
    return Settings()


settings = get_settings()
