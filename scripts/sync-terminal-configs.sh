#!/usr/bin/env bash
# =============================================================================
# sync-terminal-configs.sh — 把仓库里的 fastfetch / htop / btop 配置推到设备
#
#   真相源: configs/rootfs/home/magma/.config/{fastfetch,htop,btop}/
#   设备端: /home/magma/.config/{fastfetch,htop,btop}/
#
#   用法:
#     scripts/sync-terminal-configs.sh <device-ip> [user]
#     DEV_SSH_KEY=~/.ssh/dropbear_ed25519_host_key scripts/sync-terminal-configs.sh 192.168.x.90
#
#   连接走 dropbear 的 dbclient（本机没有 OpenSSH 客户端也能用）。
#   **对端登录 shell 是 fish**，所以远端命令一律包成 `sh -c '...'` 或从 stdin 喂 `bash -s`。
#
#   同步完自动做 sha256 比对；任何一个文件不一致即以非 0 退出。
#   详见 docs/terminal-tools.md。
# =============================================================================
set -euo pipefail

DEV_HOST="${1:?用法: $0 <device-ip> [user]}"
DEV_USER="${2:-magma}"
DEV_SSH_KEY="${DEV_SSH_KEY:-$HOME/.ssh/dropbear_ed25519_host_key}"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC="$REPO_ROOT/configs/rootfs/home/$DEV_USER"

FILES=(
  ".config/fastfetch/config.jsonc"
  ".config/htop/htoprc"
  ".config/btop/btop.conf"
  ".config/btop/themes/elish.theme"
)

dc() { dbclient -i "$DEV_SSH_KEY" -q "${DEV_USER}@${DEV_HOST}" "$@"; }

# --- 0. 前置检查 ------------------------------------------------------------
for f in "${FILES[@]}"; do
  [ -f "$SRC/$f" ] || { echo "缺少源文件: $SRC/$f" >&2; exit 1; }
done
[ -f "$DEV_SSH_KEY" ] || { echo "缺少密钥: $DEV_SSH_KEY" >&2; exit 1; }

echo "==> 目标 ${DEV_USER}@${DEV_HOST}"

# --- 1. 打包并推送（tar 保留相对路径，解包到 $HOME）------------------------
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

tar -czf "$TMP/terminal-configs.tar.gz" -C "$SRC" "${FILES[@]}"
echo "==> 打包 $(stat -c %s "$TMP/terminal-configs.tar.gz") 字节"

dc "sh -c 'mkdir -p \$HOME/.config/btop/themes'"
dc 'sh -c "cat > /tmp/terminal-configs.tar.gz"' < "$TMP/terminal-configs.tar.gz"
dc "sh -c 'cd \$HOME && tar -xzf /tmp/terminal-configs.tar.gz && rm -f /tmp/terminal-configs.tar.gz'"
echo "==> 已解包到 \$HOME"

# --- 2. 校验：远端 sha256 必须与本地一致 -----------------------------------
echo "==> 校验内容"
fail=0
for f in "${FILES[@]}"; do
  local_sum=$(sha256sum "$SRC/$f" | cut -d' ' -f1)
  if remote_sum=$(dc "sh -c 'sha256sum \$HOME/$f 2>/dev/null | cut -d\" \" -f1'"); then
    if [ "$local_sum" = "$remote_sum" ]; then
      printf '  ok   %s\n' "$f"
    else
      printf '  FAIL %s\n       本地 %s\n       远端 %s\n' "$f" "$local_sum" "$remote_sum" >&2
      fail=1
    fi
  else
    printf '  FAIL %s (远端不可读)\n' "$f" >&2
    fail=1
  fi
done

if [ "$fail" -ne 0 ]; then
  echo "==> 同步失败：至少一个文件与仓库不一致" >&2
  exit 1
fi

echo "==> 全部一致。重启对应程序即可看到新外观："
echo "      foot -e fastfetch"
echo "      foot -e htop"
echo "      foot -e btop"
