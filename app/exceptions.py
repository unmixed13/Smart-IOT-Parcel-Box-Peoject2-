"""
Centralized error handling.

Custom exception types express domain errors clearly at the call site
(`raise QRCodeNotFoundError(...)`), and the registered handlers below
translate them into consistent JSON error responses across the whole API,
so every client (dashboard, ESP32 firmware) can rely on one error shape:

    { "error": "<machine-readable-code>", "detail": "<human message>" }
"""
import logging

from fastapi import FastAPI, Request, status
from fastapi.exceptions import RequestValidationError
from fastapi.responses import JSONResponse
from starlette.exceptions import HTTPException as StarletteHTTPException

logger = logging.getLogger("parcel_box")


class ParcelBoxError(Exception):
    """Base class for all domain-specific errors."""

    status_code: int = status.HTTP_400_BAD_REQUEST
    error_code: str = "bad_request"

    def __init__(self, detail: str):
        self.detail = detail
        super().__init__(detail)


class QRCodeNotFoundError(ParcelBoxError):
    status_code = status.HTTP_404_NOT_FOUND
    error_code = "qr_code_not_found"


class QRCodeExpiredError(ParcelBoxError):
    status_code = status.HTTP_403_FORBIDDEN
    error_code = "qr_code_expired"


class DeviceOfflineError(ParcelBoxError):
    status_code = status.HTTP_503_SERVICE_UNAVAILABLE
    error_code = "device_offline"


class FileTooLargeError(ParcelBoxError):
    # 413 Payload Too Large. Name kept version-agnostic since Starlette
    # renamed HTTP_413_REQUEST_ENTITY_TOO_LARGE -> HTTP_413_CONTENT_TOO_LARGE
    # in newer releases; the raw status code is stable across versions.
    status_code = 413
    error_code = "file_too_large"


class UnsupportedFileTypeError(ParcelBoxError):
    status_code = status.HTTP_415_UNSUPPORTED_MEDIA_TYPE
    error_code = "unsupported_file_type"


class MQTTPublishError(ParcelBoxError):
    status_code = status.HTTP_502_BAD_GATEWAY
    error_code = "mqtt_publish_failed"


def register_exception_handlers(app: FastAPI) -> None:
    """Attach all centralized handlers to the FastAPI app instance."""

    @app.exception_handler(ParcelBoxError)
    async def parcel_box_error_handler(request: Request, exc: ParcelBoxError):
        logger.warning("Domain error on %s: %s", request.url.path, exc.detail)
        return JSONResponse(
            status_code=exc.status_code,
            content={"error": exc.error_code, "detail": exc.detail},
        )

    @app.exception_handler(RequestValidationError)
    async def validation_error_handler(request: Request, exc: RequestValidationError):
        return JSONResponse(
            status_code=status.HTTP_422_UNPROCESSABLE_ENTITY,
            content={
                "error": "validation_error",
                "detail": "Request validation failed",
                "errors": exc.errors(),
            },
        )

    @app.exception_handler(StarletteHTTPException)
    async def http_exception_handler(request: Request, exc: StarletteHTTPException):
        return JSONResponse(
            status_code=exc.status_code,
            content={"error": "http_error", "detail": exc.detail},
        )

    @app.exception_handler(Exception)
    async def unhandled_exception_handler(request: Request, exc: Exception):
        # Catch-all: never leak stack traces to hardware/dashboard clients.
        logger.exception("Unhandled exception on %s", request.url.path)
        return JSONResponse(
            status_code=status.HTTP_500_INTERNAL_SERVER_ERROR,
            content={"error": "internal_server_error", "detail": "An unexpected error occurred"},
        )
