#!/usr/bin/env bash
# perf-audit-noise.sh — 审计调度噪声源：只探测 + 给建议，默认不改任何东西。
#
# 检查项：CPU 热插拔脚本/服务、core_ctl、msm_performance、sched_boost、
#         cpu_boost 输入 boost、task_boost、WALT 下游节点、sched_energy_aware、
#         sched_rt_runtime_us、以及 upmigrate/downmigrate 一类迁移阈值。
#
# 每一项输出：存在/不存在 + 路径 + 建议（保留/关闭）+ 预期收益 + 回退命令。
# 默认 print-only；只有 --apply 且逐项确认（y）时才会写入/禁用。
#
# 用法:
#   scripts/perf-audit-noise.sh             # 审计（默认，不修改）
#   scripts/perf-audit-noise.sh --apply     # 逐项确认后写入/禁用
#   scripts/perf-audit-noise.sh --help
#
# 退出码: 0 = 正常（无论是否存在噪声源），2 = 用法错误。

set -uo pipefail

PROG=$(basename "$0")
APPLY=0
CHECKS_FILE=""
CHANGED=0
PRESENT=0
ABSENT=0
INDEX=0

say() { printf '%s
' "$*"; }
warn() { printf '%s: warn: %s
' "$PROG" "$*" >&2; }
err() { printf '%s: error: %s
' "$PROG" "$*" >&2; }

usage() {
    sed -n '3,20p' "$0" | sed 's/^# \{0,1\}//'
}

cleanup() {
    if [ -n "$CHECKS_FILE" ] && [ -f "$CHECKS_FILE" ]; then rm -f "$CHECKS_FILE"; fi
}
trap cleanup EXIT

while [ $# -gt 0 ]; do
    case "$1" in
        --apply) APPLY=1; shift ;;
        --help|-h) usage; exit 0 ;;
        *) err "unknown option: $1 (try --help)"; exit 2 ;;
    esac
done

CHECKS_FILE=$(mktemp) || { err "cannot create a temporary file"; exit 1; }
cat >"$CHECKS_FILE" <<'CHECKS_EOF'
cpu-hotplug|glob|/etc/systemd/system/*hotplug*.service /usr/lib/systemd/system/*hotplug*.service /etc/udev/rules.d/*hotplug* /etc/init.d/*hotplug*|关闭|热插拔会在运行期改变在线 CPU 集合，cpuset 掩码与 cpu_capacity 拓扑随之抖动（可能出现空簇）；本方案要求拓扑稳定|systemctl disable --now <unit>；或删除对应 udev 规则后 udevadm control --reload|
msm-performance|path|/sys/module/msm_performance /sys/kernel/msm_performance /etc/systemd/system/msm_performance.service /usr/lib/systemd/system/msm_performance.service|关闭|厂商性能 HINT 直接把频率/核数拉到上限，覆盖 schedutil 与 EAS 的决策，同时抬高功耗与发热|rmmod msm_performance；systemctl disable --now msm_performance|
core-ctl|path|/sys/module/core_ctl /sys/devices/system/cpu/cpu0/core_ctl /sys/devices/system/cpu/cpu4/core_ctl|关闭|core_ctl 按负载自行停核/开核，与 EAS 的 CPU 选择重复，且会让小核簇在扫描间隔内忽大忽小|写入 0 到 <core_ctl>/enable（回退：写入 1）|0
sched-boost|path|/proc/sys/kernel/sched_boost /sys/kernel/sched_boost /sys/module/sched_boost|关闭|sched_boost 会给命中任务加权并整体上拉，破坏“只改放置、不改优先级”的边界|写入 0 到 /proc/sys/kernel/sched_boost|0
cpu-input-boost|path|/sys/module/cpu_boost/parameters/input_boost_enabled /sys/module/cpu_boost/parameters/input_boost_freq|保留（先测再决定）|输入 boost 周期性把频率顶到上限：功耗与发热明显，但能掩盖真实输入延迟，使 A/B 失真|写入 0 到 input_boost_enabled（回退：写入 1）|0
task-boost|path|/sys/module/task_boost /sys/kernel/task_boost /proc/sys/kernel/task_boost|关闭|按任务名直接加权，等于绕过 schedplace 的规则表，A/B 结果无法归因|写入 0 到对应 enable 节点（视厂商实现）|0
walt-downstream|path|/proc/sys/kernel/sched_walt_rotate /sys/kernel/debug/sched_features|保留（不要开启）|WALT 是安卓/厂商下游补丁，主线 EAS 没有这些接口；其窗口数值不可迁移，开启只会让两组测量不可比|保持缺省；若存在 debugfs 则勿改 sched_features|
migrate-thresholds|path|/proc/sys/kernel/sched_upmigrate /proc/sys/kernel/sched_downmigrate /proc/sys/kernel/sched_group_upmigrate /proc/sys/kernel/sched_group_downmigrate|不可调（主线 EAS 无此接口）|迁移阈值由内核 EAS/schedutil 内部决定；套用安卓数值在主线上既没有节点也没有意义|无需回退：不存在即不可写|
migration-cost-ns|path|/proc/sys/kernel/sched_migration_cost_ns|保留|这是 CFS 的启发式窗口，不是 EAS 的上下迁移阈值；改它只会让测量不可比，不能当安卓数值用|保持缺省；不需要回退|
sched-energy-aware|path|/proc/sys/kernel/sched_energy_aware|保留|EAS 总开关；关掉会退化为纯负载均衡，能效与放置目标一起失效|若被关闭：写入 1|
sched-rt-runtime|path|/proc/sys/kernel/sched_rt_runtime_us|保留|实时带宽限制是音频/合成线程的安全网，改动会引入爆音或卡顿，不属于放置范畴|保持缺省（通常 950000/1000000）|
android-postboot|path|/vendor/bin/init.qcom.post_boot.sh /vendor/etc/init/hw/init.qcom.post_boot.sh /system/etc/init/hw/init.qcom.post_boot.sh|关闭（在设备上）|post_boot 会在开机时写 governor/boost/core_ctl，覆盖 schedplace 的放置前提；建议先审计它写了什么再决定|在设备上停用对应 init 服务并重启后复查|
CHECKS_EOF

total=0
while IFS='|' read -r id rest; do
    [ -n "$id" ] || continue
    total=$((total + 1))
done <"$CHECKS_FILE"

collect_matches() {
    local candidates=$1 candidate
    for candidate in $candidates; do
        [ -e "$candidate" ] || continue
        printf '%s
' "$candidate"
    done
}

confirm() {
    local prompt=$1 answer
    printf '%s [y/N] ' "$prompt"
    read -r answer
    case "$answer" in
        y|Y|yes|YES) return 0 ;;
        *) return 1 ;;
    esac
}

apply_one() {
    local kind=$1 match=$2 suggest=$3
    if [ "$(id -u)" -ne 0 ]; then
        warn "需要 root 才能修改 $match（当前 uid=$(id -u)），已跳过"
        return 0
    fi
    case "$kind" in
        glob)
            if ! confirm "对 $match 执行 systemctl disable --now ?"; then
                say "  已跳过（用户拒绝）"
                return 0
            fi
            if systemctl disable --now "$(basename "$match")" 2>/dev/null; then
                CHANGED=$((CHANGED + 1))
                say "  已执行: systemctl disable --now $(basename "$match")"
            else
                warn "systemctl 失败: $match"
            fi
            ;;
        path)
            if [ -z "$suggest" ]; then
                say "  该项只审计不修改（没有安全的自动写入值）"
                return 0
            fi
            if [ ! -w "$match" ]; then
                warn "$match 不可写（root? 或该节点只读），已跳过"
                return 0
            fi
            if ! confirm "写入 '$suggest' 到 $match ?"; then
                say "  已跳过（用户拒绝）"
                return 0
            fi
            if printf '%s' "$suggest" >"$match" 2>/dev/null; then
                CHANGED=$((CHANGED + 1))
                say "  已写入: $match = $suggest"
            else
                warn "写入失败: $match"
            fi
            ;;
        *) say "  该项仅提供建议，不自动修改" ;;
    esac
}

say "== perf-audit-noise =="
say "host: $(uname -srm) / $(nproc) CPUs / kernel cmdline 噪声项不可见时属正常"
say "mode: $([ "$APPLY" -eq 1 ] && printf 'apply（逐项确认）' || printf 'print-only（不修改任何文件）')"
say "checks: $total"

while IFS='|' read -r id kind candidates advice benefit rollback suggest; do
    [ -n "$id" ] || continue
    INDEX=$((INDEX + 1))
    matches=$(collect_matches "$candidates")
    if [ -n "$matches" ]; then
        PRESENT=$((PRESENT + 1))
        status="存在"
        detail=$(printf '%s
' "$matches" | tr '
' ' ')
    else
        ABSENT=$((ABSENT + 1))
        status="不存在"
        detail=$candidates
    fi
    say ""
    say "== [$INDEX/$total] $id =="
    say "  状态    : $status"
    say "  路径    : $detail"
    say "  建议    : $advice"
    say "  预期收益: $benefit"
    say "  回退    : $rollback"
    if [ "$id" = migrate-thresholds ]; then
        if [ -z "$matches" ]; then
            say "  结论    : 不可调，勿套用安卓数值（主线 EAS 没有 upmigrate/downmigrate 这类阈值节点）"
        else
            say "  结论    : 这是下游补丁节点；数值带厂商 patch 语义，不要跨内核套用"
        fi
    fi
    if [ "$APPLY" -eq 1 ] && [ -n "$matches" ]; then
        while IFS= read -r match; do
            [ -n "$match" ] || continue
            apply_one "$kind" "$match" "$suggest"
        done <<MATCHES_EOF
$matches
MATCHES_EOF
    fi
done <"$CHECKS_FILE"

say ""
say "== 目标机（Android / SM8250）常见路径：宿主上不存在属预期，需在设备上运行本脚本 =="
for device_path in /vendor/bin/init.qcom.post_boot.sh /vendor/etc/init/hw/init.qcom.post_boot.sh /sys/kernel/msm_performance /sys/devices/system/cpu/cpu0/core_ctl; do
    if [ -e "$device_path" ]; then
        say "  存在    $device_path"
    else
        say "  不存在  $device_path"
    fi
done

say ""
if [ "$APPLY" -eq 1 ]; then
    say "summary: $total 项检查，存在 $PRESENT 项，缺省 $ABSENT 项，已修改 $CHANGED 项"
else
    say "summary: $total 项检查，存在 $PRESENT 项，缺省 $ABSENT 项，未做任何修改（print-only）"
fi
exit 0
