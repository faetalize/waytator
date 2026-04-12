#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/build}"
PREFIX="${PREFIX:-${HOME}/.local}"
BINDIR="${BINDIR:-${PREFIX}/bin}"

if [ -d "${BUILD_DIR}" ]; then
  meson setup "${BUILD_DIR}" --buildtype=release --prefix "${PREFIX}" --reconfigure
else
  meson setup "${BUILD_DIR}" --buildtype=release --prefix "${PREFIX}"
fi

meson compile -C "${BUILD_DIR}"
install -Dm755 "${BUILD_DIR}/src/waytator" "${BINDIR}/waytator"

printf 'Installed waytator to %s\n' "${BINDIR}/waytator"

case ":${PATH}:" in
  *":${BINDIR}:"*) ;;
  *)
    printf 'Warning: %s is not on PATH\n' "${BINDIR}"
    ;;
esac
