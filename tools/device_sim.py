#!/usr/bin/env python3
"""Device simulator for the hermes-epaper protocol (docs/protocol.md).

Speaks the exact wire protocol the ESP32 firmware speaks, so the gateway
adapter can be developed and tested without hardware. Stdlib only.

Usage:
    device_sim.py --url http://localhost:8791 --token "$EPAPER_DEVICE_TOKEN" status
    device_sim.py ... send "What's on my calendar tomorrow?"
    device_sim.py ... watch                 # long-poll and print items
    device_sim.py ... inbox --after 41      # one-shot poll
    device_sim.py ... ack 42
    device_sim.py ... resolve <prompt_id> Approve
    device_sim.py ... history
    device_sim.py ... voice recording.wav

State (device id, last seen seq) persists in ~/.hermes-epaper-sim.json,
mirroring the device's NVS.
"""

from __future__ import annotations

import argparse
import json
import sys
import urllib.request
import uuid
from pathlib import Path

STATE_FILE = Path.home() / ".hermes-epaper-sim.json"


def load_state() -> dict:
    if STATE_FILE.exists():
        return json.loads(STATE_FILE.read_text())
    return {"device_id": uuid.uuid4().hex[:12], "last_seq": 0}


def save_state(state: dict) -> None:
    STATE_FILE.write_text(json.dumps(state, indent=2))


class Device:
    def __init__(self, base_url: str, token: str, device_id: str):
        self.base = base_url.rstrip("/")
        self.token = token
        self.device_id = device_id

    def request(self, method: str, path: str, body: dict | None = None,
                raw: bytes | None = None, headers: dict | None = None) -> dict:
        data = raw
        hdrs = {
            "Authorization": f"Bearer {self.token}",
            "X-Device-Id": self.device_id,
        }
        if body is not None:
            data = json.dumps(body).encode()
            hdrs["Content-Type"] = "application/json"
        if headers:
            hdrs.update(headers)
        req = urllib.request.Request(
            self.base + path, data=data, headers=hdrs, method=method
        )
        with urllib.request.urlopen(req, timeout=70) as resp:
            payload = resp.read()
            return json.loads(payload) if payload else {}

    # -- protocol ---------------------------------------------------------

    def status(self) -> dict:
        return self.request("GET", "/epaper/v1/status")

    def send_text(self, text: str) -> dict:
        return self.request(
            "POST", "/epaper/v1/messages",
            {"id": str(uuid.uuid4()), "text": text},
        )

    def send_voice(self, wav_path: str) -> dict:
        data = Path(wav_path).read_bytes()
        return self.request(
            "POST", "/epaper/v1/voice", raw=data,
            headers={"Content-Type": "audio/wav", "X-Msg-Id": str(uuid.uuid4())},
        )

    def inbox(self, after: int, wait: int = 0) -> dict:
        return self.request("GET", f"/epaper/v1/inbox?after={after}&wait={wait}")

    def ack(self, upto: int) -> dict:
        return self.request("POST", "/epaper/v1/inbox/ack", {"upto": upto})

    def resolve(self, prompt_id: str, choice: str) -> dict:
        return self.request(
            "POST", f"/epaper/v1/prompts/{prompt_id}/resolve", {"choice": choice}
        )

    def history(self, limit: int = 20) -> dict:
        return self.request("GET", f"/epaper/v1/history?limit={limit}")


def print_items(result: dict) -> None:
    for item in result.get("items", []):
        if item.get("kind") == "prompt":
            print(f"[{item['seq']}] PROMPT ({item.get('prompt_type')}) "
                  f"id={item.get('prompt_id')}: {item.get('text')!r} "
                  f"choices={item.get('choices')}")
        else:
            print(f"[{item['seq']}] {item.get('text')}")
    if result.get("agent_state"):
        print(f"  agent_state={result['agent_state']} next={result.get('next')}")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--url", default="http://localhost:8791")
    ap.add_argument("--token", required=True)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("status")
    p_send = sub.add_parser("send")
    p_send.add_argument("text")
    p_voice = sub.add_parser("voice")
    p_voice.add_argument("wav")
    p_inbox = sub.add_parser("inbox")
    p_inbox.add_argument("--after", type=int, default=None)
    p_inbox.add_argument("--wait", type=int, default=0)
    sub.add_parser("watch")
    p_ack = sub.add_parser("ack")
    p_ack.add_argument("upto", type=int)
    p_res = sub.add_parser("resolve")
    p_res.add_argument("prompt_id")
    p_res.add_argument("choice")
    p_hist = sub.add_parser("history")
    p_hist.add_argument("--limit", type=int, default=20)
    args = ap.parse_args()

    state = load_state()
    dev = Device(args.url, args.token, state["device_id"])

    if args.cmd == "status":
        print(json.dumps(dev.status(), indent=2))
    elif args.cmd == "send":
        print(dev.send_text(args.text))
    elif args.cmd == "voice":
        print(dev.send_voice(args.wav))
    elif args.cmd == "inbox":
        after = state["last_seq"] if args.after is None else args.after
        result = dev.inbox(after, wait=args.wait)
        print_items(result)
        if result.get("items"):
            dev.ack(result["next"])
            state["last_seq"] = result["next"]
            save_state(state)
    elif args.cmd == "watch":
        print(f"watching as device {state['device_id']} (Ctrl-C to stop)")
        try:
            while True:
                result = dev.inbox(state["last_seq"], wait=50)
                if result.get("items"):
                    print_items(result)
                    dev.ack(result["next"])
                    state["last_seq"] = result["next"]
                    save_state(state)
        except KeyboardInterrupt:
            pass
    elif args.cmd == "ack":
        print(dev.ack(args.upto))
        state["last_seq"] = args.upto
        save_state(state)
    elif args.cmd == "resolve":
        print(dev.resolve(args.prompt_id, args.choice))
    elif args.cmd == "history":
        for entry in dev.history(args.limit).get("items", []):
            arrow = ">" if entry["dir"] == "in" else "<"
            print(f"{arrow} [{entry['kind']}] {entry['text']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
