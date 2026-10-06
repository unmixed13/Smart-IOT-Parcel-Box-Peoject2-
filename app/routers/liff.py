"""
Endpoints for the LINE LIFF web app (see /liff): users create delivery QR codes.
QR = random UUID, valid 24 h, error-correction level M, stored as a Pending event.
"""
import io

import segno
from fastapi import APIRouter, Depends, HTTPException
from sqlalchemy import select
from sqlalchemy.ext.asyncio import AsyncSession

from app.config import settings
from app.database import get_db
from app.liff_auth import current_line_user
from app.models import BoxBinding, QREvent, QRStatus
from app.schemas import QRCreated, QRCreateRequest, QRRead
from app.services import qr_service
from app.timeutil import utcnow

router = APIRouter(prefix="/liff", tags=["liff"])


def _svg(code: str) -> str:
    buf = io.BytesIO()
    segno.make(code, error="m").save(buf, kind="svg", scale=6, border=2, xmldecl=False, nl=False)
    return buf.getvalue().decode()


@router.get("/config")
async def liff_config():
    return {"liff_id": settings.liff_id}


@router.get("/me")
async def me(user: str = Depends(current_line_user), db: AsyncSession = Depends(get_db)):
    boxes = [b for (b,) in (await db.execute(select(BoxBinding.box_id).where(BoxBinding.line_user_id == user))).all()]
    return {"boxes": boxes}  # empty -> the page shows "not linked to a box yet"


@router.post("/qr", response_model=QRCreated, status_code=201)
async def create_qr(body: QRCreateRequest, user: str = Depends(current_line_user), db: AsyncSession = Depends(get_db)):
    try:
        ev = await qr_service.create_qr_event(db, user, body.box_id)
    except LookupError:
        raise HTTPException(403, "not_bound: this LINE account is not linked to that box")
    except ValueError:
        raise HTTPException(422, "box_id_required: this account is linked to several boxes")
    return QRCreated(event_id=ev.id, code=ev.code, box_id=ev.box_id, status=ev.status,
                     expires_at=ev.expires_at, qr_svg=_svg(ev.code))


@router.get("/qr", response_model=list[QRRead])
async def my_qr_codes(user: str = Depends(current_line_user), db: AsyncSession = Depends(get_db)):
    stmt = (select(QREvent).where(QREvent.line_user_id == user, QREvent.status.in_([QRStatus.PENDING, QRStatus.UNLOCKING]),
                                  QREvent.expires_at > utcnow()).order_by(QREvent.created_at.desc()).limit(20))
    return (await db.execute(stmt)).scalars().all()
