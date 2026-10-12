#!/usr/bin/env python3
"""Prunes old rows from the grill event log (scripts/login_service.py's
POST /log-event, table "grill_events" — see that script's EVENT_LOG_SCHEMA,
kept in sync with this file's own CREATE TABLE below rather than imported,
since this is meant to run standalone via `docker exec`/cron, not as part
of the long-running login service process).

2026-10-05, the "what's resetting the setpoint to 130?" investigation
(docker/README.md's "Grill event log" section has the full picture): the
event log only ever records things worth looking at in the first place
(every POST /command, BLE connect/disconnect, an unexplained setpoint
change) — never routine polling noise — so there's no "keep only the
interesting ones" filtering left to do here. What this script does is
bound how far back even that smaller set is kept, so the database doesn't
grow forever on a Pi's small SD card.

Configuration, all from the environment (matching login_service.py's style):
  EVENT_LOG_DB_PATH — same file login_service.py writes to (default:
    grill_events.db next to this script, i.e. /data/grill_events.db inside
    the auth container — see docker/auth.Dockerfile's VOLUME).
  EVENT_LOG_RETENTION_DAYS — rows received more than this many days ago are
    deleted (default: 30). Pass --days to override for one run without
    touching the environment.

Usage — intended to run on a schedule from OUTSIDE the container, since
nothing in this Docker stack can schedule anything on the host itself
(same limitation docker/README.md's build-cache cron note already
documents for `docker builder prune`):

    # crontab -e (as the user docker compose runs under), once daily
    0 3 * * * cd ~/openpit32 && docker compose exec -T auth \\
        python3 cleanup_grill_events.py >>~/openpit32-cleanup.log 2>&1

`docker compose exec` (rather than a hardcoded `docker exec <container
name>`) is what's robust to whatever Compose actually named the container
instantiating this project with.
"""

from __future__ import annotations

import argparse
import os
import sqlite3
import time
from pathlib import Path

DEFAULT_DB_PATH = Path(__file__).resolve().parent / "grill_events.db"
DEFAULT_RETENTION_DAYS = 30


def prune(db_path: Path, retention_days: int, dry_run: bool) -> int:
    if not db_path.exists():
        print(f"{db_path} does not exist yet — nothing to prune")
        return 0
    cutoff = int(time.time()) - retention_days * 86400
    conn = sqlite3.connect(str(db_path))
    try:
        # CREATE TABLE IF NOT EXISTS so a cron run started before the table
        # has ever been written to (e.g. EVENT_LOG_TOKEN was only just set)
        # doesn't fail outright — matches login_service.py's own schema.
        conn.execute(
            """
            CREATE TABLE IF NOT EXISTS grill_events (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                received_at INTEGER NOT NULL,
                ts INTEGER,
                event TEXT NOT NULL,
                action TEXT,
                value REAL,
                old_value REAL,
                ok INTEGER,
                error TEXT,
                source_ip TEXT,
                detail TEXT
            )
            """
        )
        if dry_run:
            (count,) = conn.execute(
                "SELECT COUNT(*) FROM grill_events WHERE received_at < ?", (cutoff,)
            ).fetchone()
            print(f"would delete {count} row(s) older than {retention_days} days")
            return count
        cur = conn.execute("DELETE FROM grill_events WHERE received_at < ?", (cutoff,))
        conn.commit()
        deleted = cur.rowcount
        print(f"deleted {deleted} row(s) older than {retention_days} days")
        return deleted
    finally:
        conn.close()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--days", type=int, default=None,
                        help="override EVENT_LOG_RETENTION_DAYS for this run")
    parser.add_argument("--dry-run", action="store_true",
                        help="report how many rows would be deleted without deleting them")
    args = parser.parse_args()

    db_path = Path(os.environ.get("EVENT_LOG_DB_PATH", str(DEFAULT_DB_PATH)))
    retention_days = args.days if args.days is not None else int(
        os.environ.get("EVENT_LOG_RETENTION_DAYS", DEFAULT_RETENTION_DAYS))
    prune(db_path, retention_days, args.dry_run)


if __name__ == "__main__":
    main()
