# Login-only auth service (scripts/login_service.py) — everything else
# nginx.conf.template proxies goes straight to the grill's ESP32 now
# (Phase 8, docs/ESP32_FIRMWARE_PLAN.md — this replaces the old
# docker/sidecar.Dockerfile's Python BLE bridge entirely). Build from the
# repo root: docker build -f docker/auth.Dockerfile .
FROM python:3.12-slim

WORKDIR /app
COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt
COPY scripts/login_service.py ./

# Reachable from other hosts on the LAN (the web container's browser client
# calls back into this port via nginx's auth_request); AUTH_SECRET_PATH
# keeps a generated session-signing secret on the mounted volume so it
# survives a container restart — the same /data/.auth_secret path the old
# sidecar container used for the same file, so upgrading from that image
# doesn't invalidate anyone's existing session.
ENV LOGIN_SERVICE_HOST=0.0.0.0 \
    AUTH_SECRET_PATH=/data/.auth_secret \
    PYTHONUNBUFFERED=1

VOLUME ["/data"]
EXPOSE 8091

CMD ["python", "login_service.py"]
