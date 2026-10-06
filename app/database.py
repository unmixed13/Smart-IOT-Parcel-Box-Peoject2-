"""
Async SQLAlchemy engine + session management.

Uses SQLAlchemy 2.0 async style throughout. The engine is created once at
import time; sessions are created per-request via the `get_db` dependency
so each request gets an isolated, auto-closed transaction scope.
"""
from collections.abc import AsyncGenerator

from sqlalchemy.exc import SQLAlchemyError
from sqlalchemy.ext.asyncio import (
    AsyncSession,
    async_sessionmaker,
    create_async_engine,
)
from sqlalchemy.orm import DeclarativeBase

from app.config import settings

# `future=True` + async driver (aiosqlite / asyncpg) gives full asyncio support.
# `pool_pre_ping` guards against stale connections on long-lived deployments.
engine = create_async_engine(
    settings.database_url,
    echo=settings.debug,
    pool_pre_ping=True,
    future=True,
)

AsyncSessionLocal = async_sessionmaker(
    bind=engine,
    class_=AsyncSession,
    expire_on_commit=False,
    autoflush=False,
)


class Base(DeclarativeBase):
    """Shared declarative base for all ORM models."""
    pass


async def get_db() -> AsyncGenerator[AsyncSession, None]:
    """FastAPI dependency yielding a request-scoped async DB session."""
    async with AsyncSessionLocal() as session:
        try:
            yield session
        except Exception:
            await session.rollback()
            raise
        finally:
            await session.close()


async def init_db(attempts: int = 30, delay: float = 2.0) -> None:
    """
    Create tables on startup if they don't exist.

    NOTE: This is a convenience for dev/small deployments. For production,
    use Alembic migrations (scaffolded via `alembic init migrations`)
    instead of relying on create_all.
    """
    # Retries so the service survives starting before MySQL is up (boot order).
    import asyncio
    import logging

    for attempt in range(1, attempts + 1):
        try:
            async with engine.begin() as conn:
                await conn.run_sync(Base.metadata.create_all)
            return
        except (OSError, SQLAlchemyError) as exc:
            if attempt == attempts:
                raise
            logging.getLogger("parcel_box").warning(
                "Database not ready (%s); retry %d/%d in %.0fs", exc.__class__.__name__, attempt, attempts, delay
            )
            await asyncio.sleep(delay)


async def close_db() -> None:
    """Dispose the connection pool cleanly on shutdown."""
    await engine.dispose()
