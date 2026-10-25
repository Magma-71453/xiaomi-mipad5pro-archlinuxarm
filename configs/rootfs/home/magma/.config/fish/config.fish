# ~/.config/fish/config.fish — xiaomi-elish session bootstrap
#
# The tablet boots to tty1 with agetty --autologin magma; the login shell (fish)
# there becomes the MangoWM Wayland session.  XDG_RUNTIME_DIR must be the
# systemd-managed /run/user/<uid> directory, because PipeWire/WirePlumber keep
# their sockets there and applications (Firefox, ...) only find them when the
# variable points at the same place.

if status is-interactive
    # interactive-only customisation goes here
end

if status is-login
    if not set -q MANGO_RUNNING
        if test (tty) = /dev/tty1
            set -gx MANGO_RUNNING 1
            if test -d /run/user/(id -u)
                set -gx XDG_RUNTIME_DIR /run/user/(id -u)
            else
                set -gx XDG_RUNTIME_DIR $HOME/.xdg
            end
            mkdir -p $XDG_RUNTIME_DIR
            chmod 700 $XDG_RUNTIME_DIR
            set -gx XDG_SESSION_TYPE wayland
            set -gx XDG_CURRENT_DESKTOP mango
            set -gx XDG_SESSION_DESKTOP mango
            set -gx MOZ_ENABLE_WAYLAND 1
            set -gx GDK_BACKEND wayland
            set -gx QT_QPA_PLATFORM wayland
            set -gx SDL_VIDEODRIVER wayland
            # The RTC has no battery: give systemd-timesyncd a moment so that
            # applications do not start with a 1970 clock (HTTPS would fail).
            for i in (seq 1 20)
                if test (date +%Y) -ge 2024
                    break
                end
                sleep 1
            end
            # wipe the boot log / DSP warnings so the desktop starts clean
            printf '\033[H\033[2J\033[3J' > /dev/tty1
            set -l log $HOME/mango.log
            echo "=== mango start "(date -Is)" rt=$XDG_RUNTIME_DIR ===" > $log
            ls -ld $XDG_RUNTIME_DIR >> $log 2>&1
            ls -l /dev/dri >> $log 2>&1
            id >> $log 2>&1
            # desktop components start from mango itself:
            #   exec_once=/usr/local/bin/mango-shell-start  (in /etc/mango/config.conf)
            /usr/bin/mango >> $log 2>&1
            echo "=== mango exited rc=$status ===" >> $log
        end
    end
end
