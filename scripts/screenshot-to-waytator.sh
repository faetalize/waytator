#!/usr/bin/env bash

set -euo pipefail

timeout_seconds="${WAYTATOR_SCREENSHOT_TIMEOUT:-15}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/build}"

require_command() {
  if ! command -v "$1" >/dev/null 2>&1; then
    printf 'missing required command: %s\n' "$1" >&2
    exit 1
  fi
}

cleanup() {
  if [[ -n "${STREAM_PID:-}" ]]; then
    kill "$STREAM_PID" >/dev/null 2>&1 || true
    wait "$STREAM_PID" >/dev/null 2>&1 || true
  fi
}

resolve_waytator_bin() {
  if [[ -n "${WAYTATOR_BIN:-}" ]]; then
    if [[ -x "${WAYTATOR_BIN}" ]]; then
      printf '%s\n' "${WAYTATOR_BIN}"
      return 0
    fi

    printf 'WAYTATOR_BIN is not executable: %s\n' "${WAYTATOR_BIN}" >&2
    exit 1
  fi

  if [[ -x "${BUILD_DIR}/src/waytator" ]]; then
    printf '%s\n' "${BUILD_DIR}/src/waytator"
    return 0
  fi

  if command -v waytator >/dev/null 2>&1; then
    command -v waytator
    return 0
  fi

  printf 'could not find waytator. Build it in %s, install it on PATH, or set WAYTATOR_BIN.\n' "${BUILD_DIR}" >&2
  exit 1
}

require_command niri
require_command jq
WAYTATOR_BIN="$(resolve_waytator_bin)"

tmp_pipe=$(mktemp -u)
mkfifo "$tmp_pipe"
trap 'rm -f "$tmp_pipe"; cleanup' EXIT

niri msg --json event-stream > "$tmp_pipe" &
STREAM_PID=$!

niri msg action screenshot >/dev/null

deadline=$((SECONDS + timeout_seconds))
screenshot_path=""

while (( SECONDS < deadline )); do
  if ! read -t 1 line < "$tmp_pipe"; then
    continue
  fi

  screenshot_path="$(echo "$line" | jq -r 'select(.ScreenshotCaptured != null) | .ScreenshotCaptured.path' 2>/dev/null || true)"

  if [[ -n "$screenshot_path" ]]; then
    break
  fi
done

if [[ -z "$screenshot_path" ]]; then
  printf 'timed out waiting for niri to report the screenshot path\n' >&2
  exit 1
fi

setsid -f "$WAYTATOR_BIN" "$screenshot_path" >/dev/null 2>&1
