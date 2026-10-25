#!/usr/bin/env bash
# =============================================================================
# build-kernel.sh — 为 xiaomi-elish (SM8250) 交叉编译 postmarketOS 内核 (aarch64)
#
#   工具链: LLVM (clang/lld/llvm-*)，ARCH=arm64，OUT-OF-TREE (O=)
#   源码  : qualcomm-sm8250/linux  sm8250-<ver> 归档
#   config: pmaports main 的 config-postmarketos-qcom-sm8250.aarch64（仓库内）
#
#   幂等: 已下载/已解压/已配置则跳过；--rebuild 丢弃目标树后重编。
#   产物: <OUT>/Image.gz <OUT>/dtbs/qcom/*.dtb <OUT>/modules.tar.zst
#         <OUT>/config-<ver> <OUT>/SHA256SUMS  +  <MANIFEST>（与其他模块合并）
#   中间产物全部位于 $BUILD_DIR（默认 .build/kbuild）。
# =============================================================================
set -euo pipefail

STAGE="init"
trap 'rc=$?; echo "[build-kernel] FAILED stage=${STAGE} rc=${rc}" >&2; echo "[build-kernel] see logs under ${BUILD_DIR:-?}" >&2' ERR

# ---------------------------------------------------------------- 参数/路径 --
ROOT_DIR="${ROOT_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"
REPO_DIR="${REPO_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"

KERNEL_TAG="${KERNEL_TAG:-sm8250-7.2.0_git20261010}"
KERNEL_SRC_NAME="${KERNEL_SRC_NAME:-linux-$KERNEL_TAG}"
CFG_REL_NAME="${CFG_REL_NAME:-${KERNEL_TAG#sm8250-}}"
SRC_URL="${SRC_URL:-https://gitlab.postmarketos.org/soc/qualcomm-sm8250/linux/-/archive/${KERNEL_TAG}/${KERNEL_SRC_NAME}.tar.gz}"
# sha512 来自 pmaports main device/testing/linux-postmarketos-qcom-sm8250/APKBUILD (sha512sums)
KERNEL_SHA512="${KERNEL_SHA512:-05df1589411d2817fecc403402f1dea2c405c5de102bf92e23089e430fc2c623702bd778fbea5b87f70ea02e9eff7f02a7de292f895d47ff40ae88dafdfbab58}"
CFG_IN="${CFG_IN:-$REPO_DIR/configs/kernel/config-postmarketos-qcom-sm8250.aarch64}"
# 相对上游 pmaports config 的本地修正，按文件名顺序合并（见 configs/kernel/fragments/）
FRAG_DIR="${FRAG_DIR:-$REPO_DIR/configs/kernel/fragments}"

DL_DIR="${DL_DIR:-$ROOT_DIR/.build/dl}"
BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/.build/kbuild}"
SRC_DIR="${SRC_DIR:-$BUILD_DIR/src/$KERNEL_SRC_NAME}"
OBJ_DIR="${OBJ_DIR:-$BUILD_DIR/obj}"
MODROOT="${MODROOT:-$BUILD_DIR/modinstall}"
TOOLS_DIR="${TOOLS_DIR:-$ROOT_DIR/.build/tools}"
PKG_DL_DIR="${PKG_DL_DIR:-$ROOT_DIR/.build/dl/pkg}"
OUT_DIR="${OUT_DIR:-$REPO_DIR/output/20261024/kernel}"
MANIFEST="${MANIFEST:-$REPO_DIR/output/20261024/MANIFEST.kernel}"
LOG_FILE="${LOG_FILE:-$BUILD_DIR/build.log}"
JOBS="${JOBS:-16}"
ARCH="${ARCH:-arm64}"
MIRROR="${MIRROR:-https://geo.mirror.pkgbuild.com}"

TARBALL="$DL_DIR/$KERNEL_SRC_NAME.tar.gz"
REBUILD=0
FORCE_FETCH=0
KREL=""

usage() { cat <<'EOF'
用法: build-kernel.sh [--rebuild] [--fetch] [--help]
  --rebuild  删除目标树 $OBJ_DIR / $MODROOT 与 build.log 后完整重编（源码保留）
  --fetch    强制重新下载源码归档
环境变量覆盖: ROOT_DIR REPO_DIR DL_DIR BUILD_DIR SRC_DIR OBJ_DIR MODROOT TOOLS_DIR
             OUT_DIR MANIFEST JOBS ARCH KERNEL_TAG SRC_URL KERNEL_SHA512 CFG_IN FRAG_DIR
EOF
}

for arg in "$@"; do
  case "$arg" in
    --rebuild) REBUILD=1 ;;
    --fetch)   FORCE_FETCH=1 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "[build-kernel] unknown arg: $arg" >&2; usage; exit 2 ;;
  esac
done

log()  { printf '[build-kernel] %s\n' "$*"; }
die()  { printf '[build-kernel] ERROR: %s\n' "$*" >&2; exit 1; }
stage(){ STAGE="$1"; log "==== stage: $1 ===="; }

# --------------------------------------------------------------- 依赖工具链 --
# 用户级安装缺失依赖（禁止系统级安装）：下载 Arch x86_64 包并解包到 $TOOLS_DIR
fetch_pkg() { # $1=包文件名  $2=URL 中的文件名（缺省同 $1）
  local fname="$1" urlfname="${2:-$1}"
  mkdir -p "$PKG_DL_DIR" "$TOOLS_DIR"
  if [ ! -f "$PKG_DL_DIR/$fname" ]; then
    log "fetch pkg $fname"
    curl -fL --retry 3 --retry-delay 3 -o "$PKG_DL_DIR/$fname" \
      "$MIRROR/extra/os/x86_64/$urlfname" || die "cannot download $fname"
  fi
  bsdtar -xf "$PKG_DL_DIR/$fname" -C "$TOOLS_DIR" --no-same-owner || die "cannot extract $fname"
}

prepare_tools() {
  stage tools
  export PATH="$TOOLS_DIR/usr/bin:$PATH"
  command -v bc >/dev/null 2>&1 || fetch_pkg bc-1.08.2-1-x86_64.pkg.tar.zst
  # dtc: 内核自带 scripts/dtc 构建；host 缺失时仅做补充安装（非硬依赖）
  command -v dtc >/dev/null 2>&1 || fetch_pkg dtc-1.8.1-1-x86_64.pkg.tar.zst 'dtc-1%3A1.8.1-1-x86_64.pkg.tar.zst'
  for t in bc clang ld.lld llvm-ar llvm-nm llvm-objcopy pahole flex bison make python3 perl; do
    command -v "$t" >/dev/null 2>&1 || die "missing build tool: $t"
  done
  log "bc=$(command -v bc) dtc=$(command -v dtc || echo none)"
}

# ------------------------------------------------------------------ 源码获取 --
verify_tarball() { # $1=文件
  [ -f "$1" ] || return 1
  local got; got="$(sha512sum "$1" | cut -d' ' -f1)"
  [ "$got" = "$KERNEL_SHA512" ]
}

fetch_source() {
  stage fetch
  mkdir -p "$DL_DIR"
  if [ "$FORCE_FETCH" = 0 ] && verify_tarball "$TARBALL"; then
    log "tarball cached & sha512 ok: $(stat -c%s "$TARBALL") bytes"
    return 0
  fi
  log "download $SRC_URL"
  curl -fL --retry 3 --retry-delay 5 -C - -o "$TARBALL" "$SRC_URL" || die "download failed"
  verify_tarball "$TARBALL" || die "sha512 mismatch for $TARBALL"
  log "tarball sha512 ok: $(stat -c%s "$TARBALL") bytes"
}

extract_source() {
  stage extract
  if [ -f "$SRC_DIR/Makefile" ]; then
    log "source tree cached: $SRC_DIR"
  else
    mkdir -p "$(dirname "$SRC_DIR")"
    log "extract -> $(dirname "$SRC_DIR")"
    tar -xzf "$TARBALL" -C "$(dirname "$SRC_DIR")" --no-same-owner || die "extract failed"
    if [ ! -d "$SRC_DIR" ] && [ -d "$(dirname "$SRC_DIR")/$KERNEL_SRC_NAME" ]; then
      mv "$(dirname "$SRC_DIR")/$KERNEL_SRC_NAME" "$SRC_DIR"
    fi
    [ -f "$SRC_DIR/Makefile" ] || die "unexpected archive layout under $(dirname "$SRC_DIR")"
  fi
  local v; v="$(grep -m1 '^VERSION' "$SRC_DIR/Makefile")"
  log "Makefile $v"
}

# ------------------------------------------------------------------- 配置 ----
configure() {
  stage configure
  [ -f "$CFG_IN" ] || die "config not found: $CFG_IN"
  mkdir -p "$OBJ_DIR"
  local stamp="$OBJ_DIR/.config-stamp" frags=() newer=""
  if [ -d "$FRAG_DIR" ]; then
    shopt -s nullglob; frags=("$FRAG_DIR"/*.config); shopt -u nullglob
  fi
  if [ -f "$stamp" ] && [ -d "$FRAG_DIR" ]; then
    newer="$(find "$FRAG_DIR" -name '*.config' -newer "$stamp" -print -quit 2>/dev/null || true)"
  fi
  if [ "$REBUILD" = 0 ] && [ -f "$stamp" ] && [ -f "$OBJ_DIR/.config" ] \
     && [ "$stamp" -nt "$CFG_IN" ] && [ "$stamp" -nt "$SRC_DIR/Makefile" ] && [ -z "$newer" ]; then
    log "config cached ($(grep -c '=y$' "$OBJ_DIR/.config") =y symbols)"
    return 0
  fi
  # 基准 config + 片段合并：merge_config.sh -m 只合并不跑 make，随后统一 olddefconfig 解析依赖
  log "merge fragments: ${frags[*]:-<none>}"
  ( cd "$OBJ_DIR" && KCONFIG_CONFIG="$OBJ_DIR/.config" \
      "$SRC_DIR/scripts/kconfig/merge_config.sh" -m -O "$OBJ_DIR" \
      "$CFG_IN" "${frags[@]}" ) 2>&1 | tee "$BUILD_DIR/config-merge.log" | tail -20
  make -C "$SRC_DIR" O="$OBJ_DIR" ARCH="$ARCH" LLVM=1 olddefconfig \
    2>&1 | tee "$BUILD_DIR/config.log" | tail -5
  [ -f "$OBJ_DIR/.config" ] || die "olddefconfig produced no .config"
  touch "$stamp"
  log "config ready: $(grep -c '=y$' "$OBJ_DIR/.config") =y symbols"
}

# ------------------------------------------------------------------- 构建 ----
BUILD_SECONDS=0
count_warnings() { grep -c 'warning:' "$LOG_FILE" 2>/dev/null || true; }

build_kernel() {
  stage build
  mkdir -p "$BUILD_DIR"
  # 保留上一轮日志：空跑（make 无实际工作）不应抹掉上一次全量编译的告警证据
  if [ -s "$LOG_FILE" ]; then mv -f "$LOG_FILE" "$LOG_FILE.prev"; fi
  : > "$LOG_FILE"
  local t0 t1 rc
  t0="$(date +%s)"
  set +e
  make -C "$SRC_DIR" O="$OBJ_DIR" ARCH="$ARCH" LLVM=1 -j"$JOBS" Image.gz dtbs modules \
    2>&1 | tee "$LOG_FILE"
  rc="${PIPESTATUS[0]}"
  set -e
  t1="$(date +%s)"
  BUILD_SECONDS=$(( t1 - t0 ))
  [ "$rc" -eq 0 ] || die "make failed (rc=$rc) after ${BUILD_SECONDS}s; see $LOG_FILE"
  log "build ok in ${BUILD_SECONDS}s, warnings=$(count_warnings)"
}

# ------------------------------------------------------------- 模块安装/打包 --
install_modules() {
  stage modules_install
  KREL="$(make -s -C "$SRC_DIR" O="$OBJ_DIR" ARCH="$ARCH" LLVM=1 kernelrelease)"
  [ -n "$KREL" ] || die "kernelrelease empty"
  log "kernelrelease=$KREL"
  rm -rf "$MODROOT"; mkdir -p "$MODROOT"
  make -C "$SRC_DIR" O="$OBJ_DIR" ARCH="$ARCH" LLVM=1 \
       INSTALL_MOD_PATH="$MODROOT" INSTALL_MOD_STRIP=1 modules_install \
       >>"$LOG_FILE" 2>&1 || die "modules_install failed"
  find "$MODROOT/lib/modules" -maxdepth 2 \( -name build -o -name source \) -exec rm -f {} + 2>/dev/null || true
  mkdir -p "$OUT_DIR"
  tar -C "$MODROOT" --zstd --owner=0 --group=0 -cf "$OUT_DIR/modules.tar.zst" lib/modules \
    || die "modules tar failed"
  log "modules: $(find "$MODROOT/lib/modules" -name '*.ko*' | wc -l) module files ($(grep -qx 'CONFIG_MODULE_COMPRESS=y' "$OBJ_DIR/.config" && echo '.ko.zst' || echo '.ko')), tar=$(stat -c%s "$OUT_DIR/modules.tar.zst") bytes"
}

# --------------------------------------------------------------- 产物收集 ----
collect() {
  stage collect
  local img="$OBJ_DIR/arch/arm64/boot/Image.gz"
  local dboe="$OBJ_DIR/arch/arm64/boot/dts/qcom/sm8250-xiaomi-elish-boe.dtb"
  local dcsot="$OBJ_DIR/arch/arm64/boot/dts/qcom/sm8250-xiaomi-elish-csot.dtb"
  for f in "$img" "$dboe" "$dcsot"; do [ -f "$f" ] || die "missing artifact: $f"; done
  mkdir -p "$OUT_DIR/dtbs/qcom"
  install -m0644 "$img"   "$OUT_DIR/Image.gz"
  install -m0644 "$dboe"  "$OUT_DIR/dtbs/qcom/sm8250-xiaomi-elish-boe.dtb"
  install -m0644 "$dcsot" "$OUT_DIR/dtbs/qcom/sm8250-xiaomi-elish-csot.dtb"
  install -m0644 "$OBJ_DIR/.config" "$OUT_DIR/config-$CFG_REL_NAME"
  ( cd "$OUT_DIR" && sha256sum Image.gz \
      dtbs/qcom/sm8250-xiaomi-elish-boe.dtb \
      dtbs/qcom/sm8250-xiaomi-elish-csot.dtb \
      modules.tar.zst "config-$CFG_REL_NAME" > SHA256SUMS )
  log "SHA256SUMS written"
}

# --------------------------------------------------------------- MANIFEST ----
# 与其他模块共写同一 MANIFEST.kernel：仅替换 kernel/ 前缀行，保留其它模块行；flock 串行化。
write_manifest() {
  stage manifest
  local ts sha size tmp entry name part
  ts="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  tmp="$(mktemp "$OUT_DIR/.manifest.XXXXXX")"
  {
    printf '# MANIFEST.kernel — 可刷入产物清单 | 字段: 路径|目标分区|SHA256|构建时间(UTC)|大小(字节)\n'
    printf '# 生成: scripts/build-kernel.sh (module=kernel)  kernel=%s\n' "$CFG_REL_NAME"
    for entry in "Image.gz:boot_b" \
                 "dtbs/qcom/sm8250-xiaomi-elish-boe.dtb:boot_b" \
                 "dtbs/qcom/sm8250-xiaomi-elish-csot.dtb:boot_b" \
                 "modules.tar.zst:linux" \
                 "config-$CFG_REL_NAME:-" \
                 "SHA256SUMS:-"; do
      name="${entry%:*}"
      part="${entry##*:}"
      sha="$(sha256sum "$OUT_DIR/$name" | cut -d' ' -f1)"
      size="$(stat -c%s "$OUT_DIR/$name")"
      printf 'kernel/%s|%s|%s|%s|%s\n' "$name" "$part" "$sha" "$ts" "$size"
    done
  } > "$tmp"

  mkdir -p "$(dirname "$MANIFEST")"
  ( flock 9
    local keep="$tmp.keep"
    if [ -f "$MANIFEST" ]; then grep -v -E '^(kernel/|#)' "$MANIFEST" > "$keep" || : ; else : > "$keep"; fi
    head -2 "$tmp" > "$MANIFEST.new"
    if [ -s "$keep" ]; then cat "$keep" >> "$MANIFEST.new"; fi
    tail -n +3 "$tmp" >> "$MANIFEST.new"
    mv -f "$MANIFEST.new" "$MANIFEST"
    rm -f "$keep"
  ) 9>"$MANIFEST.lock"
  rm -f "$tmp"
  log "MANIFEST: $MANIFEST ($(grep -c '^kernel/' "$MANIFEST" || true) kernel rows)"
}

# ------------------------------------------------------------------- 汇总 ----
summary() {
  stage summary
  log "Image.gz        $(stat -c%s "$OUT_DIR/Image.gz") bytes  $(sha256sum "$OUT_DIR/Image.gz" | cut -d' ' -f1)"
  log "dtb boe         $(stat -c%s "$OUT_DIR/dtbs/qcom/sm8250-xiaomi-elish-boe.dtb") bytes"
  log "dtb csot        $(stat -c%s "$OUT_DIR/dtbs/qcom/sm8250-xiaomi-elish-csot.dtb") bytes"
  log "modules.tar.zst $(stat -c%s "$OUT_DIR/modules.tar.zst") bytes"
  log "kernelrelease   $KREL"
  log "build seconds   $BUILD_SECONDS"
  log "warnings        $(count_warnings)"
  log "OUT_DIR         $OUT_DIR"
}

main() {
  if [ "$REBUILD" = 1 ]; then
    stage clean
    log "--rebuild: removing $OBJ_DIR $MODROOT $LOG_FILE"
    rm -rf "$OBJ_DIR" "$MODROOT"
    rm -f "$LOG_FILE" "$BUILD_DIR/config.log"
  fi
  prepare_tools
  fetch_source
  extract_source
  configure
  build_kernel
  install_modules
  collect
  write_manifest
  summary
  stage done
}

main "$@"
