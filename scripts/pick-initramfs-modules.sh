#!/usr/bin/env bash
# =============================================================================
# pick-initramfs-modules.sh — 从一次内核构建的 modinstall 树里裁出 initramfs 需要的那一小撮模块
#
#   为什么需要它：
#     make modules_install 产出的是一整棵 lib/modules/<krel>（几百个 .ko.zst），
#     而 initramfs 只需要"面板驱动 + 它的依赖"（CONFIG_DRM_PANEL_*=m）。
#     build-initramfs.sh --modules 接受的正是 lib/modules/<krel> 的子集，
#     本脚本按 modules.dep 做依赖闭包，产出该子集并给出可直传的路径。
#
#   用法: scripts/pick-initramfs-modules.sh --modules-root DIR --dest DIR [--pattern RELDIR]...
#     --modules-root  modinstall 里的 .../lib/modules/<krel>（必须含 modules.dep）
#     --dest          子集落盘目录，产物为 <dest>/lib/modules/<krel>
#     --pattern       相对 modules-root 的子目录，可重复；缺省 kernel/drivers/gpu/drm/panel
#     --krel          覆盖 <krel>（缺省取 --modules-root 的 basename）
#
#   输出（机器可读，便于 CI 取值）：
#     pick.modules_root = <dest>/lib/modules/<krel>   ← 直接喂给 build-initramfs.sh --modules
#     pick.krel / pick.count / pick.missing_pattern
#   退出码: 0 = 至少挑到一个模块；1 = 一个都没挑到（面板驱动没编出来，属于硬失败）
# =============================================================================
set -euo pipefail

MODULES_ROOT=
DEST=
KREL=
PATTERNS=()

usage() { sed -n '2,25p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }
say() { printf '%s\n' "$*"; }

while [ $# -gt 0 ]; do
    case "$1" in
        --modules-root) MODULES_ROOT=${2:-}; shift 2 ;;
        --dest)         DEST=${2:-};         shift 2 ;;
        --pattern)      PATTERNS+=("${2:-}"); shift 2 ;;
        --krel)         KREL=${2:-};         shift 2 ;;
        -h|--help)      usage; exit 0 ;;
        *) die "unknown argument: $1" ;;
    esac
done

[ -n "$MODULES_ROOT" ] || die "--modules-root is required"
[ -d "$MODULES_ROOT" ] || die "--modules-root is not a directory: $MODULES_ROOT"
[ -f "$MODULES_ROOT/modules.dep" ] || die "no modules.dep under $MODULES_ROOT (run make modules_install first)"
[ -n "$DEST" ] || die "--dest is required"
[ "${#PATTERNS[@]}" -gt 0 ] || PATTERNS=(kernel/drivers/gpu/drm/panel)

KREL=${KREL:-$(basename "$MODULES_ROOT")}
OUT="$DEST/lib/modules/$KREL"
DEPFILE="$MODULES_ROOT/modules.dep"

# modules.dep 的一行形如 "<krel 内相对路径>: <依赖> <依赖>"
deps_of() { # $1=相对路径 -> 依赖相对路径（空格分隔，可能为空）
    awk -v key="$1:" 'index($0, key) == 1 { sub(/^[^:]*:[ ]*/, ""); print; exit }' "$DEPFILE"
}

declare -A SHIPPED=()
QUEUE=()

seed_from_pattern() { # $1=相对子目录
    local dir="$MODULES_ROOT/$1" found=0 entry rel
    [ -d "$dir" ] || { say "pick.missing_pattern = $1"; return 0; }
    while IFS= read -r entry; do
        rel="${entry#"$MODULES_ROOT"/}"
        QUEUE+=("$rel")
        found=$((found + 1))
    done < <(find "$dir" -type f \( -name '*.ko' -o -name '*.ko.zst' \) | LC_ALL=C sort)
    say "pick.pattern_files = $1:$found"
}

for pattern in "${PATTERNS[@]}"; do
    seed_from_pattern "$pattern"
done

# 依赖闭包：BFS，深度由 modules.dep 自身决定，不需要猜层数
head_index=0
while [ "$head_index" -lt "${#QUEUE[@]}" ]; do
    rel="${QUEUE[$head_index]}"
    head_index=$((head_index + 1))
    [ -n "${SHIPPED[$rel]:-}" ] && continue
    [ -f "$MODULES_ROOT/$rel" ] || { say "pick.absent_dep = $rel"; continue; }
    SHIPPED["$rel"]=1
    for dep in $(deps_of "$rel"); do
        [ -n "${SHIPPED[$dep]:-}" ] && continue
        QUEUE+=("$dep")
    done
done

COUNT=${#SHIPPED[@]}
[ "$COUNT" -gt 0 ] || die "no modules matched ${PATTERNS[*]} under $MODULES_ROOT"

# 落盘 + 重写 modules.dep（只保留真正打进去的行，/init 才不会对着几百个不存在的路径 insmod）
mkdir -p "$OUT"
for rel in "${!SHIPPED[@]}"; do
    mkdir -p "$OUT/$(dirname "$rel")"
    cp -p "$MODULES_ROOT/$rel" "$OUT/$rel"
done
WANT=$(mktemp)
trap 'rm -f "$WANT"' EXIT
printf '%s\n' "${!SHIPPED[@]}" | LC_ALL=C sort > "$WANT"
awk -v list="$WANT" 'BEGIN { while ((getline k < list) > 0) want[k] = 1 }
    { key = $0; sub(/:.*/, "", key); if (key in want) print }' "$DEPFILE" > "$OUT/modules.dep"

say "pick.krel = $KREL"
say "pick.count = $COUNT"
say "pick.decompressed_hint = .ko.zst 会由 build-initramfs.sh 解压成 .ko（busybox insmod 读不了 zstd）"
say "pick.modules_root = $OUT"
