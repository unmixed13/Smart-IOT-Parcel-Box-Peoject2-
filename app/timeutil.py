"""UTC helpers. MySQL/SQLite return naive datetimes even for DateTime(timezone=True)
columns, so every value read from the DB must go through `as_utc` before it is
compared with an aware `utcnow()`."""
from datetime import datetime, timezone


def utcnow() -> datetime:
    return datetime.now(timezone.utc)


def as_utc(dt: datetime | None) -> datetime | None:
    if dt is None:
        return None
    return dt.replace(tzinfo=timezone.utc) if dt.tzinfo is None else dt.astimezone(timezone.utc)
