#!/usr/bin/env bash
# flash-elish.sh - the only flashing path for xiaomi-elish.
#
# HARD POLICY (do not weaken):
#   * flash targets: boot_b and linux only. Every other partition (system_b,
#     dtbo_b, userdata, vendor_boot, super, boot_a, ...) is refused with exit 2.
#   * erase: forbidden, with exactly one authorised exception authorised by the
#     user on 2026-10-24 and recorded as docs/decisions.md D-009 - a single
#     "fastboot erase dtbo_b", and only when the operator passes the explicit
#     --allow-dtbo-erase switch AND repeats the confirmation at execution time.
#     Without the switch this script cannot erase anything, ever. dtbo_b holds
#     only the (regenerated) device tree overlay; its erase is the sole write
#     outside the normal flash path and it never touches a data/OS partition.
#   * zero touch on everything else: userdata, vendor_boot, super, system_b,
#     persist, modem, ... are never read for writes, never erased, never
#     flashed. Any request naming them exits non-zero before fastboot runs.
#   * the only non-flash fastboot operations allowed are getvar (read-only),
#     reboot-bootloader, and the gated erase above. oem/format/wipe/flashall are
#     refused with exit 2.
#   * default mode is dry-run: every command is printed, never executed. Real
#     writes need --execute plus image existence, a verified SHA256 and a
#     fastboot getvar partition-size:<part> size check. Every executed step is
#     appended to output/<date>/FLASH-LOG so the dtbo_b exception is auditable
#     (scripts/audit-hygiene.sh verifies that log).
#
# Partition labels are resolved by the bootloader, never by a hardcoded block
# device. For understanding only: on this board boot_b is typically
# /dev/block/sde37 and "linux" is the ext4 rootfs partition - the script never
# uses those paths and contains no code that could write to them.
set -uo pipefail

REPO_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
OUTPUT_DATE=${OUTPUT_DATE:-20261024}

# Immutable whitelists. PART_BOOT/PART_ROOT only pick defaults from the first.
ALLOWED_PARTITIONS="boot_b linux"
ERASE_ALLOWPART=dtbo_b
PART_BOOT=${PART_BOOT:-boot_b}
PART_ROOT=${PART_ROOT:-linux}
FASTBOOT_BIN=${FASTBOOT:-fastboot}
FLASH_LOG=${FLASH_LOG:-$REPO_ROOT/output/${OUTPUT_DATE}/FLASH-LOG}

MODE=dry-run
BOOT_IMG=
ROOT_IMG=
BOOT_SHA=
ROOT_SHA=
MANIFEST=
MANIFEST_DIR=${MANIFEST_DIR:-$REPO_ROOT/output/${OUTPUT_DATE}}
REBOOT_BOOTLOADER=0
ALLOW_DTBO_ERASE=0
CONFIRM_ERASE=
CONFIRM_GIVEN=0
MANIFEST_EXPLICIT=0

FLASH_PARTS=()
FLASH_IMGS=()
FLASH_SHAS=()
ERASE_PARTS=()
EXTRA_OPS=()

say()  { printf '%s\n' "$*"; }
info() { printf '[flash] %s\n' "$*"; }
die_runtime() { printf '[flash] ERROR: %s\n' "$*" >&2; exit 1; }
deny() {
    printf '[flash] DENIED: %s\n' "$*" >&2
    exit 2
}

usage() {
    cat <<'USAGE'
usage: flash-elish.sh [options]

  --boot IMG             image for the boot partition (default PART_BOOT=boot_b)
  --root IMG             image for the root partition (default PART_ROOT=linux)
  --flash PART=IMG       flash request; PART must be boot_b or linux
  --erase PART           refused unless PART=dtbo_b and --allow-dtbo-erase is set
  --allow-dtbo-erase     enable the single D-009 exception (erase dtbo_b only)
  --confirm-erase PART   second confirmation for --execute (must equal dtbo_b)
  --input-confirm        read the confirmation token from stdin when interactive
  --flash-log PATH       where executed steps are appended (default: output/<date>/FLASH-LOG)
  --op reboot-bootloader allowed; --op NAME=PART refused unless it is erase=dtbo_b
  --reboot-bootloader    convenience for --op reboot-bootloader
  --boot-sha256 HEX      expected digest for --boot (else sidecar/MANIFEST)
  --root-sha256 HEX      expected digest for --root (else sidecar/MANIFEST)
  --manifest PATH        use exactly this MANIFEST (no default search)
                         default search order: output/<date>/MANIFEST, then
                         MANIFEST.boot, then every MANIFEST.* fragment; a
                         '<image>.sha256' sidecar is always tried first
  --fastboot PATH        fastboot binary (default: fastboot)
  --execute              really run fastboot (default: dry-run, print only)
  --dry-run              force dry-run
  -h, --help             this text

Exit codes: 0 ok, 1 preflight/runtime failure, 2 policy denial.
USAGE
}

partition_allowed() {
    local candidate=$1 other
    for other in $ALLOWED_PARTITIONS; do
        [ "$candidate" = "$other" ] && return 0
    done
    return 1
}

require_allowed_partition() {
    local candidate=$1
    if [ -z "$candidate" ]; then
        deny "empty partition name"
    fi
    partition_allowed "$candidate" && return 0
    deny "partition '$candidate' is not whitelisted. Only boot_b (boot) and linux (rootfs) may be flashed; system_b/dtbo_b/userdata/vendor_boot/super and every other partition are forbidden"
}

# The single authorised erase, gated by --allow-dtbo-erase (decisions.md D-009).
require_erase_allowed() {
    local candidate=$1
    if [ "$ALLOW_DTBO_ERASE" -ne 1 ]; then
        deny "erase of '$candidate' refused: fastboot erase is forbidden (no --allow-dtbo-erase switch given). The only exception is a single 'erase dtbo_b' (decisions.md D-009)"
    fi
    if [ "$candidate" != "$ERASE_ALLOWPART" ]; then
        deny "erase of '$candidate' refused: the D-009 exception covers $ERASE_ALLOWPART and nothing else. userdata/vendor_boot/super/system_b are never erased"
    fi
    return 0
}

guard_operation() {
    local op=$1 part=${2:-}
    case "$op" in
        flash)            require_allowed_partition "$part" ;;
        erase)            require_erase_allowed "$part" ;;
        getvar|reboot-bootloader) return 0 ;;
        *) deny "fastboot operation '$op' is forbidden; only flash (boot_b/linux), getvar, reboot-bootloader and the gated 'erase dtbo_b' are allowed" ;;
    esac
}

add_flash() {
    FLASH_PARTS+=("$1")
    FLASH_IMGS+=("$2")
    FLASH_SHAS+=("$3")
}

while [ $# -gt 0 ]; do
    case "$1" in
        --boot)   BOOT_IMG=$2; shift 2 ;;
        --root)   ROOT_IMG=$2; shift 2 ;;
        --flash)  add_flash "${2%%=*}" "${2#*=}" ""; shift 2 ;;
        --erase)  ERASE_PARTS+=("$2"); shift 2 ;;
        --allow-dtbo-erase) ALLOW_DTBO_ERASE=1; shift ;;
        --confirm-erase) CONFIRM_ERASE=$2; CONFIRM_GIVEN=1; shift 2 ;;
        --flash-log) FLASH_LOG=$2; shift 2 ;;
        --op)     EXTRA_OPS+=("$2"); shift 2 ;;
        --reboot-bootloader) REBOOT_BOOTLOADER=1; shift ;;
        --boot-sha256) BOOT_SHA=$2; shift 2 ;;
        --root-sha256) ROOT_SHA=$2; shift 2 ;;
        --manifest) MANIFEST=$2; MANIFEST_EXPLICIT=1; shift 2 ;;
        --fastboot) FASTBOOT_BIN=$2; shift 2 ;;
        --execute) MODE=execute; shift ;;
        --dry-run) MODE=dry-run; shift ;;
        -h|--help) usage; exit 0 ;;
        *) deny "unknown argument '$1' (refusing to guess)" ;;
    esac
done

# ---------------------------------------------------------------- policy ---
for requested_erase in ${ERASE_PARTS[@]+"${ERASE_PARTS[@]}"}; do
    require_erase_allowed "$requested_erase"
done

for extra_op in ${EXTRA_OPS[@]+"${EXTRA_OPS[@]}"}; do
    case "$extra_op" in
        reboot-bootloader)
            REBOOT_BOOTLOADER=1
            ;;
        *=*)
            op_name=${extra_op%%=*}
            op_part=${extra_op#*=}
            guard_operation "$op_name" "$op_part"
            if [ "$op_name" = erase ]; then
                ERASE_PARTS+=("$op_part")
                continue
            fi
            deny "operation '$op_name' with a partition argument is not supported"
            ;;
        *)
            guard_operation "$extra_op" ""
            deny "operation '$extra_op' is not implemented"
            ;;
    esac
done

if [ -n "$BOOT_IMG" ]; then
    require_allowed_partition "$PART_BOOT"
    add_flash "$PART_BOOT" "$BOOT_IMG" "$BOOT_SHA"
fi
if [ -n "$ROOT_IMG" ]; then
    require_allowed_partition "$PART_ROOT"
    add_flash "$PART_ROOT" "$ROOT_IMG" "$ROOT_SHA"
fi

if [ "${#FLASH_PARTS[@]}" -eq 0 ] && [ "${#ERASE_PARTS[@]}" -eq 0 ] && [ "$REBOOT_BOOTLOADER" -eq 0 ]; then
    say "nothing to do (no --boot/--root/--flash/--erase/--reboot-bootloader given)"
    usage >&2
    exit 1
fi

for request_part in ${FLASH_PARTS[@]+"${FLASH_PARTS[@]}"}; do
    require_allowed_partition "$request_part"
done

# ------------------------------------------------------------- preflight ---
# Read the sha256 a manifest line assigns to a basename. Accepts both
# 'name target sha256 time size' and 'path | target | sha256 | size | time'
# (scripts/collect-manifest.sh), and matches on the basename so that
# 'rootfs/linux-rootfs-f2fs.img' also answers for 'linux-rootfs-f2fs.img'.
manifest_sha_for() {
    awk -v want="$2" '
        /^[[:space:]]*#/ { next }
        {
            line = $0
            gsub(/[|]/, " ", line)
            n = split(line, f, /[[:space:]]+/)
            if (n < 2) next
            path = f[1]
            sub(/.*\//, "", path)
            if (path != want) next
            for (i = 2; i <= n; i++) {
                if (length(f[i]) == 64 && f[i] ~ /^[0-9a-f]+$/) { print f[i]; exit }
            }
        }' "$1"
}

manifest_sources() {
    if [ "$MANIFEST_EXPLICIT" -eq 1 ]; then
        printf '%s\n' "$MANIFEST"
        return 0
    fi
    local ordered
    for ordered in "$MANIFEST_DIR/MANIFEST" "$MANIFEST_DIR/MANIFEST.boot"; do
        [ -f "$ordered" ] && printf '%s\n' "$ordered"
    done
    shopt -s nullglob
    for ordered in "$MANIFEST_DIR"/MANIFEST.*; do
        printf '%s\n' "$ordered"
    done
    shopt -u nullglob
    return 0
}

expected_sha256() {
    local image=$1 explicit=$2 base value candidate
    if [ -n "$explicit" ]; then
        printf '%s' "$explicit"
        return 0
    fi
    if [ -f "$image.sha256" ]; then
        value=$(awk 'NR==1 {print $1}' "$image.sha256")
        [ -n "$value" ] && { printf '%s' "$value"; return 0; }
    fi
    base=$(basename "$image")
    while IFS= read -r candidate; do
        [ -n "$candidate" ] || continue
        [ -f "$candidate" ] || continue
        value=$(manifest_sha_for "$candidate" "$base")
        [ -n "$value" ] && { printf '%s' "$value"; return 0; }
    done <<< "$(manifest_sources)"
    return 1
}

declare -a PREFLIGHT_SIZE=()

preflight_image() {
    local index=$1 part=$2 image=$3 explicit_sha=$4 want actual expected role_sha role_flag actual_now

    [ -f "$image" ] || die_runtime "image for $part does not exist: $image"
    want=$(stat -c %s "$image")
    [ "$want" -gt 0 ] || die_runtime "image for $part is empty: $image"

    # An explicit digest belongs to a partition ROLE, not to a CLI form: whichever
    # of --root/--flash linux=... addresses the root partition may use --root-sha256.
    role_sha=$explicit_sha
    role_flag=--boot-sha256
    if [ "$part" = "$PART_ROOT" ]; then
        role_flag=--root-sha256
        [ -n "$role_sha" ] || role_sha=$ROOT_SHA
    elif [ "$part" = "$PART_BOOT" ]; then
        [ -n "$role_sha" ] || role_sha=$BOOT_SHA
    fi

    if expected=$(expected_sha256 "$image" "$role_sha"); then
        actual=$(sha256sum "$image" | cut -d' ' -f1)
        if [ "$actual" != "$expected" ]; then
            die_runtime "sha256 mismatch for $image: expected $expected, got $actual"
        fi
        info "sha256 ok ($part): $actual"
    else
        actual_now=$(sha256sum "$image" | cut -d' ' -f1)
        info "WARN no expected sha256 known for $image (partition $part)"
        info "     fix by passing:      $role_flag $actual_now"
        info "     or write a sidecar:  sha256sum '$image' > '$image.sha256'"
        info "     or regenerate:       scripts/collect-manifest.sh   (searched: $MANIFEST_DIR)"
        if [ "$MODE" = execute ]; then
            die_runtime "refusing --execute without a verifiable sha256 for $image (see the three options above)"
        fi
    fi
    PREFLIGHT_SIZE[index]=$want
}

index=0
while [ "$index" -lt "${#FLASH_PARTS[@]}" ]; do
    preflight_image "$index" "${FLASH_PARTS[$index]}" "${FLASH_IMGS[$index]}" "${FLASH_SHAS[$index]}"
    index=$((index + 1))
done

# ------------------------------------------------------------- execution ---
append_flash_log() {
    [ "$MODE" = execute ] || return 0
    mkdir -p "$(dirname "$FLASH_LOG")" || die_runtime "cannot create directory for $FLASH_LOG"
    printf '%s  fastboot %s -> ok\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$*" >> "$FLASH_LOG" ||
        die_runtime "cannot append to $FLASH_LOG"
    info "logged to $FLASH_LOG: fastboot $*"
}

run_fastboot() {
    guard_operation "$1" "${2:-}"
    if [ "$MODE" = execute ]; then
        printf '[flash] $ %s %s\n' "$FASTBOOT_BIN" "$*"
        "$FASTBOOT_BIN" "$@"
        local rc=$?
        [ "$rc" -eq 0 ] || die_runtime "fastboot $1 failed (rc=$rc)"
        append_flash_log "$@"
        return 0
    fi
    printf '[dry-run] %s %s\n' "$FASTBOOT_BIN" "$*"
    return 0
}

# Second confirmation, required for any real erase (the --allow-dtbo-erase
# switch is the first one). Non-interactive runs must pass --confirm-erase.
confirm_erase_execution() {
    local part=$1 typed=
    printf '[flash] ERASE %s: irreversible, single D-009 exception\n' "$part"
    printf '[flash] would run: %s erase %s\n' "$FASTBOOT_BIN" "$part"
    if [ "$CONFIRM_GIVEN" -eq 1 ]; then
        if [ "$CONFIRM_ERASE" != "$part" ]; then
            die_runtime "confirmation mismatch: --confirm-erase '${CONFIRM_ERASE}' does not match '$part'"
        fi
        info "confirmation 1/2: --allow-dtbo-erase   confirmation 2/2: --confirm-erase $part"
        return 0
    fi
    if [ -t 0 ]; then
        printf 'type "%s" to confirm the erase: ' "$part"
        read -r typed || typed=
        if [ "$typed" != "$part" ]; then
            die_runtime "erase confirmation refused (typed '${typed}')"
        fi
        info "confirmation accepted interactively"
        return 0
    fi
    die_runtime "erase needs a second confirmation: pass --confirm-erase $part (not interactive)"
}

partition_size_of() {
    local part=$1 raw parsed
    raw=$("$FASTBOOT_BIN" getvar partition-size:"$part" 2>&1) || return 1
    parsed=$(printf '%s\n' "$raw" | sed -n 's/^partition-size:[^:]*: *//p' | head -1 | tr -d '[:space:]')
    [ -n "$parsed" ] || return 1
    case "$parsed" in
        0x*|0X*) printf '%d' "$parsed" 2>/dev/null || return 1 ;;
        *)       printf '%s' "$parsed" ;;
    esac
}

check_partition_size() {
    local part=$1 image_size=$2 capacity
    if [ "$MODE" != execute ]; then
        printf '[dry-run] %s getvar partition-size:%s   (would compare against %d bytes)\n' "$FASTBOOT_BIN" "$part" "$image_size"
        return 0
    fi
    printf '[flash] $ %s getvar partition-size:%s\n' "$FASTBOOT_BIN" "$part"
    if ! capacity=$(partition_size_of "$part"); then
        die_runtime "cannot read partition-size for $part; refusing to flash without a size check"
    fi
    case "$capacity" in
        ''|*[!0-9]*) die_runtime "unparsable partition-size '$capacity' for $part" ;;
    esac
    info "partition $part capacity=$capacity bytes, image=$image_size bytes"
    if [ "$image_size" -gt "$capacity" ]; then
        die_runtime "image for $part ($image_size bytes) exceeds partition size ($capacity bytes); aborting before any write"
    fi
    return 0
}

info "mode=$MODE fastboot=$FASTBOOT_BIN flash=[${FLASH_PARTS[*]:-}] erase=[${ERASE_PARTS[*]:-}] allow_dtbo_erase=$ALLOW_DTBO_ERASE"

# 1) the gated erase (only dtbo_b can get here)
for erase_part in ${ERASE_PARTS[@]+"${ERASE_PARTS[@]}"}; do
    require_erase_allowed "$erase_part"
    if [ "$MODE" = execute ]; then
        confirm_erase_execution "$erase_part"
        run_fastboot erase "$erase_part"
    else
        printf '[dry-run] %s erase %s   (gated: needs --execute --allow-dtbo-erase --confirm-erase %s)\n' \
            "$FASTBOOT_BIN" "$erase_part" "$erase_part"
    fi
done

# 2) flashes (boot_b / linux only)
index=0
while [ "$index" -lt "${#FLASH_PARTS[@]}" ]; do
    part=${FLASH_PARTS[$index]}
    image=${FLASH_IMGS[$index]}
    require_allowed_partition "$part"
    check_partition_size "$part" "${PREFLIGHT_SIZE[$index]}"
    run_fastboot flash "$part" "$image"
    index=$((index + 1))
done

# 3) reboot-bootloader (allowed, not a write)
if [ "$REBOOT_BOOTLOADER" -eq 1 ]; then
    run_fastboot reboot-bootloader
fi

if [ "$MODE" = execute ]; then
    info "executed ${#FLASH_PARTS[@]} flash and ${#ERASE_PARTS[@]} erase operation(s); nothing outside boot_b/linux/$ERASE_ALLOWPART was touched"
else
    info "dry-run complete: ${#FLASH_PARTS[@]} flash and ${#ERASE_PARTS[@]} erase operation(s) printed, fastboot was not invoked"
    info "re-run with --execute (plus verified images) to write the device"
fi
exit 0
