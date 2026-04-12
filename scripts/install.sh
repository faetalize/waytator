#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/build}"
PREFIX="${PREFIX:-${HOME}/.local}"
BINDIR="${BINDIR:-${PREFIX}/bin}"

path_line="export PATH=\"${BINDIR}:\$PATH\""

pick_shell_rc() {
  case "$(basename "${SHELL:-}")" in
    bash)
      if [ -f "${HOME}/.bashrc" ]; then
        printf '%s\n' "${HOME}/.bashrc"
      else
        printf '%s\n' "${HOME}/.profile"
      fi
      ;;
    zsh)
      printf '%s\n' "${HOME}/.zshrc"
      ;;
    sh|dash)
      printf '%s\n' "${HOME}/.profile"
      ;;
    *)
      return 1
      ;;
  esac
}

maybe_add_bindir_to_path() {
  local rc_file
  local reply

  case ":${PATH}:" in
    *":${BINDIR}:"*)
      return
      ;;
  esac

  if [ ! -t 0 ]; then
    printf 'Warning: %s is not on PATH\n' "${BINDIR}"
    return
  fi

  if ! rc_file="$(pick_shell_rc)"; then
    printf 'Warning: %s is not on PATH\n' "${BINDIR}"
    printf 'Add this to your shell config manually:\n%s\n' "${path_line}"
    return
  fi

  if [ -f "${rc_file}" ] && grep -Fqx "${path_line}" "${rc_file}"; then
    return
  fi

  printf '%s is not on PATH. Add it to %s? [y/N] ' "${BINDIR}" "${rc_file}"
  read -r reply

  case "${reply}" in
    y|Y|yes|YES)
      mkdir -p "$(dirname "${rc_file}")"
      if [ -f "${rc_file}" ] && [ -s "${rc_file}" ]; then
        printf '\n%s\n' "${path_line}" >> "${rc_file}"
      else
        printf '%s\n' "${path_line}" >> "${rc_file}"
      fi
      printf 'Added %s to %s\n' "${BINDIR}" "${rc_file}"
      printf 'Open a new shell or run: %s\n' "${path_line}"
      ;;
    *)
      printf 'Skipping PATH update. Add this manually if needed:\n%s\n' "${path_line}"
      ;;
  esac
}

if [ -d "${BUILD_DIR}" ]; then
  meson setup "${BUILD_DIR}" "${ROOT_DIR}" --buildtype=release --prefix "${PREFIX}" --reconfigure
else
  meson setup "${BUILD_DIR}" "${ROOT_DIR}" --buildtype=release --prefix "${PREFIX}"
fi

meson compile -C "${BUILD_DIR}"
install -Dm755 "${BUILD_DIR}/src/waytator" "${BINDIR}/waytator"

printf 'Installed waytator to %s\n' "${BINDIR}/waytator"
maybe_add_bindir_to_path
