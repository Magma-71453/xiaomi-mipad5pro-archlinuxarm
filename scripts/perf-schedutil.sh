#!/usr/bin/env bash
# perf-schedutil.sh — 审视 schedutil 的“余量策略”旋钮（先探测，再决定写什么）。
#
# 设计原则：
#   * 只使用本机真实存在且可写的节点；缺失项一律 SKIP 并说明原因；
#   * 任何数值都必须由调用者显式给出（--rate-limit-us / --clamp-min / --clamp-max），
#     脚本本身不内置任何“推荐数值”，避免在不同 SoC 上盲目套用；
#   * 写入前先把原值备份到 $OUT 或 /var/lib/schedplace/backup，可用 --restore 回退；
#   * 没有 schedutil（例如 amd-pstate、intel_pstate、或 pure performance 平台）时
#     --show 仍然退出 0，并解释为什么没有可调项。
#
# 用法:
#   scripts/perf-schedutil.sh --show
#   scripts/perf-schedutil.sh --apply --rate-limit-us 2000 [--clamp-min N] [--clamp-max N]
#   scripts/perf-schedutil.sh --restore [--backup-dir DIR] [--backup-file FILE]
#
# 选项:
#   --show              只探测并打印（默认动作）
#   --apply             写入显式给出的数值（先备份、再确认、写后校验）
#   --restore           从备份恢复原值
#   --rate-limit-us N   每个策略的 schedutil/rate_limit_us（需该节点存在）
#   --clamp-min N       /proc/sys/kernel/sched_util_clamp_min
#   --clamp-max N       /proc/sys/kernel/sched_util_clamp_max
#   --backup-dir DIR    备份目录（默认 /var/lib/schedplace/backup）
#   --backup-file FILE  --restore 时指定具体备份文件
#   --yes               跳过交互确认（非交互环境必须显式给出）
#   --help
#
# 退出码: 0 = 正常（含“无可调项”），1 = 执行失败，2 = 用法错误。

set -uo pipefail

PROG=$(basename "$0")
MODE=show
OUT_DIR=""
BACKUP_FILE=""
RATE_LIMIT=""
CLAMP_MIN=""
CLAMP_MAX=""
ASSUME_YES=0
DEFAULT_BACKUP=/var/lib/schedplace/backup
PLAN_FILE=""
SKIPPED=0

say() { printf '%s
' "$*"; }
warn() { printf '%s: warn: %s
' "$PROG" "$*" >&2; }
err() { printf '%s: error: %s
' "$PROG" "$*" >&2; }

usage() {
    sed -n '3,40p' "$0" | sed 's/^# \{0,1\}//'
}

cleanup() {
    if [ -n "$PLAN_FILE" ] && [ -f "$PLAN_FILE" ]; then rm -f "$PLAN_FILE"; fi
}
trap cleanup EXIT

while [ $# -gt 0 ]; do
    case "$1" in
        --show) MODE=show; shift ;;
        --apply) MODE=apply; shift ;;
        --restore) MODE=restore; shift ;;
        --yes) ASSUME_YES=1; shift ;;
        --rate-limit-us)
            shift
            if [ $# -eq 0 ]; then err "--rate-limit-us needs a value"; exit 2; fi
            RATE_LIMIT=$1; shift ;;
        --clamp-min)
            shift
            if [ $# -eq 0 ]; then err "--clamp-min needs a value"; exit 2; fi
            CLAMP_MIN=$1; shift ;;
        --clamp-max)
            shift
            if [ $# -eq 0 ]; then err "--clamp-max needs a value"; exit 2; fi
            CLAMP_MAX=$1; shift ;;
        --backup-dir)
            shift
            if [ $# -eq 0 ]; then err "--backup-dir needs a value"; exit 2; fi
            OUT_DIR=$1; shift ;;
        --out)
            shift
            if [ $# -eq 0 ]; then err "--out needs a value"; exit 2; fi
            OUT_DIR=$1; shift ;;
        --backup-file)
            shift
            if [ $# -eq 0 ]; then err "--backup-file needs a value"; exit 2; fi
            BACKUP_FILE=$1; shift ;;
        --help|-h) usage; exit 0 ;;
        *) err "unknown option: $1 (try --help)"; exit 2 ;;
    esac
done

backup_dir() {
    if [ -n "$OUT_DIR" ]; then printf '%s
' "$OUT_DIR"; else printf '%s
' "$DEFAULT_BACKUP"; fi
}

is_integer() {
    case "$1" in
        ''|*[!0-9]*) return 1 ;;
        *) return 0 ;;
    esac
}

list_policies() {
    local found=0 dir
    for dir in /sys/devices/system/cpu/cpufreq/policy*; do
        [ -d "$dir" ] || continue
        printf '%s
' "$dir"
        found=1
    done
    if [ "$found" -eq 1 ]; then return 0; fi
    for dir in /sys/devices/system/cpu/cpu*/cpufreq; do
        [ -d "$dir" ] || continue
        realpath "$dir"
    done | sort -u
}

read_or() {
    local file=$1 fallback=$2 value
    if [ -r "$file" ]; then
        value=$(cat "$file" 2>/dev/null)
        if [ -n "$value" ]; then printf '%s
' "$value"; return 0; fi
    fi
    printf '%s
' "$fallback"
}

policy_value() {
    local policy=$1 name=$2
    read_or "$policy/$name" "-"
}

rate_limit_path() {
    printf '%s/schedutil/rate_limit_us
' "$1"
}

show_knob() {
    local path=$1
    if [ ! -e "$path" ]; then
        say "  absent      $path"
        return 0
    fi
    if [ -w "$path" ]; then
        say "  present rw  $path = $(read_or "$path" "?")"
    else
        say "  present ro  $path = $(read_or "$path" "?")"
    fi
}

has_schedutil() {
    local policy governor available
    for policy in $(list_policies); do
        governor=$(policy_value "$policy" scaling_governor)
        available=$(policy_value "$policy" scaling_available_governors)
        if [ "$governor" = schedutil ]; then return 0; fi
        case " $available " in
            *" schedutil "*) return 0 ;;
        esac
    done
    return 1
}

show_mode() {
    local policies count policy driver governor cur max min rl
    policies=$(list_policies)
    count=0
    if [ -n "$policies" ]; then count=$(printf '%s
' "$policies" | grep -c .); fi

    say "== schedutil probe =="
    if [ -f /sys/fs/cgroup/cgroup.controllers ]; then
        say "cgroup v2: unified hierarchy present (/sys/fs/cgroup/cgroup.controllers)"
    else
        say "cgroup v2: NOT detected (cpuset placement needs cgroup v2 or plain affinity)"
    fi
    if [ "$count" -eq 0 ]; then
        say "cpufreq: no policy directories under /sys/devices/system/cpu/cpufreq"
        say "verdict: no CPUFreq policy => nothing to tune; schedutil knobs do not exist here."
        say "         schedplace still works: it only changes thread placement."
        return 0
    fi
    driver=$(policy_value "$(printf '%s
' "$policies" | head -n1)" scaling_driver)
    say "cpufreq: $count policy dir(s), scaling_driver=$driver"
    say ""
    printf '  %-10s %-14s %-13s %-10s %-10s %-10s %s
'         POLICY DRIVER GOVERNOR CUR MAX MIN RATE_LIMIT_US
    for policy in $policies; do
        driver=$(policy_value "$policy" scaling_driver)
        governor=$(policy_value "$policy" scaling_governor)
        cur=$(policy_value "$policy" scaling_cur_freq)
        max=$(policy_value "$policy" scaling_max_freq)
        min=$(policy_value "$policy" scaling_min_freq)
        if [ -e "$(rate_limit_path "$policy")" ]; then
            rl=$(read_or "$(rate_limit_path "$policy")" "-")
        else
            rl="n/a"
        fi
        printf '  %-10s %-14s %-13s %-10s %-10s %-10s %s
'             "$(basename "$policy")" "$driver" "$governor" "$cur" "$max" "$min" "$rl"
    done
    say ""
    say "global knobs (probed, never assumed):"
    show_knob /proc/sys/kernel/sched_util_clamp_min
    show_knob /proc/sys/kernel/sched_util_clamp_max
    show_knob /proc/sys/kernel/sched_util_clamp_min_rt_default
    show_knob /proc/sys/kernel/sched_energy_aware
    show_knob /proc/sys/kernel/sched_rt_runtime_us
    say ""

    if has_schedutil; then
        say "verdict: schedutil is available."
        say "         headroom knobs are opt-in: pass explicit values to --apply."
        say "         e.g. --apply --rate-limit-us N (N is SoC/workload specific, no default)."
    else
        say "verdict: schedutil is NOT available on this host."
        say "         present governors:"
        for policy in $policies; do
            say "           $(basename "$policy"): $(policy_value "$policy" scaling_available_governors)"
        done
        say "         reason: this platform uses a different cpufreq driver (e.g. amd-pstate/"
        say "                 intel-pstate) or a fixed governor; tuning schedutil/rate_limit_us"
        say "                 would write nowhere. Nothing to do, exit 0."
    fi
    return 0
}

plan_add() {
    printf '%s|%s
' "$1" "$2" >>"$PLAN_FILE"
}

ensure_backup_dir() {
    local dir
    dir=$(backup_dir)
    if ! mkdir -p "$dir" 2>/dev/null; then
        err "cannot create the backup directory $dir (use --out DIR for a writable location)"
        return 1
    fi
    if [ ! -w "$dir" ]; then
        err "backup directory $dir is not writable (use --out DIR)"
        return 1
    fi
    printf '%s
' "$dir"
    return 0
}

apply_mode() {
    local policies policy target value dir stamp backup path old failures writes
    if [ -z "$RATE_LIMIT" ] && [ -z "$CLAMP_MIN" ] && [ -z "$CLAMP_MAX" ]; then
        say "nothing requested: schedplace never writes a guessed value."
        say "usage: $PROG --apply --rate-limit-us N [--clamp-min N] [--clamp-max N]"
        say "run '$PROG --show' first to see which knobs actually exist here."
        return 0
    fi
    if [ -n "$RATE_LIMIT" ] && ! is_integer "$RATE_LIMIT"; then
        err "--rate-limit-us must be an integer number of microseconds"; exit 2
    fi
    if [ -n "$CLAMP_MIN" ] && ! is_integer "$CLAMP_MIN"; then
        err "--clamp-min must be an integer"; exit 2
    fi
    if [ -n "$CLAMP_MAX" ] && ! is_integer "$CLAMP_MAX"; then
        err "--clamp-max must be an integer"; exit 2
    fi

    PLAN_FILE=$(mktemp) || { err "cannot create a plan file"; exit 1; }
    policies=$(list_policies)
    if [ -n "$RATE_LIMIT" ]; then
        for policy in $policies; do
            target=$(rate_limit_path "$policy")
            if [ -e "$target" ]; then
                plan_add "$target" "$RATE_LIMIT"
            else
                SKIPPED=$((SKIPPED + 1))
                warn "SKIP $target (this kernel/policy does not expose schedutil/rate_limit_us)"
            fi
        done
    fi
    if [ -n "$CLAMP_MIN" ]; then
        target=/proc/sys/kernel/sched_util_clamp_min
        if [ -e "$target" ]; then plan_add "$target" "$CLAMP_MIN"
        else SKIPPED=$((SKIPPED + 1)); warn "SKIP $target (not present: CONFIG_UCLAMP_TASK off?)"; fi
    fi
    if [ -n "$CLAMP_MAX" ]; then
        target=/proc/sys/kernel/sched_util_clamp_max
        if [ -e "$target" ]; then plan_add "$target" "$CLAMP_MAX"
        else SKIPPED=$((SKIPPED + 1)); warn "SKIP $target (not present: CONFIG_UCLAMP_TASK off?)"; fi
    fi

    writes=$(printf '%s
' "$(grep -c . "$PLAN_FILE" 2>/dev/null || true)")
    if [ -z "$writes" ] || [ "$writes" -eq 0 ]; then
        err "no writable target for the requested values ($SKIPPED skipped)"
        exit 1
    fi

    dir=$(ensure_backup_dir) || exit 1
    stamp=$(date +%Y%m%dT%H%M%S)
    backup=$dir/schedutil-$stamp.backup
    : >"$backup" || { err "cannot write $backup"; exit 1; }
    say "== plan =="
    while IFS='|' read -r path value; do
        [ -n "$path" ] || continue
        old=$(read_or "$path" "?")
        printf '%s|%s
' "$path" "$old" >>"$backup"
        say "  $path: $old -> $value"
    done <"$PLAN_FILE"
    say "backup written to $backup"

    if [ "$ASSUME_YES" -ne 1 ]; then
        if [ ! -t 0 ]; then
            err "refusing to write without confirmation on a non-interactive stdin (use --yes)"
            exit 1
        fi
        printf 'apply these %s write(s)? [y/N] ' "$writes"
        read -r answer
        case "$answer" in
            y|Y|yes|YES) ;;
            *) say "aborted by user; nothing written"; exit 0 ;;
        esac
    fi

    failures=0
    while IFS='|' read -r path value; do
        [ -n "$path" ] || continue
        if ! printf '%s' "$value" >"$path" 2>/dev/null; then
            err "write failed: $path (are you root? is the file writable?)"
            failures=$((failures + 1))
            continue
        fi
        if [ "$(read_or "$path" "?")" != "$value" ]; then
            err "verification failed: $path does not read back as $value"
            failures=$((failures + 1))
            continue
        fi
        say "  applied $path = $value"
    done <"$PLAN_FILE"
    if [ "$failures" -gt 0 ]; then
        err "$failures write(s) failed; backup kept at $backup"
        exit 1
    fi
    say "done: $writes write(s) applied, $SKIPPED skipped; restore with --restore"
    return 0
}

restore_mode() {
    local dir file path old restored failed
    dir=$(backup_dir)
    if [ -n "$BACKUP_FILE" ]; then
        file=$BACKUP_FILE
    else
        file=$(ls -1t "$dir"/schedutil-*.backup 2>/dev/null | head -n1)
    fi
    if [ -z "$file" ] || [ ! -f "$file" ]; then
        say "no backup found under $dir; nothing to restore (this host may never have been tuned)"
        return 0
    fi
    say "== restore from $file =="
    restored=0
    failed=0
    while IFS='|' read -r path old; do
        [ -n "$path" ] || continue
        if [ ! -e "$path" ]; then
            warn "skip $path (node no longer exists)"
            continue
        fi
        if printf '%s' "$old" >"$path" 2>/dev/null && [ "$(read_or "$path" "?")" = "$old" ]; then
            say "  restored $path = $old"
            restored=$((restored + 1))
        else
            err "cannot restore $path (needs root?)"
            failed=$((failed + 1))
        fi
    done <"$file"
    if [ "$failed" -gt 0 ]; then
        err "$failed restore(s) failed"
        exit 1
    fi
    say "done: $restored value(s) restored"
    return 0
}

case "$MODE" in
    show) show_mode ;;
    apply) apply_mode ;;
    restore) restore_mode ;;
    *) err "internal: unknown mode $MODE"; exit 2 ;;
esac
