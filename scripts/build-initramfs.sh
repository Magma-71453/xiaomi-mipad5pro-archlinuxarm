#!/usr/bin/env bash
# Build the elish initramfs: newc cpio + gzip, containing
#   /init               POSIX sh PID 1 (configs/initramfs/init)
#   /bin/busybox        static aarch64 busybox + applet symlinks
#   /lib/modules/...    optional kernel modules (--modules), e.g. the DRM panel
#   /initramfs.modules  manifest of what was shipped ("no extra modules" if none)
#
# Kernel facts this script relies on (from the kernel module, 2026-10-24):
#   CONFIG_F2FS_FS=y / CONFIG_EXT4_FS=y  -> rootfs drivers are builtin, no .ko needed
#   CONFIG_DRM_MSM=y                     -> no msm module needed
#   CONFIG_DRM_PANEL_NOVATEK_NT36523=m   -> panel module ships in the initramfs so
#                                           the screen (and the console log) comes up early
#   MODULE_COMPRESS_ALL=y + MODULE_COMPRESS_ZSTD=y
#                                        -> modules_install emits *.ko.zst, but busybox
#                                           insmod cannot read zstd: every .ko.zst is
#                                           decompressed to .ko before packing, and
#                                           modules.dep is rewritten accordingly.
#
# The root filesystem probe order stays f2fs then ext4 (decisions.md D-010).
#
# Usage:
#   scripts/build-initramfs.sh [--busybox PATH] [--modules DIR|FILE] [--out PATH]
#   scripts/build-initramfs.sh --test
#
# --modules accepts a directory (a lib/modules/<ver>/ subset, e.g. the panel module
# and its dependencies) or a single .ko / .ko.zst file. Both compressions are fine.
set -uo pipefail

REPO_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
OUTPUT_DATE=${OUTPUT_DATE:-20261024}
BUILD_ROOT=${BUILD_ROOT:-$(cd "$REPO_ROOT/.." && pwd)/.build}
INIT_SRC=${INIT_SRC:-$REPO_ROOT/configs/initramfs/init}
DEFAULT_BUSYBOX=${BUILD_ROOT}/tools/busybox-static/root/usr/bin/busybox-static
BUSYBOX_URL=http://ports.ubuntu.com/ubuntu-ports/pool/universe/b/busybox/busybox-static_1.38.0-3ubuntu5_arm64.deb

BUSYBOX=$DEFAULT_BUSYBOX
MODULES=
FIRMWARE=
EXTRA=
OUT=$REPO_ROOT/output/${OUTPUT_DATE}/boot/initramfs.cpio.gz
MODE=build
MODULE_WARN_LIMIT=32

REQUIRED_APPLETS="sh mount umount mkdir sleep switch_root grep cat ls printf ln rm find sed insmod"
OPTIONAL_APPLETS="mdev dmesg sync poweroff reboot modprobe readlink test cut tr head tail wc date touch chmod chown mknod dd gzip tar uname ps kill xargs sort awk env echo cp mv free more hexdump"

die() { printf 'error: %s\n' "$*" >&2; exit 1; }
say() { printf '%s\n' "$*"; }

usage() {
    cat <<'USAGE'
usage: scripts/build-initramfs.sh [options]
  --busybox PATH   static aarch64 busybox (default: .build/tools/busybox-static/root/usr/bin/busybox-static)
  --modules PATH   directory (lib/modules/<ver> subset) or a single .ko / .ko.zst file
  --firmware DIR   copy DIR into /lib/firmware inside the initramfs (GPU panel firmware)
  --extra DIR      copy DIR into / inside the initramfs (f2fs-tools for offline resize)
                   .ko.zst inputs are decompressed to .ko because busybox insmod cannot read zstd
  --out PATH       output cpio.gz (default: output/<date>/boot/initramfs.cpio.gz)
  --test           run the mocked-logic tests (no device touched)
USAGE
    exit 0
}

while [ $# -gt 0 ]; do
    case "$1" in
        --busybox)  BUSYBOX=$2; shift 2 ;;
        --modules)  MODULES=$2; shift 2 ;;
        --firmware) FIRMWARE=$2; shift 2 ;;
        --extra)    EXTRA=$2; shift 2 ;;
        --out)      OUT=$2; shift 2 ;;
        --test)     MODE=test; shift ;;
        -h|--help)  usage ;;
        *)          die "unknown argument: $1" ;;
    esac
done

applet_present() {
    LC_ALL=C grep -aqF -- "$1" "$BUSYBOX"
}

zstd_tool() {
    if command -v zstd >/dev/null 2>&1; then printf 'zstd'
    elif command -v unzstd >/dev/null 2>&1; then printf 'unzstd'
    fi
}

# Copy the requested modules into <root>/lib/modules/<ver>/ and normalise
# compression: busybox insmod cannot decompress zstd, so xx.ko.zst -> xx.ko.
stage_modules() {
    local root=$1 modules=$2 dest ver staged=0 decompressed=0 entry zst tool dep
    [ -n "$modules" ] || return 0

    if [ -d "$modules" ]; then
        ver=${MODULES_VERSION:-$(basename "$modules")}
        dest="$root/lib/modules/$ver"
        mkdir -p "$dest"
        cp -a "$modules"/. "$dest"/
    elif [ -f "$modules" ]; then
        ver=${MODULES_VERSION:-elish}
        dest="$root/lib/modules/$ver"
        mkdir -p "$dest"
        cp -a "$modules" "$dest"/
    else
        die "--modules path does not exist: $modules"
    fi

    tool=$(zstd_tool)
    while IFS= read -r zst; do
        case "$zst" in *.ko.zst) ;; *) continue ;; esac
        [ -n "$tool" ] || die "found $zst but neither zstd nor unzstd is available to decompress it"
        "$tool" -d -q -f -o "${zst%.zst}" -- "$zst" || die "failed to decompress $zst"
        rm -f "$zst" || die "failed to remove $zst after decompression"
        decompressed=$((decompressed + 1))
    done <<< "$(find "$dest" -type f -name '*.ko.zst' 2>/dev/null | LC_ALL=C sort)"

    # modules.dep would still point at the compressed names
    for dep in "$dest/modules.dep" "$dest/modules.dep.bin"; do
        [ -f "$dep" ] || continue
        case "$dep" in
            *.bin) : ;;
            *) sed -i 's/\.ko\.zst/.ko/g' "$dep" ;;
        esac
    done

    DEPLOYED_MODULES=()
    while IFS= read -r entry; do
        [ -n "$entry" ] || continue
        DEPLOYED_MODULES+=("${entry#$root}")
        staged=$((staged + 1))
    done <<< "$(find "$dest" -type f -name '*.ko' 2>/dev/null | LC_ALL=C sort)"

    MODULE_ZST_DECOMPRESSED=$decompressed
    MODULE_COUNT=$staged
    if [ "$staged" -gt "$MODULE_WARN_LIMIT" ]; then
        say "warning: $staged modules staged; the convention is a curated lib/modules/<ver> subset (panel module + dependencies), not the whole tree" >&2
    fi
    return 0
}

write_module_manifest() {
    local root=$1 entry
    if [ "${#DEPLOYED_MODULES[@]}" -eq 0 ]; then
        printf 'no extra modules\n' > "$root/initramfs.modules"
        return 0
    fi
    {
        printf 'elish initramfs modules (paths are inside the initramfs)\n'
        for entry in "${DEPLOYED_MODULES[@]}"; do
            printf '%s\n' "$entry"
        done
    } > "$root/initramfs.modules"
    return 0
}

stage_root() {
    local root=$1 busybox=$2 modules=$3
    local applet missing_required=
    rm -rf "$root"
    mkdir -p "$root"/bin "$root"/sbin "$root"/dev "$root"/proc "$root"/sys \
             "$root"/newroot "$root"/tmp "$root"/run "$root"/mnt "$root"/etc "$root"/lib

    install -m 0755 "$busybox" "$root/bin/busybox"
    for applet in $REQUIRED_APPLETS; do
        if applet_present "$applet"; then
            ln -sfn busybox "$root/bin/$applet"
        else
            missing_required="$missing_required $applet"
        fi
    done
    [ -z "$missing_required" ] || die "busybox lacks required applet(s):$missing_required"
    for applet in $OPTIONAL_APPLETS; do
        applet_present "$applet" && ln -sfn busybox "$root/bin/$applet"
    done
    ln -sfn busybox "$root/bin/["

    install -m 0755 "$INIT_SRC" "$root/init"

    DEPLOYED_MODULES=()
    MODULE_COUNT=0
    MODULE_ZST_DECOMPRESSED=0
    if [ -n "$FIRMWARE" ]; then
        [ -d "$FIRMWARE" ] || die "--firmware path does not exist: $FIRMWARE"
        mkdir -p "$root/lib/firmware"
        cp -a "$FIRMWARE"/. "$root/lib/firmware"/
        say "build.firmware_files = $(find "$root/lib/firmware" -type f | wc -l)"
    fi
    if [ -n "$EXTRA" ]; then
        [ -d "$EXTRA" ] || die "--extra path does not exist: $EXTRA"
        cp -a "$EXTRA"/. "$root"/
        say "build.extra_files = $(find "$EXTRA" -type f | wc -l)"
    fi
    stage_modules "$root" "$modules" || return 1
    write_module_manifest "$root" || return 1
    return 0
}

pack_cpio() {
    local root=$1 out=$2 extra=()
    if cpio --help 2>&1 | grep -q -- '--reproducible'; then
        extra=(--reproducible)
    fi
    mkdir -p "$(dirname "$out")"
    ( cd "$root" && find . -print0 | LC_ALL=C sort -z |
      cpio --null -o -H newc --owner=0:0 "${extra[@]}" 2>/dev/null ) | gzip -n -9 > "$out"
    [ -s "$out" ] || die "cpio/gzip produced an empty archive"
    return 0
}

report_image() {
    local out=$1 root=$2
    say "build.out = $out"
    say "build.size_bytes = $(stat -c %s "$out")"
    say "build.sha256 = $(sha256sum "$out" | cut -d' ' -f1)"
    say "build.entries = $(gzip -dc "$out" | cpio -t 2>/dev/null | wc -l)"
    say "build.busybox = $BUSYBOX ($(stat -c %s "$BUSYBOX") bytes)"
    say "build.init_script = $INIT_SRC"
    say "build.init_sha256 = $(sha256sum "$INIT_SRC" | cut -d' ' -f1)"
    say "build.root_fs_drivers = builtin (CONFIG_F2FS_FS=y, CONFIG_EXT4_FS=y); probe order f2fs then ext4"
    if [ "${MODULE_COUNT:-0}" -eq 0 ] && [ -z "$MODULES" ]; then
        say "build.modules = no extra modules (--modules not given)"
    elif [ "${MODULE_COUNT:-0}" -eq 0 ]; then
        say "build.modules = no extra modules (nothing found under '$MODULES')"
    else
        say "build.modules = ${MODULE_COUNT} .ko shipped (${MODULE_ZST_DECOMPRESSED:-0} decompressed from .ko.zst)"
        say "build.module_list:"
        printf '  %s\n' "${DEPLOYED_MODULES[@]}"
    fi
    say "build.compression = gzip -9 -n over newc cpio"
    say "build.root_layout:"
    ( cd "$root" && find . -maxdepth 2 -mindepth 1 | LC_ALL=C sort | sed 's/^/  /' | head -25 )
}

stage_and_pack() {
    local root=$1
    [ -x "$BUSYBOX" ] || die "static busybox not found at $BUSYBOX; fetch it with:
  curl -sSLo busybox.deb $BUSYBOX_URL
  bsdtar -xf busybox.deb data.tar.zst && bsdtar -xf data.tar.zst"
    stage_root "$root" "$BUSYBOX" "$MODULES" || return 1
    pack_cpio "$root" "$OUT" || return 1
    return 0
}

# ---------------------------------------------------------------- test mode --
TEST_FAILURES=0
t_ok()  { say "  PASS $*"; }
t_bad() { say "  FAIL $*"; TEST_FAILURES=$((TEST_FAILURES + 1)); }

make_mocks() {
    local mock=$1
    mkdir -p "$mock"
    cat > "$mock/mount" <<'MOCK'
#!/bin/sh
printf 'mount %s\n' "$*" >> ${MOCK_LOG:-/dev/null}
prev=
fstype=
for arg in "$@"; do
    if [ "$prev" = "-t" ]; then fstype=$arg; fi
    prev=$arg
done
if [ -n "${MOCK_FAIL_FS:-}" ] && [ "${MOCK_FAIL_FS:-}" = "$fstype" ]; then exit 1; fi
exit 0
MOCK
    cat > "$mock/umount" <<'MOCK'
#!/bin/sh
printf 'umount %s\n' "$*" >> ${MOCK_LOG:-/dev/null}
exit 0
MOCK
    cat > "$mock/switch_root" <<'MOCK'
#!/bin/sh
printf 'switch_root %s\n' "$*" >> ${MOCK_LOG:-/dev/null}
exit 0
MOCK
    cat > "$mock/insmod" <<'MOCK'
#!/bin/sh
printf 'insmod %s\n' "$*" >> ${MOCK_LOG:-/dev/null}
if [ "${MOCK_INSSMOD_FAIL:-0}" = "1" ]; then
    printf 'insmod: can not insert module: File exists\n' >&2
    exit 1
fi
exit 0
MOCK
    chmod +x "$mock"/mount "$mock"/umount "$mock"/switch_root "$mock"/insmod
}

prepare_root() {
    local root=$1
    mkdir -p "$root/mock" "$root/dev/disk/by-partlabel" "$root/sys/class/block" \
             "$root/newroot/sbin" "$root/nomods"
    printf '#!/bin/sh\nexit 0\n' > "$root/newroot/sbin/init"
    chmod 0755 "$root/newroot/sbin/init"
    make_mocks "$root/mock"
}

run_init() {
    local root=$1 log=$2 moddir=$3; shift 3
    env PATH="$root/mock:$PATH" \
        INIT_SKIP_BASE_MOUNTS=1 \
        INIT_MODDIR="$moddir" \
        INIT_SYSFSDIR="$root/sys" \
        MOCK_LOG="$root/mock.log" \
        "$@" sh "$INIT_SRC" >"$log" 2>&1
}

test_partlabel_lookup() {
    local root=$1
    prepare_root "$root"
    : > "$root/dev/disk/by-partlabel/linux"
    if run_init "$root" "$root/log" "$root/nomods" INIT_DEVDIR="$root/dev" \
            INIT_NEWROOT="$root/newroot" INIT_TIMEOUT=5 \
       && grep -q "mount -t f2fs -o ro $root/dev/disk/by-partlabel/linux" "$root/mock.log" \
       && grep -q "root mounted read-only as f2fs" "$root/log" \
       && grep -q "switch_root $root/newroot /sbin/init" "$root/mock.log"; then
        t_ok "partlabel lookup: linux found, f2fs tried first and mounted, switch_root called"
    else
        t_bad "partlabel lookup: expected f2fs-first mount then switch_root"
        sed 's/^/    /' "$root/log" | head -12
    fi
}

test_fstype_fallback() {
    local root=$1
    prepare_root "$root"
    : > "$root/dev/disk/by-partlabel/linux"
    if run_init "$root" "$root/log" "$root/nomods" INIT_DEVDIR="$root/dev" \
            INIT_NEWROOT="$root/newroot" INIT_TIMEOUT=5 MOCK_FAIL_FS=f2fs \
       && grep -q "mount -t f2fs -o ro" "$root/mock.log" \
       && grep -q "root mounted read-only as ext4" "$root/log" \
       && grep -q "switch_root $root/newroot /sbin/init" "$root/mock.log"; then
        t_ok "fstype fallback: f2fs refused by mock -> ext4 mounted -> switch_root"
    else
        t_bad "fstype fallback: expected f2fs failure then ext4 mount"
        sed 's/^/    /' "$root/log" | head -12
    fi
}

test_timeout() {
    local root=$1 start elapsed
    prepare_root "$root"
    start=$(date +%s)
    if run_init "$root" "$root/log" "$root/nomods" INIT_DEVDIR="$root/dev" \
            INIT_NEWROOT="$root/newroot" INIT_TIMEOUT=2; then
        t_bad "timeout: init exited 0 without a root device"
        return
    fi
    elapsed=$(( $(date +%s) - start ))
    if grep -q "timeout after 2s waiting for $root/dev/disk/by-partlabel/linux" "$root/log" \
       && [ "$elapsed" -ge 2 ] && [ "$elapsed" -le 8 ]; then
        t_ok "timeout branch: waited ${elapsed}s, reported the missing partlabel node, exited non-zero"
    else
        t_bad "timeout branch: expected timeout message and 2-8s wall clock, got ${elapsed}s"
        sed 's/^/    /' "$root/log" | head -12
    fi
}

test_sysfs_fallback() {
    local root=$1
    prepare_root "$root"
    mkdir -p "$root/sys/class/block/sde38"
    printf 'DEVNAME=sde38\nPARTNAME=linux\n' > "$root/sys/class/block/sde38/uevent"
    : > "$root/dev/sde38"
    if run_init "$root" "$root/log" "$root/nomods" INIT_DEVDIR="$root/dev" \
            INIT_NEWROOT="$root/newroot" INIT_TIMEOUT=5 \
       && grep -q "resolved PARTNAME=linux to $root/dev/sde38" "$root/log" \
       && [ -L "$root/dev/disk/by-partlabel/linux" ] \
       && grep -q "switch_root $root/newroot /sbin/init" "$root/mock.log"; then
        t_ok "sysfs fallback: PARTNAME=linux resolved from /sys/class/block, symlink rebuilt, boot continued"
    else
        t_bad "sysfs fallback: expected sysfs PARTNAME resolution"
        sed 's/^/    /' "$root/log" | head -12
    fi
}

test_panel_module_insmod() {
    local root=$1
    prepare_root "$root"
    mkdir -p "$root/mods/kernel/drivers/gpu/drm/panel"
    head -c 4096 /dev/zero > "$root/mods/kernel/drivers/gpu/drm/panel/panel-novatek-nt36523.ko"
    : > "$root/dev/disk/by-partlabel/linux"
    if run_init "$root" "$root/log" "$root/mods" INIT_DEVDIR="$root/dev" \
            INIT_NEWROOT="$root/newroot" INIT_TIMEOUT=5 \
       && grep -q "insmod ok: $root/mods/kernel/drivers/gpu/drm/panel/panel-novatek-nt36523.ko" "$root/log" \
       && grep -q "root mounted read-only as f2fs" "$root/log"; then
        t_ok "panel module shipped in --modules is insmod'd, then f2fs mounts the root"
    else
        t_bad "panel module insmod: expected an insmod of the shipped module"
        sed 's/^/    /' "$root/log" | head -12
    fi
}

test_module_failure_nonfatal() {
    local root=$1
    prepare_root "$root"
    mkdir -p "$root/mods/kernel/drivers/gpu/drm/panel"
    head -c 4096 /dev/zero > "$root/mods/kernel/drivers/gpu/drm/panel/panel-novatek-nt36523.ko"
    : > "$root/dev/disk/by-partlabel/linux"
    if run_init "$root" "$root/log" "$root/mods" INIT_DEVDIR="$root/dev" \
            INIT_NEWROOT="$root/newroot" INIT_TIMEOUT=5 MOCK_INSSMOD_FAIL=1 \
       && grep -q "insmod skipped: $root/mods/kernel/drivers/gpu/drm/panel/panel-novatek-nt36523.ko" "$root/log" \
       && grep -q "root mounted read-only as f2fs" "$root/log" \
       && grep -q "switch_root $root/newroot /sbin/init" "$root/mock.log"; then
        t_ok "insmod failure is logged as a warning and non-fatal; mount + switch_root continue"
    else
        t_bad "non-fatal insmod: expected init to continue after a failed insmod"
        sed 's/^/    /' "$root/log" | head -12
    fi
}

test_zst_decompression() {
    local work=$1 image=$1/zst.cpio.gz root=$1/zst-stage src=$1/zstsrc
    mkdir -p "$work"
    local panel=kernel/drivers/gpu/drm/panel/panel-novatek-nt36523.ko
    mkdir -p "$src/$(dirname "$panel")"
    printf 'fake panel module payload\n' > "$src/$panel"
    zstd -q -f -o "$src/$panel.zst" "$src/$panel" || { t_bad "zstd compression of the fixture failed"; return; }
    rm -f "$src/$panel"
    printf '# modules.dep style entry\n%s.zst:\n' "$panel" > "$src/modules.dep"
    if ! MODULES="$src" OUT="$image" stage_and_pack "$root" 2>"$work/zst.err"; then
        t_bad "zst module build failed"; sed 's/^/    /' "$work/zst.err" | head -10; return
    fi
    local listing
    listing=$(gzip -dc "$image" | cpio -t 2>/dev/null | sed -e 's|^[.]/||')
    if printf '%s\n' "$listing" | grep -qx "lib/modules/$(basename "$src")/$panel" \
       && ! printf '%s\n' "$listing" | grep -q '\.ko\.zst'; then
        t_ok ".ko.zst input becomes a plain .ko in the archive (busybox insmod cannot read zstd)"
    else
        t_bad ".ko.zst input: expected decompressed .ko and no .ko.zst in the listing"
        printf '%s\n' "$listing" | grep -E 'modules|ko' | head -8 | sed 's/^/    /'
    fi
    gzip -dc "$image" | cpio -i --to-stdout "lib/modules/$(basename "$src")/modules.dep" 2>/dev/null |
        grep -q "\.ko:" && t_ok "modules.dep was rewritten from .ko.zst to .ko" ||
        t_bad "modules.dep rewrite: still references .ko.zst"
}

test_no_modules_manifest() {
    local work=$1 image=$1/nomods.cpio.gz root=$1/nomods-stage
    mkdir -p "$work"
    if ! MODULES= OUT="$image" stage_and_pack "$root" 2>"$work/nomods.err"; then
        t_bad "build without --modules failed"; sed 's/^/    /' "$work/nomods.err" | head -10; return
    fi
    local listing entry
    listing=$(gzip -dc "$image" | cpio -t 2>/dev/null | sed -e 's|^[.]/||')
    if printf '%s\n' "$listing" | grep -qx 'initramfs.modules'; then
        t_ok "/initramfs.modules is part of the archive"
    else
        t_bad "/initramfs.modules missing from the archive"
    fi
    entry=$(gzip -dc "$image" | cpio -i --to-stdout initramfs.modules 2>/dev/null)
    assert_no_modules() {
        if [ "$entry" = "no extra modules" ]; then
            t_ok "module manifest says 'no extra modules' when --modules is absent"
        else
            t_bad "module manifest without --modules: expected 'no extra modules', got [$entry]"
        fi
    }
    assert_no_modules
}

test_archive_contents() {
    local work=$1 image=$1/test-initramfs.cpio.gz root=$1/stage
    mkdir -p "$work/fakemods/elish-7.2.0/kernel/drivers/gpu/drm/panel"
    printf 'panel payload\n' > "$work/fakemods/elish-7.2.0/kernel/drivers/gpu/drm/panel/panel-novatek-nt36523.ko"
    if ! MODULES="$work/fakemods/elish-7.2.0" OUT="$image" stage_and_pack "$root" 2>"$work/build.err"; then
        t_bad "archive contents: build failed"; sed 's/^/    /' "$work/build.err" | head -20; return
    fi
    local listing
    listing=$(gzip -dc "$image" | cpio -t 2>/dev/null | sed -e 's|^[.]/||')
    if printf '%s\n' "$listing" | grep -qx 'init' \
       && printf '%s\n' "$listing" | grep -qx 'bin/busybox' \
       && printf '%s\n' "$listing" | grep -qx 'bin/mount' \
       && printf '%s\n' "$listing" | grep -qx 'bin/insmod' \
       && printf '%s\n' "$listing" | grep -qx 'lib/modules/elish-7.2.0/kernel/drivers/gpu/drm/panel/panel-novatek-nt36523.ko' \
       && printf '%s\n' "$listing" | grep -qx 'initramfs.modules'; then
        t_ok "archive contents: /init, /bin/busybox, applet symlinks, panel .ko and module manifest present"
    else
        t_bad "archive contents: expected /init, /bin/busybox, /bin/mount, /bin/insmod, panel .ko, initramfs.modules"
        printf '%s\n' "$listing" | grep -E 'init|busybox|mount|insmod|modules' | head -12 | sed 's/^/    /'
    fi
}

run_tests() {
    local tmp
    tmp=$(mktemp -d)
    say "== build-initramfs.sh --test (mocked mount/switch_root/insmod, no device touched) =="
    say "-- busybox: $BUSYBOX"
    [ -x "$BUSYBOX" ] || die "static busybox not found at $BUSYBOX (pass --busybox PATH)"
    test_partlabel_lookup "$tmp/t1"
    test_fstype_fallback "$tmp/t2"
    test_timeout "$tmp/t3"
    test_sysfs_fallback "$tmp/t4"
    test_panel_module_insmod "$tmp/t5"
    test_module_failure_nonfatal "$tmp/t6"
    test_zst_decompression "$tmp/t7"
    test_no_modules_manifest "$tmp/t8"
    test_archive_contents "$tmp/t9"
    rm -rf "$tmp"
    if [ "$TEST_FAILURES" -ne 0 ]; then
        say "== initramfs tests: $TEST_FAILURES failure(s) =="
        return 1
    fi
    say "== initramfs tests: all passed =="
    return 0
}

if [ "$MODE" = test ]; then
    run_tests
    exit $?
fi

ROOT=${BUILD_ROOT}/initramfs/${OUTPUT_DATE}/root
stage_and_pack "$ROOT" || exit 1
report_image "$OUT" "$ROOT"
