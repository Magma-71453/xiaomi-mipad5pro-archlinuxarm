#!/usr/bin/env bash
# =============================================================================
# fetch-gpu-firmware.sh — 下载 elish（SM8250 / Adreno 650）initramfs 早期加载所需的 GPU 固件
#
#   为什么需要：CONFIG_DRM_MSM=y 在内核早期（rootfs 挂载前，实测 ~1.4s）请求
#   qcom/a650_gmu.bin、qcom/a650_sqe.fw 与 DT 指定的
#   qcom/sm8250/xiaomi/elish/a650_zap.mbn；initramfs 缺这些文件时 request_firmware
#   返回 -2，GPU/DRM 起不来（bug.md B-058；2026-10-24 真机记录）。
#
#   来源：
#     qcom/a650_gmu.bin、qcom/a650_sqe.fw
#       linux-firmware（kernel.org cgit，二进制原文件）
#     qcom/sm8250/xiaomi/elish/a650_zap.mbn
#       lujianhua/xiaomi-elish-firmware（elish 内核维护者的设备固件镜像）
#
#   输出：默认 <repo>/../.build/firmware/elish-gpu/（可用 FW_DIR 或 --out 覆盖）。
#   每个文件下载后按 sha256 校验，不匹配即退出。
#
# 用法:
#   scripts/fetch-gpu-firmware.sh [--out DIR] [--force]
#   FW_DIR=/path/to/dir scripts/fetch-gpu-firmware.sh
# =============================================================================
set -euo pipefail

REPO_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BUILD=${BUILD:-$(cd "$REPO_DIR/.." && pwd)/.build}
FW_DIR=${FW_DIR:-$BUILD/firmware/elish-gpu}
FORCE=0

usage() { sed -n '2,27p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }
log() { printf '[fetch-gpu-firmware] %s\n' "$*"; }

while [ $# -gt 0 ]; do
    case "$1" in
        --out)   FW_DIR=${2:-}; shift 2 ;;
        --force) FORCE=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) die "unknown argument: $1 (try --help)" ;;
    esac
done

command -v curl >/dev/null 2>&1 || die "缺少 curl"
command -v sha256sum >/dev/null 2>&1 || die "缺少 sha256sum"
mkdir -p "$FW_DIR"

# 相对路径 | URL | sha256（上游二进制；校验失败说明源文件被改动，需人工确认）
FILES=(
  "qcom/a650_gmu.bin|https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/plain/qcom/a650_gmu.bin|df79fa8482fcc7587bb239b9c9e2b24bce4729893b990fe3f82b30da8175b01c"
  "qcom/a650_sqe.fw|https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/plain/qcom/a650_sqe.fw|c863ceac3a823af215205d7803776bfdc68098c2c2a5d2fd05b4aa77cd336b4b"
  "qcom/sm8250/xiaomi/elish/a650_zap.mbn|https://raw.githubusercontent.com/lujianhua/xiaomi-elish-firmware/master/sm8250/a650_zap.mbn|eafea874e09a0f3a12050d34590ba6c9b787cdee1c9df2fefa468c740dc8e2ad"
)

for entry in "${FILES[@]}"; do
    IFS='|' read -r rel url want <<< "$entry"
    dest="$FW_DIR/$rel"
    if [ "$FORCE" -eq 0 ] && [ -s "$dest" ]; then
        got=$(sha256sum "$dest" | cut -d' ' -f1)
        if [ "$got" = "$want" ]; then
            log "复用 $dest (sha256 ok)"
            continue
        fi
    fi
    mkdir -p "$(dirname "$dest")"
    log "下载 $url"
    tmp="$dest.part"
    curl -fL --retry 3 --retry-delay 3 -o "$tmp" "$url" || die "下载失败: $url"
    got=$(sha256sum "$tmp" | cut -d' ' -f1)
    if [ "$got" != "$want" ]; then
        rm -f "$tmp"
        die "sha256 不匹配: $rel got=$got want=$want"
    fi
    mv "$tmp" "$dest"
    log "ok $dest ($(stat -c%s "$dest") bytes)"
done

log "完成: $FW_DIR"
find "$FW_DIR" -type f -printf '  %s  %p\n' 2>/dev/null | sort -k2
