#!/bin/sh
#checks an installed waytator package brought everything it needs: no missing libraries, wl-copy and tesseract
#from its dependencies, and a binary that starts. waytator needs a display even for --help, so it runs on GTK's
#Broadway backend, which needs no compositor. gtk4-broadwayd has to be installed already
set -eu

missing=$(ldd /usr/bin/waytator | grep 'not found' || true)
if [ -n "$missing" ]; then
  echo "missing libraries:"
  echo "$missing"
  exit 1
fi

command -v wl-copy
command -v tesseract

XDG_RUNTIME_DIR=$(mktemp -d)
export XDG_RUNTIME_DIR
gtk4-broadwayd :5 &
broadway=$!
trap 'kill $broadway' EXIT

#display :5 listens on broadway6.socket
tries=0
while [ ! -S "$XDG_RUNTIME_DIR/broadway6.socket" ]; do
  tries=$((tries + 1))
  if [ "$tries" -gt 50 ]; then
    echo "gtk4-broadwayd didn't start"
    exit 1
  fi
  sleep 0.2
done

usage=$(GDK_BACKEND=broadway BROADWAY_DISPLAY=:5 GDK_DEBUG=no-portals waytator --help)
echo "$usage"
echo "$usage" | grep -q '^Usage: waytator '
