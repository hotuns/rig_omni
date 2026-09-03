# RIG Control Server

This server adds an independent control channel to RIG Puppy while leaving the vendor cloud connection unchanged.

## Start

Run from source:

```bash
cp server/.env.example .env
docker compose up --build -d
```

Or pull the published image:

```bash
cp .env.docker.example .env
docker compose -f docker-compose.hub.yml up -d
```

Open `http://localhost:8000`. In production, set `RIG_PUBLIC_URL` to the public HTTPS origin and proxy both `/ws/device` and the HTTP routes through Caddy or Nginx.

## Provision a Puppy

1. Add the device in the web console using its Wi-Fi MAC address as the device ID.
2. Record the generated per-device token.
3. Activate the ESP-IDF environment, then build and flash without changing the repository's existing `sdkconfig`:

```bash
scripts/configure-puppy-control.sh \
  --url ws://YOUR_PUBLIC_IP:8000/ws/device \
  --token YOUR_DEVICE_TOKEN \
  --port /dev/cu.usbmodem1101
```

Use `wss://YOUR_DOMAIN/ws/device` when the server is behind HTTPS. The first flash writes the complete image and partition table; later firmware releases can use OTA.

The device connects with these headers:

```text
Authorization: Bearer <device-token>
X-Device-Id: <device-id>
```

## TTS

Set `RIG_TTS_API_KEY` to enable the OpenAI-compatible `/audio/speech` adapter. Uploaded and generated audio is converted by FFmpeg to 16 kHz mono OGG Opus before it is exposed to the device.

## Workflow format

```json
{
  "id": "sedentary-reminder",
  "name": "Sedentary reminder",
  "steps": [
    {"type": "audio", "url": "https://robot.example.com/media/reminder.ogg"},
    {"type": "tool", "name": "self.dog.Wave", "arguments": {}},
    {"type": "delay", "ms": 500},
    {"type": "tool", "name": "self.dog.Reset", "arguments": {}}
  ]
}
```
