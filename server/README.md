# RIG Control Server

This server adds an independent control channel to RIG Puppy while leaving the vendor cloud connection unchanged.

## Start

```bash
cp server/.env.example .env
docker compose up --build -d
```

Open `http://localhost:8000`. In production, set `RIG_PUBLIC_URL` to the public HTTPS origin and proxy both `/ws/device` and the HTTP routes through Caddy or Nginx.

## Provision a Puppy

1. Add the device in the web console using its Wi-Fi MAC address as the device ID.
2. Record the generated per-device token.
3. Set `CUSTOM_CONTROL_SERVER_URL` and `CUSTOM_CONTROL_TOKEN` in `idf.py menuconfig`, or write the same values to the `custom_ctl` NVS namespace as `url` and `token`.
4. Flash the complete image and partition table once. Later firmware releases can use OTA.

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
