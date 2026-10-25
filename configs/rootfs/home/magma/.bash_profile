# ~/.bash_profile for user magma on the xiaomi-elish tablet.
#
# tty1 runs agetty --autologin magma, so the login shell there becomes the
# MangoWM session.  XDG_RUNTIME_DIR lives under $HOME because logind does not
# create /run/user/<uid> in this image (no working session/dbus yet); pointing
# it at a non-existent directory makes wl_display_add_socket_auto() fail and
# mango exits with rc=1.
if [ -z "${MANGO_RUNNING:-}" ] && [ "$(tty)" = "/dev/tty1" ]; then
    export MANGO_RUNNING=1
    # Prefer the systemd-managed runtime dir: PipeWire/WirePlumber keep their
    # sockets in /run/user/<uid>, and applications (e.g. Firefox) only find
    # them when XDG_RUNTIME_DIR points there as well.
    if [ -d "/run/user/$(id -u)" ]; then rt="/run/user/$(id -u)"; else rt="$HOME/.xdg"; fi
    mkdir -p "$rt"; chmod 700 "$rt"
    export XDG_RUNTIME_DIR="$rt"
    export XDG_SESSION_TYPE=wayland XDG_CURRENT_DESKTOP=mango XDG_SESSION_DESKTOP=mango
    # make toolkits use native Wayland: X11 (XWayland) clients are scaled by the
    # compositor on this HiDPI panel, which makes them look blurry.
    export MOZ_ENABLE_WAYLAND=1 GDK_BACKEND=wayland QT_QPA_PLATFORM=wayland
    export SDL_VIDEODRIVER=wayland
    { echo "=== mango start $(date -Is) rt=$rt ==="; ls -ld "$rt" 2>&1; ls -l /dev/dri 2>&1; id; } > "$HOME/mango.log" 2>&1
    # The panel is 1600x2560 at ~275 dpi; mango auto-picks scale 2, which makes
    # every client that cannot do HiDPI (most XWayland apps) look blurry.
    # Pin scale 1 so everything is rendered 1:1.
    /usr/bin/mango -s "wlr-randr --output DSI-1 --scale 1.4" >> "$HOME/mango.log" 2>&1
    echo "=== mango exited rc=$? ===" >> "$HOME/mango.log"
fi

# plain shell on other ttys / ssh sessions
[ -f /etc/bash.bashrc ] && . /etc/bash.bashrc 2>/dev/null || true
