# Grill sidecar (scripts/grill_sidecar.py): talks Bluetooth LE to the grill
# through an ESPHome bluetooth_proxy ESP32, exposes it as local HTTP.
# Build from the repo root: docker build -f docker/sidecar.Dockerfile .
FROM python:3.12-slim

WORKDIR /app
COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt
COPY scripts/*.py ./

# Reachable from other hosts on the LAN (the web container's browser client
# calls back into this port); GRILL_ENV_PATH keeps a password fetched via
# /setup on the mounted volume so it survives a container restart.
ENV GRILL_SIDECAR_HOST=0.0.0.0 \
    GRILL_ENV_PATH=/data/.grill_env \
    PYTHONUNBUFFERED=1

VOLUME ["/data"]
EXPOSE 8091

CMD ["python", "grill_sidecar.py"]
