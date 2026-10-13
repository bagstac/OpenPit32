#!/bin/sh
# Grill event log retention — what the crontab on srv-mc-01 actually calls.
#
# The crontab entry runs this every hour at :00, and it does the prune only in
# the hour that is 05:00 in America/New_York. That indirection exists because
# srv-mc-01 runs Etc/UTC and Ubuntu 24.04's cron (vixie 3.0pl1) supports no
# CRON_TZ: any fixed UTC hour is right for half the year and an hour out for
# the other half ("0 9 * * *" is 05:00 EDT but 04:00 EST). Gating on the
# Eastern wall clock keeps the run at 05:00 local straight through the DST
# change with no twice-a-year cron edit — and it still works if that box is
# ever moved to local time, which devices/srv-mc-01.md lists as an open
# question.
#
# Deployed as (crontab -l as brian, the user docker compose runs under):
#
#   0 * * * * /home/brian/openpit32/scripts/grill-event-log-prune.sh >>/home/brian/openpit32-cleanup.log 2>&1
#
# Invoke with `sh` rather than relying on the exec bit: this repo is checked
# out on Windows too, where git does not carry one.
#
# EVENT_LOG_PRUNE_HOUR overrides which Eastern hour runs the prune (default
# 05) — the same env-override style as cleanup_grill_events.py's
# EVENT_LOG_RETENTION_DAYS, and what makes the positive path testable without
# waiting for 05:00. Run it with the CURRENT Eastern hour to exercise the real
# prune immediately:
#
#   EVENT_LOG_PRUNE_HOUR="$(TZ=America/New_York date +%H)" \
#       sh scripts/grill-event-log-prune.sh
set -eu

hour="$(printf '%02d' "${EVENT_LOG_PRUNE_HOUR:-5}")"
[ "$(TZ=America/New_York date +%H)" = "$hour" ] || exit 0

cd /home/brian/openpit32
exec /usr/bin/docker compose exec -T auth python3 cleanup_grill_events.py
