"""Hermes platform adapter for the hermes-epaper device.

Implements the device protocol in docs/protocol.md: a small HTTP(S) API the
ESP32-S3 e-ink device talks to, backed by a durable SQLite outbox
(``outbox.py``). The device posts messages (text or voice) and polls for
output; nothing is pushed to it, because it sleeps.

Hermes-facing contract (per the "Adding a Platform Adapter" guide):
  * inbound  -> build a MessageEvent and call ``self.handle_message(event)``
  * outbound -> ``send()`` appends to the outbox; delivery is at-least-once
                via sequence numbers + device acks
  * voice    -> MessageType.VOICE events with gateway-cached media; the
                central STT pipeline transcribes (no STT code here)
  * prompts  -> approvals/clarifies are queued as structured outbox items
                and resolved via tools.approval / tools.clarify_gateway
"""

from __future__ import annotations

import asyncio
import hmac
import json
import logging
import os
import tempfile
import time
from pathlib import Path

from aiohttp import web

from gateway.config import Platform, PlatformConfig
from gateway.platforms._shared import extra_or_secret, get_scoped_secret, seed_extra_from_env
from gateway.platforms.base import BasePlatformAdapter, SendResult
from gateway.platforms.event import MessageEvent, MessageType
from gateway.platforms.helpers import MessageDeduplicator

from .outbox import Outbox

logger = logging.getLogger(__name__)

PLATFORM_HINT = (
    "You are chatting via a monochrome e-ink display (800x480). "
    "Keep responses concise and plain; avoid tables and heavy markdown; "
    "prefer short paragraphs and simple lists."
)

DEFAULT_PORT = 8791
MAX_WAIT_S = 50


def _data_dir() -> Path:
    """Plugin data directory (outbox DB, cached voice uploads).

    Uses EPAPER_DATA_DIR when set; otherwise ~/.hermes/epaper. In production
    this maps to the plugin system's plugin_data_dir().
    """
    base = os.environ.get("EPAPER_DATA_DIR") or os.path.join(
        os.path.expanduser("~/.hermes"), "epaper"
    )
    path = Path(base)
    path.mkdir(parents=True, exist_ok=True)
    return path


class EpaperAdapter(BasePlatformAdapter):
    def __init__(self, config: PlatformConfig):
        super().__init__(config, Platform("epaper"))
        extra = config.extra or {}
        self._token = extra_or_secret(extra, "device_token", "EPAPER_DEVICE_TOKEN")
        self._port = int(extra_or_secret(extra, "port", "EPAPER_PORT") or DEFAULT_PORT)
        self._bind = extra_or_secret(extra, "bind", "EPAPER_BIND") or "0.0.0.0"
        self._wake_interval_s = int(extra.get("wake_interval_s", 900))
        self._outbox = Outbox(_data_dir() / "outbox.db")
        self._dedup = MessageDeduplicator(ttl_seconds=600)
        self._runner: web.AppRunner | None = None
        # Advisory only: turns in flight (incremented on inbound, decremented
        # on send). The device uses it for a "thinking..." frame.
        self._pending_turns = 0

    # -- lifecycle ------------------------------------------------------

    async def connect(self, *, is_reconnect: bool = False) -> bool:
        app = web.Application(middlewares=[self._auth_middleware])
        app.router.add_post("/epaper/v1/messages", self._post_message)
        app.router.add_post("/epaper/v1/voice", self._post_voice)
        app.router.add_get("/epaper/v1/inbox", self._get_inbox)
        app.router.add_post("/epaper/v1/inbox/ack", self._post_ack)
        app.router.add_post("/epaper/v1/prompts/{prompt_id}/resolve", self._post_resolve)
        app.router.add_get("/epaper/v1/history", self._get_history)
        app.router.add_get("/epaper/v1/status", self._get_status)
        self._runner = web.AppRunner(app)
        await self._runner.setup()
        await web.TCPSite(self._runner, self._bind, self._port).start()
        self._mark_connected()
        logger.info("epaper adapter listening on %s:%s", self._bind, self._port)
        return True

    async def disconnect(self) -> None:
        if self._runner is not None:
            await self._runner.cleanup()
            self._runner = None
        self._outbox.close()
        self._mark_disconnected()

    # -- Hermes outbound --------------------------------------------------

    async def send(self, chat_id, content, reply_to=None, metadata=None):
        seq = self._outbox.append(
            "message", {"text": content, "reply_to": reply_to}
        )
        self._outbox.add_transcript("out", "text", content)
        self._pending_turns = max(0, self._pending_turns - 1)
        return SendResult(success=True, message_id=f"ep-{seq}")

    async def get_chat_info(self, chat_id):
        return {"name": "ePaper", "type": "dm"}

    # NOTE: no send_typing override — display_tier "minimal" means the
    # gateway does not stream or show progress; silence-then-response.

    # -- auth ---------------------------------------------------------------

    @web.middleware
    async def _auth_middleware(self, request, handler):
        auth = request.headers.get("Authorization", "")
        token = auth.removeprefix("Bearer ").strip()
        if not self._token or not hmac.compare_digest(token, self._token):
            return web.json_response({"error": "unauthorized"}, status=401)
        if not request.headers.get("X-Device-Id"):
            return web.json_response({"error": "missing X-Device-Id"}, status=400)
        return await handler(request)

    # -- device endpoints ---------------------------------------------------

    def _chat_id(self, request) -> str:
        return f"epaper-{request.headers['X-Device-Id']}"

    def _source(self, request):
        return self.build_source(
            chat_id=self._chat_id(request),
            chat_name="ePaper",
            chat_type="dm",
            user_id="owner",
            user_name="Owner",
        )

    async def _post_message(self, request):
        body = await request.json()
        msg_id, text = body.get("id"), (body.get("text") or "").strip()
        if not msg_id or not text:
            return web.json_response({"error": "id and text required"}, status=400)
        if self._dedup.is_duplicate(msg_id):
            return web.json_response({"id": msg_id, "status": "duplicate"}, status=202)
        event = MessageEvent(
            text=text,
            message_type=MessageType.TEXT,
            source=self._source(request),
            message_id=msg_id,
        )
        self._pending_turns += 1
        self._outbox.add_transcript("in", "text", text)
        await self.handle_message(event)
        return web.json_response({"id": msg_id, "status": "accepted"}, status=202)

    async def _post_voice(self, request):
        msg_id = request.headers.get("X-Msg-Id")
        if not msg_id:
            return web.json_response({"error": "missing X-Msg-Id"}, status=400)
        if self._dedup.is_duplicate(msg_id):
            return web.json_response({"id": msg_id, "status": "duplicate"}, status=202)
        audio = await request.read()
        # Cache the upload and hand the path to the gateway media cache; the
        # central STT pipeline transcribes VOICE events. Field for the cached
        # media follows gateway.platforms.event (same pattern as the Matrix
        # adapter's cached audio) — confirm exact field name against
        # event.py at integration time. See protocol.md §10.4.
        voice_dir = _data_dir() / "voice"
        voice_dir.mkdir(parents=True, exist_ok=True)
        fd, path = tempfile.mkstemp(suffix=".wav", dir=voice_dir)
        with os.fdopen(fd, "wb") as f:
            f.write(audio)
        event = MessageEvent(
            text="",
            message_type=MessageType.VOICE,
            source=self._source(request),
            message_id=msg_id,
            media_urls=[path],
        )
        self._pending_turns += 1
        self._outbox.add_transcript("in", "voice", "[voice message]")
        await self.handle_message(event)
        return web.json_response({"id": msg_id, "status": "accepted"}, status=202)

    async def _get_inbox(self, request):
        after = int(request.query.get("after", 0))
        wait = min(int(request.query.get("wait", 0)), MAX_WAIT_S)
        deadline = time.monotonic() + wait
        items = self._outbox.fetch_after(after)
        while not items and time.monotonic() < deadline:
            await asyncio.sleep(0.5)
            items = self._outbox.fetch_after(after)
        return web.json_response(
            {
                "items": items,
                "next": items[-1]["seq"] if items else after,
                "agent_state": "working" if self._pending_turns > 0 else "idle",
            }
        )

    async def _post_ack(self, request):
        body = await request.json()
        self._outbox.ack(int(body.get("upto", 0)))
        return web.json_response({"ok": True})

    async def _post_resolve(self, request):
        prompt_id = request.match_info["prompt_id"]
        prompt = self._outbox.get_prompt(prompt_id)
        if prompt is None or prompt["resolved"]:
            return web.json_response({"error": "unknown prompt"}, status=404)
        choice = (await request.json()).get("choice", "")
        resolved = False
        try:
            if prompt["prompt_type"] == "approval":
                from tools.approval import resolve_gateway_approval

                resolved = bool(resolve_gateway_approval(prompt_id, choice))
            else:
                from tools.clarify_gateway import resolve_gateway_clarify

                resolved = bool(resolve_gateway_clarify(prompt_id, choice))
        except Exception:
            logger.exception("prompt resolution failed; falling back to text")
        if not resolved:
            # Degrade path (protocol §5): deliver the choice as a plain
            # message — the gateway's numbered-text fallback accepts it.
            event = MessageEvent(
                text=choice,
                message_type=MessageType.TEXT,
                source=self._source(request),
                message_id=f"resolve-{prompt_id}",
            )
            await self.handle_message(event)
        self._outbox.mark_prompt_resolved(prompt_id)
        return web.json_response({"ok": True})

    async def _get_history(self, request):
        limit = int(request.query.get("limit", 20))
        return web.json_response({"items": self._outbox.history(limit)})

    async def _get_status(self, request):
        return web.json_response(
            {
                "ok": True,
                "protocol": 1,
                "server_time": int(time.time()),
                "agent_state": "working" if self._pending_turns > 0 else "idle",
                "wake_interval_s": self._wake_interval_s,
            }
        )


# -- plugin registration ----------------------------------------------------


def check_requirements() -> bool:
    return bool(get_scoped_secret("EPAPER_DEVICE_TOKEN"))


def _env_enablement() -> dict | None:
    if not (get_scoped_secret("EPAPER_DEVICE_TOKEN") or "").strip():
        return None
    return seed_extra_from_env(
        (
            ("EPAPER_DEVICE_TOKEN", "device_token", None),
            ("EPAPER_PORT", "port", int),
            ("EPAPER_BIND", "bind", None),
        ),
        home_env="EPAPER_HOME_CHANNEL",
    )


async def _standalone_send(pconfig, chat_id, message, *, thread_id=None,
                           media_files=None, force_document=False):
    """Cron delivery from outside the gateway process: enqueue only."""
    outbox = Outbox(_data_dir() / "outbox.db")
    try:
        seq = outbox.append("message", {"text": message, "reply_to": None})
        outbox.add_transcript("out", "text", message)
        return {"success": True, "message_id": f"ep-{seq}"}
    finally:
        outbox.close()


def register(ctx):
    import dataclasses

    kwargs = dict(
        name="epaper",
        label="ePaper",
        adapter_factory=EpaperAdapter,
        check_fn=check_requirements,
        required_env=["EPAPER_DEVICE_TOKEN"],
        env_enablement_fn=_env_enablement,
        cron_deliver_env_var="EPAPER_HOME_CHANNEL",
        standalone_sender_fn=_standalone_send,
        allowed_users_env="EPAPER_ALLOWED_USERS",
        max_message_length=2000,
        platform_hint=PLATFORM_HINT,
        emoji="📟",
    )
    # Feature-detect newer PlatformEntry fields (older cores reject
    # unknown kwargs) — pattern from the adapter guide.
    from gateway.platform_registry import PlatformEntry

    known = {f.name for f in dataclasses.fields(PlatformEntry)}
    if "display_tier" in known:
        kwargs["display_tier"] = "minimal"
    ctx.register_platform(**kwargs)
