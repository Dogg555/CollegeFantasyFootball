FROM postgres:16-bookworm

ENV PYTHONDONTWRITEBYTECODE=1 \
    PYTHONUNBUFFERED=1 \
    PIP_DISABLE_PIP_VERSION_CHECK=1

RUN apt-get update \
 && apt-get install -y --no-install-recommends ca-certificates python3 python3-venv \
 && python3 -m venv /opt/venv \
 && /opt/venv/bin/pip install --no-cache-dir boto3==1.40.1 \
 && groupadd --system --gid 10002 backup \
 && useradd --system --uid 10002 --gid backup --home-dir /home/backup --create-home --shell /usr/sbin/nologin backup \
 && rm -rf /var/lib/apt/lists/*

COPY --chown=backup:backup scripts/offsite_backup.py /opt/cff/offsite_backup.py

USER 10002:10002
ENTRYPOINT ["/opt/venv/bin/python", "/opt/cff/offsite_backup.py"]
