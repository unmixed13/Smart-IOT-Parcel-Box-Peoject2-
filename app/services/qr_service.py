"""
QR code life cycle (thesis table 3.1) and the device-event state machine.

    Pending   --valid scan-->            Unlocking
    Pending   --24 h elapsed-->          Expired
    Unlocking --same code scanned-->     Unlocking (same Event-ID, timer restarts)
    Unlocking --door opened-->           Used
    Unlocking --ESP32 reports timeout--> Pending
    Unlocking --no report in 15 s-->     Used      (Fail-Secure)
    Used      --late "timeout" report--> Used      (rejected)

Every function here works on one AsyncSession and returns what the caller
should notify; sending LINE messages is the caller's job (after commit).
"""
import uuid
from dataclasses import dataclass, field
from datetime import timedelta

from sqlalchemy import select
from sqlalchemy.ext.asyncio import AsyncSession

from app.config import settings
from app.models import AccessLog, AccessResult, BoxBinding, Device, EventType, QREvent, QRStatus
from app.schemas import DeviceEventRequest, DeviceEventResponse, QRVerifyResponse
from app.timeutil import as_utc, utcnow


@dataclass
class Notice:
    """A LINE message to send once the DB transaction has committed."""
    to: str | None
    text: str
    image_path: str | None = None


@dataclass
class Outcome:
    notices: list[Notice] = field(default_factory=list)
    broadcast: list[dict] = field(default_factory=list)


def _log(db: AsyncSession, device_id: str, etype: EventType, result: AccessResult, notes: str, code: str | None = None) -> None:
    db.add(AccessLog(device_id=device_id, event_type=etype, result=result, qr_code_scanned=code, notes=notes))


def parse_code(raw: str) -> str | None:
    try:
        return str(uuid.UUID(raw.strip()))
    except (ValueError, AttributeError):
        return None


# --- QR creation (LIFF) ---

async def create_qr_event(db: AsyncSession, line_user_id: str, box_id: str | None) -> QREvent:
    """Create a Pending QR for a user. `box_id` may be omitted if the user owns exactly one box."""
    boxes = [b for (b,) in (await db.execute(select(BoxBinding.box_id).where(BoxBinding.line_user_id == line_user_id))).all()]
    if not boxes:
        raise LookupError("not_bound")
    if box_id is None:
        if len(boxes) > 1:
            raise ValueError("box_id_required")
        box_id = boxes[0]
    elif box_id not in boxes:
        raise LookupError("not_bound")

    now = utcnow()
    event = QREvent(
        code=str(uuid.uuid4()), box_id=box_id, line_user_id=line_user_id, status=QRStatus.PENDING,
        created_at=now, expires_at=now + timedelta(hours=settings.qr_ttl_hours),
    )
    db.add(event)
    _log(db, box_id, EventType.QR_CREATED, AccessResult.GRANTED, f"QR created for {line_user_id[:6]}…", event.code)
    await db.commit()
    await db.refresh(event)
    return event


# --- Scan verification (ESP32 main controller) ---

async def _locked_event(db: AsyncSession, *where) -> QREvent | None:
    return (await db.execute(select(QREvent).where(*where).with_for_update())).scalar_one_or_none()


def _deny(reason: str) -> QRVerifyResponse:
    return QRVerifyResponse(granted=False, reason=reason)


async def verify_scan(db: AsyncSession, device: Device, raw_code: str) -> tuple[QRVerifyResponse, Outcome]:
    out = Outcome()
    code = parse_code(raw_code)
    event = await _locked_event(db, QREvent.code == code) if code else None

    if event is None or event.box_id != device.box:
        _log(db, device.device_id, EventType.QR_SCAN, AccessResult.DENIED, "invalid_code", raw_code[:255])
        await db.commit()
        out.notices.append(Notice(None, f"⚠️ Denied access attempt at {device.box}: invalid_code"))
        out.broadcast.append({"event": "qr_verified", "device_id": device.device_id, "granted": False, "reason": "invalid_code"})
        return _deny("invalid_code"), out

    now = utcnow()
    reason: str | None = None

    if event.status == QRStatus.PENDING and as_utc(event.expires_at) <= now:
        event.status = QRStatus.EXPIRED
        reason = "expired"
    elif event.status == QRStatus.EXPIRED:
        reason = "expired"
    elif event.status == QRStatus.USED:
        reason = "already_used"
    elif event.status == QRStatus.UNLOCKING and as_utc(event.unlocking_since) + timedelta(seconds=settings.unlocking_timeout_seconds) <= now:
        # Nobody reported within the window: Fail-Secure, burn the code.
        event.status = QRStatus.USED
        event.used_at = now
        reason = "already_used"

    if reason:
        _log(db, device.device_id, EventType.QR_SCAN, AccessResult.DENIED, reason, event.code)
        await db.commit()
        out.notices.append(Notice(None, f"⚠️ Denied access attempt at {device.box}: {reason}"))
        out.broadcast.append({"event": "qr_verified", "device_id": device.device_id, "granted": False, "reason": reason})
        return _deny(reason), out

    rescan = event.status == QRStatus.UNLOCKING
    event.status = QRStatus.UNLOCKING
    event.unlocking_since = now
    event.unlock_count += 1
    _log(db, device.device_id, EventType.QR_SCAN, AccessResult.GRANTED,
         "rescan: same Event-ID, timer restarted" if rescan else "valid code: unlocking", event.code)
    await db.commit()
    out.broadcast.append({"event": "qr_verified", "device_id": device.device_id, "granted": True,
                          "reason": "access_granted", "event_id": str(event.id)})
    return QRVerifyResponse(granted=True, reason="access_granted", event_id=event.id,
                            unlock_seconds=settings.unlock_seconds,
                            door_open_alert_seconds=settings.door_ajar_seconds), out


# --- Device events (door / lock / manual) ---

async def apply_device_event(db: AsyncSession, device: Device, req: DeviceEventRequest) -> tuple[DeviceEventResponse, Outcome]:
    out = Outcome()
    now = utcnow()

    if req.type == "manual_unlock":
        _log(db, device.device_id, EventType.MANUAL_UNLOCK, AccessResult.GRANTED, "Box opened with the manual override button")
        await db.commit()
        owners = [u for (u,) in (await db.execute(select(BoxBinding.line_user_id).where(BoxBinding.box_id == device.box))).all()]
        for u in owners or [None]:
            out.notices.append(Notice(u, f"🔑 Box {device.box} was opened with the manual override (no photo)."))
        out.broadcast.append({"event": "manual_unlock", "device_id": device.device_id})
        return DeviceEventResponse(accepted=True, detail="logged"), out

    if req.type == "tamper":
        _log(db, device.device_id, EventType.TAMPER, AccessResult.ERROR, "door opened while locked")
        await db.commit()
        owners = [u for (u,) in (await db.execute(select(BoxBinding.line_user_id).where(BoxBinding.box_id == device.box))).all()]
        for u in owners or [None]:
            out.notices.append(Notice(u, f"🚨 Box {device.box}: the door was opened while locked (possible tampering)."))
        out.broadcast.append({"event": "tamper", "device_id": device.device_id})
        return DeviceEventResponse(accepted=True, detail="logged"), out

    if req.event_id is None:
        return DeviceEventResponse(accepted=False, detail="event_id_required"), out
    event = await _locked_event(db, QREvent.id == req.event_id)
    if event is None or event.box_id != device.box:
        return DeviceEventResponse(accepted=False, detail="unknown_event"), out

    async def done(accepted: bool, detail: str = "") -> tuple[DeviceEventResponse, Outcome]:
        # Rejections roll back (rollback expires ORM attributes), so snapshot the status first.
        resp = DeviceEventResponse(accepted=accepted, status=event.status, detail=detail)
        if not accepted:
            await db.rollback()
        return resp, out

    if req.type == "door_opened":
        if event.status == QRStatus.UNLOCKING:
            event.status, event.used_at, event.door_open_since = QRStatus.USED, now, now
            _log(db, device.device_id, EventType.DOOR_OPEN, AccessResult.GRANTED, f"door opened, event {event.id}", event.code)
            _door_notice(out, event, f"🚪 Box {event.box_id}: the door was opened.")
        elif event.status == QRStatus.USED and event.door_open_since is None and event.lock_reported_at is None:
            event.door_open_since = now  # code was burned by Fail-Secure but the door really opened
            _log(db, device.device_id, EventType.DOOR_OPEN, AccessResult.GRANTED, f"door opened after fail-secure, event {event.id}", event.code)
            _door_notice(out, event, f"🚪 Box {event.box_id}: the door was opened.")
        elif event.status != QRStatus.USED:
            return await done(False, "not_unlocking")
        await db.commit()
        out.broadcast.append({"event": "door_open", "device_id": device.device_id, "event_id": str(event.id)})
        return await done(True)

    if req.type == "unlock_timeout":
        if event.status == QRStatus.UNLOCKING:
            event.status, event.unlocking_since = QRStatus.PENDING, None
            _log(db, device.device_id, EventType.UNLOCK_TIMEOUT, AccessResult.DENIED, f"unlock window elapsed, event {event.id}", event.code)
            await db.commit()
            return await done(True)
        return await done(False, "rejected_late_report")  # Used stays Used

    if req.type == "locked":
        if event.status != QRStatus.USED:
            return await done(False, "not_used")
        if event.lock_reported_at is None:
            event.lock_reported_at = now
            event.image_deadline = now + timedelta(seconds=settings.image_wait_seconds)
            event.door_open_since = None
            _log(db, device.device_id, EventType.LOCKED, AccessResult.GRANTED, f"door closed and locked, event {event.id}", event.code)
            _door_notice(out, event, f"🔒 Box {event.box_id}: the door was closed and locked.")
            await db.commit()
            out.broadcast.append({"event": "locked", "device_id": device.device_id, "event_id": str(event.id)})
        return await done(True)

    # door_ajar
    if event.status == QRStatus.USED and not event.ajar_alerted and event.lock_reported_at is None:
        event.ajar_alerted = True
        _log(db, device.device_id, EventType.DOOR_AJAR, AccessResult.ERROR, f"door left open, event {event.id}", event.code)
        await db.commit()
        out.notices.append(Notice(event.line_user_id, _ajar_text(device.box)))
        return await done(True)
    return await done(False, "already_alerted_or_closed")


def _door_notice(out: Outcome, event: QREvent, text: str) -> None:
    if settings.notify_door_events:
        out.notices.append(Notice(event.line_user_id, text))


def _ajar_text(box_id: str) -> str:
    return f"🚪 Box {box_id}: the door has been open for more than {settings.door_ajar_seconds} seconds. Please check it."


# --- Image matching ---

async def find_awaiting_capture(db: AsyncSession, box_id: str, event_id: uuid.UUID | None = None, lock: bool = False) -> QREvent | None:
    now = utcnow()
    stmt = select(QREvent).where(
        QREvent.box_id == box_id, QREvent.status == QRStatus.USED,
        QREvent.lock_reported_at.is_not(None), QREvent.image_path.is_(None), QREvent.notified.is_(False),
    )
    if event_id is not None:
        stmt = stmt.where(QREvent.id == event_id)
    stmt = stmt.order_by(QREvent.lock_reported_at.desc())
    if lock:
        stmt = stmt.with_for_update()
    for ev in (await db.execute(stmt)).scalars():
        if as_utc(ev.image_deadline) > now:
            return ev
    return None


def delivery_notice(event: QREvent, image_path: str | None) -> Notice:
    text = f"📦 A parcel was delivered to box {event.box_id}."
    if image_path is None:
        text += f" (No photo received within {settings.image_wait_seconds} s.)"
    return Notice(event.line_user_id, text, image_path)


# --- Timers (run by the sweeper) ---

async def sweep(db: AsyncSession) -> Outcome:
    """Apply all time-driven transitions once. Idempotent; safe to call every second."""
    out = Outcome()
    now = utcnow()

    for ev in (await db.execute(select(QREvent).where(QREvent.status == QRStatus.UNLOCKING).with_for_update())).scalars():
        if as_utc(ev.unlocking_since) + timedelta(seconds=settings.unlocking_timeout_seconds) <= now:
            ev.status, ev.used_at = QRStatus.USED, now
            _log(db, ev.box_id, EventType.UNLOCK_TIMEOUT, AccessResult.ERROR, f"no report in {settings.unlocking_timeout_seconds}s: fail-secure, event {ev.id}", ev.code)

    for ev in (await db.execute(select(QREvent).where(QREvent.status == QRStatus.PENDING, QREvent.expires_at <= now).with_for_update())).scalars():
        ev.status = QRStatus.EXPIRED
        _log(db, ev.box_id, EventType.QR_EXPIRED, AccessResult.DENIED, f"QR expired, event {ev.id}", ev.code)

    used = (await db.execute(select(QREvent).where(QREvent.status == QRStatus.USED).with_for_update())).scalars().all()
    for ev in used:
        if ev.lock_reported_at is not None and not ev.notified and ev.image_path is None and as_utc(ev.image_deadline) <= now:
            ev.notified = True
            out.notices.append(delivery_notice(ev, None))
        if ev.door_open_since is not None and ev.lock_reported_at is None and not ev.ajar_alerted \
                and as_utc(ev.door_open_since) + timedelta(seconds=settings.door_ajar_seconds) <= now:
            ev.ajar_alerted = True
            _log(db, ev.box_id, EventType.DOOR_AJAR, AccessResult.ERROR, f"door open > {settings.door_ajar_seconds}s, event {ev.id}", ev.code)
            out.notices.append(Notice(ev.line_user_id, _ajar_text(ev.box_id)))

    await db.commit()
    return out
