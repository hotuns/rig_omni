import hashlib
import json
import os
import secrets
import sqlite3
import threading
import time
from contextlib import contextmanager


DATABASE_PATH = os.getenv("RIG_DATABASE", "server/data/rig-control.db")
_lock = threading.Lock()


def token_hash(token: str) -> str:
    return hashlib.sha256(token.encode("utf-8")).hexdigest()


@contextmanager
def connection():
    directory = os.path.dirname(DATABASE_PATH)
    if directory:
        os.makedirs(directory, exist_ok=True)
    conn = sqlite3.connect(DATABASE_PATH, check_same_thread=False)
    conn.row_factory = sqlite3.Row
    try:
        yield conn
        conn.commit()
    finally:
        conn.close()


def init_db() -> None:
    with _lock, connection() as conn:
        conn.executescript(
            """
            CREATE TABLE IF NOT EXISTS devices (
                id TEXT PRIMARY KEY,
                name TEXT NOT NULL,
                token_hash TEXT NOT NULL,
                online INTEGER NOT NULL DEFAULT 0,
                last_seen INTEGER,
                firmware_version TEXT,
                status_json TEXT NOT NULL DEFAULT '{}'
            );
            CREATE TABLE IF NOT EXISTS workflows (
                id TEXT PRIMARY KEY,
                device_id TEXT NOT NULL,
                name TEXT NOT NULL,
                steps_json TEXT NOT NULL,
                updated_at INTEGER NOT NULL
            );
            CREATE TABLE IF NOT EXISTS reminders (
                id TEXT PRIMARY KEY,
                device_id TEXT NOT NULL,
                name TEXT NOT NULL,
                workflow_id TEXT NOT NULL,
                enabled INTEGER NOT NULL,
                days_json TEXT NOT NULL,
                start_minute INTEGER NOT NULL,
                end_minute INTEGER NOT NULL,
                interval_minutes INTEGER NOT NULL,
                timezone_offset_minutes INTEGER NOT NULL,
                updated_at INTEGER NOT NULL
            );
            CREATE TABLE IF NOT EXISTS commands (
                id TEXT PRIMARY KEY,
                device_id TEXT NOT NULL,
                kind TEXT NOT NULL,
                request_json TEXT NOT NULL,
                status TEXT NOT NULL,
                result_json TEXT,
                created_at INTEGER NOT NULL,
                completed_at INTEGER
            );
            CREATE TABLE IF NOT EXISTS media (
                id TEXT PRIMARY KEY,
                name TEXT NOT NULL,
                filename TEXT NOT NULL,
                source TEXT NOT NULL,
                created_at INTEGER NOT NULL
            );
            CREATE TABLE IF NOT EXISTS camera_snapshots (
                id TEXT PRIMARY KEY,
                device_id TEXT NOT NULL,
                filename TEXT NOT NULL,
                created_at INTEGER NOT NULL
            );
            """
        )
    bootstrap_id = os.getenv("BOOTSTRAP_DEVICE_ID", "").strip()
    bootstrap_token = os.getenv("BOOTSTRAP_DEVICE_TOKEN", "").strip()
    if bootstrap_id and bootstrap_token:
        create_device(bootstrap_id, "RIG Puppy", bootstrap_token)


def rows(query: str, params=()):
    with _lock, connection() as conn:
        return [dict(row) for row in conn.execute(query, params).fetchall()]


def row(query: str, params=()):
    result = rows(query, params)
    return result[0] if result else None


def execute(query: str, params=()) -> None:
    with _lock, connection() as conn:
        conn.execute(query, params)


def create_device(device_id: str, name: str, token: str | None = None) -> str:
    token = token or secrets.token_urlsafe(32)
    execute(
        "INSERT INTO devices(id,name,token_hash) VALUES(?,?,?) "
        "ON CONFLICT(id) DO UPDATE SET name=excluded.name, token_hash=excluded.token_hash",
        (device_id, name, token_hash(token)),
    )
    return token


def authenticate_device(device_id: str, token: str) -> bool:
    device = row("SELECT token_hash FROM devices WHERE id=?", (device_id,))
    return bool(device and secrets.compare_digest(device["token_hash"], token_hash(token)))


def device_payload(device: dict) -> dict:
    value = dict(device)
    value.pop("token_hash", None)
    value["online"] = bool(value.get("online"))
    value["status"] = json.loads(value.pop("status_json", "{}") or "{}")
    return value


def now() -> int:
    return int(time.time())
