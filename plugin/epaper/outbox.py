"""Durable outbox + transcript ring for the epaper adapter.

The device is battery-powered and sleeps, so nothing is pushed to it:
everything the agent sends is appended here first (SQLite, in the plugin's
data directory) and drained by the device polling ``GET /epaper/v1/inbox``.

The same store backs the adapter's ``standalone_sender_fn``, so cron jobs
running outside the gateway process enqueue into the identical outbox.

Delivery semantics are at-least-once: items stay until the device acks the
sequence number *after rendering* (see docs/protocol.md §4.4, §8).
"""

from __future__ import annotations

import json
import sqlite3
import threading
import time
from pathlib import Path

_SCHEMA = """
CREATE TABLE IF NOT EXISTS outbox (
    seq     INTEGER PRIMARY KEY AUTOINCREMENT,
    kind    TEXT NOT NULL,
    payload TEXT NOT NULL,
    ts      REAL NOT NULL
);
CREATE TABLE IF NOT EXISTS meta (
    key   TEXT PRIMARY KEY,
    value TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS transcript (
    id   INTEGER PRIMARY KEY AUTOINCREMENT,
    dir  TEXT NOT NULL,           -- 'in' (user/device) or 'out' (agent)
    kind TEXT NOT NULL,           -- 'text' | 'voice' | 'prompt'
    text TEXT NOT NULL,
    ts   REAL NOT NULL
);
CREATE TABLE IF NOT EXISTS prompts (
    prompt_id   TEXT PRIMARY KEY,
    prompt_type TEXT NOT NULL,    -- 'approval' | 'clarify'
    payload     TEXT NOT NULL,    -- JSON: text, choices, multi_select, session refs
    resolved    INTEGER NOT NULL DEFAULT 0,
    ts          REAL NOT NULL
);
"""

TRANSCRIPT_KEEP = 200


class Outbox:
    """Thread-safe SQLite store shared by the adapter and standalone sends."""

    def __init__(self, db_path: str | Path):
        self._path = Path(db_path)
        self._path.parent.mkdir(parents=True, exist_ok=True)
        self._lock = threading.Lock()
        self._db = sqlite3.connect(self._path, check_same_thread=False)
        self._db.executescript(_SCHEMA)
        self._db.commit()

    # -- outbound ---------------------------------------------------------

    def append(self, kind: str, payload: dict) -> int:
        """Append an outbox item; returns its sequence number."""
        with self._lock:
            cur = self._db.execute(
                "INSERT INTO outbox (kind, payload, ts) VALUES (?, ?, ?)",
                (kind, json.dumps(payload), time.time()),
            )
            self._db.commit()
            return int(cur.lastrowid)

    def fetch_after(self, seq: int, limit: int = 50) -> list[dict]:
        """Items with ``seq`` greater than the given one, oldest first."""
        with self._lock:
            rows = self._db.execute(
                "SELECT seq, kind, payload, ts FROM outbox "
                "WHERE seq > ? ORDER BY seq ASC LIMIT ?",
                (seq, limit),
            ).fetchall()
        return [
            {"seq": r[0], "kind": r[1], **json.loads(r[2]), "ts": r[3]}
            for r in rows
        ]

    def last_seq(self) -> int:
        with self._lock:
            row = self._db.execute(
                "SELECT COALESCE(MAX(seq), 0) FROM outbox"
            ).fetchone()
        return int(row[0])

    def ack(self, upto: int) -> None:
        """Drop items the device has rendered (seq <= upto)."""
        with self._lock:
            self._db.execute("DELETE FROM outbox WHERE seq <= ?", (upto,))
            self._db.execute(
                "INSERT INTO meta (key, value) VALUES ('last_acked', ?) "
                "ON CONFLICT(key) DO UPDATE SET value = excluded.value",
                (str(upto),),
            )
            self._db.commit()

    # -- transcript ring ----------------------------------------------------

    def add_transcript(self, direction: str, kind: str, text: str) -> None:
        with self._lock:
            self._db.execute(
                "INSERT INTO transcript (dir, kind, text, ts) VALUES (?, ?, ?, ?)",
                (direction, kind, text, time.time()),
            )
            self._db.execute(
                "DELETE FROM transcript WHERE id NOT IN "
                "(SELECT id FROM transcript ORDER BY id DESC LIMIT ?)",
                (TRANSCRIPT_KEEP,),
            )
            self._db.commit()

    def history(self, limit: int = 20) -> list[dict]:
        with self._lock:
            rows = self._db.execute(
                "SELECT dir, kind, text, ts FROM transcript "
                "ORDER BY id DESC LIMIT ?",
                (limit,),
            ).fetchall()
        return [
            {"dir": r[0], "kind": r[1], "text": r[2], "ts": r[3]}
            for r in reversed(rows)
        ]

    # -- prompts ------------------------------------------------------------

    def put_prompt(self, prompt_id: str, prompt_type: str, payload: dict) -> int:
        seq = self.append(
            "prompt",
            {"prompt_id": prompt_id, "prompt_type": prompt_type, **payload},
        )
        with self._lock:
            self._db.execute(
                "INSERT OR REPLACE INTO prompts (prompt_id, prompt_type, payload, ts) "
                "VALUES (?, ?, ?, ?)",
                (prompt_id, prompt_type, json.dumps(payload), time.time()),
            )
            self._db.commit()
        return seq

    def get_prompt(self, prompt_id: str) -> dict | None:
        with self._lock:
            row = self._db.execute(
                "SELECT prompt_type, payload, resolved FROM prompts "
                "WHERE prompt_id = ?",
                (prompt_id,),
            ).fetchone()
        if row is None:
            return None
        return {
            "prompt_type": row[0],
            "payload": json.loads(row[1]),
            "resolved": bool(row[2]),
        }

    def mark_prompt_resolved(self, prompt_id: str) -> None:
        with self._lock:
            self._db.execute(
                "UPDATE prompts SET resolved = 1 WHERE prompt_id = ?",
                (prompt_id,),
            )
            self._db.commit()

    def close(self) -> None:
        with self._lock:
            self._db.close()
