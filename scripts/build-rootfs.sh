#!/usr/bin/env bash
# =============================================================================
# build-rootfs.sh — assemble an Arch Linux ARM (aarch64) root filesystem for the
# Xiaomi Mi Pad 5 Pro (xiaomi-elish, SM8250) and pack it into
#   (a) an archive: ArchLinuxARM-aarch64-elish-rootfs.tar.zst
#   (b) a raw ext4 image (4096-byte blocks) for "fastboot flash linux"
#
# No root is required. Payloads are unpacked with bsdtar, the whole run is
# re-executed under fakeroot (so mke2fs -d and bsdtar record uid/gid 0 for the
# files the caller owns) and aarch64 binaries (ldconfig, localedef, visudo) are
# executed through the x86_64 qemu-aarch64-static in user mode.
#
# Nothing is written into the git repository except the artifacts:
#   <build-root>/dl        download cache (base tarball, repo DBs, packages)
#   <build-root>/pkg       downloaded ALARM aarch64 packages
#   <build-root>/rootfs/tree   the assembled root filesystem
#   <build-root>/rootfs/.state stamps, resolver, pristine /etc snapshot, logs
#
# Usage: scripts/build-rootfs.sh [options]
#   --modules DIR    install a kernel module tree: DIR/lib/modules/<kver> (or DIR
#                    itself when it already is a lib/modules tree) into the
#                    rootfs and refresh modules.dep with depmod -b
#   --modules also accepts a modules.tar.zst archive produced by the kernel build
#   --mango DIR      install the MangoWM runtime staged by the mango module
#                    (DIR/bin, DIR/etc/mango, DIR/share/..., DIR/lib/systemd/user)
#   --firmware DIR   copy the firmware tree DIR/ into the rootfs root (expects a
#                    usr/lib/firmware/... layout, as produced from the pmaports
#                    firmware-xiaomi-elish package)
#   --size SIZE      image size, default 16G (auto-grown when the tree needs more;
#                    the partition is ~195 GiB, grow it later with resize.f2fs)
#   --also-ext4      additionally build an ext4 image (secondary, not default)
#   --f2fs-sector-size N  f2fs superblock sector size, default 4096 (use 512 or
#                    9/512 if a mount reports a sector size mismatch)
#   --jobs N         parallel package downloads (default 8)
#   --no-locale-gen  skip the aarch64 localedef pass
#   --refresh-db     re-download the ALARM package databases
#   --clean          remove the assembler work tree and rebuild from scratch
#   --no-fakeroot    do not re-exec under fakeroot (ownership = calling user)
#   -h, --help       show this help
#
# Exit status 0 means: every artifact was produced and verified.
# =============================================================================
set -euo pipefail

SCRIPT_PATH=$0
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
REPO_ROOT=$(cd "$SCRIPT_DIR/.." && pwd)
CONFIG_DIR=$REPO_ROOT/configs/rootfs
PKG_LIST=$CONFIG_DIR/packages.list
OUT_DIR=$REPO_ROOT/output/20261024/rootfs
MANIFEST=$REPO_ROOT/output/20261024/MANIFEST.rootfs

BUILD_ROOT=$(cd "$REPO_ROOT/.." && pwd)/.build
WORK=$BUILD_ROOT/rootfs
TREE=$WORK/tree
STATE=$WORK/.state
DL=$BUILD_ROOT/dl
PKGCACHE=$BUILD_ROOT/pkg
QEMU_DIR=$BUILD_ROOT/tools/qemu
QEMU=$QEMU_DIR/usr/bin/qemu-aarch64-static

ALARM_BASE_URL=""
[ -n "$ALARM_BASE_URL" ] || ALARM_BASE_URL=http://os.archlinuxarm.org/os/ArchLinuxARM-aarch64-latest.tar.gz
ALARM_MIRROR=""
[ -n "$ALARM_MIRROR" ] || ALARM_MIRROR=http://mirror.archlinuxarm.org/aarch64
ARCH_MIRROR=""
[ -n "$ARCH_MIRROR" ] || ARCH_MIRROR=https://geo.mirror.pkgbuild.com
QEMU_PKG_FILE=qemu-user-static-11.1.2-1-x86_64.pkg.tar.zst
QEMU_PKG_URL=$ARCH_MIRROR/extra/os/x86_64/$QEMU_PKG_FILE
REPOS="core extra alarm"

MODULES_DIR=""
FIRMWARE_DIR=""
IMG_SIZE=""
[ -n "$IMG_SIZE" ] || IMG_SIZE=16G
ALSO_EXT4=0
F2FS_SECTOR_SIZE=""
[ -n "$F2FS_SECTOR_SIZE" ] || F2FS_SECTOR_SIZE=4096
F2FS_FEATURES=extra_attr,inode_checksum,flexible_inline_xattr
F2FS_VERSION=1.17.0-1
F2FS_PKG_FILE=f2fs-tools-$F2FS_VERSION-x86_64.pkg.tar.zst
F2FS_DIR=$BUILD_ROOT/tools/f2fs
F2FS_BIN=$F2FS_DIR/usr/bin
F2FS_LIB=$F2FS_DIR/usr/lib
F2FS_IMG=""
EXT4_IMG=""
TARBALL=""
F2FS_PATCH_DIFF=0
F2FS_FIX_LINES=0
F2FS_FAIL2=0
F2FS_COPY_OFFSETS=""
F2FS_FIX2=0
F2FS_COPY_DIFF=0
MANGO_DIR=""
F2FS_VINODE=0
F2FS_VNODE=0
F2FS_VBLOCKS=0
F2FS_SRC_ENTRIES=0
F2FS_SRC_INODES=0
F2FS_SRC_BYTES=0
F2FS_LSS=""
JOBS=""
[ -n "$JOBS" ] || JOBS=8
DO_LOCALEGEN=1
REFRESH_DB=0
DO_CLEAN=0

# ---------------------------------------------------------------- arg parsing
while [ $# -gt 0 ]; do
  case "$1" in
    --modules)        MODULES_DIR=$2; shift 2 ;;
    --firmware)       FIRMWARE_DIR=$2; shift 2 ;;
    --size)           IMG_SIZE=$2; shift 2 ;;
    --jobs)           JOBS=$2; shift 2 ;;
    --no-locale-gen)  DO_LOCALEGEN=0; shift ;;
    --refresh-db)     REFRESH_DB=1; shift ;;
    --mango)          MANGO_DIR=$2; shift 2 ;;
    --also-ext4)      ALSO_EXT4=1; shift ;;
    --f2fs-sector-size) F2FS_SECTOR_SIZE=$2; shift 2 ;;
    --clean)          DO_CLEAN=1; shift ;;
    --no-fakeroot)    export ELISH_NO_FAKEROOT=1; shift ;;
    -h|--help)        sed -n '2,36p' "$SCRIPT_PATH"; exit 0 ;;
    *)                echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

# ------------------------------------------------------------ fakeroot re-exec
# mke2fs -d and "bsdtar -c" must see uid/gid 0 for the files we own; fakeroot
# reports exactly that for files owned by the invoking user.
if [ -z "$(env | grep -c '^FAKEROOTKEY=')" ] && [ -z "$(printenv ELISH_NO_FAKEROOT || true)" ]; then
  command -v fakeroot >/dev/null 2>&1 || { echo "fakeroot is required" >&2; exit 2; }
  exec fakeroot -- env ELISH_IN_FAKEROOT=1 bash "$SCRIPT_PATH" "$@"
fi

mkdir -p "$STATE" "$OUT_DIR"
LOG=$STATE/build.log
: > "$LOG"

log()    { printf '[%s] %s\n' "$(date -u +%H:%M:%S)" "$*" | tee -a "$LOG"; }
warn()   { printf '[%s] WARN: %s\n' "$(date -u +%H:%M:%S)" "$*" | tee -a "$LOG" >&2; }
die()    { printf '[%s] FATAL: %s\n' "$(date -u +%H:%M:%S)" "$*" | tee -a "$LOG" >&2; exit 1; }
banner() { log "=== $* ==="; }

need_cmd() {
  local c
  for c in "$@"; do
    command -v "$c" >/dev/null 2>&1 || die "required command not found: $c"
  done
}

parse_size() {
  python3 - "$1" <<'PYEOF'
import sys, re
s = sys.argv[1].strip()
m = re.match(r"^([0-9]+)([KkMmGgTt]?)$", s)
if not m:
    sys.exit("bad size: " + s)
print(int(m.group(1)) * {"": 1, "K": 1024, "M": 1024**2, "G": 1024**3, "T": 1024**4}[m.group(2).upper()])
PYEOF
}

# ================================================================ fetch stage
fetch_base() {
  banner "stage: fetch ALARM aarch64 root tarball"
  mkdir -p "$DL"
  local tarball=$DL/ArchLinuxARM-aarch64-latest.tar.gz t0 t1
  if [ -s "$tarball" ]; then
    log "cached: $tarball ($(stat -c %s "$tarball") bytes)"
  else
    t0=$(date +%s)
    curl -fL --retry 3 -C - -o "$tarball" "$ALARM_BASE_URL" \
      -w 'http=%{http_code} bytes=%{size_download} avg_speed_Bps=%{speed_download} url=%{url_effective}\n' \
      | tee -a "$LOG"
    t1=$(date +%s)
    log "download wall time: $((t1 - t0))s"
  fi
  [ -s "$tarball" ] || die "base tarball missing"
  BASE_TARBALL=$tarball
  BASE_SIZE=$(stat -c %s "$tarball")
  log "base tarball: $BASE_SIZE bytes"
}

fetch_dbs() {
  banner "stage: fetch ALARM package databases"
  mkdir -p "$DL/alarmdb"
  local repo f
  for repo in $REPOS; do
    f=$DL/alarmdb/$repo.db.tar.gz
    if [ -s "$f" ] && [ "$REFRESH_DB" -eq 0 ]; then
      log "cached: $repo.db.tar.gz ($(stat -c %s "$f") bytes)"
      continue
    fi
    curl -fL --retry 3 -o "$f" "$ALARM_MIRROR/$repo/$repo.db.tar.gz" \
      -w "$repo.db http=%{http_code} bytes=%{size_download}\n" | tee -a "$LOG"
  done
}

fetch_qemu() {
  banner "stage: fetch qemu-user-static (aarch64 user-mode emulation)"
  if [ -x "$QEMU" ]; then
    log "cached: $("$QEMU" --version | head -1)"
    return 0
  fi
  mkdir -p "$QEMU_DIR"
  local pkg=$DL/$QEMU_PKG_FILE
  if [ ! -s "$pkg" ]; then
    curl -fL --retry 3 -o "$pkg" "$QEMU_PKG_URL" \
      -w 'qemu-user-static http=%{http_code} bytes=%{size_download}\n' | tee -a "$LOG"
  fi
  bsdtar -x -f "$pkg" -C "$QEMU_DIR" usr/bin/qemu-aarch64-static
  chmod 755 "$QEMU"
  log "installed: $("$QEMU" --version | head -1)"
}


# ============================================================ f2fs host tools
fetch_f2fs_tools() {
  banner "stage: fetch f2fs-tools (host x86_64, used to create the image)"
  if [ -x "$F2FS_BIN/mkfs.f2fs" ]; then
    log "cached: $(f2fs_run mkfs.f2fs -V 2>&1 | head -1)"
    return 0
  fi
  mkdir -p "$F2FS_DIR" "$DL"
  local pkg=$DL/$F2FS_PKG_FILE
  if [ ! -s "$pkg" ]; then
    curl -fL --retry 3 -o "$pkg" "$ARCH_MIRROR/extra/os/x86_64/$F2FS_PKG_FILE"       -w 'f2fs-tools http=%{http_code} bytes=%{size_download}
' | tee -a "$LOG"
  fi
  bsdtar -x -f "$pkg" -C "$F2FS_DIR"
  chmod 755 "$F2FS_BIN"/*
  log "installed: $(f2fs_run mkfs.f2fs -V 2>&1 | head -1) ($(ls "$F2FS_BIN" | wc -l) tools)"
}

# ============================================================== resolve stage
write_resolver() {
  mkdir -p "$STATE"
  cat > "$STATE/resolve.py" <<'PYEOF'
#!/usr/bin/env python3
"""Offline dependency resolver for the ALARM repo databases (core/extra/alarm).

Reads <repo>.db.tar.gz (desc + depends members), resolves the dependency closure
of the requested packages and prints one TSV row per package to add:
    repo TAB filename TAB name TAB version TAB csize TAB sha256
Packages already present in the base rootfs are skipped.
"""
import sys, os, tarfile, re

REPO_PRIO = {"core": 0, "extra": 1, "alarm": 2}
# Virtual dependency names must never drag in a foreign GPU-vendor package.
PREFER = {"opengl-driver": "mesa", "libva-driver": "mesa", "libgl": "libglvnd",
          "libegl": "libglvnd", "libgles": "libglvnd"}
DENY = {"nvidia-utils", "nvidia-open-dkms", "nvidia-settings", "mesa-amber",
        "vulkan-asahi", "vulkan-broadcom", "vulkan-dzn", "vulkan-gfxstream",
        "vulkan-intel", "vulkan-nouveau", "vulkan-radeon", "vulkan-swrast",
        "vulkan-virtio", "vulkan-huawei", "vulkan-panfrost", "vulkan-powervr"}


def parse_fields(text):
    rec, cur = {}, None
    for line in text.splitlines():
        if len(line) > 2 and line.startswith("%") and line.endswith("%"):
            cur = line.strip("%")
            rec[cur] = []
        elif line.strip() == "":
            cur = None
        elif cur is not None:
            rec[cur].append(line.strip())
    return rec


def load_db(dbdir):
    pkgs = {}
    for repo in REPO_PRIO:
        path = os.path.join(dbdir, repo + ".db.tar.gz")
        if not os.path.exists(path):
            continue
        entries = {}
        with tarfile.open(path, "r:gz") as tf:
            for m in tf.getmembers():
                if not (m.name.endswith("/desc") or m.name.endswith("/depends")):
                    continue
                base = m.name.rpartition("/")[0]
                try:
                    txt = tf.extractfile(m).read().decode("utf-8", "replace")
                except Exception:
                    continue
                entries.setdefault(base, {}).update(parse_fields(txt))
        for base, rec in entries.items():
            name = rec.get("NAME", [None])[0]
            if not name:
                continue
            rec["_repo"] = repo
            rec["_dir"] = base
            pkgs.setdefault(name.lower(), []).append(rec)
    return pkgs


def segs(v):
    return re.findall(r"[0-9]+|[A-Za-z]+", v)


def cmp_segs(sa, sb):
    for i in range(max(len(sa), len(sb))):
        x = sa[i] if i < len(sa) else "0"
        y = sb[i] if i < len(sb) else "0"
        if x.isdigit() and y.isdigit():
            if int(x) != int(y):
                return -1 if int(x) < int(y) else 1
        elif x != y:
            return -1 if x < y else 1
    return 0


def vercmp(a, b):
    def split(v):
        epoch = 0
        if ":" in v:
            e, v = v.split(":", 1)
            epoch = int(e)
        rel = None
        if "-" in v:
            v, rel = v.rsplit("-", 1)
        return epoch, segs(v), rel

    ea, sa, ra = split(a)
    eb, sb, rb = split(b)
    if ea != eb:
        return -1 if ea < eb else 1
    c = cmp_segs(sa, sb)
    if c:
        return c
    if ra and rb:
        return cmp_segs(segs(ra), segs(rb))
    return 0


def check_constraint(ver, depstr):
    m = re.match(r"^([^<>=]+?)(>=|<=|=|>|<)(.+)$", depstr)
    if not m:
        return True
    c = vercmp(ver, m.group(3))
    return {"=": c == 0, ">=": c >= 0, "<=": c <= 0, ">": c > 0, "<": c < 0}[m.group(2)]


def dep_base(dep):
    m = re.match(r"^([^<>=]+?)(>=|<=|=|>|<).*$", dep)
    return m.group(1) if m else dep


def build_provides_index(pkgs):
    idx = {}
    for name, recs in pkgs.items():
        for rec in recs:
            for p in rec.get("PROVIDES", []):
                idx.setdefault(re.split(r"[=<>]", p)[0].lower(), []).append(rec)
    return idx


def pick(recs, base=None):
    recs = [r for r in recs if r["NAME"][0] not in DENY]
    if not recs:
        return None
    want = PREFER.get(base)
    if want:
        for r in recs:
            if r["NAME"][0] == want:
                return r
    return sorted(recs, key=lambda r: (REPO_PRIO.get(r["_repo"], 9), r["NAME"][0]))[0]


def main():
    dbdir, installed_file, requested = sys.argv[1], sys.argv[2], sys.argv[3:]
    pkgs = load_db(dbdir)
    prov = build_provides_index(pkgs)
    installed = {}
    if installed_file != "-" and os.path.exists(installed_file):
        for line in open(installed_file):
            line = line.strip()
            if line:
                n, _, v = line.partition(" ")
                installed[n.lower()] = v

    chosen, warnings, seen = {}, [], set()
    queue = list(requested)
    while queue:
        dep = queue.pop(0)
        if dep in seen:
            continue
        seen.add(dep)
        base = dep_base(dep).lower()
        if base.startswith("cmd:"):
            base = base[4:]
        if base in installed and check_constraint(installed[base], dep):
            continue
        if base in chosen:
            continue
        rec = None
        if base in pkgs:
            rec = pick(pkgs[base], base)
        elif base in prov:
            rec = pick(prov[base], base)
        if rec is None:
            if base not in installed:
                warnings.append("UNRESOLVED " + dep)
            continue
        name = rec["NAME"][0]
        if name in installed or name in chosen:
            continue
        chosen[name] = rec
        for d in rec.get("DEPENDS", []):
            queue.append(d)

    total, rows = 0, []
    for name in sorted(chosen):
        rec = chosen[name]
        cs = (rec.get("CSIZE", ["0"])[0] or "0").strip() or "0"
        total += int(cs)
        rows.append("\t".join([rec["_repo"], rec.get("FILENAME", [""])[0], name,
                               rec.get("VERSION", [""])[0], cs,
                               rec.get("SHA256SUM", [""])[0]]))
    for w in sorted(set(warnings)):
        print("WARN " + w, file=sys.stderr)
    print("SUMMARY packages=%d csize_bytes=%d" % (len(rows), total), file=sys.stderr)
    sys.stdout.write("\n".join(rows) + ("\n" if rows else ""))


if __name__ == "__main__":
    main()
PYEOF
}

list_installed() {
  local root=$1 d n v
  for d in "$root"/var/lib/pacman/local/*/; do
    [ -f "$d/desc" ] || continue
    n=$(sed -n '/^%NAME%$/{n;p;q}' "$d/desc")
    v=$(sed -n '/^%VERSION%$/{n;p;q}' "$d/desc")
    [ -n "$n" ] && echo "$n $v"
  done | sort
}

resolve_packages() {
  banner "stage: resolve package closure"
  write_resolver
  list_installed "$TREE" > "$STATE/installed.txt"
  log "base rootfs records $(wc -l < "$STATE/installed.txt") packages"
  # shellcheck disable=SC2046
  python3 "$STATE/resolve.py" "$DL/alarmdb" "$STATE/installed.txt" \
      $(grep -h -v -e '^#' -e '^$' "$PKG_LIST") \
      > "$STATE/pkgs.tsv" 2> "$STATE/resolve.log"
  tee -a "$LOG" < "$STATE/resolve.log"
  [ -s "$STATE/pkgs.tsv" ] || die "resolver produced no packages"
  log "resolved $(wc -l < "$STATE/pkgs.tsv") packages to add"
}

# ============================================================= download stage
download_packages() {
  banner "stage: download ALARM aarch64 packages"
  mkdir -p "$PKGCACHE"
  local helper=$STATE/dlone.sh wl=$STATE/download.list repo file name ver cs sha t0 t1
  cat > "$helper" <<'SHEOF'
#!/usr/bin/env bash
set -euo pipefail
url=$1; out=$2; sha=$3
if [ -s "$out" ]; then
  if [ -z "$sha" ]; then exit 0; fi
  if echo "$sha  $out" | sha256sum -c --status -; then exit 0; fi
  rm -f "$out"
fi
curl -fsSL --retry 3 -o "$out.part" "$url"
if [ -n "$sha" ]; then
  echo "$sha  $out.part" | sha256sum -c --status - || {
    rm -f "$out.part"; echo "SHA256 MISMATCH: $url" >&2; exit 1; }
fi
mv "$out.part" "$out"
SHEOF
  chmod 755 "$helper"
  : > "$wl"
  while IFS=$(printf '\t') read -r repo file name ver cs sha; do
    [ -n "$file" ] || continue
    printf '%s\t%s\t%s\n' "$ALARM_MIRROR/$repo/$file" "$PKGCACHE/$file" "$sha" >> "$wl"
  done < "$STATE/pkgs.tsv"
  t0=$(date +%s)
  xargs -P "$JOBS" -L 1 "$helper" < "$wl"
  t1=$(date +%s)
  log "downloaded $(wc -l < "$wl") packages in $((t1 - t0))s, cache $(du -sb "$PKGCACHE" | cut -f1) bytes"
}

# ================================================================ tree stage
tree_stamp() {
  {
    echo "base=$BASE_SIZE"
    sha256sum < "$STATE/pkgs.tsv"
  } | sha256sum | cut -d' ' -f1
}

write_db_entry_writer() {
  cat > "$STATE/mkdbentry.py" <<'PYEOF'
"""Create /var/lib/pacman/local/<name>-<ver>/{desc,files,mtree} for one package.

build-rootfs.sh unpacks packages with bsdtar (no root), which leaves pacman's
local database unaware of them: 'pacman -Q' does not list the package and any
later install that touches the same paths aborts with
'<pkg>: /path exists in filesystem'.  This writer derives the database entry
from the package's own metadata (.PKGINFO / .FILES / .MTREE), which is exactly
what pacman would have written.
"""
import os
import sys
import tarfile

# .PKGINFO key -> %SECTION% in the local database entry
SECTION = {
    "pkgname": "NAME",
    "pkgver": "VERSION",
    "pkgbase": "BASE",
    "pkgdesc": "DESC",
    "url": "URL",
    "arch": "ARCH",
    "builddate": "BUILDDATE",
    "packager": "PACKAGER",
    "size": "SIZE",
    "license": "LICENSE",
    "depend": "DEPENDS",
    "optdepend": "OPTDEPENDS",
    "provides": "PROVIDES",
    "conflict": "CONFLICTS",
    "replaces": "REPLACES",
    "group": "GROUPS",
}
# pacman writes these sections in this order
ORDER = ["NAME", "VERSION", "BASE", "DESC", "URL", "ARCH", "BUILDDATE",
         "PACKAGER", "SIZE", "REASON", "LICENSE", "VALIDATION", "DEPENDS",
         "OPTDEPENDS", "PROVIDES", "CONFLICTS", "REPLACES", "GROUPS"]


def main():
    pkg, tree = sys.argv[1], sys.argv[2]
    with tarfile.open(pkg) as tf:
        names = tf.getnames()
        try:
            pkginfo = tf.extractfile(".PKGINFO").read().decode("utf-8", "replace")
        except (KeyError, AttributeError):
            print("WARN no .PKGINFO in %s" % pkg, file=sys.stderr)
            return 0
        files_raw = b""
        if ".FILES" in names:
            files_raw = tf.extractfile(".FILES").read()
        mtree = b""
        if ".MTREE" in names:
            mtree = tf.extractfile(".MTREE").read()

    values = {}
    for line in pkginfo.splitlines():
        if not line or line.startswith("#") or " = " not in line:
            continue
        key, val = line.split(" = ", 1)
        values.setdefault(SECTION.get(key, key), []).append(val)

    if "NAME" not in values or "VERSION" not in values:
        print("WARN incomplete .PKGINFO in %s" % pkg, file=sys.stderr)
        return 0
    name = values["NAME"][0]
    version = values["VERSION"][0]
    dbdir = os.path.join(tree, "var/lib/pacman/local", "%s-%s" % (name, version))
    os.makedirs(dbdir, exist_ok=True)

    values.setdefault("REASON", ["0"])
    lines = []
    for section in ORDER:
        if section not in values:
            continue
        lines.append("%%%s%%" % section)
        lines.extend(values[section])
        lines.append("")
    with open(os.path.join(dbdir, "desc"), "w", encoding="utf-8") as fh:
        fh.write("\n".join(lines).rstrip("\n") + "\n")

    if not files_raw and ".FILES" not in names:
        # derive the file list from the archive itself (dot-files are metadata)
        files_raw = "\n".join(
            n for n in names if not n.startswith(".")).encode()
    with open(os.path.join(dbdir, "files"), "wb") as fh:
        fh.write(b"%FILES%\n")
        fh.write(files_raw if files_raw.endswith(b"\n") else files_raw + b"\n")

    if mtree:
        with open(os.path.join(dbdir, "mtree"), "wb") as fh:
            fh.write(mtree)

    for entry in ("desc", "files", "mtree"):
        path = os.path.join(dbdir, entry)
        if os.path.exists(path):
            os.chmod(path, 0o644)
    return 0


if __name__ == "__main__":
    sys.exit(main())
PYEOF
}

extract_base() {
  banner "stage: extract the ALARM base tarball"
  chmod -R u+rwX "$TREE" 2>/dev/null || true
  rm -rf "$TREE"
  mkdir -p "$TREE"
  local t0 t1
  t0=$(date +%s)
  # --no-same-owner: chown is not permitted; fakeroot already reports the files
  # we own as 0:0, which is what mke2fs -d and bsdtar will record.
  bsdtar -x -p -f "$BASE_TARBALL" -C "$TREE" --no-same-owner --no-xattrs --no-acls
  t1=$(date +%s)
  log "extracted in $((t1 - t0))s: $(find "$TREE" | wc -l) entries, $(du -sh "$TREE" | cut -f1) on disk"
}

extract_packages() {
  banner "stage: unpack ALARM packages into the tree"
  write_db_entry_writer
  : > "$STATE/install-scripts.txt"
  local repo file name ver cs sha pkg t0 t1 n=0
  t0=$(date +%s)
  while IFS=$(printf '\t') read -r repo file name ver cs sha; do
    [ -n "$file" ] || continue
    pkg=$PKGCACHE/$file
    if bsdtar -tf "$pkg" | grep -qx '.INSTALL'; then
      printf '%s %s (%s)\n' "$name" "$ver" "$repo" >> "$STATE/install-scripts.txt"
    fi
    bsdtar -x -p -f "$pkg" -C "$TREE" --no-same-owner \
      --no-xattrs --no-acls \
      --exclude='.PKGINFO' --exclude='.BUILDINFO' --exclude='.MTREE' --exclude='.INSTALL'
    python3 "$STATE/mkdbentry.py" "$pkg" "$TREE" || warn "could not register $name in the pacman db"
    n=$((n + 1))
  done < "$STATE/pkgs.tsv"
  rm -f "$TREE/.PKGINFO" "$TREE/.BUILDINFO" "$TREE/.MTREE" "$TREE/.INSTALL"
  t1=$(date +%s)
  log "unpacked $n packages in $((t1 - t0))s"
  {
    echo "# Packages added on top of the ALARM base tarball by build-rootfs.sh."
    echo "# They were unpacked with bsdtar and registered in /var/lib/pacman/local"
    echo "# by mkdbentry.py, so 'pacman -Q' lists them and later installs do not"
    echo "# trip over 'file exists in filesystem'."
    echo "# name version repo"
    awk -F'\t' '{printf "%s %s %s\n", $3, $4, $1}' "$STATE/pkgs.tsv"
  } > "$TREE/etc/elish-packages.txt"
  chmod 644 "$TREE/etc/elish-packages.txt"
  if [ -s "$STATE/install-scripts.txt" ]; then
    warn "$(wc -l < "$STATE/install-scripts.txt") added packages ship a .INSTALL script which is NOT executed"
  fi
}

snapshot_pristine() {
  banner "stage: snapshot pristine /etc"
  local files="etc/passwd etc/group etc/shadow etc/gshadow etc/fstab etc/hostname etc/hosts etc/locale.gen etc/locale.conf etc/resolv.conf etc/sudoers etc/pacman.conf"
  # shellcheck disable=SC2086
  tar -C "$TREE" --ignore-failed-read -cf "$STATE/pristine.tar" $files etc/sudoers.d 2>/dev/null || true
  log "pristine snapshot: $(stat -c %s "$STATE/pristine.tar") bytes"
}

restore_pristine() {
  [ -f "$STATE/pristine.tar" ] || die "pristine snapshot missing (state was wiped without rebuilding)"
  banner "stage: restore pristine /etc before customisation (idempotent re-runs)"
  tar -C "$TREE" -xpf "$STATE/pristine.tar"
}

# ======================================================== aarch64 exec helper
qemu_guest() {
  "$QEMU" -L "$TREE" "$@"
}

run_ldconfig() {
  banner "stage: rebuild the aarch64 ld.so.cache"
  rm -f "$TREE/etc/ld.so.cache"
  log "cmd: qemu-aarch64-static -L <rootfs> <rootfs>/usr/bin/ldconfig -r <rootfs>"
  qemu_guest "$TREE/usr/bin/ldconfig" -r "$TREE" 2>&1 | tee -a "$LOG" | tail -3
  [ -s "$TREE/etc/ld.so.cache" ] || die "ld.so.cache was not generated"
  log "ld.so.cache: $(stat -c %s "$TREE/etc/ld.so.cache") bytes"
}

run_locale_gen() {
  if [ "$DO_LOCALEGEN" -ne 1 ]; then
    warn "locale compilation skipped (--no-locale-gen); /etc/locale.conf is enough for LANG to work but non-UTF-8 C locale fallbacks may differ"
    return 0
  fi
  banner "stage: compile locales with the aarch64 localedef"
  local loc cs input t0 t1
  t0=$(date +%s)
  while read -r loc cs; do
    case "$loc" in ''|'#'*) continue ;; esac
    [ -n "$cs" ] || continue
    # "en_US.UTF-8 UTF-8" -> input "en_US", charset "UTF-8", name "en_US.UTF-8"
    input=$(echo "$loc" | cut -d. -f1)
    log "localedef --prefix=<rootfs> -i $input -f $cs $loc"
    qemu_guest "$TREE/usr/bin/localedef" --prefix="$TREE" -i "$input" -f "$cs" "$loc" \
      2>&1 | tee -a "$LOG" | tail -5
  done < "$CONFIG_DIR/etc/locale.gen.wanted"
  t1=$(date +%s)
  [ -s "$TREE/usr/lib/locale/locale-archive" ] || die "locale-archive missing"
  log "locale-archive: $(stat -c %s "$TREE/usr/lib/locale/locale-archive") bytes in $((t1 - t0))s"
  qemu_guest "$TREE/usr/bin/localedef" --prefix="$TREE" --list-archive | tail -4 | tee -a "$LOG"
}

# ============================================================ customise stage
apply_static_configs() {
  banner "stage: install config files from configs/rootfs"
  local src dst
  for src in "$CONFIG_DIR/etc/fstab" "$CONFIG_DIR/etc/hostname" "$CONFIG_DIR/etc/hosts" \
             "$CONFIG_DIR/etc/locale.conf" "$CONFIG_DIR/etc/resolv.conf"; do
    dst=$TREE/etc/$(basename "$src")
    rm -f "$dst"
    install -D -m 644 "$src" "$dst"
    log "installed /etc/$(basename "$src")"
  done
  install -D -m 440 "$CONFIG_DIR/etc/sudoers.d/10-wheel" "$TREE/etc/sudoers.d/10-wheel"
  install -D -m 644 "$CONFIG_DIR/usr/lib/udev/rules.d/81-libssc-xiaomi-elish.rules" \
    "$TREE/usr/lib/udev/rules.d/81-libssc-xiaomi-elish.rules"
  install -D -m 644 "$CONFIG_DIR/usr/share/alsa/ucm2/Xiaomi/elish/elish.conf" \
    "$TREE/usr/share/alsa/ucm2/Xiaomi/elish/elish.conf"
  install -D -m 644 "$CONFIG_DIR/usr/share/alsa/ucm2/Xiaomi/elish/HiFi.conf" \
    "$TREE/usr/share/alsa/ucm2/Xiaomi/elish/HiFi.conf"
  install -D -m 644 "$CONFIG_DIR/usr/share/hexagonrpcd/hexagonrpcd-sdsp.conf" \
    "$TREE/usr/share/hexagonrpcd/hexagonrpcd-sdsp.conf"
  mkdir -p "$TREE/usr/share/alsa/ucm2/conf.d/sm8250"
  ln -sfn ../../Xiaomi/elish/elish.conf \
    "$TREE/usr/share/alsa/ucm2/conf.d/sm8250/Xiaomi Mi Pad 5 Pro.conf"
  log "installed udev rule, ALSA UCM2 profile + conf.d link, hexagonrpcd confd"
}

apply_locales() {
  banner "stage: enable locales in /etc/locale.gen"
  python3 - "$TREE/etc/locale.gen" "$CONFIG_DIR/etc/locale.gen.wanted" <<'PYEOF'
import sys
gen_path, want_path = sys.argv[1], sys.argv[2]
wanted = []
for line in open(want_path):
    line = line.strip()
    if line and not line.startswith("#"):
        wanted.append(" ".join(line.split()))
lines = open(gen_path).read().splitlines()
out, seen = [], set()
for line in lines:
    stripped = line.strip()
    if stripped.startswith("#"):
        cand = " ".join(stripped.lstrip("#").split())
        for w in wanted:
            if cand == w:
                line = w
                seen.add(w)
                break
    out.append(line)
for w in wanted:
    if w not in seen:
        out.append(w)
open(gen_path, "w").write("\n".join(out) + "\n")
print("locale.gen activated:", wanted)
PYEOF
  grep -v -e '^#' -e '^$' "$TREE/etc/locale.gen" | tee -a "$LOG"
}

apply_timezone() {
  banner "stage: set the timezone to Asia/Shanghai"
  [ -f "$TREE/usr/share/zoneinfo/Asia/Shanghai" ] || die "zoneinfo/Asia/Shanghai is missing"
  ln -sfn ../usr/share/zoneinfo/Asia/Shanghai "$TREE/etc/localtime"
  echo "Asia/Shanghai" > "$TREE/etc/timezone"
  log "/etc/localtime -> $(readlink "$TREE/etc/localtime")"
}

apply_users() {
  banner "stage: user magma, locked root, wheel in sudoers"
  local hash
  hash=$(openssl passwd -6 -salt elishmagma magma)
  chmod 0600 "$TREE/etc/shadow" "$TREE/etc/gshadow" 2>/dev/null || true
  python3 - "$TREE" "$hash" <<'PYEOF'
import sys, os, subprocess
tree, pw_hash = sys.argv[1], sys.argv[2]
USER, UID, GID = "magma", 1000, 1000
SUPP = ["wheel", "audio", "video", "input", "render", "kvm", "lp", "optical",
        "rfkill", "storage", "network", "power", "games", "users"]


def read(p):
    with open(os.path.join(tree, p)) as f:
        return [l.rstrip("\n") for l in f if l.strip()]


def write(p, lines, mode=None):
    path = os.path.join(tree, p)
    with open(path, "w") as f:
        f.write("\n".join(lines) + "\n")
    if mode:
        os.chmod(path, mode)


pw = [l for l in read("etc/passwd") if l.split(":")[2] != str(UID)]
pw.append(":".join([USER, "x", str(UID), str(GID), "", "/home/" + USER, "/bin/bash"]))
write("etc/passwd", pw, 0o644)

sh, out = read("etc/shadow"), []
for line in sh:
    f = line.split(":")
    if f[0] in (USER, "alarm"):
        continue
    if f[0] == "root" and not f[1].startswith("!"):
        f[1] = "!" + f[1]
        line = ":".join(f)
    out.append(line)
out.append(":".join([USER, pw_hash, "19000", "0", "99999", "7", "", "", ""]))
write("etc/shadow", out, 0o600)

gr = [l for l in read("etc/group") if l.split(":")[2] != str(GID)]
names = [l.split(":")[0] for l in gr]
# the stock "alarm" user is removed above, so drop it from every member list
for _i, _line in enumerate(gr):
    _f = _line.split(":")
    if len(_f) == 4 and _f[3]:
        _f[3] = ",".join([m for m in _f[3].split(",") if m and m != "alarm"])
        gr[_i] = ":".join(_f)
for g in SUPP:
    if g not in names:
        continue
    for i, line in enumerate(gr):
        f = line.split(":")
        if f[0] != g:
            continue
        members = [m for m in f[3].split(",") if m]
        if USER not in members:
            members.append(USER)
        f[3] = ",".join(members)
        gr[i] = ":".join(f)
# pick a free gid for the seat group (ALARM base already uses 999=adm)
_used = set()
for _line in gr:
    _f = _line.split(":")
    if len(_f) >= 3 and _f[2].isdigit():
        _used.add(int(_f[2]))
_seat_gid = 996
while _seat_gid in _used and _seat_gid > 100:
    _seat_gid -= 1
if "seat" not in [g.split(":")[0] for g in gr]:
    gr.append(":".join(["seat", "x", str(_seat_gid), USER]))
else:
    for _i, _line in enumerate(gr):
        _f = _line.split(":")
        if _f[0] != "seat":
            continue
        _m = [m for m in _f[3].split(",") if m]
        if USER not in _m:
            _f[3] = ",".join(_m + [USER])
            gr[_i] = ":".join(_f)
gr.append(":".join([USER, "x", str(GID), ""]))
write("etc/group", gr, 0o644)

try:
    gs = [l for l in read("etc/gshadow")
          if l.split(":")[0] != "alarm" and l.split(":")[2] != str(GID)]
except FileNotFoundError:
    gs = []
for _i, _line in enumerate(gs):
    _f = _line.split(":")
    if len(_f) == 4 and _f[3]:
        _f[3] = ",".join([m for m in _f[3].split(",") if m and m != "alarm"])
        gs[_i] = ":".join(_f)
for i, line in enumerate(gs):
    f = line.split(":")
    if f[0] in SUPP:
        members = [m for m in f[3].split(",") if m]
        if USER not in members:
            f[3] = ",".join(members + [USER])
            gs[i] = ":".join(f)
# keep /etc/gshadow in sync with the seat group we may have just added:
# a group present in /etc/group but missing from /etc/gshadow makes
# shadow.service fail ('Verify integrity of password and group files')
# and leaves the system in the 'degraded' state.
if "seat" not in [l.split(":")[0] for l in gs]:
    gs.append(":".join(["seat", "!", "", USER]))
gs.append(":".join([USER, "!", "", ""]))
write("etc/gshadow", gs, 0o600)

home = os.path.join(tree, "home", USER)
subprocess.run(["rm", "-rf", os.path.join(tree, "home", "alarm")], check=False)
os.makedirs(home, exist_ok=True)
os.chown(home, UID, GID)
os.chmod(home, 0o755)
print("created user", USER, "uid", UID, "gid", GID)
PYEOF
  log "passwd: $(grep -c '^' "$TREE/etc/passwd") entries; magma -> $(grep '^magma:' "$TREE/etc/passwd")"
  log "shadow: root=$(cut -d: -f2 "$TREE/etc/shadow" | head -1 | cut -c1-3)... magma_hash_method=$(cut -d'$' -f2 <<< "$hash")"
  log "group wheel: $(grep '^wheel:' "$TREE/etc/group")"
  log "verifying /etc/sudoers with the aarch64 visudo"
  qemu_guest "$TREE/usr/bin/visudo" -c -f "$TREE/etc/sudoers" 2>&1 | tee -a "$LOG" || warn "visudo check returned non-zero"
}

apply_sudoers() {
  banner "stage: activate the %wheel entry in /etc/sudoers"
  local f=$TREE/etc/sudoers
  if [ ! -f "$f" ]; then
    warn "/etc/sudoers is missing (sudo package not installed?)"
    return 0
  fi
  chmod 0640 "$f"
  python3 - "$f" <<'PYEOF'
import sys
path = sys.argv[1]
want = "%wheel ALL=(ALL:ALL) ALL"
key = want.replace(" ", "")
lines = open(path).read().splitlines()
done = False
for i, line in enumerate(lines):
    s = line.strip()
    if s.startswith("#") and s.lstrip("#").strip().replace(" ", "") == key:
        lines[i] = want
        done = True
if not done and want not in lines:
    lines.append(want)
open(path, "w").write("\n".join(lines) + "\n")
print("sudoers %wheel line active:", done)
PYEOF
  chmod 440 "$f"
  grep -n '^%wheel' "$f" | tee -a "$LOG"
}

prune_boot() {
  banner "stage: prune kernel and initramfs from /boot (they belong on boot_b)"
  local before after
  before=$(du -sb "$TREE/boot" | cut -f1)
  find "$TREE/boot" -mindepth 1 -maxdepth 1 -exec rm -rf {} +
  mkdir -p "$TREE/boot"
  after=$(du -sb "$TREE/boot" | cut -f1)
  log "/boot pruned: $before -> $after bytes; no kernel/initramfs shipped in this rootfs"
  ls -la "$TREE/boot" | tee -a "$LOG"
}

systemd_enable() {
  local unit=$1 dir src install target alias link
  src=""
  for dir in usr/lib/systemd/system etc/systemd/system; do
    if [ -f "$TREE/$dir/$unit" ]; then src=/$dir/$unit; break; fi
  done
  if [ -z "$src" ]; then
    warn "systemd unit not present in the rootfs: $unit (enable skipped)"
    return 0
  fi
  install=$(awk '/^[[]/{f=0} /^[[]Install[]]/{f=1} f' "$TREE$src")
  echo "$install" | sed -n 's/^WantedBy=//p' | tr ' ' '\n' | while read -r target; do
    [ -n "$target" ] || continue
    mkdir -p "$TREE/etc/systemd/system/$target.wants"
    ln -sfn "$(realpath -m --relative-to="$TREE/etc/systemd/system/$target.wants" "$TREE$src")" \
      "$TREE/etc/systemd/system/$target.wants/$unit"
  done
  echo "$install" | sed -n 's/^RequiredBy=//p' | tr ' ' '\n' | while read -r target; do
    [ -n "$target" ] || continue
    mkdir -p "$TREE/etc/systemd/system/$target.requires"
    ln -sfn "$(realpath -m --relative-to="$TREE/etc/systemd/system/$target.requires" "$TREE$src")" \
      "$TREE/etc/systemd/system/$target.requires/$unit"
  done
  echo "$install" | sed -n 's/^Alias=//p' | tr ' ' '\n' | while read -r alias; do
    [ -n "$alias" ] || continue
    mkdir -p "$TREE/etc/systemd/system"
    ln -sfn "$(realpath -m --relative-to="$TREE/etc/systemd/system" "$TREE$src")" \
      "$TREE/etc/systemd/system/$alias"
  done
  log "enabled $unit (from $src)"
}

apply_systemd() {
  banner "stage: enable systemd units"
  # NetworkManager is chosen over systemd-networkd: the tablet connects through
  # ath11k 802.11ax and bluez; NetworkManager provides nmcli, WiFi profile
  # handling, polkit integration and a correct default route out of the box,
  # whereas networkd would need hand-written .network files plus a separate
  # wpa_supplicant unit and configuration for the same result.
  local u
  for u in NetworkManager.service systemd-timesyncd.service bluetooth.service \
           seatd.service fstrim.timer systemd-oomd.service sshd.service sshdgenkeys.service; do
    systemd_enable "$u"
  done
  # template unit: getty@tty1 must be enabled by hand (there is no getty@tty1.service file)
  if [ -f "$TREE/usr/lib/systemd/system/getty@.service" ]; then
    mkdir -p "$TREE/etc/systemd/system/getty.target.wants"
    ln -sfn ../../../usr/lib/systemd/system/getty@.service \
      "$TREE/etc/systemd/system/getty.target.wants/getty@tty1.service"
    log "enabled getty@tty1.service (from the getty@.service template)"
  else
    warn "getty@.service missing: tty1 autologin will not start"
  fi
  rm -f "$TREE/etc/systemd/system/multi-user.target.wants/systemd-networkd.service"
  rm -f "$TREE/etc/systemd/system/sockets.target.wants/systemd-networkd.socket"
  log "network stack: NetworkManager enabled, systemd-networkd left disabled"
  # the ALARM base ships some absolute symlinks (e.g. multi-user.target.wants/
  # remote-fs.target -> /usr/lib/...), which would dangle inside the flashed rootfs
  local l tgt n=0
  while IFS= read -r l; do
    tgt=$(readlink "$l")
    case "$tgt" in
      /*)
        if [ -e "$TREE$tgt" ]; then
          ln -sfn "$(realpath -m --relative-to="$(dirname "$l")" "$TREE$tgt")" "$l"
          n=$((n + 1))
        else
          warn "absolute symlink target missing in rootfs: $l -> $tgt"
        fi
        ;;
    esac
  done < <(find "$TREE/etc" -type l 2>/dev/null)
  log "rewrote $n absolute symlinks under /etc as relative links"
  find "$TREE/etc/systemd/system" -type l -printf '%p -> %l\n' | sort | tee -a "$LOG"
}

install_modules() {
  banner "stage: kernel modules (from the kernel module build)"
  if [ -z "$MODULES_DIR" ]; then
    warn "--modules not given; module installation SKIPPED"
    return 0
  fi
  local src kver ex d b libs
  if [ -f "$MODULES_DIR" ]; then
    ex=$WORK/modules-extract
    rm -rf "$ex"
    mkdir -p "$ex"
    bsdtar -x -f "$MODULES_DIR" -C "$ex" --no-same-owner
    src=$ex/lib/modules
    log "unpacked modules archive: $MODULES_DIR"
  elif [ -d "$MODULES_DIR/lib/modules" ]; then
    src=$MODULES_DIR/lib/modules
  elif [ -d "$MODULES_DIR" ]; then
    src=$MODULES_DIR
  else
    warn "modules source $MODULES_DIR does not exist; SKIPPED"
    return 0
  fi
  kver=$(ls "$src" | grep -E '^[0-9]' | head -1)
  if [ -z "$kver" ]; then
    warn "no <version> directory under $src; SKIPPED"
    return 0
  fi
  log "modules in the tree BEFORE install: $(ls "$TREE/lib/modules" 2>/dev/null | tr '\n' ' ')"
  if [ -d "$TREE/lib/modules" ]; then
    for d in "$TREE"/lib/modules/*; do
      [ -d "$d" ] || continue
      b=$(basename "$d")
      if [ "$b" != "$kver" ]; then
        log "removing stale module tree lib/modules/$b (not the running kernel)"
        rm -rf "$d"
      fi
    done
  fi
  mkdir -p "$TREE/lib/modules"
  rm -rf "$TREE/lib/modules/$kver"
  cp -a "$src/$kver" "$TREE/lib/modules/"
  log "modules in the tree AFTER install: $(ls "$TREE/lib/modules" | tr '\n' ' ')"
  libs=$(find "$TREE/lib/modules/$kver" -name '*.ko*' | wc -l)
  log "copied $libs module files into lib/modules/$kver"
  log "cmd: depmod -b <rootfs> $kver"
  depmod -b "$TREE" "$kver" 2>&1 | tee -a "$LOG"
  log "modules.dep lines: $(wc -l < "$TREE/lib/modules/$kver/modules.dep" 2>/dev/null) ; modules.dep.bin: $(stat -c %s "$TREE/lib/modules/$kver/modules.dep.bin" 2>/dev/null) bytes"
  log "modules.alias lines: $(wc -l < "$TREE/lib/modules/$kver/modules.alias" 2>/dev/null)"
  if [ -f "$TREE/lib/modules/$kver/kernel/drivers/gpu/drm/panel/panel-novatek-nt36523.ko.zst" ]; then
    log "panel module present: lib/modules/$kver/kernel/drivers/gpu/drm/panel/panel-novatek-nt36523.ko.zst ($(stat -c %s "$TREE/lib/modules/$kver/kernel/drivers/gpu/drm/panel/panel-novatek-nt36523.ko.zst") bytes)"
  else
    warn "panel-novatek-nt36523.ko.zst NOT found in lib/modules/$kver"
  fi
}

install_mango() {
  banner "stage: MangoWM runtime files"
  if [ -z "$MANGO_DIR" ]; then
    warn "--mango not given; MangoWM runtime installation SKIPPED"
    return 0
  fi
  if [ ! -d "$MANGO_DIR" ]; then
    warn "--mango $MANGO_DIR does not exist; SKIPPED"
    return 0
  fi
  local f missing=0 total=0
  for f in mango mangonag mmsg; do
    if [ -f "$MANGO_DIR/bin/$f" ]; then
      install -D -m 755 "$MANGO_DIR/bin/$f" "$TREE/usr/bin/$f"
      log "installed /usr/bin/$f ($(stat -c %s "$MANGO_DIR/bin/$f") bytes)"
    fi
  done
  for f in "$MANGO_DIR"/etc/mango/*; do
    [ -f "$f" ] || continue
    install -D -m 644 "$f" "$TREE/etc/mango/$(basename "$f")"
    log "installed /etc/mango/$(basename "$f")"
  done
  for f in "$MANGO_DIR"/share/wayland-sessions/*; do
    [ -f "$f" ] || continue
    install -D -m 644 "$f" "$TREE/usr/share/wayland-sessions/$(basename "$f")"
    log "installed /usr/share/wayland-sessions/$(basename "$f")"
  done
  for f in "$MANGO_DIR"/share/man/man1/*; do
    [ -f "$f" ] || continue
    install -D -m 644 "$f" "$TREE/usr/share/man/man1/$(basename "$f")"
    log "installed /usr/share/man/man1/$(basename "$f")"
  done
  for f in "$MANGO_DIR"/share/xdg-desktop-portal/*; do
    [ -f "$f" ] || continue
    install -D -m 644 "$f" "$TREE/usr/share/xdg-desktop-portal/$(basename "$f")"
    log "installed /usr/share/xdg-desktop-portal/$(basename "$f")"
  done
  for f in "$MANGO_DIR"/lib/systemd/user/*; do
    [ -f "$f" ] || continue
    install -D -m 644 "$f" "$TREE/usr/lib/systemd/user/$(basename "$f")"
    log "installed /usr/lib/systemd/user/$(basename "$f")"
  done
  # verify every DT_NEEDED of the installed binaries resolves inside the tree
  for f in mango mangonag mmsg; do
    [ -x "$TREE/usr/bin/$f" ] || continue
    for lib in $(LC_ALL=C readelf -d "$TREE/usr/bin/$f" 2>/dev/null | sed -n 's/.*Shared library: \[\(.*\)\]/\1/p'); do
      total=$((total + 1))
      if ! find "$TREE/usr/lib" -maxdepth 3 -name "$lib" -print -quit 2>/dev/null | grep -q .; then
        warn "unresolved DT_NEEDED in /usr/bin/$f: $lib"
        missing=$((missing + 1))
      fi
    done
  done
  log "mango DT_NEEDED resolution: $((total - missing))/$total resolved inside the rootfs"
  ls -l "$TREE/usr/bin/mango" "$TREE/etc/mango" "$TREE/usr/share/wayland-sessions" 2>&1 | tee -a "$LOG" | tail -8
}

install_firmware() {
  banner "stage: device firmware"
  if [ -z "$FIRMWARE_DIR" ]; then
    warn "--firmware not given; firmware installation SKIPPED (see docs/findings/rootfs.md)"
    return 0
  fi
  if [ ! -d "$FIRMWARE_DIR" ]; then
    warn "--firmware $FIRMWARE_DIR does not exist; firmware installation SKIPPED"
    return 0
  fi
  cp -a "$FIRMWARE_DIR/." "$TREE/"
  log "firmware: $(find "$FIRMWARE_DIR" -type f | wc -l) files, $(du -sh "$FIRMWARE_DIR" | cut -f1)"
  ls "$TREE/usr/lib/firmware/qcom/sm8250/xiaomi/elish/" 2>/dev/null | head -12 | tee -a "$LOG"
}

collect_special_modes() {
  banner "stage: collect suid/sgid modes from the source archives"
  python3 - "$BASE_TARBALL" "$PKGCACHE" "$STATE/pkgs.tsv" "$STATE/special-modes.txt" <<'PYEOF'
import sys, tarfile, os
base, pkgcache, pkgs_tsv, out = sys.argv[1:5]
rows = []


def scan(path):
    try:
        with tarfile.open(path, "r:*") as tf:
            for m in tf:
                if (m.mode & 0o6000) and (m.isfile() or m.isdir()):
                    rows.append((m.mode, m.name.lstrip("./")))
    except Exception as e:
        print("WARN could not scan %s: %s" % (path, e), file=sys.stderr)


scan(base)
for line in open(pkgs_tsv):
    f = line.rstrip(chr(10)).split(chr(9))
    if len(f) >= 2 and f[1]:
        scan(os.path.join(pkgcache, f[1]))
seen, n = set(), 0
with open(out, "w") as fh:
    for mode, name in rows:
        if not name or name in seen:
            continue
        seen.add(name)
        fh.write("%o %s%s" % (mode, name, chr(10)))
        n += 1
print("collect: %d entries with suid/sgid bits" % n)
PYEOF
}

apply_special_modes() {
  banner "stage: restore suid/sgid bits (dropped by non-root extraction)"
  local n=0 mode path
  [ -s "$STATE/special-modes.txt" ] || { warn "no special-mode list, skipping"; return 0; }
  while read -r mode path; do
    [ -n "$path" ] || continue
    if [ -e "$TREE/$path" ]; then
      if chmod "$mode" "$TREE/$path" 2>/dev/null; then n=$((n + 1)); fi
    fi
  done < "$STATE/special-modes.txt"
  log "chmod applied to $n paths from $(wc -l < "$STATE/special-modes.txt") collected entries"
  log "suid files now present: $(find "$TREE" -perm -4000 | wc -l); sgid: $(find "$TREE" -perm -2000 | wc -l)"
  ls -l "$TREE/usr/bin/sudo" "$TREE/usr/bin/passwd" "$TREE/usr/bin/mount" "$TREE/usr/lib/dbus-daemon-launch-helper" 2>&1 | tee -a "$LOG"
}

make_tree_readable() {
  banner "stage: make unreadable paths readable for the artifact builders"
  local d n=0
  while IFS= read -r d; do
    chmod 700 "$d" 2>/dev/null && n=$((n + 1))
    log "dir was not readable, set to 0700: $d"
  done < <(find "$TREE" -type d \( ! -perm -u+r -o ! -perm -u+x \) 2>/dev/null)
  while IFS= read -r d; do
    chmod u+r "$d" 2>/dev/null && n=$((n + 1))
    log "file was not readable, added u+r: $d"
  done < <(find "$TREE" -type f ! -perm -u+r 2>/dev/null)
  log "adjusted $n paths so bsdtar/sload can read every entry"
}

install_login_session() {
  banner "stage: login, autologin and MangoWM session wiring"
  # 1. tty1 autologin drop-in
  install -D -m 644 "$CONFIG_DIR/etc/systemd/system/getty@tty1.service.d/autologin.conf" \
    "$TREE/etc/systemd/system/getty@tty1.service.d/autologin.conf"
  log "installed /etc/systemd/system/getty@tty1.service.d/autologin.conf"
  # 2. session launcher
  install -D -m 755 "$CONFIG_DIR/usr/local/bin/mango-session" "$TREE/usr/local/bin/mango-session"
  log "installed /usr/local/bin/mango-session (0755)"
  # 3. autostart from the login shell of magma
  install -D -m 644 "$CONFIG_DIR/home/magma/.bash_profile" "$TREE/home/magma/.bash_profile"
  chown 1000:1000 "$TREE/home/magma/.bash_profile" 2>/dev/null || true
  log "installed /home/magma/.bash_profile (0644, owner 1000:1000)"
  # 4. NetworkManager helper + wireless profile template (no password stored)
  install -D -m 644 "$CONFIG_DIR/usr/share/elish/wifi-profile.nmconnection.template" \
    "$TREE/usr/share/elish/wifi-profile.nmconnection.template"
  install -D -m 755 "$CONFIG_DIR/usr/local/bin/elish-wifi-setup" "$TREE/usr/local/bin/elish-wifi-setup"
  log "installed /usr/share/elish/wifi-profile.nmconnection.template and /usr/local/bin/elish-wifi-setup (0755)"
  # 5. verify the magma password hash and the groups it needs
  local h g
  h=$(awk -F: '$1=="magma"{print $2}' "$TREE/etc/shadow")
  case "$h" in
    '$6$'*) log "magma shadow field: SHA-512 crypt hash present (prefix \$6\$)" ;;
    *) warn "magma shadow field is not a \$6\$ hash: '$(echo "$h" | cut -c1-4)'" ;;
  esac
  g=$(awk -F: '$1=="magma"{print $4}' "$TREE/etc/passwd")
  log "magma primary gid: $g ; supplementary: $(grep -E '^[a-z]+:[^:]*:[0-9]+:.*magma' "$TREE/etc/group" | cut -d: -f1 | tr '\n' ' ')"
  log "seatd.service enabled: $([ -L "$TREE/etc/systemd/system/multi-user.target.wants/seatd.service" ] && echo yes || echo NO)"
  log "NetworkManager enabled: $([ -L "$TREE/etc/systemd/system/multi-user.target.wants/NetworkManager.service" ] && echo yes || echo NO)"
  log "nmcli present: $([ -x "$TREE/usr/bin/nmcli" ] && echo yes || echo NO)"
  log "sshd unit present: $([ -f "$TREE/usr/lib/systemd/system/sshd.service" ] && echo yes || echo NO)"
  log "host keys in image: $(ls "$TREE/etc/ssh"/ssh_host_* 2>/dev/null | wc -l) (expect 0: generated on first boot by sshdgenkeys.service)"
}

customise() {
  restore_pristine
  if [ ! -s "$STATE/special-modes.txt" ]; then
    collect_special_modes
  fi
  apply_special_modes
  make_tree_readable
  apply_static_configs
  apply_locales
  apply_timezone
  apply_users
  apply_sudoers
  prune_boot
  apply_systemd
  install_login_session
  install_modules
  install_mango
  install_firmware
  run_ldconfig
  run_locale_gen
  banner "stage: customised tree summary"
  log "tree apparent size: $(du -sb "$TREE" | cut -f1) bytes, on disk $(du -sh "$TREE" | cut -f1)"
  log "packages added on top of the base tarball: $(wc -l < "$STATE/pkgs.tsv")"
}


# ============================================================== artifact stage
make_tarball() {
  banner "stage: pack the root filesystem into tar.zst"
  local out=$OUT_DIR/ArchLinuxARM-aarch64-elish-rootfs.tar.zst t0 t1
  rm -f "$out"
  t0=$(date +%s)
  ( cd "$TREE" && bsdtar -c --uid 0 --gid 0 --uname root --gname root -f - . ) \
    | zstd -q -T0 -11 -f -o "$out"
  t1=$(date +%s)
  zstd -t "$out" 2>&1 | tail -1 | tee -a "$LOG"
  log "tar.zst: $(stat -c %s "$out") bytes in $((t1 - t0))s"
  TARBALL=$out
}

f2fs_run() {
  local tool=$1
  shift
  ( export LD_LIBRARY_PATH=$F2FS_LIB; exec "$F2FS_BIN/$tool" "$@" )
}

write_sector_helper() {
  cat > "$STATE/f2fs_setsector.py" <<'PYEOF'
#!/usr/bin/env python3
"""Rewrite the f2fs sector-size fields in both redundant superblocks.

mkfs.f2fs 1.17.0 parses "-w <bytes>" but ignores it for file-backed images,
because the sector size can only be queried with BLKSSZGET on a block device.
Both fsck.f2fs and dump.f2fs then re-derive the field from the host device, so
this can only be applied as the very last step, after the tool based checks.
Only log_sectorsize and log_sectors_per_block change: every address in the
superblock is expressed in 4 KiB blocks, so nothing else needs to be touched.
"""
import sys, struct

MAGIC = 0xF2F52010
OFF = 1024
BLOCK = 4096


def main():
    path, sector_size = sys.argv[1], int(sys.argv[2])
    want_lss = sector_size.bit_length() - 1
    want_lspb = (BLOCK // sector_size).bit_length() - 1
    if sector_size < 512 or sector_size * (1 << want_lspb) != BLOCK:
        raise SystemExit("unsupported sector size: %d" % sector_size)
    with open(path, "r+b") as f:
        for sb in (OFF, BLOCK + OFF):
            f.seek(sb)
            magic, _maj, _min, lss, lspb = struct.unpack("<IHHII", f.read(16))
            if magic != MAGIC:
                raise SystemExit("no f2fs superblock at %d" % sb)
            print("sb@%d: log_sectorsize=%d log_sectors_per_block=%d" % (sb, lss, lspb))
            if (1 << lss) == sector_size and lss + lspb == 12:
                print("  already %d-byte sectors" % sector_size)
                continue
            f.seek(sb + 8)
            f.write(struct.pack("<II", want_lss, want_lspb))
            print("  -> log_sectorsize=%d log_sectors_per_block=%d" % (want_lss, want_lspb))


main()
PYEOF
}

image_size_for_tree() {
  local tree_bytes need want
  tree_bytes=$(du -sb "$TREE" | cut -f1)
  need=$(( tree_bytes + tree_bytes / 10 + 402653184 ))
  want=$(parse_size "$IMG_SIZE")
  if [ "$want" -lt "$need" ]; then want=$need; fi
  echo $(( (want + 1073741824 - 1) / 1073741824 * 1073741824 ))
}

make_image_f2fs() {
  banner "stage: build the f2fs raw image (primary artifact)"
  local out=$OUT_DIR/linux-rootfs-f2fs.img want t0 t1 rc
  rm -f "$out"
  want=$(image_size_for_tree)
  log "image apparent size: $want bytes ($((want / 1073741824)) GiB); tree $(du -sb "$TREE" | cut -f1) bytes"
  truncate -s "$want" "$out"
  # -b 4096 : mkfs.f2fs(8): "the kernel is only able to mount f2fs filesystems
  #           where the block size matches the page size" (page size is 4096).
  # -w N    : wanted sector size (see findings for the full discussion).
  # -R 0:0  : root dirent owner; mkfs.f2fs(8) defaults to getuid()/getgid().
  # -O ...  : only features this kernel can mount (no encrypt/verity/casefold/
  #           compression: those are absent from the kernel .config).
  log "mkfs.f2fs -f -l linux -b 4096 -w $F2FS_SECTOR_SIZE -R 0:0 -O $F2FS_FEATURES $out"
  f2fs_run mkfs.f2fs -f -l linux -b 4096 -w "$F2FS_SECTOR_SIZE" -R 0:0 \
    -O "$F2FS_FEATURES" "$out" 2>&1 | tee -a "$LOG" | tail -12
  t0=$(date +%s)
  log "sload.f2fs -f <tree> -P <image>"
  # sload runs its own internal fsck pass and reports
  #   [FSCK] valid_block_count matching with CP  [Fail] [0xd6004, 0xd51c1]
  # followed by fix_checkpoint(); it exits non-zero because of that self-reported
  # mismatch even though the on-disk result is consistent. The authoritative gate
  # is the independent fsck.f2fs -f run in verify_f2fs(), so only warn here.
  rc=0
  f2fs_run sload.f2fs -f "$TREE" -P "$out" 2>&1 | tr '\r' '\n' \
    | grep -v 'Free segments' | tee -a "$LOG" | tail -8 || rc=$?
  t1=$(date +%s)
  if [ "$rc" -ne 0 ]; then
    warn "sload.f2fs exited $rc (its internal fsck self-report); see build.log, fsck.f2fs -f below is the gate"
  fi
  log "sload finished in $((t1 - t0))s; image $(du -h "$out" | cut -f1) on disk"
  F2FS_IMG=$out
}

make_image_ext4() {
  banner "stage: build the ext4 raw image (optional, --also-ext4)"
  local out=$OUT_DIR/linux-rootfs-ext4.img want
  rm -f "$out"
  want=$(image_size_for_tree)
  truncate -s "$want" "$out"
  mke2fs -q -t ext4 -b 4096 -O ^metadata_csum_seed -m 1 -L linux -d "$TREE" -F "$out" \
    || die "mke2fs failed"
  log "ext4 image: $(stat -c %s "$out") bytes apparent, $(du -h "$out" | cut -f1) on disk"
  EXT4_IMG=$out
}

verify_f2fs() {
  local img=$F2FS_IMG rc=0 vinode vnode vbc keep scratch
  banner "stage: verify the f2fs image"
  write_sector_helper
  # 1. first fix pass ON THE DELIVERED IMAGE: sload leaves the SIT bitmaps of
  #    freshly written segments in a state fsck has to normalise, and fsck only
  #    converges when its fix is written back to the very image we ship.
  f2fs_run fsck.f2fs -f "$img" > "$STATE/fsck.f2fs.out" 2>&1 || rc=$?
  log "fsck.f2fs -f (fix pass, persisted in the delivered image) exit=$rc"
  F2FS_FIX_LINES=$(grep -ci 'fixing' "$STATE/fsck.f2fs.out" || true)
  log "fixing lines in the first pass: $F2FS_FIX_LINES"
  vinode=$(grep 'valid_inode_count matched' "$STATE/fsck.f2fs.out" | sed -n 's/.*\[0x\([0-9a-f]*\)\].*/\1/p')
  vnode=$(grep 'valid_node_count matching with CP (nat lookup)' "$STATE/fsck.f2fs.out" | sed -n 's/.*\[0x\([0-9a-f]*\)\].*/\1/p')
  vbc=$(grep 'valid_block_count matching' "$STATE/fsck.f2fs.out" | sed -n 's/.*\[0x\([0-9a-f]*\)\].*/\1/p')
  F2FS_VINODE=$(( 16#$vinode ))
  F2FS_VNODE=$(( 16#$vnode ))
  F2FS_VBLOCKS=$(( 16#$vbc ))
  F2FS_SRC_ENTRIES=$(find "$TREE" | wc -l)
  F2FS_SRC_INODES=$(find "$TREE" -printf '%i\n' | sort -u | wc -l)
  F2FS_SRC_BYTES=$(du -sb "$TREE" | cut -f1)
  log "f2fs valid inodes=$F2FS_VINODE vs source entries=$F2FS_SRC_ENTRIES (distinct source inodes=$F2FS_SRC_INODES)"
  log "f2fs valid blocks=$F2FS_VBLOCKS ($((F2FS_VBLOCKS * 4096)) bytes) vs source apparent bytes=$F2FS_SRC_BYTES"
  # 2. superblock / feature evidence on a COPY (fsck -l and dump.f2fs both
  #    rewrite the superblock, so they must never touch the delivered image)
  scratch=$WORK/evidence.img
  cp --sparse=always "$img" "$scratch"
  f2fs_run fsck.f2fs -l "$scratch" > "$STATE/fsck-l.out" 2>&1
  F2FS_LSS=$(sed -n 's/^log_sectorsize: *//p' "$STATE/fsck-l.out" | tr -d ' ')
  f2fs_run dump.f2fs "$scratch" > "$STATE/dump.f2fs.out" 2>&1
  f2fs_run f2fslabel "$scratch" > "$STATE/f2fslabel.out" 2>&1 || true
  rm -f "$scratch"
  # 3. optional 4096-sector fixup as the very last write to the delivered image
  if [ "$F2FS_SECTOR_SIZE" != "512" ]; then
    keep=$WORK/pre-sector-patch.img
    cp --sparse=always "$img" "$keep"
    python3 "$STATE/f2fs_setsector.py" "$img" "$F2FS_SECTOR_SIZE" | tee -a "$LOG"
    { cmp -l "$keep" "$img" || true; } > "$STATE/sector-patch.diff"
    F2FS_PATCH_DIFF=$(wc -l < "$STATE/sector-patch.diff")
    log "sector-size patch changed $F2FS_PATCH_DIFF bytes (expected 4)"
    rm -f "$keep"
  fi
  # 4. independent cleanliness proof on a fresh copy of the DELIVERED image.
  #    NOTE: 'fsck.f2fs -f' prints '[FSCK] fixing SIT types' unconditionally (it
  #    also prints it on a file it has just written - measured twice in a row),
  #    so that line is NOT a corruption signal. The real signals are: the
  #    [FSCK] check table (no [Fail]), the exit code, and whether fsck changes
  #    any byte of the copy apart from the superblock sector-size words.
  scratch=$WORK/verify-copy.img
  cp --sparse=always "$img" "$scratch"
  f2fs_run fsck.f2fs -f "$scratch" > "$STATE/fsck-secondcopy.out" 2>&1 || true
  F2FS_FIX2=$(grep -ci 'fixing' "$STATE/fsck-secondcopy.out" || true)
  F2FS_FAIL2=$(grep -c '\[Fail\]' "$STATE/fsck-secondcopy.out" || true)
  log "copy-of-delivered fsck.f2fs -f: 'fixing SIT types' lines=$F2FS_FIX2 (informational, always 1), [Fail] lines=$F2FS_FAIL2 (must be 0)"
  { cmp -l "$img" "$scratch" || true; } > "$STATE/delivered-vs-fsck.diff"
  F2FS_COPY_DIFF=$(wc -l < "$STATE/delivered-vs-fsck.diff")
  F2FS_COPY_OFFSETS=$(awk '{printf "%s ", $1}' "$STATE/delivered-vs-fsck.diff" || true)
  log "bytes fsck changed in the copy: $F2FS_COPY_DIFF at octal offsets [$F2FS_COPY_OFFSETS] (expect 4 at 1033 1037 5129 5133 = the two superblock sector-size words)"
  rm -f "$scratch"
  if [ "$F2FS_FAIL2" != "0" ]; then
    warn "fsck reports $F2FS_FAIL2 [Fail] lines on a copy of the delivered image - investigate"
  fi
  if [ "$F2FS_COPY_DIFF" != "4" ]; then
    warn "fsck changed $F2FS_COPY_DIFF bytes in the copy, expected only the 4 superblock words"
  fi
  return 0
}
verify_ext4() {
  local rc=0
  banner "stage: verify the ext4 image"
  e2fsck -fn "$EXT4_IMG" > "$STATE/e2fsck.out" 2>&1 || rc=$?
  log "e2fsck -fn exit=$rc (0 = clean)"
  return 0
}

verify_artifacts() {
  local rep=$OUT_DIR/BUILD-REPORT.txt f files
  banner "stage: collect the verification report"
  files="$TARBALL $F2FS_IMG"
  if [ "$ALSO_EXT4" -eq 1 ]; then files="$files $EXT4_IMG"; fi
  {
    echo "# build-rootfs.sh verification report"
    echo "generated: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "host: $(uname -sr); $(f2fs_run mkfs.f2fs -V 2>&1 | head -1); $(depmod --version | head -1)"
    echo
    echo "## f2fs: fsck.f2fs -f"
    tail -20 "$STATE/fsck.f2fs.out"
    echo
    echo "## f2fs: counts"
    echo "source entries        : $F2FS_SRC_ENTRIES"
    echo "source distinct inodes: $F2FS_SRC_INODES"
    echo "source apparent bytes : $F2FS_SRC_BYTES"
    echo "image valid inodes    : $F2FS_VINODE"
    echo "image valid nodes     : $F2FS_VNODE"
    echo "image valid blocks    : $F2FS_VBLOCKS of 4096 bytes = $((F2FS_VBLOCKS * 4096)) bytes"
    echo "log_sectorsize as produced by mkfs.f2fs: $F2FS_LSS"
    echo "requested f2fs sector size: $F2FS_SECTOR_SIZE"
    echo "fixing lines in the persisted fsck pass: $F2FS_FIX_LINES"
    echo "fixing SIT lines when fsck runs on a copy of the delivered image: $F2FS_FIX2 (informational, always 1; the cleanliness criteria are the [Fail] count and the cmp offsets below)"
    echo "cmp -l delivered vs fsck-copy differing bytes: $F2FS_COPY_DIFF (must be 4)"
    echo "those bytes are at octal offsets: $F2FS_COPY_OFFSETS (1033 1037 5129 5133 = the two superblock sector-size words)"
    echo "[Fail] lines when fsck runs on a copy of the delivered image: $F2FS_FAIL2 (must be 0)"
    echo "sector-size patch bytes changed: $F2FS_PATCH_DIFF (4 = 2 superblocks x 2 words)"
    if [ -s "$STATE/sector-patch.diff" ]; then cat "$STATE/sector-patch.diff"; fi
    echo
    echo "## f2fs: fsck.f2fs -l (superblock, label)"
    grep -E 'magic|log_sector|log_block|block_count|Filesystem volume name|uuid' "$STATE/fsck-l.out"
    echo
    echo "## f2fs: dump.f2fs (features)"
    grep -E 'superblock features|total FS sectors|Segments per section|Sections per zone' "$STATE/dump.f2fs.out"
    echo
    echo "## f2fs: f2fslabel"
    tail -3 "$STATE/f2fslabel.out"
    echo
    if [ "$ALSO_EXT4" -eq 1 ]; then
      echo "## ext4: e2fsck -fn"
      tail -8 "$STATE/e2fsck.out"
      echo
      echo "## ext4: dumpe2fs -h"
      dumpe2fs -h "$EXT4_IMG" 2>/dev/null | grep -E 'Filesystem volume name|Block size|Block count|Filesystem features'
      echo
      echo "## ext4: debugfs read-back"
      for f in etc/fstab etc/hostname etc/hosts; do
        echo "--- /$f ---"
        debugfs -R "cat /$f" "$EXT4_IMG" 2>/dev/null | head -12
      done
      echo
    fi
    echo "## artifacts"
    sha256sum $files
    echo
    echo "## sizes"
    stat -c '%n apparent_bytes=%s' $files
    du -h $files
    echo
    echo "## login / autologin / session wiring"
    grep -E "autologin|mango-session|magma shadow|magma primary gid|nmcli present|sshd unit present|host keys in image|getty@tty1|seatd.service enabled|NetworkManager enabled" "$LOG" | tail -14
    echo
    echo "## added packages shipping a .INSTALL script (NOT executed)"
    cat "$STATE/install-scripts.txt"
    echo
    echo "## packages added on top of the base tarball ($(wc -l < "$STATE/pkgs.tsv"))"
    awk -F'\t' '{printf "%s %s %s\n", $3, $4, $1}' "$STATE/pkgs.tsv"
  } > "$rep" 2>&1
  log "verification report: $rep"
  grep -E '## |valid inodes|valid blocks|superblock features|log_sectorsize|total FS sectors|patch changed' "$rep" | tee -a "$LOG"
}

write_manifest() {
  banner "stage: write MANIFEST.rootfs and SHA256SUMS"
  local now sha_t sha_f sha_e sums
  sums=$REPO_ROOT/output/20261024/SHA256SUMS
  now=$(date -u +%Y-%m-%dT%H:%M:%SZ)
  sha_t=$(sha256sum "$TARBALL" | cut -d' ' -f1)
  sha_f=$(sha256sum "$F2FS_IMG" | cut -d' ' -f1)
  sha_e=""
  if [ "$ALSO_EXT4" -eq 1 ]; then sha_e=$(sha256sum "$EXT4_IMG" | cut -d' ' -f1); fi
  {
    echo "# MANIFEST.rootfs — xiaomi-elish Arch Linux ARM (aarch64) root filesystem"
    echo "# 构建时间 (UTC): $now"
    echo "# 只允许写入 boot_b 与 linux 两个分区；下列产物目标均为 linux 分区"
    echo "# f2fs 为默认产物，tar.zst 为等价归档，ext4 仅在 --also-ext4 时生成"
    echo "# 机器可校验的伴生文件：同目录 SHA256SUMS（cd output/20261024 && sha256sum -c SHA256SUMS）"
    echo "# 文件名 | 目标分区 | SHA256 | 构建时间 | 大小(bytes)"
    printf '%s | %s | %s | %s | %s\n' \
      "rootfs/$(basename "$F2FS_IMG")" linux "$sha_f" "$now" "$(stat -c %s "$F2FS_IMG")"
    printf '%s | %s | %s | %s | %s\n' \
      "rootfs/$(basename "$TARBALL")" linux "$sha_t" "$now" "$(stat -c %s "$TARBALL")"
    if [ "$ALSO_EXT4" -eq 1 ]; then
      printf '%s | %s | %s | %s | %s\n' \
        "rootfs/$(basename "$EXT4_IMG")" linux "$sha_e" "$now" "$(stat -c %s "$EXT4_IMG")"
    fi
  } > "$MANIFEST"
  chmod 644 "$MANIFEST"
  {
    printf '%s  rootfs/%s\n' "$sha_f" "$(basename "$F2FS_IMG")"
    printf '%s  rootfs/%s\n' "$sha_t" "$(basename "$TARBALL")"
    if [ "$ALSO_EXT4" -eq 1 ]; then printf '%s  rootfs/%s\n' "$sha_e" "$(basename "$EXT4_IMG")"; fi
  } > "$sums"
  chmod 644 "$sums"
  tee -a "$LOG" < "$MANIFEST"
  log "SHA256SUMS:"
  tee -a "$LOG" < "$sums"
  ( cd "$REPO_ROOT/output/20261024" && sha256sum -c SHA256SUMS ) | tee -a "$LOG"
}
# ======================================================================= main
main() {
  need_cmd curl bsdtar zstd mke2fs e2fsck dumpe2fs debugfs sha256sum python3 openssl truncate realpath install cmp
  need_cmd fakeroot
  mkdir -p "$STATE" "$OUT_DIR"
  banner "build-rootfs.sh start"
  log "repo: $REPO_ROOT"
  log "build root: $BUILD_ROOT"
  log "output: $OUT_DIR"
  log "target image size: $IMG_SIZE ; f2fs sector size: $F2FS_SECTOR_SIZE ; also-ext4: $ALSO_EXT4"

  if [ "$DO_CLEAN" -eq 1 ]; then
    log "--clean: removing $WORK"
    if [ -d "$TREE" ]; then chmod -R u+rwX "$TREE" 2>/dev/null || true; fi
    rm -rf "$WORK"
    mkdir -p "$STATE" "$OUT_DIR"
    : > "$LOG"
  fi

  fetch_base
  fetch_dbs
  fetch_qemu
  fetch_f2fs_tools

  if [ ! -d "$TREE/var/lib/pacman/local" ]; then
    extract_base
  fi
  resolve_packages

  local want_stamp have_stamp
  want_stamp=$(tree_stamp)
  have_stamp=none
  if [ -f "$STATE/tree.stamp" ]; then have_stamp=$(cat "$STATE/tree.stamp"); fi
  if [ "$want_stamp" = "$have_stamp" ] && [ -f "$STATE/pristine.tar" ]; then
    banner "reusing the assembled tree (stamp $have_stamp)"
  else
    download_packages
    extract_base
    extract_packages
    snapshot_pristine
    tree_stamp > "$STATE/tree.stamp"
  fi

  customise
  make_tarball
  make_image_f2fs
  verify_f2fs
  if [ "$ALSO_EXT4" -eq 1 ]; then
    make_image_ext4
    verify_ext4
  fi
  verify_artifacts
  write_manifest

  banner "build-rootfs.sh finished"
  log "f2fs image: $F2FS_IMG"
  log "archive:    $TARBALL"
  if [ "$ALSO_EXT4" -eq 1 ]; then log "ext4 image: $EXT4_IMG"; fi
  log "manifest:   $MANIFEST"
}

main "$@"
