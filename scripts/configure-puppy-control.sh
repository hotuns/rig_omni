#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage:
  scripts/configure-puppy-control.sh --url WS_URL --token DEVICE_TOKEN --port SERIAL_PORT

Example:
  scripts/configure-puppy-control.sh \
    --url ws://YOUR_PUBLIC_IP:8000/ws/device \
    --token YOUR_DEVICE_TOKEN \
    --port /dev/cu.usbmodem1101
EOF
}

server_url=""
device_token=""
serial_port=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --url)
      server_url="${2:-}"
      shift 2
      ;;
    --token)
      device_token="${2:-}"
      shift 2
      ;;
    --port)
      serial_port="${2:-}"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown argument: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

if [[ ! "$server_url" =~ ^wss?:// ]] || [[ "$server_url" == *['"'\$'\n'\$'\r']* ]]; then
  echo "--url must be a valid ws:// or wss:// address." >&2
  exit 2
fi

if [[ -z "$device_token" ]] || [[ "$device_token" == *['"'\$'\n'\$'\r'[:space:]]* ]]; then
  echo "--token must be non-empty and contain no whitespace or quotes." >&2
  exit 2
fi

if [[ -z "$serial_port" ]]; then
  echo "--port is required." >&2
  exit 2
fi

if command -v idf.py >/dev/null 2>&1; then
  idf_py="$(command -v idf.py)"
elif [[ -n "${IDF_PATH:-}" && -x "${IDF_PATH}/tools/idf.py" ]]; then
  idf_py="${IDF_PATH}/tools/idf.py"
else
  echo "idf.py was not found. Activate the ESP-IDF environment first." >&2
  exit 1
fi

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
work_dir="$(mktemp -d "${TMPDIR:-/tmp}/rig-puppy-flash.XXXXXX")"
trap 'rm -rf "$work_dir"' EXIT

defaults_file="$work_dir/custom-control.defaults"
sdkconfig_file="$work_dir/sdkconfig"
build_dir="$work_dir/build"

cat >"$defaults_file" <<EOF
CONFIG_CUSTOM_CONTROL_ENABLED=y
CONFIG_CUSTOM_CONTROL_SERVER_URL="$server_url"
CONFIG_CUSTOM_CONTROL_TOKEN="$device_token"
EOF
chmod 600 "$defaults_file"

echo "Building firmware for ESP32-S3..."
cd "$repo_root"
"$idf_py" \
  -B "$build_dir" \
  -D "SDKCONFIG=$sdkconfig_file" \
  -D "SDKCONFIG_DEFAULTS=$repo_root/sdkconfig.defaults;$repo_root/sdkconfig.defaults.esp32s3;$defaults_file" \
  set-target esp32s3 build

echo "Flashing firmware to $serial_port..."
"$idf_py" -B "$build_dir" -p "$serial_port" flash
echo "Firmware flashed successfully."
