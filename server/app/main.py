import asyncio
import json
import os
import pathlib
import subprocess
import uuid

import httpx
import edge_tts
from fastapi import FastAPI, File, Header, HTTPException, UploadFile, WebSocket, WebSocketDisconnect
from fastapi.responses import FileResponse
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel, Field

from . import database as db


BASE_DIR = pathlib.Path(__file__).resolve().parent
MEDIA_DIR = pathlib.Path(os.getenv("RIG_MEDIA_DIR", "server/data/media"))
PUBLIC_URL = os.getenv("RIG_PUBLIC_URL", "http://localhost:8000").rstrip("/")
MEDIA_DIR.mkdir(parents=True, exist_ok=True)

app = FastAPI(title="RIG Control", version="1.0.0")
app.mount("/static", StaticFiles(directory=BASE_DIR / "static"), name="static")
app.mount("/media", StaticFiles(directory=MEDIA_DIR), name="media")


class DeviceCreate(BaseModel):
    id: str
    name: str = "RIG Puppy"
    token: str | None = None


class CommandRequest(BaseModel):
    name: str
    arguments: dict = Field(default_factory=dict)


class WorkflowRequest(BaseModel):
    id: str | None = None
    device_id: str
    name: str
    steps: list[dict]


class ReminderRequest(BaseModel):
    id: str | None = None
    device_id: str
    name: str
    workflow_id: str
    enabled: bool = True
    days: list[int] = Field(default_factory=lambda: [1, 2, 3, 4, 5])
    start_minute: int = 540
    end_minute: int = 1080
    interval_minutes: int = 45
    timezone_offset_minutes: int = 480


class TtsRequest(BaseModel):
    text: str
    name: str = "Generated speech"
    voice: str | None = None


class WakeWordRequest(BaseModel):
    wake_word: str
    wake_display: str
    wake_threshold: int = 20


class WifiUpdateRequest(BaseModel):
    ssid: str
    password: str = ""


async def create_tts_media(request: TtsRequest) -> dict:
    media_id = str(uuid.uuid4())
    source = MEDIA_DIR / f"{media_id}.source"
    destination = MEDIA_DIR / f"{media_id}.ogg"
    api_key = os.getenv("RIG_TTS_API_KEY", "")
    if api_key:
        base_url = os.getenv("RIG_TTS_BASE_URL", "https://api.openai.com/v1").rstrip("/")
        async with httpx.AsyncClient(timeout=90) as client:
            response = await client.post(
                f"{base_url}/audio/speech",
                headers={"Authorization": f"Bearer {api_key}"},
                json={"model": os.getenv("RIG_TTS_MODEL", "gpt-4o-mini-tts"), "voice": request.voice or os.getenv("RIG_TTS_VOICE", "alloy"), "input": request.text},
            )
            if response.status_code >= 400:
                raise HTTPException(502, response.text)
            source.write_bytes(response.content)
    else:
        voice = request.voice or os.getenv("RIG_EDGE_TTS_VOICE", "zh-CN-XiaoxiaoNeural")
        proxy = os.getenv("RIG_TTS_PROXY") or None
        await edge_tts.Communicate(request.text, voice, proxy=proxy).save(str(source))
    try:
        transcode_to_ogg(source, destination)
    finally:
        source.unlink(missing_ok=True)
    db.execute("INSERT INTO media VALUES(?,?,?,?,?)", (media_id, request.name, destination.name, "tts", db.now()))
    return {"id": media_id, "url": f"{PUBLIC_URL}/media/{destination.name}", "filename": destination.name}


class DeviceHub:
    def __init__(self):
        self.connections: dict[str, WebSocket] = {}
        self.locks: dict[str, asyncio.Lock] = {}
        self.browsers: set[WebSocket] = set()

    async def publish(self, event: dict):
        stale = []
        for browser in self.browsers:
            try:
                await browser.send_json(event)
            except Exception:
                stale.append(browser)
        for browser in stale:
            self.browsers.discard(browser)

    async def connect(self, device_id: str, websocket: WebSocket):
        await websocket.accept()
        self.connections[device_id] = websocket
        self.locks.setdefault(device_id, asyncio.Lock())
        db.execute("UPDATE devices SET online=1,last_seen=? WHERE id=?", (db.now(), device_id))
        await self.publish({"type": "device.online", "device_id": device_id, "timestamp": db.now()})

    async def disconnect(self, device_id: str, websocket: WebSocket):
        if self.connections.get(device_id) is not websocket:
            return
        self.connections.pop(device_id, None)
        db.execute("UPDATE devices SET online=0,last_seen=? WHERE id=?", (db.now(), device_id))
        await self.publish({"type": "device.offline", "device_id": device_id, "timestamp": db.now()})

    async def send(self, device_id: str, message: dict):
        websocket = self.connections.get(device_id)
        if not websocket:
            raise HTTPException(409, "Device is offline")
        async with self.locks[device_id]:
            await websocket.send_json(message)


hub = DeviceHub()


def envelope(message_type: str, device_id: str, payload: dict, message_id: str | None = None) -> dict:
    return {
        "type": message_type,
        "id": message_id or str(uuid.uuid4()),
        "device_id": device_id,
        "timestamp": db.now(),
        "payload": payload,
    }


def workflows_for(device_id: str) -> list[dict]:
    values = db.rows("SELECT * FROM workflows WHERE device_id=? ORDER BY updated_at", (device_id,))
    return [{"id": item["id"], "name": item["name"], "steps": json.loads(item["steps_json"])} for item in values]


def reminders_for(device_id: str) -> list[dict]:
    values = db.rows("SELECT * FROM reminders WHERE device_id=? ORDER BY updated_at", (device_id,))
    return [{
        "id": item["id"], "name": item["name"], "workflow_id": item["workflow_id"],
        "enabled": bool(item["enabled"]), "days": json.loads(item["days_json"]),
        "start_minute": item["start_minute"], "end_minute": item["end_minute"],
        "interval_minutes": item["interval_minutes"],
        "timezone_offset_minutes": item["timezone_offset_minutes"],
    } for item in values]


async def sync_device(device_id: str):
    await hub.send(device_id, envelope("workflow.sync", device_id, {"workflows": workflows_for(device_id)}))
    await hub.send(device_id, envelope("reminder.sync", device_id, {"reminders": reminders_for(device_id)}))
    await hub.send(device_id, envelope("heartbeat", device_id, {"server_time": db.now()}))


@app.on_event("startup")
def startup():
    db.init_db()


@app.get("/")
def index():
    return FileResponse(BASE_DIR / "static" / "index.html")


@app.get("/devices")
@app.get("/control")
@app.get("/workflows")
@app.get("/reminders")
@app.get("/media-library")
@app.get("/network")
def app_page():
    return FileResponse(BASE_DIR / "static" / "index.html")


@app.websocket("/ws/device")
async def device_websocket(websocket: WebSocket):
    device_id = websocket.headers.get("x-device-id", "").strip().upper()
    authorization = websocket.headers.get("authorization", "")
    token = authorization.removeprefix("Bearer ").strip()
    if not device_id or not db.authenticate_device(device_id, token):
        await websocket.close(code=4401)
        return
    await hub.connect(device_id, websocket)
    try:
        await sync_device(device_id)
        while True:
            message = await websocket.receive_json()
            message_type = message.get("type")
            payload = message.get("payload") or {}
            db.execute("UPDATE devices SET online=1,last_seen=? WHERE id=?", (db.now(), device_id))
            if message_type == "hello":
                db.execute("UPDATE devices SET firmware_version=? WHERE id=?", (payload.get("firmware_version"), device_id))
            elif message_type == "status":
                db.execute("UPDATE devices SET status_json=? WHERE id=?", (json.dumps(payload, ensure_ascii=False), device_id))
                await hub.publish({"type": "device.status", "device_id": device_id, "timestamp": db.now(), "payload": payload})
            elif message_type in {"command.result", "workflow.result"}:
                db.execute(
                    "UPDATE commands SET status=?,result_json=?,completed_at=? WHERE id=?",
                    ("completed" if payload.get("success") else "failed", json.dumps(payload, ensure_ascii=False), db.now(), message.get("id")),
                )
                await hub.publish({"type": "command.result", "device_id": device_id, "id": message.get("id"), "timestamp": db.now(), "payload": payload})
            elif message_type in {"wifi.list.result", "wifi.update.result", "wifi.forget.result"}:
                await hub.publish({"type": message_type, "device_id": device_id, "id": message.get("id"), "timestamp": db.now(), "payload": payload})
    except WebSocketDisconnect:
        pass
    finally:
        await hub.disconnect(device_id, websocket)


@app.websocket("/ws/control")
async def browser_websocket(websocket: WebSocket):
    await websocket.accept()
    hub.browsers.add(websocket)
    await websocket.send_json({"type": "control.ready", "timestamp": db.now()})
    try:
        while True:
            await websocket.receive_text()
    except WebSocketDisconnect:
        pass
    finally:
        hub.browsers.discard(websocket)


@app.get("/api/devices")
def list_devices():
    return [db.device_payload(item) for item in db.rows("SELECT * FROM devices ORDER BY name")]


@app.post("/api/devices")
def add_device(request: DeviceCreate):
    token = db.create_device(request.id, request.name, request.token)
    return {"id": request.id, "name": request.name, "token": token}


@app.post("/api/devices/{device_id}/commands")
async def send_command(device_id: str, request: CommandRequest):
    command_id = str(uuid.uuid4())
    message = envelope("command.execute", device_id, request.model_dump(), command_id)
    db.execute(
        "INSERT INTO commands(id,device_id,kind,request_json,status,created_at) VALUES(?,?,?,?,?,?)",
        (command_id, device_id, "command", json.dumps(message), "sent", db.now()),
    )
    await hub.send(device_id, message)
    return {"id": command_id, "status": "sent"}


@app.get("/api/devices/{device_id}/commands")
def command_history(device_id: str):
    return db.rows("SELECT * FROM commands WHERE device_id=? ORDER BY created_at DESC LIMIT 50", (device_id,))


@app.get("/api/workflows")
def list_workflows(device_id: str):
    return workflows_for(device_id)


@app.post("/api/workflows")
async def save_workflow(request: WorkflowRequest):
    workflow_id = request.id or str(uuid.uuid4())
    db.execute(
        "INSERT INTO workflows(id,device_id,name,steps_json,updated_at) VALUES(?,?,?,?,?) "
        "ON CONFLICT(id) DO UPDATE SET name=excluded.name,steps_json=excluded.steps_json,updated_at=excluded.updated_at",
        (workflow_id, request.device_id, request.name, json.dumps(request.steps, ensure_ascii=False), db.now()),
    )
    if request.device_id in hub.connections:
        await sync_device(request.device_id)
    return {"id": workflow_id}


@app.delete("/api/workflows/{workflow_id}")
async def delete_workflow(workflow_id: str):
    item = db.row("SELECT device_id FROM workflows WHERE id=?", (workflow_id,))
    if not item:
        raise HTTPException(404, "Workflow not found")
    db.execute("DELETE FROM workflows WHERE id=?", (workflow_id,))
    if item["device_id"] in hub.connections:
        await sync_device(item["device_id"])
    return {"success": True}


@app.post("/api/workflows/{workflow_id}/execute")
async def execute_workflow(workflow_id: str):
    item = db.row("SELECT * FROM workflows WHERE id=?", (workflow_id,))
    if not item:
        raise HTTPException(404, "Workflow not found")
    command_id = str(uuid.uuid4())
    message = envelope("workflow.execute", item["device_id"], {"workflow_id": workflow_id}, command_id)
    db.execute(
        "INSERT INTO commands(id,device_id,kind,request_json,status,created_at) VALUES(?,?,?,?,?,?)",
        (command_id, item["device_id"], "workflow", json.dumps(message), "sent", db.now()),
    )
    await hub.send(item["device_id"], message)
    return {"id": command_id, "status": "sent"}


@app.get("/api/reminders")
def list_reminders(device_id: str):
    return reminders_for(device_id)


@app.post("/api/reminders")
async def save_reminder(request: ReminderRequest):
    reminder_id = request.id or str(uuid.uuid4())
    db.execute(
        "INSERT INTO reminders VALUES(?,?,?,?,?,?,?,?,?,?,?) "
        "ON CONFLICT(id) DO UPDATE SET name=excluded.name,workflow_id=excluded.workflow_id,enabled=excluded.enabled,days_json=excluded.days_json,start_minute=excluded.start_minute,end_minute=excluded.end_minute,interval_minutes=excluded.interval_minutes,timezone_offset_minutes=excluded.timezone_offset_minutes,updated_at=excluded.updated_at",
        (reminder_id, request.device_id, request.name, request.workflow_id, int(request.enabled), json.dumps(request.days), request.start_minute, request.end_minute, request.interval_minutes, request.timezone_offset_minutes, db.now()),
    )
    if request.device_id in hub.connections:
        await sync_device(request.device_id)
    return {"id": reminder_id}


@app.delete("/api/reminders/{reminder_id}")
async def delete_reminder(reminder_id: str):
    item = db.row("SELECT device_id FROM reminders WHERE id=?", (reminder_id,))
    if not item:
        raise HTTPException(404, "Reminder not found")
    db.execute("DELETE FROM reminders WHERE id=?", (reminder_id,))
    if item["device_id"] in hub.connections:
        await sync_device(item["device_id"])
    return {"success": True}


def transcode_to_ogg(source: pathlib.Path, destination: pathlib.Path):
    subprocess.run(
        ["ffmpeg", "-y", "-i", str(source), "-ac", "1", "-ar", "16000", "-c:a", "libopus", "-frame_duration", "60", str(destination)],
        check=True, capture_output=True,
    )


@app.get("/api/media")
def list_media():
    return [{**item, "url": f"{PUBLIC_URL}/media/{item['filename']}"} for item in db.rows("SELECT * FROM media ORDER BY created_at DESC")]


@app.post("/api/media/upload")
async def upload_media(file: UploadFile = File(...)):
    media_id = str(uuid.uuid4())
    source = MEDIA_DIR / f"{media_id}.source"
    destination = MEDIA_DIR / f"{media_id}.ogg"
    source.write_bytes(await file.read())
    try:
        transcode_to_ogg(source, destination)
    finally:
        source.unlink(missing_ok=True)
    db.execute("INSERT INTO media VALUES(?,?,?,?,?)", (media_id, file.filename or "Uploaded audio", destination.name, "upload", db.now()))
    return {"id": media_id, "url": f"{PUBLIC_URL}/media/{destination.name}"}


@app.post("/api/media/tts")
async def generate_tts(request: TtsRequest):
    return await create_tts_media(request)


@app.post("/api/devices/{device_id}/speak")
async def speak_text(device_id: str, request: TtsRequest):
    item = await create_tts_media(request)
    return await play_media(device_id, item["id"])


@app.post("/api/devices/{device_id}/camera/snapshot")
async def request_snapshot(device_id: str):
    snapshot_id = str(uuid.uuid4())
    command = CommandRequest(name="self.camera.upload_snapshot", arguments={"url": f"{PUBLIC_URL}/api/camera/uploads/{snapshot_id}"})
    result = await send_command(device_id, command)
    return {**result, "snapshot_id": snapshot_id}


@app.post("/api/devices/{device_id}/wake-word")
async def update_wake_word(device_id: str, request: WakeWordRequest):
    message_id = str(uuid.uuid4())
    await hub.send(device_id, envelope("config.update", device_id, request.model_dump(), message_id))
    return {"id": message_id, "status": "sent", "rebooting": True}


@app.post("/api/devices/{device_id}/wifi/scan")
async def scan_wifi(device_id: str):
    message_id = str(uuid.uuid4())
    await hub.send(device_id, envelope("wifi.list", device_id, {}, message_id))
    return {"id": message_id, "status": "scanning"}


@app.post("/api/devices/{device_id}/wifi")
async def update_wifi(device_id: str, request: WifiUpdateRequest):
    message_id = str(uuid.uuid4())
    await hub.send(device_id, envelope("wifi.update", device_id, request.model_dump(), message_id))
    return {"id": message_id, "status": "sent", "rebooting": True}


@app.delete("/api/devices/{device_id}/wifi/{ssid}")
async def forget_wifi(device_id: str, ssid: str):
    message_id = str(uuid.uuid4())
    await hub.send(device_id, envelope("wifi.forget", device_id, {"ssid": ssid}, message_id))
    return {"id": message_id, "status": "sent"}


@app.post("/api/camera/uploads/{snapshot_id}")
async def receive_snapshot(snapshot_id: str, file: UploadFile = File(...), device_id: str = Header(alias="Device-Id")):
    normalized = device_id.strip().upper()
    if not db.row("SELECT id FROM devices WHERE id=?", (normalized,)):
        raise HTTPException(404, "Device not found")
    filename = f"camera-{snapshot_id}.jpg"
    (MEDIA_DIR / filename).write_bytes(await file.read())
    db.execute("INSERT OR REPLACE INTO camera_snapshots VALUES(?,?,?,?)", (snapshot_id, normalized, filename, db.now()))
    await hub.publish({"type": "camera.snapshot", "device_id": normalized, "timestamp": db.now(), "payload": {"id": snapshot_id, "url": f"{PUBLIC_URL}/media/{filename}"}})
    return {"id": snapshot_id, "url": f"{PUBLIC_URL}/media/{filename}"}


@app.get("/api/devices/{device_id}/camera/snapshots")
def list_snapshots(device_id: str):
    return [{**row, "url": f"{PUBLIC_URL}/media/{row['filename']}"} for row in db.rows("SELECT * FROM camera_snapshots WHERE device_id=? ORDER BY created_at DESC LIMIT 20", (device_id.upper(),))]


@app.post("/api/devices/{device_id}/media/{media_id}/play")
async def play_media(device_id: str, media_id: str):
    item = db.row("SELECT filename FROM media WHERE id=?", (media_id,))
    if not item:
        raise HTTPException(404, "Media not found")
    workflow_id = str(uuid.uuid4())
    message = envelope("workflow.execute", device_id, {"workflow": {"steps": [{"type": "audio", "url": f"{PUBLIC_URL}/media/{item['filename']}"}]}}, workflow_id)
    db.execute(
        "INSERT INTO commands(id,device_id,kind,request_json,status,created_at) VALUES(?,?,?,?,?,?)",
        (workflow_id, device_id, "media", json.dumps(message), "sent", db.now()),
    )
    await hub.send(device_id, message)
    return {"id": workflow_id, "status": "sent"}
