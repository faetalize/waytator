#!/usr/bin/env bash

set -euo pipefail

timeout_seconds="${WAYTATOR_SCREENSHOT_TIMEOUT:-15}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/build}"
clipboard_mime="${WAYTATOR_SCREENSHOT_CLIPBOARD_MIME:-image/png}"
clipboard_name="${WAYTATOR_SCREENSHOT_NAME:-Screenshot from $(date '+%Y-%m-%d %H-%M-%S').png}"

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
  if command -v waytator >/dev/null 2>&1; then
    command -v waytator
    return 0
  fi

  if [[ -x "${BUILD_DIR}/src/waytator" ]]; then
    printf '%s\n' "${BUILD_DIR}/src/waytator"
    return 0
  fi

  printf 'could not find waytator. Install it on PATH or build it in %s.\n' "${BUILD_DIR}" >&2
  exit 1
}

clipboard_image_mime() {
  local types

  types="$(wl-paste --list-types 2>/dev/null || true)"

  if [[ $'\n'"$types"$'\n' == *$'\n'"$clipboard_mime"$'\n'* ]]; then
    printf '%s\n' "$clipboard_mime"
    return 0
  fi

  while IFS= read -r mime; do
    if [[ "$mime" == image/* ]]; then
      printf '%s\n' "$mime"
      return 0
    fi
  done <<< "$types"

  return 1
}

clipboard_image_fingerprint() {
  local mime="$1"

  wl-paste --type "$mime" 2>/dev/null | sha256sum | {
    read -r hash _
    printf '%s:%s\n' "$mime" "$hash"
  }
}

open_clipboard_image() {
  local mime="$1"

  setsid -f bash -c 'wl-paste --type "$1" | "$2" --stdin --name "$3"' bash "$mime" "$waytator_bin" "$clipboard_name" >/dev/null 2>&1
}

require_command niri
require_command jq
require_command wl-paste
require_command sha256sum
waytator_bin="$(resolve_waytator_bin)"
initial_clipboard_fingerprint=""
initial_clipboard_mime="$(clipboard_image_mime || true)"

if [[ -n "$initial_clipboard_mime" ]]; then
  initial_clipboard_fingerprint="$(clipboard_image_fingerprint "$initial_clipboard_mime" || true)"
fi

tmp_pipe=$(mktemp -u)
mkfifo "$tmp_pipe"
trap 'rm -f "$tmp_pipe"; cleanup' EXIT

niri msg --json event-stream > "$tmp_pipe" &
STREAM_PID=$!
exec 3< "$tmp_pipe"

niri msg action screenshot >/dev/null

deadline=$((SECONDS + timeout_seconds))
screenshot_path=""
clipboard_fingerprint=""
clipboard_mime_after=""

while (( SECONDS < deadline )); do
  if ! read -t 1 -u 3 line; then
    clipboard_mime_after="$(clipboard_image_mime || true)"
    if [[ -n "$clipboard_mime_after" ]]; then
      clipboard_fingerprint="$(clipboard_image_fingerprint "$clipboard_mime_after" || true)"

      if [[ -n "$clipboard_fingerprint" && "$clipboard_fingerprint" != "$initial_clipboard_fingerprint" ]]; then
        break
      fi
    fi

    continue
  fi

  screenshot_path="$(printf '%s\n' "$line" | jq -r '.ScreenshotCaptured.path? // empty' 2>/dev/null || true)"

  if [[ -n "$screenshot_path" ]]; then
    break
  fi

  clipboard_mime_after="$(clipboard_image_mime || true)"
  if [[ -n "$clipboard_mime_after" ]]; then
    clipboard_fingerprint="$(clipboard_image_fingerprint "$clipboard_mime_after" || true)"

    if [[ -n "$clipboard_fingerprint" && "$clipboard_fingerprint" != "$initial_clipboard_fingerprint" ]]; then
      break
    fi
  fi
done

if [[ -n "$screenshot_path" ]]; then
  setsid -f "$waytator_bin" "$screenshot_path" >/dev/null 2>&1
elif [[ -n "$clipboard_fingerprint" && "$clipboard_fingerprint" != "$initial_clipboard_fingerprint" ]]; then
  open_clipboard_image "$clipboard_mime_after"
else
  printf 'timed out waiting for niri to report a screenshot path or image clipboard update\n' >&2
  exit 1
fi
