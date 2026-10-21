# Hermes ePaper Protocol — v0.1 (Draft)

Device ↔ gateway protocol for a portable e-ink chat client for
[Hermes Agent](https://hermes-agent.nousresearch.com), built on the Waveshare
ESP32-S3-ePaper-3.97 board.

The server side is a Hermes **platform plugin** (`epaper`) implementing the
standard `BasePlatformAdapter` contract. The device is a thin client: it speaks
only this protocol and knows nothing about Hermes internals.

## 1. Design constraints

**From the hardware**

- ESP32-S3: 2.4 GHz Wi-Fi only (no Ethernet, no cellular, no Tailscale client),
  TLS-capable, 8 MB PSRAM. Battery powered.
- 800×480 monochrome e-paper: full refresh is slow (~1 s+), partial refresh is
  cheap. No streaming UI, no typing indicators, no inline editing.
- The device sleeps (staged light sleep; see docs/hardware.md). It
  **cannot receive pushes** — all delivery is device-initiated polling.
- Input: rocker (up/down scroll, middle push = select, middle hold = back),
  BOOT (hold = push-to-talk), PWR via the PMIC (press = home/wake, hold =
  power menu), on-screen keyboard, BLE keyboard. See docs/controls.md and
  docs/hardware.md.

**From Hermes**

- Agent turns take seconds to minutes. The adapter guide is explicit: inline
  replies inside a request window are not feasible. **The protocol is
  asynchronous end to end** — a POST accepts a message; the reply arrives
  later through the outbox.
- The gateway's "silence, then response" presentation is a documented,
  supported pattern for constrained platforms. We register the platform with
  `display_tier: "minimal"`: no streaming edits, no tool-progress bubbles.
- Sessions are per-chat. The device is its own chat and gets its own session,
  like every other platform.

## 2. Architecture

```
┌────────────┐   HTTPS + JSON    ┌──────────────────┐   MessageEvent   ┌─────────┐
│  ESP32-S3  │ ────────────────► │  epaper adapter  │ ───────────────► │  Hermes │
│  firmware  │ ◄──────────────── │  (gateway plugin)│ ◄─────────────── │  agent  │
└────────────┘   poll / long-poll └──────────────────┘   send()          └─────────┘
```

- **Inbound** (device → agent): the adapter builds a `MessageEvent`
  (`gateway.platforms.event`) and calls `handle_message()`.
- **Outbound** (agent → device): the gateway calls the adapter's `send()`,
  which appends to a **durable outbox**. The device drains the outbox by
  polling. Nothing is ever pushed to the device.
- The outbox lives in the plugin's data directory (SQLite via
  `plugin_db()`), so it survives gateway restarts and is shared with the
  adapter's `standalone_sender_fn` for out-of-process cron delivery.

## 3. Transport & auth

- HTTPS, JSON bodies, UTF-8. Base path: `/epaper/v1`.
  Plain HTTP is permitted on a trusted LAN only (config flag); token auth is
  required either way.
- Every request carries:
  - `Authorization: Bearer <device_token>`
  - `X-Device-Id: <device_id>` (stable per device, e.g. MAC-derived)
- One device = one chat: `chat_id = "epaper-<device_id>"`, `chat_type = "dm"`.
- **Pairing (v0.1):** pre-shared token. The adapter is configured with
  `EPAPER_DEVICE_TOKEN`; the same token is entered on the device's setup
  screen once (OSK or BLE keyboard) and stored in NVS. Reusing Hermes'
  DM pairing-code flow is a v2 candidate.

## 4. Endpoints

### 4.1 `POST /epaper/v1/messages` — send a text message

```json
{ "id": "3f6b…uuid", "text": "What's on my calendar tomorrow?", "ts": 1792531200 }
```

- `id` is a device-generated UUID. The adapter deduplicates on it
  (`MessageDeduplicator`), so retries after a timeout are safe.
- Response: `202 { "id": "…", "status": "accepted" }`. Never blocks on the agent.

### 4.2 `POST /epaper/v1/voice` — send a voice message

- Raw audio body, `Content-Type: audio/wav`, `X-Msg-Id: <uuid>`.
- Baseline format: **WAV, 16 kHz, mono, 16-bit** (what the ES8311 codec
  produces most simply). Opus-in-Ogg is a v2 optimization.
- The adapter caches the audio via the gateway media cache and emits a
  `MessageEvent` with `message_type = MessageType.VOICE`. **Transcription is
  done by the gateway's central STT pipeline** — the adapter contains no STT
  code, exactly like the bundled Matrix/Telegram adapters.
- Response: `202`.

### 4.3 `GET /epaper/v1/inbox?after=<seq>&wait=<secs>` — drain the outbox

- Returns outbox items with `seq > after`, in order.
- `wait` (0–50) long-polls: the request is held until an item arrives or the
  wait expires. The device long-polls while awake and interactive; it polls
  with `wait=0` on wake-from-sleep checks.
- Response:

```json
{
  "items": [
    { "seq": 41, "kind": "message", "id": "…", "text": "…", "reply_to": "…", "ts": 1792531260 },
    { "seq": 42, "kind": "prompt", "prompt_id": "…", "prompt_type": "approval",
      "text": "Run `rm -rf ./build`?", "choices": ["Approve", "Deny"], "multi_select": false }
  ],
  "next": 42,
  "agent_state": "idle"
}
```

- `agent_state` is `"working"` while a turn is in flight, so the device can
  show a simple "thinking…" frame without a typing indicator.
- Item kinds:
  - `message` — an agent reply, a proactive/cron delivery, or a gateway
    notice (busy-acks like ⏳ are ordinary messages).
  - `prompt` — a structured approval or clarify request (see §5).

### 4.4 `POST /epaper/v1/inbox/ack` — acknowledge delivery

```json
{ "upto": 42 }
```

- The device acks **after rendering**. Delivery is at-least-once: un-acked
  items are returned again by the next inbox poll (mirroring the gateway's
  own delivery-ledger semantics). The adapter prunes items ≤ the acked seq.

### 4.5 `POST /epaper/v1/prompts/<prompt_id>/resolve` — answer a prompt

```json
{ "choice": "Approve" }
```

- The adapter resolves through Hermes' public helpers:
  `tools.approval.resolve_gateway_approval` for approvals,
  `tools.clarify_gateway.resolve_gateway_clarify` for clarify questions.

### 4.6 `GET /epaper/v1/history?limit=20` — render cache for boot

- Returns the last N exchanges (both directions) so the device can repaint
  its scrollback after a reboot or long sleep. The adapter keeps this ring in
  its data store. It is a **render cache, not the source of truth** — the
  session transcript lives in Hermes.

### 4.7 `GET /epaper/v1/status` — health & config

```json
{ "ok": true, "protocol": 1, "server_time": 1792531200,
  "agent_state": "idle", "wake_interval_s": 900 }
```

- Doubles as the device's time source (RTC sync) and config channel
  (wake interval, poll parameters).

## 5. Prompts: approvals & clarify questions

Hermes surfaces two interactive prompt types to platforms: **exec approvals**
and **clarify questions** (with `choices` / `multi_select`). Platforms with
native buttons resolve them natively; platforms without fall back to numbered
text replies.

- **Protocol decision:** prompts are first-class inbox items (§4.3), rendered
  on-device as a knob-scrollable choice list. Resolution posts to §4.5.
- **Degrade path:** if no structured prompt is pending, gateway text prompts
  still work — the device offers `/approve` and `/deny` quick actions in the
  composer, sent as ordinary messages.
- **Open implementation question:** the public helpers for *resolving*
  prompts are documented; the exact adapter-side hook by which a pending
  prompt is *announced* to a plugin adapter must be confirmed against
  `gateway/platforms/base.py` during implementation. This does not change the
  wire protocol — only the adapter internals.

## 6. Flows

**Text turn.** Compose (OSK / BLE keyboard) → `POST /messages` (202) →
"sent" frame → long-poll inbox → reply item arrives → one full refresh →
`ack`. Slash commands (`/new`, `/status`, `/stop`, …) are ordinary text
messages; the composer keeps a commands menu for them.

**Voice turn.** BOOT hold → record → release → `POST /voice` → same as text.
The transcript appears in the session like any voice note on Telegram.

**Proactive / cron delivery.** `send()` enqueues while the device sleeps →
device wakes on its timer interval (default 15 min, from `/status`) or on PWR
press → poll → if items: render, optional speaker beep → ack → sleep.
Cron jobs target the device with `deliver=epaper` (via
`cron_deliver_env_var="EPAPER_HOME_CHANNEL"`).

**Long turn.** If the agent is still working when the device wants to sleep,
the device sleeps anyway. The reply waits in the outbox — durability beats
liveness on e-ink.

## 7. Display contract (adapter registration)

- `platform_hint`: *"You are chatting via a monochrome e-ink display
  (800×480). Keep responses concise and plain; avoid tables and heavy
  markdown; prefer short paragraphs and simple lists."*
- `max_message_length`: 2000 (smart chunking splits longer replies into
  consecutive inbox items).
- `display_tier: "minimal"`, no `send_typing` override.
- Agent-sent media (images/files): v0.1 renders a text placeholder
  (`[image: name — view on desktop]`). Dithered image rendering is future
  work.

## 8. Reliability

- **Inbound:** UUID per message + adapter-side dedup → safe retries.
- **Outbound:** seq + ack → at-least-once, no loss across gateway restarts
  (durable outbox) or device reboots (device persists its last acked seq in
  NVS; on mismatch it resyncs from `/history`).
- **Errors:** standard HTTP codes; `401` = bad token (device returns to the
  setup screen); `429`/5xx = back off and retry with the same message id.

## 9. Versioning

- `/v1` in the path and `"protocol": 1` in `/status`. Within v1, changes are
  additive only (new optional fields, new item kinds the device may ignore).

## 10. Decisions still open

1. **Audio format** — WAV baseline (simple, big) vs Opus/Ogg (small, more
   firmware work). Spec assumes WAV for v0.1.
2. **TLS on the LAN** — self-signed cert + fingerprint pinning on the device,
   vs plain HTTP behind token auth for v0.1.
3. **Wake interval** — 15 min default trades freshness against battery.
   Configurable via `/status`.
4. **Prompt announcement hook** (§5) — confirm in `base.py` at
   implementation time; text fallback works regardless.
