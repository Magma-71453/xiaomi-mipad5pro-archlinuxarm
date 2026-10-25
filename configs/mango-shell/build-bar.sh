#!/bin/sh
# Build (and optionally install) the status bar.
#   sh build-bar.sh          # builds ./mango-bar
#   sh build-bar.sh install  # builds and installs to /usr/local/bin
set -e
cd "$(dirname "$0")"

if [ ! -f wlr-layer-shell-unstable-v1-protocol.c ]; then
	wayland-scanner client-header wlr-layer-shell-unstable-v1.xml \
		wlr-layer-shell-unstable-v1-client-protocol.h
	wayland-scanner private-code wlr-layer-shell-unstable-v1.xml \
		wlr-layer-shell-unstable-v1-protocol.c
fi
if [ ! -f xdg-shell-protocol.c ]; then
	wayland-scanner private-code \
		/usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml xdg-shell-protocol.c
	wayland-scanner client-header \
		/usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml xdg-shell-client-protocol.h
fi

# libsystemd provides sd-bus, used for the StatusNotifierWatcher/Item host
cc -O2 -Wall -Wextra -o mango-bar mango-bar.c \
	wlr-layer-shell-unstable-v1-protocol.c xdg-shell-protocol.c \
	$(pkg-config --cflags --libs wayland-client cairo libsystemd) -lm

echo "built $(pwd)/mango-bar"

if [ "$1" = "install" ]; then
	printf 'magma\n' | sudo -S -p '' install -m755 mango-bar /usr/local/bin/
	pkill -x mango-bar 2>/dev/null || true
	sleep 0.5
	setsid /usr/local/bin/mango-bar </dev/null >/tmp/mango-bar.log 2>&1 &
	echo "installed and restarted"
fi
