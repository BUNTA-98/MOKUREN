#!/usr/bin/env python3
"""
Phase 4: Redis -> Telegram event listener.

Subscribes to the `mokuren:events` Redis channel and forwards formatted
notifications to a Telegram chat via the Bot API.

Environment variables (loaded from a .env file):
    TELEGRAM_BOT_TOKEN  - Telegram bot token
    TELEGRAM_CHAT_ID    - target chat id
    REDIS_HOST          - optional, defaults to 127.0.0.1
    REDIS_PORT          - optional, defaults to 6379
    REDIS_CHANNEL       - optional, defaults to mokuren:events
"""

import json
import os
import sys
import time

import redis
import requests
from dotenv import load_dotenv

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------

load_dotenv()

TELEGRAM_BOT_TOKEN = os.getenv("TELEGRAM_BOT_TOKEN")
TELEGRAM_CHAT_ID = os.getenv("TELEGRAM_CHAT_ID")

REDIS_HOST = os.getenv("REDIS_HOST", "127.0.0.1")
REDIS_PORT = int(os.getenv("REDIS_PORT", "6379"))
REDIS_CHANNEL = os.getenv("REDIS_CHANNEL", "mokuren:events")

TELEGRAM_API_URL = "https://api.telegram.org/bot{token}/sendMessage"
TELEGRAM_TIMEOUT = 10  # seconds
REDIS_RETRY_DELAY = 5  # seconds between reconnect attempts


# ---------------------------------------------------------------------------
# Formatting
# ---------------------------------------------------------------------------

def _fmt(value, default="n/a"):
    """Format a numeric value for display, tolerating None/missing."""
    if value is None:
        return default
    if isinstance(value, float):
        return f"{value:.4f}"
    return str(value)


def format_message(payload: dict) -> str:
    """Turn a parsed event payload into a human-readable Telegram message."""
    action = payload.get("action", "UNKNOWN")

    if action == "SYSTEM_STARTUP":
        return (
            "\U0001F7E2 *Mokuren started*\n"
            f"Dataset: `{payload.get('dataset', 'n/a')}`\n"
            f"Mode: `{payload.get('mode', 'n/a')}`\n"
            f"Time: `{payload.get('timestamp', 'n/a')}`"
        )

    if action == "SYSTEM_SHUTDOWN":
        return (
            "\U0001F534 *Mokuren stopped*\n"
            f"Reason: `{payload.get('reason', 'n/a')}`\n"
            f"Time: `{payload.get('timestamp', 'n/a')}`"
        )

    if action == "TRADE_OPEN":
        direction = payload.get("direction", "n/a")
        arrow = "\U0001F7E9" if str(direction).upper() in ("BUY", "LONG") else "\U0001F7E5"
        return (
            f"{arrow} *Trade opened* ({direction})\n"
            f"Entry: `{_fmt(payload.get('entry_price'))}`\n"
            f"Stop: `{_fmt(payload.get('stop_loss'))}`\n"
            f"Take profit: `{_fmt(payload.get('take_profit'))}`\n"
            f"Volume: `{_fmt(payload.get('volume'))}`\n"
            f"Time: `{payload.get('timestamp', 'n/a')}`"
        )

    if action == "POSITION_CLOSED":
        pnl = payload.get("pnl")
        try:
            pnl_val = float(pnl)
            pnl_icon = "\U0001F4B0" if pnl_val >= 0 else "\U0001F4C9"
        except (TypeError, ValueError):
            pnl_icon = "\u2753"
        return (
            f"{pnl_icon} *Position closed*\n"
            f"PnL: `{_fmt(pnl)}`\n"
            f"Exit reason: `{payload.get('exit_reason', 'n/a')}`\n"
            f"Exit price: `{_fmt(payload.get('exit_price'))}`\n"
            f"Time: `{payload.get('timestamp', 'n/a')}`"
        )

    if action == "PROP_HALT":
        return (
            "\U0001F6D1 *Prop guard halt*\n"
            f"Reason: `{payload.get('reason', 'n/a')}`\n"
            f"Symbol: `{payload.get('symbol', 'n/a')}`\n"
            f"Price: `{_fmt(payload.get('price'))}`\n"
            f"Time: `{payload.get('timestamp', 'n/a')}`"
        )

    # Fallback for unknown actions: dump the raw payload.
    return f"\u2139\uFE0F *Event: {action}*\n```\n{json.dumps(payload, indent=2)}\n```"


# ---------------------------------------------------------------------------
# Telegram
# ---------------------------------------------------------------------------

def send_telegram(text: str) -> bool:
    """Send a message to Telegram. Returns True on success."""
    url = TELEGRAM_API_URL.format(token=TELEGRAM_BOT_TOKEN)
    payload = {
        "chat_id": TELEGRAM_CHAT_ID,
        "text": text,
        "parse_mode": "Markdown",
        "disable_web_page_preview": True,
    }
    try:
        resp = requests.post(url, json=payload, timeout=TELEGRAM_TIMEOUT)
        if resp.status_code != 200:
            print(
                f"[telegram] send failed: HTTP {resp.status_code} - {resp.text}",
                file=sys.stderr,
            )
            return False
        return True
    except requests.RequestException as exc:
        print(f"[telegram] network error: {exc}", file=sys.stderr)
        return False


# ---------------------------------------------------------------------------
# Redis listener
# ---------------------------------------------------------------------------

def handle_message(raw: bytes) -> None:
    """Parse a raw Redis message and forward it to Telegram."""
    try:
        payload = json.loads(raw)
    except (json.JSONDecodeError, UnicodeDecodeError) as exc:
        print(f"[redis] dropping malformed message: {exc}", file=sys.stderr)
        return

    if not isinstance(payload, dict):
        print("[redis] dropping non-object payload", file=sys.stderr)
        return

    text = format_message(payload)
    send_telegram(text)


def run_listener() -> None:
    """Blocking loop: connect to Redis and forward events forever."""
    while True:
        try:
            client = redis.Redis(host=REDIS_HOST, port=REDIS_PORT, decode_responses=False)
            pubsub = client.pubsub(ignore_subscribe_messages=True)
            pubsub.subscribe(REDIS_CHANNEL)
            print(f"[redis] subscribed to '{REDIS_CHANNEL}' on {REDIS_HOST}:{REDIS_PORT}")

            for message in pubsub.listen():
                if message is None:
                    continue
                if message.get("type") != "message":
                    continue
                handle_message(message.get("data", b""))

        except redis.RedisError as exc:
            print(f"[redis] connection error: {exc}. retrying in {REDIS_RETRY_DELAY}s", file=sys.stderr)
            time.sleep(REDIS_RETRY_DELAY)
        except KeyboardInterrupt:
            print("\n[main] interrupted, shutting down.")
            return


def main() -> int:
    if not TELEGRAM_BOT_TOKEN or not TELEGRAM_CHAT_ID:
        print(
            "error: TELEGRAM_BOT_TOKEN and TELEGRAM_CHAT_ID must be set in the environment/.env",
            file=sys.stderr,
        )
        return 1

    run_listener()
    return 0


if __name__ == "__main__":
    sys.exit(main())
