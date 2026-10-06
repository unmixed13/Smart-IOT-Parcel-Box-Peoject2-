"""Admin API (X-API-Key = HARDWARE_API_KEY): register devices and link LINE users to boxes."""
import secrets

from fastapi import APIRouter, Depends, HTTPException
from sqlalchemy import select
from sqlalchemy.exc import IntegrityError
from sqlalchemy.ext.asyncio import AsyncSession

from app.database import get_db
from app.models import BoxBinding, Device
from app.schemas import BindingCreate, BindingRead, DeviceCreate, DeviceCreated, DeviceRead
from app.security import hash_api_key, verify_hardware_api_key

router = APIRouter(prefix="/admin", tags=["admin"], dependencies=[Depends(verify_hardware_api_key)])


@router.post("/devices", response_model=DeviceCreated, status_code=201)
async def create_device(body: DeviceCreate, db: AsyncSession = Depends(get_db)):
    """Registers a device and returns its API key ONCE (only a hash is stored)."""
    key = secrets.token_urlsafe(32)
    device = Device(device_id=body.device_id, box_id=body.box_id, name=body.name, api_key_hash=hash_api_key(key))
    db.add(device)
    try:
        await db.commit()
    except IntegrityError:
        await db.rollback()
        raise HTTPException(409, "device_id already exists")
    await db.refresh(device)
    return DeviceCreated(**DeviceRead.model_validate(device).model_dump(), api_key=key)


@router.get("/devices", response_model=list[DeviceRead])
async def list_devices(db: AsyncSession = Depends(get_db)):
    return (await db.execute(select(Device).order_by(Device.device_id))).scalars().all()


@router.post("/devices/{device_id}/rotate-key", response_model=DeviceCreated)
async def rotate_key(device_id: str, db: AsyncSession = Depends(get_db)):
    device = await db.get(Device, device_id)
    if device is None:
        raise HTTPException(404, "unknown device")
    key = secrets.token_urlsafe(32)
    device.api_key_hash = hash_api_key(key)
    await db.commit()
    return DeviceCreated(**DeviceRead.model_validate(device).model_dump(), api_key=key)


@router.post("/devices/{device_id}/deactivate", response_model=DeviceRead)
async def deactivate_device(device_id: str, db: AsyncSession = Depends(get_db)):
    device = await db.get(Device, device_id)
    if device is None:
        raise HTTPException(404, "unknown device")
    device.is_active = False
    await db.commit()
    return device


@router.post("/bindings", response_model=BindingRead, status_code=201)
async def create_binding(body: BindingCreate, db: AsyncSession = Depends(get_db)):
    b = BoxBinding(line_user_id=body.line_user_id, box_id=body.box_id)
    db.add(b)
    try:
        await db.commit()
    except IntegrityError:
        await db.rollback()
        raise HTTPException(409, "binding already exists")
    await db.refresh(b)
    return b


@router.get("/bindings", response_model=list[BindingRead])
async def list_bindings(db: AsyncSession = Depends(get_db)):
    return (await db.execute(select(BoxBinding))).scalars().all()


@router.delete("/bindings/{binding_id}", status_code=204)
async def delete_binding(binding_id: str, db: AsyncSession = Depends(get_db)):
    import uuid
    try:
        b = await db.get(BoxBinding, uuid.UUID(binding_id))
    except ValueError:
        b = None
    if b is None:
        raise HTTPException(404, "unknown binding")
    await db.delete(b)
    await db.commit()
