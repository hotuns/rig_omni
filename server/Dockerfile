FROM python:3.12-slim

RUN sed -i 's|deb.debian.org/debian|mirrors.aliyun.com/debian|g; s|deb.debian.org/debian-security|mirrors.aliyun.com/debian-security|g' /etc/apt/sources.list.d/debian.sources \
    && apt-get update && apt-get install -y --no-install-recommends ffmpeg \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /app
COPY requirements.txt .
RUN pip install --no-cache-dir --index-url https://mirrors.aliyun.com/pypi/simple -r requirements.txt
COPY app ./app
RUN mkdir -p /data/media
ENV RIG_DATABASE=/data/rig-control.db RIG_MEDIA_DIR=/data/media
EXPOSE 8000
CMD ["uvicorn", "app.main:app", "--host", "0.0.0.0", "--port", "8000"]
