#!/usr/bin/env bash
# =============================================================================
# build-release.sh — 本机构建 boot / rootfs / mango 并推送到 GitHub Release
#
#   一次跑完：内核 -> GPU 固件 -> initramfs（面板驱动 + GPU 固件 + 首启扩容工具）
#             -> CSOT/BOE 两份 boot.img
#             -> 打包 output/<date>/ 里已有的 rootfs / mango / INSTALL.md -> 自校验
#             -> output/<YYYYMMDD>/（仓库约定 + MANIFEST）-> gh release（标签 yy/mm/dd）
#
#   产物落位遵循 AGENTS.md 硬约束：构建产物只放 output/<YYYYMMDD>/ 并写 MANIFEST；
#   中间树全部在 gitignore 掉的 .build/ 里。
#
# 用法:
#   scripts/build-release.sh                 # 构建 + 发布（标签 = 今天 yy/mm/dd）
#   scripts/build-release.sh --no-publish    # 只构建，产出 output/<日期>/ 与 .build/dist/
#   scripts/build-release.sh --skip-kernel   # 复用已有内核，只重打 initramfs / boot.img
#   scripts/build-release.sh --tag 26/11/08  # 指定 Release 标签（必须 yy/mm/dd）
#
# 选项:
#   --tag TAG         Release 标签与标题，yy/mm/dd（默认今天，时区 Asia/Shanghai）
#   --date YYYYMMDD   output/ 目录名（默认今天）
#   --jobs N          编译并行度（默认 min(nproc,8)，防止大核机吃爆内存）
#   --no-publish      不发 Release，只出产物
#   --skip-kernel     跳过内核编译（要求 output/<日期>/kernel 已有 Image.gz + 两个 dtb）
#   --repo OWNER/REPO 发布目标仓库（默认取 git remote origin）
#   --build-dir DIR   中间树目录（默认仓库上一级 .build，不能放仓库内）
#   FW_DIR=DIR        GPU 固件目录（默认 <build-dir>/firmware/elish-gpu；缺失时自动下载）
#   EXTRA_DIR=DIR     initramfs 额外文件树（默认 <build-dir>/initramfs-extra；放 f2fs-tools/glibc，首启扩容用）
#   -h, --help
#
# 依赖：clang lld llvm-* bc dtc flex bison pahole make python3 perl zstd cpio
#       bsdtar curl gzip；发布另需 gh 且已 gh auth login
# =============================================================================
set -euo pipefail

REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
# 中间树默认放仓库**外**（../.build），与 build-kernel.sh / build-initramfs.sh 一致：
# scripts/audit-hygiene.sh 会把仓库内的 .build/ 判成产物污染。需要换位置用 --build-dir。
BUILD=${BUILD:-$(cd "$REPO/.." && pwd)/.build}
DATE=${DATE:-$(TZ=Asia/Shanghai date +%Y%m%d)}
TAG=${TAG:-$(TZ=Asia/Shanghai date +%y/%m/%d)}
JOBS=${JOBS:-}
PUBLISH=1
SKIP_KERNEL=0
SLUG=${SLUG:-}
FW_DIR=${FW_DIR:-}

OUT=$REPO/output/$DATE
KOUT=$OUT/kernel
BOUT=$OUT/boot
DIST=$BUILD/dist
LOGS=$BUILD/logs
FWDIR=${FW_DIR:-$BUILD/firmware/elish-gpu}

usage() { sed -n '2,31p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; }
die()   { printf 'error: %s\n' "$*" >&2; exit 1; }
log()   { printf '[build-release] %s\n' "$*"; }
stage() { printf '\n[build-release] ===== %s =====\n' "$*"; }

while [ $# -gt 0 ]; do
    case "$1" in
        --tag)         TAG=${2:-}; shift 2 ;;
        --date)        DATE=${2:-}; OUT=$REPO/output/$DATE; KOUT=$OUT/kernel; BOUT=$OUT/boot; shift 2 ;;
        --jobs)        JOBS=${2:-}; shift 2 ;;
        --repo)        SLUG=${2:-}; shift 2 ;;
        --build-dir)   BUILD=${2:-}; shift 2 ;;
        --no-publish)  PUBLISH=0; shift ;;
        --skip-kernel) SKIP_KERNEL=1; shift ;;
        -h|--help)     usage; exit 0 ;;
        *) die "unknown argument: $1 (try --help)" ;;
    esac
done

printf '%s' "$TAG" | grep -Eq '^[0-9]{2}/[0-9]{2}/[0-9]{2}$' \
    || die "Release 标签必须是 yy/mm/dd，收到: $TAG"
[ -n "$JOBS" ] || JOBS=$(( $(nproc) < 8 ? $(nproc) : 8 ))
# --build-dir 只改 BUILD，DIST/LOGS 必须跟着重算（否则还指着旧的默认目录）
DIST=$BUILD/dist
LOGS=$BUILD/logs
# 中间树不允许落在仓库内：scripts/audit-hygiene.sh 会把仓库里的 .build/ 判成产物污染
case "$(realpath -m "$BUILD")/" in
    "$(realpath -m "$REPO")"/*)
        die "build-dir 不能放在仓库内（audit-hygiene 会判成产物污染）: $BUILD" ;;
esac
mkdir -p "$BUILD" "$LOGS" "$KOUT" "$BOUT"

# ------------------------------------------------------------------ 前置检查 --
need_cmd() {
    local c missing=""
    for c in "$@"; do command -v "$c" >/dev/null 2>&1 || missing="$missing $c"; done
    [ -z "$missing" ] || die "缺少命令:$missing（Arch: pacman -S clang llvm bc dtc pahole cpio libarchive zstd）"
}

stage "前置检查"
need_cmd python3 curl bsdtar cpio zstd gzip sha256sum git
if [ "$SKIP_KERNEL" -eq 0 ]; then
    need_cmd clang ld.lld llvm-ar llvm-nm llvm-objcopy bc dtc flex bison make perl pahole
fi
if [ "$PUBLISH" -eq 1 ]; then
    need_cmd gh
    gh auth status >/dev/null 2>&1 \
        || die "gh 未登录：先 gh auth login（或 export GH_TOKEN=<token>）；只构建请加 --no-publish"
    if [ -z "$SLUG" ]; then
        SLUG=$(git -C "$REPO" config --get remote.origin.url \
               | sed -e 's#^git@[^:]*:##' -e 's#^https\?://[^/]*/##' -e 's#\.git$##')
    fi
    [ -n "$SLUG" ] || die "取不到发布目标仓库，用 --repo OWNER/REPO 指定"
    log "发布目标: $SLUG   标签: $TAG"
fi
log "并行度 JOBS=$JOBS   output=$OUT"

# --------------------------------------------------------------- 1) 内核编译 --
if [ "$SKIP_KERNEL" -eq 1 ]; then
    stage "跳过内核编译（--skip-kernel）"
    for f in "$KOUT/Image.gz" \
             "$KOUT/dtbs/qcom/sm8250-xiaomi-elish-csot.dtb" \
             "$KOUT/dtbs/qcom/sm8250-xiaomi-elish-boe.dtb"; do
        [ -f "$f" ] || die "--skip-kernel 但缺少已有产物: $f"
    done
else
    stage "交叉编译内核（LLVM=1）"
    ROOT_DIR=$REPO BUILD_DIR=$BUILD/kbuild DL_DIR=$BUILD/dl \
    OUT_DIR=$KOUT MANIFEST=$OUT/MANIFEST.kernel JOBS=$JOBS \
        bash "$REPO/scripts/build-kernel.sh"
fi

# --------------------------------------------------- 2) 挑 initramfs 要的模块 --
# 面板驱动是模块（CONFIG_DRM_PANEL_*=m），必须连依赖一起放进 initramfs，
# 否则控制台要等 rootfs 的 udev 加载模块才出图。按 modules.dep 做依赖闭包。
pick_modules() {   # $1=modinstall 的 lib/modules/<krel>   $2=落盘根；末行输出可用路径
    local root=$1 base=$2 krel depfile rel dep i=0
    krel=$(basename "$root")
    depfile=$root/modules.dep
    [ -f "$depfile" ] || die "没有 modules.dep: $root（内核没跑 modules_install？）"
    local -A seen=()
    local -a queue=()
    while IFS= read -r rel; do
        queue+=("${rel#"$root"/}")
    done < <(find "$root/kernel/drivers/gpu/drm/panel" -type f \
                  \( -name '*.ko' -o -name '*.ko.zst' \) 2>/dev/null | LC_ALL=C sort)
    [ "${#queue[@]}" -gt 0 ] || die "kernel/drivers/gpu/drm/panel 下没有模块，检查 CONFIG_DRM_PANEL_*"
    while [ "$i" -lt "${#queue[@]}" ]; do
        rel=${queue[$i]}; i=$((i + 1))
        [ -n "${seen[$rel]:-}" ] && continue
        [ -f "$root/$rel" ] || continue
        seen[$rel]=1
        while IFS= read -r dep; do
            [ -n "$dep" ] && queue+=("$dep")
        done < <(awk -v k="$rel:" 'index($0, k) == 1 { sub(/^[^:]*:[ ]*/, ""); n = split($0, a, " "); for (i = 1; i <= n; i++) print a[i]; exit }' "$depfile")
    done
    local dest=$base/lib/modules/$krel
    mkdir -p "$dest"
    for rel in "${!seen[@]}"; do
        mkdir -p "$dest/$(dirname "$rel")"
        cp -p "$root/$rel" "$dest/$rel"
    done
    printf '%s\n' "${!seen[@]}" | LC_ALL=C sort > "$base/.modlist"
    awk -v list="$base/.modlist" '
        BEGIN { while ((getline k < list) > 0) want[k] = 1 }
        { key = $0; sub(/:.*/, "", key); if (key in want) print }' "$depfile" > "$dest/modules.dep"
    rm -f "$base/.modlist"
    log "面板模块子集: ${#seen[@]} 个 -> $dest"
    printf '%s\n' "$dest"
}

stage "裁剪 initramfs 模块"
KREL=$(basename "$(echo "$BUILD"/kbuild/modinstall/lib/modules/*)")
MODARG=$(pick_modules "$BUILD/kbuild/modinstall/lib/modules/$KREL" "$BUILD/modstage" | tail -1)
[ -d "$MODARG" ] || die "模块裁剪失败"

# --------------------------------------------------- 3) GPU 固件（initramfs） --
# Adreno a650 固件必须由 initramfs 提供：CONFIG_DRM_MSM=y 在内核早期（rootfs 挂载
# 之前，实测 ~1.4s）请求 qcom/a650_gmu.bin、qcom/a650_sqe.fw 与 DT 指定的
# qcom/sm8250/xiaomi/elish/a650_zap.mbn。缺文件时 request_firmware 返回 -2，
# GPU/DRM 起不来（bug.md B-058 / 2026-10-24 真机记录）。
stage "准备 GPU 固件"
firmware_complete() {
    local f
    for f in qcom/a650_gmu.bin qcom/a650_sqe.fw qcom/sm8250/xiaomi/elish/a650_zap.mbn; do
        [ -s "$FWDIR/$f" ] || return 1
    done
}
if ! firmware_complete; then
    stage "下载 GPU 固件 -> $FWDIR"
    FW_DIR=$FWDIR bash "$REPO/scripts/fetch-gpu-firmware.sh" \
        || die "GPU 固件下载失败；手动放到 $FWDIR（见 docs/flash-guide.md）后重试"
    firmware_complete || die "GPU 固件不完整: $FWDIR"
fi
log "GPU 固件: $FWDIR ($(find "$FWDIR" -type f | wc -l) files)"

# ------------------------------------------------------- 4) static busybox ----
BB=$BUILD/tools/busybox-static/root/usr/bin/busybox-static
if [ -x "$BB" ]; then
    log "复用已有 busybox: $BB"
else
    stage "下载 static busybox（aarch64）"
    URL=$(grep -m1 '^BUSYBOX_URL=' "$REPO/scripts/build-initramfs.sh" | cut -d= -f2-)
    [ -n "$URL" ] || die "scripts/build-initramfs.sh 里找不到 BUSYBOX_URL"
    tmp=$(mktemp -d)
    ( cd "$tmp"
      curl -fsSL --retry 3 --retry-delay 3 -o busybox.deb "$URL"
      bsdtar -xf busybox.deb data.tar.zst
      bsdtar -xf data.tar.zst
      mkdir -p "$(dirname "$BB")"
      install -m0755 usr/bin/busybox-static "$BB" )
    rm -rf "$tmp"
    [ -x "$BB" ] || die "busybox 安装失败: $BB"
    log "busybox: $BB ($(stat -c%s "$BB") bytes)"
fi

# ------------------------------------------------------------ 4) initramfs ----
stage "打 initramfs"
# 额外工具树：f2fs-tools（首次安装时在挂载 rootfs 前离线 resize.f2fs 扩容到整个 195 GiB）
# 及其依赖的 glibc 库。设备旧 initramfs 一直带这些文件，2026-11-04 重构建时漏掉过一次。
EXTRA_ARGS=()
EXTRA_DIR=${EXTRA_DIR:-$BUILD/initramfs-extra}
if [ -d "$EXTRA_DIR" ]; then
    EXTRA_ARGS=(--extra "$EXTRA_DIR")
    log "initramfs extra: $EXTRA_DIR ($(find "$EXTRA_DIR" -type f | wc -l) files)"
fi
BUILD_ROOT=$BUILD OUTPUT_DATE=$DATE bash "$REPO/scripts/build-initramfs.sh" \
    --busybox "$BB" --modules "$MODARG" --firmware "$FWDIR" "${EXTRA_ARGS[@]}" \
    --out "$BOUT/initramfs.cpio.gz"

# ------------------------------------------------ 5) 两份 boot.img + 自校验 --
stage "打 boot.img（CSOT / BOE）"
pack() {   # $1=dtb 文件名  $2=输出文件名  $3=boot header 里的 name（<=15 字节）
    local dtb=$KOUT/dtbs/qcom/$1 out=$BOUT/$2 hname=$3 base=${2%.img}
    [ -f "$dtb" ] || die "缺少 dtb: $dtb"
    python3 "$REPO/scripts/make-bootimg.py" \
        --kernel "$KOUT/Image.gz" \
        --ramdisk "$BOUT/initramfs.cpio.gz" \
        --dtb "$dtb" \
        --name "$hname" \
        --out "$out" > "$LOGS/$base.build.log"
    python3 "$REPO/scripts/make-bootimg.py" --verify --input "$out" > "$LOGS/$base.verify.log"
    grep -qx 'verify.status = OK' "$LOGS/$base.verify.log" \
        || die "$out 自校验失败，见 $LOGS/$base.verify.log"
    log "$(basename "$out"): $(stat -c%s "$out") bytes  sha256=$(sha256sum "$out" | cut -d' ' -f1)"
}

# 命名沿用仓库既有约定（docs/flash-guide.md、scripts/collect-manifest.sh）：boot.img = CSOT
pack sm8250-xiaomi-elish-csot.dtb boot.img         elish-csot
pack sm8250-xiaomi-elish-boe.dtb  boot-boe-alt.img elish-boe

# -------------------------------------------------- 6) 清单 + Release 资产包 --
stage "生成 MANIFEST / SHA256SUMS / Release 资产"
bash "$REPO/scripts/collect-manifest.sh" --date "$DATE" | tail -5

rm -rf "$DIST"; mkdir -p "$DIST"
ASSETS="boot.img boot-boe-alt.img initramfs.cpio.gz modules.tar.zst"
cp "$BOUT/boot.img"          "$DIST/boot.img"
cp "$BOUT/boot-boe-alt.img"  "$DIST/boot-boe-alt.img"
cp "$BOUT/initramfs.cpio.gz" "$DIST/initramfs.cpio.gz"
cp "$KOUT/modules.tar.zst"   "$DIST/modules.tar.zst"
cp "$LOGS"/*.verify.log      "$DIST/"

# rootfs / mango / 安装教程：先在 output/<date>/ 构建好，这里只负责打包。
# GitHub Release 单文件上限 2 GiB，f2fs 原始镜像以 zstd 压缩后上传（安装时先 zstd -d）。
ROOTFS_TAR=$OUT/rootfs/ArchLinuxARM-aarch64-elish-rootfs.tar.zst
ROOTFS_IMG=$OUT/rootfs/linux-rootfs-f2fs.img
ROOTFS_IMG_ZST=$DIST/linux-rootfs-f2fs.img.zst
MANGO_TAR=$OUT/mango/mango-0.18.3-aarch64.tar.zst
if [ -f "$ROOTFS_TAR" ]; then
    cp "$ROOTFS_TAR" "$DIST/"
    ASSETS="$ASSETS ArchLinuxARM-aarch64-elish-rootfs.tar.zst"
else
    log "警告：未找到 $ROOTFS_TAR（rootfs 未构建），本次 Release 不含 rootfs 归档"
fi
if [ -f "$ROOTFS_IMG" ]; then
    log "压缩 f2fs 镜像（GitHub 单资产上限 2 GiB）: $ROOTFS_IMG -> $ROOTFS_IMG_ZST"
    zstd -q -T0 -3 -f -o "$ROOTFS_IMG_ZST" "$ROOTFS_IMG"
    ASSETS="$ASSETS linux-rootfs-f2fs.img.zst"
elif [ -f "$ROOTFS_IMG_ZST" ]; then
    ASSETS="$ASSETS linux-rootfs-f2fs.img.zst"
fi
if [ -f "$MANGO_TAR" ]; then
    cp "$MANGO_TAR" "$DIST/"
    ASSETS="$ASSETS mango-0.18.3-aarch64.tar.zst"
fi
if [ -f "$REPO/INSTALL.md" ]; then
    cp "$REPO/INSTALL.md" "$DIST/INSTALL.md"
    cp "$REPO/INSTALL.md" "$OUT/INSTALL.md"
    ASSETS="$ASSETS INSTALL.md"
fi
if [ -f "$OUT/MANIFEST" ]; then
    cp "$OUT/MANIFEST" "$DIST/MANIFEST"
    ASSETS="$ASSETS MANIFEST"
fi
( cd "$DIST" && sha256sum $ASSETS > SHA256SUMS.txt )

# 产物必须能追溯到确切源码状态：构建时工作树有未提交改动就标记 +dirty，
# 避免出现"说明写 commit A、实际用了未提交修复"的情况（2026-11-04 教训）。
release_commit() {
    local commit
    commit=$(git -C "$REPO" rev-parse --short HEAD 2>/dev/null || echo unknown)
    if [ -n "$(git -C "$REPO" status --porcelain --untracked-files=no 2>/dev/null)" ]; then
        commit="$commit+dirty"
    fi
    printf '%s' "$commit"
}
CFG_FILE=$(ls "$KOUT"/config-* | head -1)
KERNEL_TAG=${CFG_FILE##*/config-}
{
    printf '# elish images %s（boot + rootfs + 安装教程）\n\n' "$TAG"
    printf -- '- 内核源码: %s（LLVM=1 交叉编译；pmaports config + configs/kernel/fragments）\n' "$KERNEL_TAG"
    printf -- '- 构建提交: %s\n' "$(release_commit)"
    printf -- '- 内核补丁: configs/kernel/patches/*.patch（已全部打进内核模块）\n'
    printf -- '- 组件: boot.img + initramfs.cpio.gz + modules.tar.zst'
    [ -f "$DIST/ArchLinuxARM-aarch64-elish-rootfs.tar.zst" ] && printf ' + rootfs'
    [ -f "$DIST/mango-0.18.3-aarch64.tar.zst" ] && printf ' + mango'
    printf ' + INSTALL.md\n'
    printf -- '- 构建时间: %s\n\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    printf '| 资产 | 用途 | 大小 (B) |\n|---|---|---|\n'
    printf '| boot.img | **CSOT** 面板（本机面板）：fastboot flash boot_b boot.img | %s |\n' "$(stat -c%s "$DIST/boot.img")"
    printf '| boot-boe-alt.img | **BOE** 面板，同上 | %s |\n' "$(stat -c%s "$DIST/boot-boe-alt.img")"
    printf '| initramfs.cpio.gz | 已打进两个 img 的 initramfs（含面板驱动 + GPU 固件） | %s |\n' "$(stat -c%s "$DIST/initramfs.cpio.gz")"
    printf '| modules.tar.zst | 内核模块，解到 linux 分区的 /lib/modules | %s |\n' "$(stat -c%s "$DIST/modules.tar.zst")"
    if [ -f "$DIST/ArchLinuxARM-aarch64-elish-rootfs.tar.zst" ]; then
        printf '| ArchLinuxARM-aarch64-elish-rootfs.tar.zst | 完整 rootfs 归档（chroot/自建镜像用） | %s |\n' "$(stat -c%s "$DIST/ArchLinuxARM-aarch64-elish-rootfs.tar.zst")"
    fi
    if [ -f "$DIST/linux-rootfs-f2fs.img.zst" ]; then
        printf '| linux-rootfs-f2fs.img.zst | linux 分区根镜像（先 zstd -d，再 fastboot flash linux） | %s |\n' "$(stat -c%s "$DIST/linux-rootfs-f2fs.img.zst")"
    fi
    if [ -f "$DIST/mango-0.18.3-aarch64.tar.zst" ]; then
        printf '| mango-0.18.3-aarch64.tar.zst | MangoWM 运行时（已内置在 rootfs，仅源码重装时用） | %s |\n' "$(stat -c%s "$DIST/mango-0.18.3-aarch64.tar.zst")"
    fi
    if [ -f "$DIST/INSTALL.md" ]; then
        printf '| INSTALL.md | 完整安装教程（从进入 fastboot 到首启验证/更新/救援） | %s |\n' "$(stat -c%s "$DIST/INSTALL.md")"
    fi
    printf '%s\n' '' '## 快速安装（完整步骤见 INSTALL.md；只碰 boot_b 与 linux 两个分区）' '' '```sh'
    printf '%s\n' 'sha256sum -c SHA256SUMS.txt                   # 先验校验和'
    printf '%s\n' 'fastboot getvar current-slot                  # 期望 a（本项目只写 _b 槽位）'
    printf '%s\n' 'zstd -d linux-rootfs-f2fs.img.zst              # 解压根分区镜像'
    printf '%s\n' 'fastboot flash linux linux-rootfs-f2fs.img    # 会清空 linux 分区！'
    printf '%s\n' 'fastboot flash boot_b boot.img                # BOE 面板换成 boot-boe-alt.img'
    printf '%s\n' 'fastboot reboot' '```'
    printf '%s\n' '' '首次开机：自动登录 magma（默认密码 <redacted>，立即 passwd 修改）；initramfs 会自动把根分区扩容到 195 GiB。'
    printf '%s\n' '' '刷错面板 = 黑屏，刷之前确认面板型号（本机是 CSOT）。'
    printf '%s\n' '不要 fastboot erase 任何分区；不要写 boot_a / dtbo_b / system_b / super 等其它分区。'
    printf '%s\n' '刷前自检：python3 scripts/make-bootimg.py --verify --input boot.img，期望 verify.status = OK。'
    printf '%s\n' '' '完整安装/救援步骤见 INSTALL.md（Release 资产同名文件）与仓库 docs/flash-guide.md。'
    printf '%s\n' '' '## 校验和' '' '```'
    cat "$DIST/SHA256SUMS.txt"
    printf '%s\n' '```'
} > "$DIST/RELEASE-NOTES.md"
# Release 说明同时留在 output/<date>/，避免只存在于可清理的中间树（G-11）
cp "$DIST/RELEASE-NOTES.md" "$OUT/RELEASE-NOTES.md"

# ------------------------------------------------------------------ 7) 发布 --
if [ "$PUBLISH" -eq 0 ]; then
    stage "跳过发布（--no-publish）"
    log "产物: $DIST"
    ls -l "$DIST"
    exit 0
fi

stage "发布 Release $TAG"
ASSETS="$ASSETS SHA256SUMS.txt"
export GH_REPO=$SLUG
# 只有 HEAD 已经在某个远端分支上时才能 --target：未推送的提交会让 GitHub 报
# "Release.target_commitish is invalid"，发布整步失败（产物没问题，卡在打 tag）。
TARGET=()
if git -C "$REPO" branch -r --contains HEAD 2>/dev/null | grep -q .; then
    TARGET=(--target "$(git -C "$REPO" rev-parse HEAD)")
else
    log "警告：HEAD 不在任何远端分支上（本地提交未推送），本次不打 --target，tag 指向远端默认分支"
fi
if gh release view "$TAG" >/dev/null 2>&1; then
    log "Release $TAG 已存在：覆盖资产 + 刷新说明"
    ( cd "$DIST" && gh release upload "$TAG" $ASSETS --clobber )
    gh release edit "$TAG" --title "$TAG" --notes-file "$DIST/RELEASE-NOTES.md"
else
    log "创建 Release $TAG"
    ( cd "$DIST" && gh release create "$TAG" $ASSETS \
        --title "$TAG" --notes-file RELEASE-NOTES.md "${TARGET[@]}" )
fi
gh release view "$TAG" --json tagName,name,url,assets \
    --jq '"release: \(.tagName) / \(.name) / \(.url)\nassets: \([.assets[].name] | join(", "))"'

stage "完成"
log "仓库产物: $OUT"
log "Release 资产: $DIST"
