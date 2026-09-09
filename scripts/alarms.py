#!/usr/bin/env python3
"""Temperature/timer alarms and Web Push (VAPID) notifications.

Two kinds of alarm, both created from the grill page's "Alarms" card:
  - "temp": fires once bridge.state[sensor] crosses `target` (>= or <=).
  - "timer": fires once `duration_seconds` have elapsed since creation.

A fired alarm sends a Web Push message to every subscribed browser (VAPID —
no third-party push service account needed, just a key pair this process
owns) and is then dropped; nothing here re-arms or repeats an alarm.

Persistence: alarms.json and push_subscriptions.json sit next to
scripts/.grill_env (pitboss_cloud.ENV_PATH's directory — a mounted volume in
Docker, see docker/sidecar.Dockerfile), so both survive a container restart
the same way the grill password does. The VAPID private key is generated
once and persisted the same way (.vapid_private_key.pem); losing it would
invalidate every browser's existing subscription, so it is not regenerated
on every start (same reasoning as grill_sidecar.py's AUTH_SECRET).

A subscription a push service reports gone (HTTP 404/410) is dropped
automatically — that's the browser having unsubscribed or cleared site data,
not a transient failure worth retrying.

VAPID_SUBJECT (env or .grill_env) is the contact the push service can show
or use to reach the sender; a mailto: URI is conventional. It is not a
secret and does not need to be a real deliverable inbox.
"""

from __future__ import annotations

import asyncio
import json
import logging
import os
import time
import uuid
from pathlib import Path
from typing import Any

from cryptography.hazmat.primitives import serialization
from py_vapid import Vapid01 as Vapid
from py_vapid.utils import b64urlencode
from pywebpush import WebPushException, webpush

import pitboss_cloud

_LOGGER = logging.getLogger("alarms")

_DATA_DIR = pitboss_cloud.ENV_PATH.parent
ALARMS_PATH = Path(os.environ.get("ALARMS_PATH") or (_DATA_DIR / "alarms.json"))
SUBSCRIPTIONS_PATH = Path(
    os.environ.get("PUSH_SUBSCRIPTIONS_PATH") or (_DATA_DIR / "push_subscriptions.json"))
VAPID_KEY_PATH = Path(
    os.environ.get("VAPID_PRIVATE_KEY_PATH") or (_DATA_DIR / ".vapid_private_key.pem"))
# `or` (not a dict-default get): docker-compose passes VAPID_SUBJECT="" when
# .env leaves it unset, which os.environ.get(key, default) would take
# literally instead of falling back — see GRILL_BOARD_ID etc. in
# pitboss_cloud.setting() for the same reasoning.
VAPID_SUBJECT = os.environ.get("VAPID_SUBJECT") or "mailto:openpit32@localhost"

CHECK_SECONDS = 5.0  # how often the monitor loop re-checks alarms

# sensor key (as decoded by pytboss / served by GET /state) -> display label
SENSORS: dict[str, str] = {
    "grillTemp": "Grill Temp",
    "smokerActTemp": "Smoker Temp",
    "p1Temp": "Probe 1",
    "p2Temp": "Probe 2",
    "p3Temp": "Probe 3",
    "p4Temp": "Probe 4",
}
COMPARISONS = ("at_or_above", "at_or_below")


class AlarmError(Exception):
    """A bad alarm request; the message is safe to show to the user."""


def _load_json(path: Path, default: Any) -> Any:
    if not path.exists():
        return default
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (ValueError, OSError) as ex:
        _LOGGER.warning("could not read %s (%s) — starting fresh", path, ex)
        return default


def _save_json(path: Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2), encoding="utf-8")


def load_or_create_vapid() -> Vapid:
    """The VAPID key pair, generated once and persisted at VAPID_KEY_PATH."""
    if VAPID_KEY_PATH.exists():
        return Vapid.from_file(str(VAPID_KEY_PATH))
    vapid = Vapid()
    vapid.generate_keys()
    VAPID_KEY_PATH.parent.mkdir(parents=True, exist_ok=True)
    vapid.save_key(str(VAPID_KEY_PATH))
    return vapid


def public_key_b64url(vapid: Vapid) -> str:
    """The public key as the base64url uncompressed point the browser's
    PushManager.subscribe() wants for applicationServerKey."""
    raw = vapid.public_key.public_bytes(
        serialization.Encoding.X962, serialization.PublicFormat.UncompressedPoint)
    return b64urlencode(raw)


class AlarmStore:
    """In-memory alarms + push subscriptions, mirrored to disk on every change."""

    def __init__(self) -> None:
        self._alarms: dict[str, dict] = {
            a["id"]: a for a in _load_json(ALARMS_PATH, [])}
        self._subscriptions: dict[str, dict] = {
            s["endpoint"]: s for s in _load_json(SUBSCRIPTIONS_PATH, [])
            if s.get("endpoint")}
        self.vapid = load_or_create_vapid()
        self.vapid_public_key = public_key_b64url(self.vapid)

    # ---- push subscriptions ----

    def add_subscription(self, subscription: dict) -> None:
        endpoint = subscription.get("endpoint")
        keys = subscription.get("keys") or {}
        if not endpoint or not keys.get("p256dh") or not keys.get("auth"):
            raise AlarmError("subscription needs endpoint and keys.p256dh/auth")
        entry = {"endpoint": endpoint, "keys": keys}
        if self._subscriptions.get(endpoint) == entry:
            # No-op re-registration: GrillDetail.razor re-POSTs its current
            # subscription on every poll tick to self-heal a subscription the
            # sidecar dropped as dead while the browser's copy is still good
            # (see the dead-subscription log below) — skip the disk write
            # when nothing actually changed, or that would hit the SD card
            # every 5 seconds for the life of the page.
            return
        self._subscriptions[endpoint] = entry
        _save_json(SUBSCRIPTIONS_PATH, list(self._subscriptions.values()))

    def remove_subscription(self, endpoint: str) -> None:
        if self._subscriptions.pop(endpoint, None) is not None:
            _save_json(SUBSCRIPTIONS_PATH, list(self._subscriptions.values()))

    # ---- alarms ----

    def list_alarms(self) -> list[dict]:
        return sorted(self._alarms.values(), key=lambda a: a["created_at"])

    def add_temp_alarm(self, sensor: str, comparison: str, target: float,
                       label: str | None) -> dict:
        if sensor not in SENSORS:
            raise AlarmError(f"unknown sensor {sensor!r}")
        if comparison not in COMPARISONS:
            raise AlarmError(f"unknown comparison {comparison!r}")
        alarm = {
            "id": uuid.uuid4().hex,
            "kind": "temp",
            "label": (label or "").strip() or SENSORS[sensor],
            "sensor": sensor,
            "comparison": comparison,
            "target": float(target),
            "duration_seconds": None,
            "fires_at": None,
            "created_at": time.time(),
        }
        self._alarms[alarm["id"]] = alarm
        _save_json(ALARMS_PATH, list(self._alarms.values()))
        return alarm

    def add_timer_alarm(self, duration_seconds: int, label: str | None) -> dict:
        if duration_seconds <= 0:
            raise AlarmError("duration must be positive")
        now = time.time()
        alarm = {
            "id": uuid.uuid4().hex,
            "kind": "timer",
            "label": (label or "").strip() or "Timer",
            "sensor": None,
            "comparison": None,
            "target": None,
            "duration_seconds": int(duration_seconds),
            "fires_at": now + duration_seconds,
            "created_at": now,
        }
        self._alarms[alarm["id"]] = alarm
        _save_json(ALARMS_PATH, list(self._alarms.values()))
        return alarm

    def remove_alarm(self, alarm_id: str) -> bool:
        if self._alarms.pop(alarm_id, None) is not None:
            _save_json(ALARMS_PATH, list(self._alarms.values()))
            return True
        return False

    # ---- push delivery ----

    def _send_push(self, title: str, body: str, tag: str) -> None:
        if not self._subscriptions:
            _LOGGER.info("alarm fired (%s) but no push subscriptions registered", tag)
            return
        payload = json.dumps({"title": title, "body": body, "tag": tag})
        dead: list[str] = []
        for endpoint, sub in list(self._subscriptions.items()):
            try:
                webpush(
                    subscription_info=sub,
                    data=payload,
                    vapid_private_key=str(VAPID_KEY_PATH),
                    vapid_claims={"sub": VAPID_SUBJECT},
                )
            except WebPushException as ex:
                status = getattr(ex.response, "status_code", None)
                if status in (404, 410):
                    # The push service itself says this registration is gone
                    # (browser unsubscribed, cleared site data, or the OS
                    # revoked it) — not a bug here, but worth a WARNING (not
                    # INFO, which the default log level hides) since it
                    # silently ends alarm notifications for that device until
                    # GrillDetail.razor's periodic re-subscribe check notices
                    # the browser has no subscription and re-shows the button.
                    _LOGGER.warning(
                        "dropping a push subscription: the push service "
                        "reported it gone (HTTP %s) while sending %r", status, tag)
                    dead.append(endpoint)
                else:
                    _LOGGER.warning("push to a subscriber failed: %s", ex)
        for endpoint in dead:
            self._subscriptions.pop(endpoint, None)
        if dead:
            _save_json(SUBSCRIPTIONS_PATH, list(self._subscriptions.values()))

    # ---- monitor loop ----

    async def monitor(self, bridge) -> None:
        """Check active alarms every CHECK_SECONDS; fire and drop due ones.

        Runs for the life of the process, independent of bridge.configured —
        a timer alarm needs no grill connection, only the clock.
        """
        while True:
            await asyncio.sleep(CHECK_SECONDS)
            try:
                self._check_once(bridge)
            except asyncio.CancelledError:
                raise
            except Exception:  # noqa: BLE001 - never let this loop die silently
                _LOGGER.exception("alarm check failed")

    def _check_once(self, bridge) -> None:
        now = time.time()
        state = bridge.state or {}
        fired: list[str] = []
        for alarm in list(self._alarms.values()):
            if alarm["kind"] == "timer":
                if now >= alarm["fires_at"]:
                    self._send_push(
                        "Timer done", f"{alarm['label']} finished.", alarm["id"])
                    fired.append(alarm["id"])
                continue
            value = state.get(alarm["sensor"])
            if value is None:
                continue
            target = alarm["target"]
            hit = (value >= target if alarm["comparison"] == "at_or_above"
                   else value <= target)
            if hit:
                self._send_push(
                    "Temperature alarm",
                    f"{alarm['label']} reached {value:.0f}° "
                    f"(target {target:.0f}°).",
                    alarm["id"])
                fired.append(alarm["id"])
        if fired:
            for alarm_id in fired:
                self._alarms.pop(alarm_id, None)
            _save_json(ALARMS_PATH, list(self._alarms.values()))
