# Controls & Input Spec

Adopted 2026-10-20; mechanism corrected 2026-10-20 against the proven
waveshare-airecorder firmware (see [hardware.md](hardware.md)). Design rule:
**conventional mappings only** — what a user of rocker-driven devices,
phones, and ESP32 voice assistants already expects. No invented gestures.

## Physical controls

The board's controls are a **rocker** (up / down / middle-push), a **BOOT**
button, and a **PWR** button — there is no rotary encoder; "scroll" is the
rocker. All are momentary and active-low.

| Control | Mechanism | Meaning |
| --- | --- | --- |
| Rocker up / down | GPIO4 / GPIO6 | Scroll; hold repeats after 350 ms |
| Rocker middle push | GPIO5 (FN) click | Select — the *only* confirm |
| Rocker middle hold | GPIO5 long-press (600 ms) | Back; at the top level, Home |
| BOOT hold | GPIO0 | Push-to-talk: hold to record, release to send |
| PWR press | AXP2101 power key (short IRQ) | Home / wake |
| PWR hold | AXP2101 power key (long IRQ, ~1 s) | Power menu (sleep, restart, off, Wi-Fi off) |

Notes:

- The semantic map is the conventional one adopted for this project; only
  the mechanism names changed when the hardware was verified (the "knob" is
  a rocker, its "push" is the FN key).
- BOOT as the talk key follows the Xiaozhi voice-assistant convention on
  ESP32 boards, and matches the reference recorder exactly (BOOT hold =
  record). GPIO0 is the boot strap pin: it must read high at reset, and a
  press that wakes the device from light sleep is consumed as wake-only —
  it never starts a recording.
- PWR is not a GPIO button: the AXP2101 PMIC decodes short vs long presses
  and raises interrupts on GPIO38. A ~6 s hold is a PMIC-enforced forced
  shutdown, so the app-level hold stays around 1 s.
- There is deliberately **no** separate "enter" vs "select": one confirm
  action everywhere.

## Input architecture

All input sources produce the same event stream; screens never care where an
event came from.

```
rocker/BOOT ──┐
PWR (PMIC) ───┤
voice (PTT) ──┼─► event queue ─► screen state machines ─► renderer
on-screen kbd ┤        ▲
BLE keyboard ─┘        └── network events (inbox items, prompts)
```

Event vocabulary: `SCROLL_UP`, `SCROLL_DOWN`, `SELECT`, `BACK`, `HOME`,
`POWER_MENU`, `PTT_START`, `PTT_STOP`, `TEXT(str)`, `KEY(ch)`.

The proven button stack is Espressif's iot_button component, which provides
press / repeat / single-click / double-click / long-press classification.
Its polling timer must be suspended across light sleep (see hardware.md).

## On-screen keyboard

- Full QWERTY grid rendered on the 800×480 panel, navigated with the rocker:
  up/down moves between rows, middle push types the focused character,
  middle hold exits to the previous screen. (The reference project ships a
  working OSK on this exact panel — the interaction model is proven.)
- A symbol/number layer and a shortcut strip (`/new`, `/status`, `/stop`,
  `/approve`, `/deny`) sit under the grid for slash commands.
- Partial refresh while typing; full refresh on send.

## BLE keyboard

- The ESP32-S3 acts as a BLE HID host (NimBLE). The S3 is **BLE-only** — the
  keyboard must be a BLE HID keyboard, or a dual-mode keyboard switched to
  its LE mode. Classic-Bluetooth-only keyboards are not supported.
- Pairing lives in Settings → Bluetooth: rocker-driven scan/select, bonding
  persisted in NVS.
- A paired keyboard produces the same `TEXT`/`KEY` events as the on-screen
  keyboard, including Enter = send and Esc = back.
- ⚠️ No precedent in the reference project (it never enables Bluetooth) —
  this is the one input path still to be proven on-device.
- **Performance rules** (implemented in `firmware/main/ble_hid.c`): the
  Bluetooth stack is never initialized at boot unless a bond already
  exists; scanning happens only while the pairing screen is open, and the
  stack fully deinitializes again if nothing bonded; a connected keyboard
  uses a 30–50 ms connection interval with latency 4 (a few tiny packets
  per keystroke). Unused BLE costs zero CPU, RAM, and radio time.

## Voice

- BOOT hold starts recording from the onboard microphone via the ES8311
  codec (16 kHz mono WAV); release stops and uploads (see protocol §4.2).
- A recording frame ("Listening… 0:03") renders on press; the sent frame
  ("Sent · waiting") follows on release.
- Maximum recording length: 120 s (matches Hermes' voice config default).
