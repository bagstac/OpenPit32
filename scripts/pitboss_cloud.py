#!/usr/bin/env python3
"""One-time fetch of the grill's Bluetooth password from the Pit Boss cloud.

The only thing this project needs from the Pit Boss cloud is the per-grill
password that authenticates RPC commands (the same password the official app
uses). It is stored on the account, so this module logs in once, reads
GET /customer-grills, and saves the chosen grill's board_id + password to
scripts/.grill_env. Nothing else talks to the cloud; the grill itself is
reached over Bluetooth via the ESP32 proxy.

Used two ways:
  - by the sidecar's POST /setup (the web UI's "Pit Boss account" dialog);
  - as a CLI, with credentials from the environment only:
      $env:PITBOSS_EMAIL="you@example.com"
      $env:PITBOSS_PASSWORD="your-account-password"
      .\\.venv312\\Scripts\\python.exe scripts\\pitboss_cloud.py

Account credentials are used for the login call and dropped; the account
token is never stored. Nothing secret is printed or logged.

Endpoints and headers: docs/PROTOCOL.md.
"""

from __future__ import annotations

import json
import os
import sys
import urllib.error
import urllib.request
from pathlib import Path

BASE = "https://api-prod.dansonscorp.com/api/v1"
ENV_PATH = Path(__file__).resolve().parent / ".grill_env"
BOARD_PREFIX = "PBV2"  # the board generation this project has been tested on


class CloudError(Exception):
    """A cloud call failed; the message is safe to show to the user."""


class GrillChoiceNeeded(CloudError):
    """The account has several grills; `grills` lists them (no passwords)."""

    def __init__(self, grills: list[dict]) -> None:
        super().__init__("several grills on the account — choose one")
        self.grills = grills


def _headers(country: str, token: str | None = None) -> dict:
    h = {
        "Accept": "application/json",
        "Content-Type": "application/json",
        "X-Localization": "en",
        "x-country": country,
        "x-store": "PB",
        "User-Agent": "PitBossAlarms/1.0 (grill-password-fetch)",
    }
    if token:
        h["Authorization"] = "Bearer " + token
    return h


def _call(method: str, path: str, body=None, *, country: str,
          token: str | None = None, timeout: float = 25):
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(BASE + path, data=data,
                                 headers=_headers(country, token), method=method)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return resp.status, json.loads(resp.read().decode("utf-8"))
    except urllib.error.HTTPError as e:
        raw = e.read().decode("utf-8", "replace")
        try:
            return e.code, json.loads(raw)
        except json.JSONDecodeError:
            return e.code, raw
    except (urllib.error.URLError, TimeoutError) as e:
        raise CloudError(f"could not reach the Pit Boss API: {e}") from None


def _summarize(rows: list[dict]) -> list[dict]:
    """Grill rows as [{grill_id, board_id, nickname, has_password}] — no passwords."""
    return [
        {
            "grill_id": g.get("id"),
            "board_id": g.get("board_id"),
            "nickname": g.get("grill_nickname"),
            "has_password": bool(g.get("password")),
        }
        for g in rows
    ]


def list_grills(email: str, password: str, country: str = "US") -> list[dict]:
    """Log in and return the account's grills, passwords withheld."""
    return _summarize(_fetch_rows(email, password, country))


def _fetch_rows(email: str, password: str, country: str) -> list[dict]:
    status, payload = _call("POST", "/login/app",
                            {"email": email, "password": password}, country=country)
    if status == 404:
        # The API answers 404 UNIDENTIFIED_CUSTOMER for wrong credentials.
        raise CloudError("login rejected — check the email and password "
                         "(and the account country, if not US)")
    if status != 200 or not isinstance(payload, dict):
        raise CloudError(f"login failed (HTTP {status})")
    token = (payload.get("data") or {}).get("token")
    if not token:
        raise CloudError("login succeeded but returned no token")

    status, payload = _call("GET", "/customer-grills", country=country, token=token)
    if status != 200 or not isinstance(payload, dict):
        raise CloudError(f"reading the account's grills failed (HTTP {status})")
    return (payload.get("data") or {}).get("customer_grills") or []


def fetch_and_save(email: str, password: str, *, country: str = "US",
                   grill_id: int | None = None, env_path: Path = ENV_PATH) -> dict:
    """Fetch the grill password and merge board_id + password into .grill_env.

    With several grills on the account, `grill_id` picks one; without it the
    only grill is used, or the only PBV2 board, else GrillChoiceNeeded is
    raised with the list. Returns {board_id, nickname} — never the password.
    """
    rows = _fetch_rows(email, password, country)
    if not rows:
        raise CloudError("no grills on this account — pair the grill in the "
                         "Pit Boss app first")
    if grill_id is not None:
        grill = next((g for g in rows if g.get("id") == grill_id), None)
        if grill is None:
            raise CloudError(f"grill id {grill_id} is not on this account")
    elif len(rows) == 1:
        grill = rows[0]
    else:
        pbv2 = [g for g in rows
                if (g.get("board_id") or "").startswith(BOARD_PREFIX)]
        if len(pbv2) != 1:
            raise GrillChoiceNeeded(_summarize(rows))
        grill = pbv2[0]

    board_id, grill_pw = grill.get("board_id"), grill.get("password")
    if not board_id or not grill_pw:
        raise CloudError("the grill record has no board_id/password yet — "
                         "finish pairing it in the Pit Boss app")

    save_env({"GRILL_BOARD_ID": board_id, "GRILL_PASSWORD": grill_pw}, env_path)
    return {"board_id": board_id, "nickname": grill.get("grill_nickname")}


def load_env(env_path: Path = ENV_PATH) -> dict:
    env = {}
    if env_path.exists():
        for line in env_path.read_text(encoding="utf-8").splitlines():
            line = line.strip()
            if line and not line.startswith("#") and "=" in line:
                key, _, value = line.partition("=")
                env[key.strip()] = value.strip()
    return env


def save_env(updates: dict, env_path: Path = ENV_PATH) -> None:
    """Merge keys into .grill_env, keeping the others (proxy host/key)."""
    env = load_env(env_path)
    env.update(updates)
    env_path.write_text(
        "".join(f"{k}={v}\n" for k, v in env.items()), encoding="utf-8")


def main() -> int:
    email = os.environ.get("PITBOSS_EMAIL")
    password = os.environ.get("PITBOSS_PASSWORD")
    if not email or not password:
        print("Set PITBOSS_EMAIL and PITBOSS_PASSWORD in the environment first "
              "(never on the command line).", file=sys.stderr)
        return 2
    try:
        info = fetch_and_save(email, password,
                              country=os.environ.get("PITBOSS_COUNTRY", "US"))
    except CloudError as ex:
        print(f"error: {ex}", file=sys.stderr)
        return 1
    print(f"Saved the grill password for {info['board_id']} "
          f"(nickname: {info['nickname']}) to {ENV_PATH}")
    print("Keep that file private; it is the key to your grill.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
