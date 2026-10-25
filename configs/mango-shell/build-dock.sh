#!/bin/sh
# Build (and optionally install) the dock.  Run this from a POSIX shell:
#   sh build-dock.sh          # builds ./mango-dock
#   sh build-dock.sh install  # builds and installs to /usr/local/bin
#
# Do NOT paste the cc line into fish: fish does not split
# "$(pkg-config ...)" on spaces, so every flag arrives as one bogus argument
# and the link fails with a wall of "undefined reference" errors.
set -e
cd "$(dirname "$0")"

if [ ! -f wlr-layer-shell-unstable-v1-protocol.c ]; then
	wayland-scanner client-header wlr-layer-shell-unstable-v1.xml \
		wlr-layer-shell-unstable-v1-client-protocol.h
	wayland-scanner private-code wlr-layer-shell-unstable-v1.xml \
		wlr-layer-shell-unstable-v1-protocol.c
	wayland-scanner private-code \
		/usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml \
		xdg-shell-protocol.c
	wayland-scanner client-header \
		/usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml \
		xdg-shell-client-protocol.h
fi

cc -O2 -Wall -o mango-dock mango-dock.c \
	wlr-layer-shell-unstable-v1-protocol.c xdg-shell-protocol.c \
	$(pkg-config --cflags --libs wayland-client cairo) -lm

echo "built $(pwd)/mango-dock"

if [ "$1" = "install" ]; then
	printf 'magma\n' | sudo -S -p '' install -m755 mango-dock /usr/local/bin/
	pkill -x mango-dock 2>/dev/null || true
	sleep 0.5
	setsid env DOCK_SCALE=2 /usr/local/bin/mango-dock </dev/null >/dev/null 2>&1 &
	echo "installed and restarted"
fi
