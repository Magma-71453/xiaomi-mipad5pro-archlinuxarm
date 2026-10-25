#!/usr/bin/env bash
# =============================================================================
# build-mango-on-device.sh — 在 elish 设备上原生构建 MangoWM (aarch64)
#
#   目标: mango 0.18.3（wlroots-0.20 / scenefx-0.5 依赖线；上游 meson.build:34/39）
#   设备: dbclient 免密登录。**对端登录 shell 是 fish**，因此
#         1) 多行脚本一律 "dbclient ... sh <<'REMOTE'" 从 stdin 喂；
#         2) 单命令包成 "sh -c '...'" 作为**单个** argv 传；
#         3) 不用 dbclient -o 选项（dropbear 报 unknown configuration option）。
#   依赖: 设备无 wlroots/scenefx，且禁止 pacman 安装 / 禁止写 /usr。脚本在宿主下载
#         ALARM aarch64 官方包 → 推送 → bsdtar 解包进 DEV_SYSROOT → .pc 前缀改写为
#         sysroot 绝对路径（不用 PKG_CONFIG_SYSROOT_DIR，理由见 step_remote_prepare）。
#   产物: OUT_DIR/stage/** + OUT_DIR/MANIFEST.mango（可直接 sha256sum -c）
#   幂等: 下载按 sha256 校验；推送按远端 sha256 只传差异；解包/装 meson 用哨兵；
#         --rebuild 丢弃远端构建树重编；--clean 丢弃远端 DEV_BASE 全部内容。
# =============================================================================
set -euo pipefail

STAGE="init"
trap 'rc=$?; echo "[mango] FAILED stage=${STAGE} rc=${rc}" >&2; echo "[mango] 远端日志: ${DEV_LOGS:-?}" >&2' ERR

# ---------------------------------------------------------------- 版本/源 --
MANGO_VER="${MANGO_VER:-0.18.3}"
MESON_WHEEL_VER="${MESON_WHEEL_VER:-1.12.1}"
ALARM_MIRROR="${ALARM_MIRROR:-http://mirror.archlinuxarm.org}"
ALARM_SUBDIR="${ALARM_SUBDIR:-extra}"
MANGO_TARBALL="mango-${MANGO_VER}.tar.gz"
MANGO_URL="${MANGO_URL:-https://codeload.github.com/mangowm/mango/tar.gz/refs/tags/${MANGO_VER}}"
MANGO_SHA256="${MANGO_SHA256:-9b9c66c55ba83131cc2e7b847239f0f7520207f448a8c580c8ca796ec9e13e41}"
MESON_WHEEL_FILE="meson-${MESON_WHEEL_VER}-py3-none-any.whl"
MESON_WHEEL_SHA256="${MESON_WHEEL_SHA256:-930bc7542cbd9f57009e182fd014eba48cf1a0180a7b9006d2d32d8f168d8b02}"
MESON_WHEEL_URL="${MESON_WHEEL_URL:-https://files.pythonhosted.org/packages/57/99/36cd25f9598eef70db97577440a70a360af505f6f01c2338a18d1556079c/${MESON_WHEEL_FILE}}"

# 需要而设备上没有的依赖包（其余依赖设备 pacman 已装，见 docs/mango.md）
PKG_SPECS=(
  "wlroots0.20-0.20.2-1-aarch64.pkg.tar.xz 31bf5f2ea29001643fffd3df21930e4254e51f30bc1a1684c476449d4ebdedc3"
  "scenefx-0.5-1-aarch64.pkg.tar.xz 98966e25d489d4a72afb4b99403c5d962240cba9a3be943bd59c0f12ccd6e71a"
  "libliftoff-0.5.0-1-aarch64.pkg.tar.xz 77c8cf79c256c0b118ee14250ba16b7e322525566daa16a9fcf048153a1214df"
  "wayland-protocols-1.49-1-any.pkg.tar.xz 9678c0d371ecfb5041b1b1d8a7986977bd82b7233984dbe25b33a54da0dbee27"
  "cjson-1.7.19-1-aarch64.pkg.tar.xz 3d303d194b6b96103718441345acd16a269f31f450a493e353cf8518cc0299d9"
)

# --------------------------------------------------------------- 宿主路径 --
ROOT_DIR="${ROOT_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"
REPO_DIR="${REPO_DIR:-${ROOT_DIR}/elish-archlinux}"
BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/.build/mango}"
DL_DIR="${DL_DIR:-${BUILD_DIR}/dl}"
SRC_DIR="${SRC_DIR:-${BUILD_DIR}/src}"
LOG_DIR="${LOG_DIR:-${BUILD_DIR}/logs}"
OUT_DIR="${OUT_DIR:-${REPO_DIR}/output/20261024/mango}"

# ---------------------------------------------------------- 设备与远端路径 --
DEV_HOST="${DEV_HOST:-192.168.1.90}"
DEV_USER="${DEV_USER:-elish}"
DEV_SSH_KEY="${DEV_SSH_KEY:-${HOME}/.ssh/id_ed25519}"
DEV_TARGET="${DEV_USER}@${DEV_HOST}"
DEV_JOBS="${DEV_JOBS:-8}"
DEV_BASE="${DEV_BASE:-/home/${DEV_USER}/elish-build}"
DEV_PKGS="${DEV_BASE}/pkgs"
DEV_SYSROOT="${DEV_BASE}/sysroot"
DEV_SRCROOT="${DEV_BASE}/src"
DEV_BUILDDIR="${DEV_BASE}/build/mango"
DEV_STAGE="${DEV_BASE}/stage"
DEV_LOGS="${DEV_BASE}/logs"
DEV_MESONLIB="${DEV_BASE}/mesonlib"
DEV_BIN="${DEV_BASE}/bin"
DEV_TARBALL="${DEV_BASE}/mango-${MANGO_VER}-aarch64.tar.zst"
DEV_SRC="${DEV_SRCROOT}/mango-${MANGO_VER}"

BUILD_TYPE="${BUILD_TYPE:-release}"
OUT_TARBALL="${OUT_DIR}/mango-${MANGO_VER}-aarch64.tar.zst"

# ------------------------------------------------------------------- 选项 --
DO_REBUILD=0; DO_CLEAN=0; SKIP_VERIFY=0
for arg in "$@"; do
  case "${arg}" in
    --rebuild)   DO_REBUILD=1 ;;
    --clean)     DO_CLEAN=1 ;;
    --no-verify) SKIP_VERIFY=1 ;;
    -h|--help)   sed -n '2,17p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1}//'; exit 0 ;;
    *) echo "[mango] unknown option: ${arg}" >&2; exit 2 ;;
  esac
done

# ------------------------------------------------------------------- 工具 --
log()  { printf '[mango] %s\n' "$*"; }
die()  { printf '[mango] ERROR: %s\n' "$*" >&2; exit 1; }
need() { command -v "${1}" >/dev/null 2>&1 || die "缺少宿主命令: ${1}"; }
sha_of() { sha256sum "${1}" | cut -d' ' -f1; }
dc() { dbclient -y -i "${DEV_SSH_KEY}" "${DEV_TARGET}" "$@"; }
# ============================================================ 步骤 0: 预检 ==
step_preflight() {
  STAGE=preflight
  log "预检宿主工具 / 目录 / 设备连通性"
  need dbclient; need curl; need bsdtar; need tar; need zstd; need sha256sum; need python3; need file
  [ -f "${DEV_SSH_KEY}" ] || die "找不到设备私钥: ${DEV_SSH_KEY}"
  mkdir -p "${DL_DIR}" "${SRC_DIR}" "${LOG_DIR}" "${OUT_DIR}"
  local info
  info=$(dc sh <<'REMOTE'
echo "remote arch=$(uname -m) kernel=$(uname -r)"
echo "remote user=$(id -un) home=$HOME"
df -h "$HOME" | tail -1
REMOTE
  ) || die "无法连接 ${DEV_TARGET}（检查 dbclient / 网络）"
  printf '%s\n' "$info" | sed 's/^/[mango]   /'
}

# ============================================================ 步骤 1: 下载 ==
fetch_one() {  # $1=url $2=out $3=want_sha256
  local url="$1" out="$2" want="$3" got
  if [ -f "$out" ] && [ "$want" != "-" ] && [ "$(sha_of "$out")" = "$want" ]; then
    log "  已存在且校验通过: $(basename "$out")"; return 0
  fi
  log "  下载 $(basename "$out")"
  curl -fsSL -o "$out.part" "$url" || die "下载失败: $url"
  mv "$out.part" "$out"
  if [ "$want" != "-" ]; then
    got=$(sha_of "$out")
    [ "$got" = "$want" ] || die "sha256 不符: $out 期望 $want 实得 $got"
  fi
}

step_fetch() {
  STAGE=fetch
  log "宿主下载 mango ${MANGO_VER} 源码 / meson wheel / ALARM 依赖包"
  fetch_one "${MANGO_URL}"  "${DL_DIR}/${MANGO_TARBALL}"  "${MANGO_SHA256}"
  fetch_one "${MESON_WHEEL_URL}" "${DL_DIR}/${MESON_WHEEL_FILE}" "${MESON_WHEEL_SHA256}"
  local spec file sha
  for spec in "${PKG_SPECS[@]}"; do
    file="${spec%% *}"; sha="${spec##* }"
    fetch_one "${ALARM_MIRROR}/aarch64/${ALARM_SUBDIR}/$file" "${DL_DIR}/$file" "$sha"
  done
  ( cd "${DL_DIR}" && sha256sum ./*.pkg.tar.xz ${MESON_WHEEL_FILE} ${MANGO_TARBALL} > "${LOG_DIR}/downloads.sha256" )
  log "  下载物校验清单: ${LOG_DIR}/downloads.sha256"
}

# ============================================================ 步骤 2: 推送 ==
step_push() {
  STAGE=push
  log "推送源码/依赖包/meson wheel 到 ${DEV_PKGS}（只传远端 sha256 不一致的）"
  local files=("${MANGO_TARBALL}" "${MESON_WHEEL_FILE}") spec
  for spec in "${PKG_SPECS[@]}"; do files+=("${spec%% *}"); done

  local tmpscript; tmpscript=$(mktemp)
  cat > "$tmpscript" <<'REMOTE'
dir=$1; shift
mkdir -p "$dir"
for f in "$@"; do
  if [ -f "$dir/$f" ]; then printf '%s  %s\n' "$(sha256sum "$dir/$f" | cut -d' ' -f1)" "$f"; fi
done
REMOTE
  local remote_sums; remote_sums=$(dc sh -s -- "${DEV_PKGS}" "${files[@]}" < "$tmpscript")
  local to_push=() f lsum rsum
  for f in "${files[@]}"; do
    lsum=$(sha_of "${DL_DIR}/$f")
    rsum=$(printf '%s\n' "$remote_sums" | awk -v n="$f" '$2==n {print $1}')
    if [ "$lsum" = "$rsum" ]; then log "  skip（远端已一致）: $f"; else to_push+=("$f"); fi
  done
  if [ "${#to_push[@]}" -gt 0 ]; then
    log "  stream 推送 ${#to_push[@]} 个文件"
    tar cf - -C "${DL_DIR}" "${to_push[@]}" | dc "sh -c 'tar xf - -C ${DEV_PKGS}'"
    dc sh -s -- "${DEV_PKGS}" "${to_push[@]}" < "$tmpscript" > "${LOG_DIR}/push-remote.sha256"
    log "  远端 sha256 存证: ${LOG_DIR}/push-remote.sha256"
  else
    log "  全部文件远端已就绪"
  fi
  rm -f "$tmpscript"
}
# ================================================== 步骤 3: 远端 sysroot ==
step_remote_prepare() {
  STAGE=remote-prepare
  log "远端: 建目录 / sysroot 解包 / .pc 改写 / meson wheel / 源码解压"
  local force=""
  [ "$DO_REBUILD" = "1" ] && force="--force"
  if [ "$DO_CLEAN" = "1" ]; then
    log "  --clean: 删除远端 ${DEV_BASE}"
    dc sh -s -- "${DEV_BASE}" <<'REMOTE'
rm -rf "$1"
REMOTE
  fi
  # 注意: dbclient 把 argv 用空格拼成一条远端命令，空串参数会塌陷导致位置错位，
  # 所以 force 用 "-" 哨兵而不是空串。
  dc sh -s -- "${DEV_BASE}" "${force:--}" "${MANGO_VER}" "${MANGO_TARBALL}" <<'REMOTE'
B=$1; FORCE=$2; VER=$3; MTAR=$4
PKGS=$B/pkgs; SYS=$B/sysroot; SRC=$B/src; MARK=$SYS/.stage-complete
set -e
mkdir -p "$PKGS" "$SYS" "$SRC" "$B/stage" "$B/logs" "$B/build" "$B/mesonlib" "$B/bin"

if [ "$FORCE" = "--force" ] || [ ! -f "$MARK" ]; then
  echo "[remote] 解包依赖包 -> $SYS"
  rm -rf "$SYS"; mkdir -p "$SYS"
  for pkg in "$PKGS"/*.pkg.tar.xz; do
    bsdtar -xf "$pkg" -C "$SYS" --exclude='.PKGINFO' --exclude='.BUILDINFO' \
           --exclude='.MTREE' --exclude='.INSTALL' --exclude='.Changelog'
    echo "[remote]   unpacked $(basename "$pkg")"
  done
  # .pc 前缀改写为 sysroot 绝对路径。
  # 为什么不用 PKG_CONFIG_SYSROOT_DIR: 它会把系统包(pixman/libdrm/...)的 -I 一起前缀化,
  # 产生 -I$SYS/usr/include/pixman-1 这类不存在的路径, 首个报错是 scenefx 头里的 pixman.h。
  for f in "$SYS"/usr/lib/pkgconfig/*.pc "$SYS"/usr/share/pkgconfig/*.pc; do
    sed -i -e "s|^prefix=/usr|prefix=$SYS/usr|" -e "s|^libdir=/usr|libdir=$SYS/usr|" \
           -e "s|^includedir=/usr|includedir=$SYS/usr|" -e "s|\${pc_sysrootdir}||g" "$f"
  done
  touch "$MARK"
  echo "[remote] sysroot 就绪: $(du -sh "$SYS" | cut -f1) / $(find "$SYS" -name '*.pc' | wc -l) 个 .pc"
else
  echo "[remote] sysroot 已就绪（哨兵 $MARK）"
fi

WHEEL=$(ls "$PKGS"/meson-*-py3-none-any.whl 2>/dev/null | head -1)
[ -n "$WHEEL" ] || { echo "[remote] FATAL: 缺 meson wheel" >&2; exit 1; }
cat > "$B/bin/meson" <<EOS
#!/bin/sh
PYTHONPATH="$B/mesonlib\${PYTHONPATH:+:\$PYTHONPATH}"
export PYTHONPATH
exec python3 -m mesonbuild.mesonmain "\$@"
EOS
chmod +x "$B/bin/meson"
if ! PATH="$B/bin:$PATH" meson --version >/dev/null 2>&1; then
  rm -rf "$B/mesonlib"; mkdir -p "$B/mesonlib"
  bsdtar -xf "$WHEEL" -C "$B/mesonlib"   # 设备无 meson 且 pip 受 PEP668 限制: 解 wheel 到私有目录
fi
echo "[remote] meson $(PATH="$B/bin:$PATH" meson --version)  wheel=$(basename "$WHEEL")"

[ -n "$MTAR" ] || { echo "[remote] FATAL: 未收到源码包名(位置参数错位)" >&2; exit 1; }
if [ "$FORCE" = "--force" ] || [ ! -d "$SRC/mango-$VER" ]; then
  rm -rf "$SRC/mango-$VER"
  tar xzf "$PKGS/$MTAR" -C "$SRC"
  echo "[remote] 源码: $SRC/mango-$VER"
fi
REMOTE
}

# ============================================================ 步骤 4: 构建 ==
step_remote_build() {
  STAGE=remote-build
  log "远端: meson setup + ninja -j${DEV_JOBS} + meson install（${BUILD_TYPE}）"
  local force=""
  [ "$DO_REBUILD" = "1" ] && force="--force"
  dc sh -s -- "${DEV_BASE}" "${force:--}" "${BUILD_TYPE}" "${DEV_JOBS}" "${MANGO_VER}" <<'REMOTE' | tee "${LOG_DIR}/remote-build.out"
B=$1; FORCE=$2; BTYPE=$3; JOBS=$4; VER=$5
set -e
export PATH=$B/bin:$PATH
export PKG_CONFIG_PATH=$B/sysroot/usr/lib/pkgconfig:$B/sysroot/usr/share/pkgconfig
export LD_LIBRARY_PATH=$B/sysroot/usr/lib
SRC=$B/src/mango-$VER
if [ "$FORCE" = "--force" ] || [ ! -f "$B/build/mango/build.ninja" ]; then
  rm -rf "$B/build/mango"
  echo "[remote] meson setup"
  ( cd "$B" && meson setup build/mango "$SRC" --prefix="$B/stage" \
      --buildtype="$BTYPE" --wrap-mode=nodownload ) > "$B/logs/setup.log" 2>&1 || {
    echo "[remote] SETUP FAILED"; tail -40 "$B/logs/setup.log"; exit 1; }
fi
grep -E 'Run-time dependency|C compiler for' "$B/logs/setup.log" || true
START=$(date +%s)
ninja -C "$B/build/mango" -j"$JOBS" > "$B/logs/build.log" 2>&1 || { tail -40 "$B/logs/build.log"; exit 1; }
END=$(date +%s)
printf '[remote] ninja %ss  FAILED=%s error=%s warning=%s\n' "$((END-START))" \
  "$(grep -c '^FAILED' "$B/logs/build.log" || true)" "$(grep -c 'error:' "$B/logs/build.log" || true)" \
  "$(grep -c 'warning:' "$B/logs/build.log" || true)"
rm -rf "$B/stage"; mkdir -p "$B/stage"
meson install -C "$B/build/mango" > "$B/logs/install.log" 2>&1 || { tail -20 "$B/logs/install.log"; exit 1; }
file "$B/stage/bin/mango"
REMOTE
}

# ============================================================ 步骤 5: 取回 ==
step_pull() {
  STAGE=pull
  log "远端打 tar.zst + 取回 + 本地解包"
  local remote_sha remote_size local_sha
  read -r remote_sha remote_size <<< "$(dc sh -s -- "${DEV_BASE}" "${DEV_TARBALL}" <<'REMOTE'
B=$1; TAR=$2
set -e
cd "$B"
tar --exclude=build -cf - stage logs | zstd -19 -T4 -q -f -o "$TAR" -
printf '%s %s\n' "$(sha256sum "$TAR" | cut -d' ' -f1)" "$(stat -c %s "$TAR")"
REMOTE
  )"
  [ -n "$remote_sha" ] || die "远端未返回 tarball sha256"
  rm -rf "${OUT_DIR}/stage" "${OUT_DIR}/logs"
  dc "sh -c 'cat ${DEV_TARBALL}'" > "${OUT_TARBALL}"
  local_sha=$(sha_of "${OUT_TARBALL}")
  [ "$local_sha" = "$remote_sha" ] || die "取回校验失败 remote=$remote_sha local=$local_sha"
  log "  tarball sha256=$local_sha（与远端一致, ${remote_size} bytes）"
  zstd -t "$OUT_TARBALL" >/dev/null
  tar xf "$OUT_TARBALL" -C "$OUT_DIR"
  log "  解包到 ${OUT_DIR}/stage 与 ${OUT_DIR}/logs"
}
# ============================================================ 步骤 6: 清单 ==
step_manifest() {
  STAGE=manifest
  log "生成 MANIFEST.mango（sha256sum -c 兼容, 附 target/build_time/size/mode 注释）"
  python3 - "${OUT_DIR}" "${MANGO_VER}" "${BUILD_TYPE}" <<'PYEOF'
import hashlib, os, sys, datetime
out, ver, btype = sys.argv[1], sys.argv[2], sys.argv[3]
stage = os.path.join(out, "stage")
rows = []
for root, _dirs, files in os.walk(stage):
    for name in files:
        full = os.path.join(root, name)
        rel = os.path.relpath(full, out)
        h = hashlib.sha256(open(full, "rb").read()).hexdigest()
        st = os.stat(full)
        rows.append((h, rel, st.st_size, oct(st.st_mode & 0o7777)[2:], st.st_mtime))
rows.sort(key=lambda r: r[1])
now = datetime.datetime.now().astimezone().isoformat(timespec="seconds")
lines = [
    "# MANIFEST.mango - MangoWM %s (aarch64) for elish / ArchLinuxARM" % ver,
    "# generated: %s buildtype=%s" % (now, btype),
    "# verify: cd <this dir> && sha256sum -c MANIFEST.mango",
    "# target=linux 表示装配进 rootfs 的 linux 根分区（路径相对 /, 详见 docs/mango.md）",
    "# 每个条目前一行注释给 target / build_time / size / mode",
]
for h, rel, size, mode, mtime in rows:
    bt = datetime.datetime.fromtimestamp(mtime).astimezone().isoformat(timespec="seconds")
    lines.append("# target=linux build_time=%s size=%d mode=0%s" % (bt, size, mode))
    lines.append("%s  %s" % (h, rel))
open(os.path.join(out, "MANIFEST.mango"), "w").write("\n".join(lines) + "\n")
print("[mango] MANIFEST.mango: %d entries" % len(rows))
PYEOF
  ( cd "${OUT_DIR}" && sha256sum -c MANIFEST.mango )
}

# ============================================================ 步骤 7: 验收 ==
step_verify() {
  STAGE=verify
  if [ "$SKIP_VERIFY" = "1" ]; then log "--no-verify: 跳过验收"; return 0; fi
  log "验收 A1(file) / A2(pkg-config + ldd) / A4(MANIFEST sha256)"
  ( cd "${OUT_DIR}" && sha256sum -c MANIFEST.mango >/dev/null ) || die "A4 失败: MANIFEST 校验不过"
  file "${OUT_DIR}/stage/bin/mango" > "${LOG_DIR}/verify-file.txt"
  sed 's/^/[mango]   /' "${LOG_DIR}/verify-file.txt"
  grep -q 'ARM aarch64' "${LOG_DIR}/verify-file.txt" || die "A1 失败: 不是 aarch64 ELF"
  dc sh -s -- "${DEV_BASE}" <<'REMOTE' > "${LOG_DIR}/verify-ldd.txt"
B=$1
export PKG_CONFIG_PATH=$B/sysroot/usr/lib/pkgconfig:$B/sysroot/usr/share/pkgconfig
export LD_LIBRARY_PATH=$B/sysroot/usr/lib
echo "--- pkg-config（编译期解析值）---"
for m in wlroots-0.20 scenefx-0.5 wayland-server libinput xkbcommon pixman-1 libdrm libcjson libpcre2-8 pangocairo; do
  printf '%-16s %s\n' "$m" "$(pkg-config --modversion "$m")"
done
echo "--- ldd（运行时实际指向, sysroot 优先）---"
ldd "$B/stage/bin/mango" | grep -E 'wlroots|scenefx|cjson|liftoff|not found' || true
REMOTE
  sed 's/^/[mango]   /' "${LOG_DIR}/verify-ldd.txt"
  if grep -q 'not found' "${LOG_DIR}/verify-ldd.txt"; then die "A2 失败: ldd 有未解析依赖"; fi
  grep -q 'wlroots-0.20' "${LOG_DIR}/verify-ldd.txt" || die "A2 失败: ldd 未列出 wlroots"
  grep -q 'scenefx-0.5' "${LOG_DIR}/verify-ldd.txt" || die "A2 失败: ldd 未列出 scenefx"
  log "验收通过（证据: ${LOG_DIR}/verify-*.txt）"
}

main() {
  step_preflight
  step_fetch
  step_push
  step_remote_prepare
  step_remote_build
  step_pull
  step_manifest
  step_verify
  STAGE=done
  log "完成: ${OUT_DIR}"
  local usage
  usage=$(dc sh -s -- "${DEV_BASE}" <<'REMOTE'
du -sh "$1"
REMOTE
  )
  log "远端占用: ${usage}"
}

main "$@"

