# hermes-epaper

A portable e-ink chat client for [Hermes Agent](https://hermes-agent.nousresearch.com),
built on the Waveshare **ESP32-S3-ePaper-3.97** board.

Talk to your self-hosted Hermes agent from a pocket e-ink device — no desktop
app, no Telegram. Push-to-talk voice, a rocker-based UI, an on-screen keyboard,
and BLE keyboard support. Replies, proactive messages, cron deliveries, and
exec-approval prompts all reach the device through a small asynchronous
protocol designed for e-ink and battery life.

## Hardware

| Part | Detail |
| --- | --- |
| Board | Waveshare ESP32-S3-ePaper-3.97 (SKU 33552) |
| MCU | ESP32-S3R8, dual-core 240 MHz, 16 MB flash, 8 MB PSRAM |
| Display | 3.97" e-paper, 800×480, monochrome |
| Audio | ES8311 codec + microphone, NS4150B amp + speaker |
| Controls | Rotary knob (rotate / push), BOOT key, PWR key |
| Power | 3.7 V LiPo via MX1.25, USB-C charging |

## How it works

```
┌────────────┐   HTTPS + JSON    ┌──────────────────┐   MessageEvent   ┌─────────┐
│  ESP32-S3  │ ────────────────► │  epaper adapter  │ ───────────────► │  Hermes │
│  firmware  │ ◄──────────────── │  (gateway plugin)│ ◄─────────────── │  agent  │
└────────────┘   poll / long-poll └──────────────────┘   send()          └─────────┘
```

- The **gateway plugin** (`plugin/epaper/`) is a standard Hermes platform
  adapter. It runs a small HTTPS server for the device, turns device posts
  into gateway message events, and queues everything the agent sends into a
  durable outbox.
- The **firmware** (`firmware/`) is an ESP-IDF project. It posts messages,
  long-polls the outbox, renders replies in single e-ink refreshes, and
  light-sleeps between checks. Voice notes are transcribed by the gateway's
  own STT pipeline — the device just uploads audio.
- The wire protocol is specified in [docs/protocol.md](docs/protocol.md);
  the control scheme in [docs/controls.md](docs/controls.md).

## Repository layout

```
docs/       Protocol spec and control/input spec
plugin/     Hermes gateway platform plugin (Python, aiohttp + SQLite)
firmware/   ESP-IDF firmware for the ESP32-S3 board
tools/      device_sim.py — a CLI simulator that speaks the device protocol
```

## Quick start

**Gateway side**

```sh
cp -r plugin/epaper ~/.hermes/plugins/
# set EPAPER_DEVICE_TOKEN (and optionally EPAPER_PORT) in ~/.hermes/.env
hermes gateway restart
```

**Device side**

```sh
cd firmware
idf.py set-target esp32s3
idf.py menuconfig   # Hermes ePaper: server URL + device token
idf.py build flash monitor
```

**No hardware yet?** Exercise the adapter with the simulator:

```sh
python3 tools/device_sim.py --url http://localhost:8791 --token "$EPAPER_DEVICE_TOKEN" send "Hello from the sim"
python3 tools/device_sim.py --url http://localhost:8791 --token "$EPAPER_DEVICE_TOKEN" watch
```

## Status

Early development. The protocol (v0.1) is settled; the plugin and firmware
are working skeletons with the hardware/driver seams marked. See the
`TODO(hw)` markers in the firmware and the open questions in the protocol
doc.

## License

MIT — see [LICENSE](LICENSE).
