from contextlib import asynccontextmanager
from pathlib import Path

from fastapi import FastAPI
from fastapi.responses import FileResponse

from .emulator import HardwareEmulator
from .models import (
    AttendancePublishRequest,
    ConfigPublishRequest,
    DeviceUpsertRequest,
    OtaPublishRequest,
    RawPublishRequest,
    SnapshotPublishRequest,
)

STATIC_DIR = Path(__file__).with_name("static")

emulator = HardwareEmulator()


@asynccontextmanager
async def lifespan(_: FastAPI):
    await emulator.start()
    try:
        yield
    finally:
        await emulator.stop()


app = FastAPI(
    title="ISIC Hardware Emulator",
    version="0.1.0",
    lifespan=lifespan,
)


@app.get("/")
async def index() -> FileResponse:
    return FileResponse(STATIC_DIR / "index.html")


@app.get("/api/state")
async def state() -> dict[str, object]:
    return await emulator.state()


@app.post("/api/devices")
async def upsert_device(request: DeviceUpsertRequest) -> dict[str, object]:
    return await emulator.upsert_device(request)


@app.post("/api/events/clear")
async def clear_events() -> dict[str, str]:
    await emulator.clear_events()
    return {"detail": "Traffic history cleared"}


@app.post("/api/publish/attendance")
async def publish_attendance(
    request: AttendancePublishRequest,
) -> dict[str, object]:
    return await emulator.publish_attendance(request)


@app.post("/api/publish/status")
async def publish_status(
    request: SnapshotPublishRequest,
) -> dict[str, object]:
    return await emulator.publish_status(request)


@app.post("/api/publish/health")
async def publish_health(
    request: SnapshotPublishRequest,
) -> dict[str, object]:
    return await emulator.publish_health(request)


@app.post("/api/publish/metrics")
async def publish_metrics(
    request: SnapshotPublishRequest,
) -> dict[str, object]:
    return await emulator.publish_metrics(request)


@app.post("/api/publish/config")
async def publish_config(
    request: ConfigPublishRequest,
) -> dict[str, object]:
    return await emulator.publish_config(request)


@app.post("/api/publish/raw")
async def publish_raw(
    request: RawPublishRequest,
) -> dict[str, object]:
    return await emulator.publish_raw(request)


@app.post("/api/publish/ota")
async def publish_ota(
    request: OtaPublishRequest,
) -> dict[str, object]:
    return await emulator.simulate_ota(request)
