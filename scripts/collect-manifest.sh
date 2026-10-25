#!/usr/bin/env bash
# collect-manifest.sh — 汇总一次构建的全部产物，生成可刷入清单与机器可校验的校验和文件。
#
# 为什么需要它：
#   每个模块只写自己的 MANIFEST.<module> 片段；刷入前需要一份"唯一清单"，并且必须与磁盘实际
#   字节一致（子代理自报的哈希曾经在重建后变旧，本脚本对此做交叉校验并报警）。
#
# 用法: scripts/collect-manifest.sh [--date YYYYMMDD] [--check-only]
#   默认在 output/<date>/ 生成 MANIFEST 与 SHA256SUMS；--check-only 只校验不写。
set -euo pipefail
REPO_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
DATE=$(date -u +%Y%m%d); CHECK_ONLY=0
while [ $# -gt 0 ]; do
  case "$1" in
    --date) DATE=$2; shift 2 ;;
    --check-only) CHECK_ONLY=1; shift ;;
    -h|--help) sed -n '2,12p' "${BASH_SOURCE[0]}"; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done
OUT="$REPO_ROOT/output/$DATE"
[ -d "$OUT" ] || { echo "no output dir: $OUT" >&2; exit 1; }

# 产物 -> 目标分区（"embedded"/"-" 表示不是独立分区镜像）
ENTRIES="
kernel/Image.gz|boot_b
kernel/dtbs/qcom/sm8250-xiaomi-elish-boe.dtb|boot_b
kernel/dtbs/qcom/sm8250-xiaomi-elish-csot.dtb|boot_b
kernel/modules.tar.zst|linux
kernel/config-7.2.0_git20261010|-
boot/boot.img|boot_b
boot/boot-boe-alt.img|boot_b
boot/initramfs.cpio.gz|embedded
rootfs/linux-rootfs-f2fs.img|linux
rootfs/ArchLinuxARM-aarch64-elish-rootfs.tar.zst|linux
mango/mango-0.18.3-aarch64.tar.zst|linux
"

fail=0; rows=""; sums=""; missing=""
while IFS='|' read -r rel target; do
  [ -z "${rel:-}" ] && continue
  f="$OUT/$rel"
  if [ ! -f "$f" ]; then missing="$missing $rel"; continue; fi
  sha=$(sha256sum "$f" | cut -d' ' -f1)
  size=$(stat -c%s "$f")
  rows="$rows$rel | $target | $sha | $size | $(date -r "$f" -u +%Y-%m-%dT%H:%M:%SZ)"$'\n'
  sums="$sums$sha  $rel"$'\n'
  printf '%s  %s\n' "$sha" "$rel" > "$f.sha256"
done <<< "$ENTRIES"

# 交叉校验各模块自报的片段（避免"清单里的哈希不是磁盘上的字节"）
while IFS= read -r frag; do
  [ -z "$frag" ] && continue
  while IFS= read -r line; do
    case "$line" in \#*|"") continue ;; esac
    claimed=$(printf '%s' "$line" | grep -oE '[0-9a-f]{64}' | head -1)
    file=$(printf '%s' "$line" | tr '|' ' ' | awk '{print $1}')
    [ -z "$claimed" ] && continue
    [ -z "$file" ] && continue
    base=$(basename "$file")
    real=$(find "$REPO_ROOT" -type f -name "$base" -print -quit 2>/dev/null || true)
    if [ -n "$real" ]; then
      actual=$(sha256sum "$real" | cut -d' ' -f1)
      if [ "$claimed" != "$actual" ]; then
        echo "MISMATCH $frag: $file claimed ${claimed:0:12}… actual ${actual:0:12}…" >&2
        fail=$((fail+1))
      fi
    else
      echo "MISSING  $frag: $file (listed but absent on disk)" >&2
      fail=$((fail+1))
    fi
  done < "$frag"
done < <(find "$OUT" -maxdepth 1 -name 'MANIFEST.*' -type f | sort)

if [ -n "$missing" ]; then echo "NOT-YET-BUILT:$missing" >&2; fi
if [ "$fail" -gt 0 ]; then echo "fragment cross-check: $fail problem(s)" >&2; exit 1; fi
if [ "$CHECK_ONLY" -eq 1 ]; then echo "check-only: fragments consistent with disk"; exit 0; fi

{
  echo "# MANIFEST — $DATE (由 scripts/collect-manifest.sh 生成，唯一产物清单)"
  echo "# 产物只允许写入 boot_b（内核/引导）与 linux（根文件系统）两个分区"
  echo "# 文件 | 目标分区 | SHA256 | 大小(bytes) | 文件时间(UTC)"
  printf '%s' "$rows"
  echo "# 对应校验: cd output/$DATE && sha256sum -c SHA256SUMS"
  [ -n "$missing" ] && echo "# 尚未构建:$missing"
} > "$OUT/MANIFEST"
printf '%s' "$sums" > "$OUT/SHA256SUMS"
echo "wrote $OUT/MANIFEST and SHA256SUMS ($(printf '%s' "$rows" | grep -c . ) entries)"
cd "$OUT" && sha256sum -c SHA256SUMS | tail -3
