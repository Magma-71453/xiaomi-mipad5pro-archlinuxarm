#!/usr/bin/env bash
# make-f2fs-image-root.sh — 用 loop 挂载方式生成 f2fs 根镜像（需要 root）。
#
# 为什么需要它：sload.f2fs 在本次 16 GiB rootfs 上会把部分大文件写坏
# （实测 usr/lib/systemd/libsystemd-shared-261.2-1.so 大小不变但 .dynamic 段被摧毁，
#  readelf 报 'no .dynamic section'，条目从 31 变 1；libcrypto/libpython 亦被写坏）。
# 因此改用「mkfs + 挂载 + tar 解包」这条不会碰数据的路径。
#
# 用法: sudo bash scripts/make-f2fs-image-root.sh
#   产物: output/<date>/rootfs/linux-rootfs-f2fs.img（原文件备份为 .sload.bak）
set -euo pipefail
REPO_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
DATE=${OUTPUT_DATE:-20261024}
RD="$REPO_ROOT/output/$DATE/rootfs"
TAR="$RD/ArchLinuxARM-aarch64-elish-rootfs.tar.zst"
IMG="$RD/linux-rootfs-f2fs.img"
# Default to the *exact* size of the linux partition (209,545,494,528 bytes =''
# 0x30E2A00000, read from the bootloader with `fastboot getvar''
# partition-size:linux`) so the filesystem fills the partition from the start''
# and no offline/online resize step is needed on first boot.''
SIZE=${SIZE:-209545494528}
export PATH="$REPO_ROOT/../.build/tools/f2fs-tools/usr/bin:$PATH"
export LD_LIBRARY_PATH="$REPO_ROOT/../.build/tools/f2fs-tools/usr/lib:${LD_LIBRARY_PATH:-}"

[ -f "$TAR" ] || { echo "missing $TAR" >&2; exit 1; }
[ "$(id -u)" = 0 ] || { echo "run me with sudo (loop mount needs root)" >&2; exit 1; }
command -v mkfs.f2fs >/dev/null || { echo "mkfs.f2fs not found" >&2; exit 1; }

WORK=$(mktemp -d /var/tmp/elish-img.XXXXXX)
NEW="$RD/linux-rootfs-f2fs.img.rootnew"
cleanup() { mountpoint -q "$WORK" && umount "$WORK" || true; rmdir "$WORK" 2>/dev/null || true; }
trap cleanup EXIT

echo "[1/6] create $NEW ($SIZE)"
rm -f "$NEW"; truncate -s "$SIZE" "$NEW"
echo "[2/6] mkfs.f2fs -b 4096 -l linux"
mkfs.f2fs -f -l linux -b 4096 -O extra_attr,inode_checksum,flexible_inline_xattr "$NEW" >/dev/null
echo "[3/6] loop mount"
mount -o loop "$NEW" "$WORK"
echo "[4/6] extract $TAR (numeric owner, preserve modes/suid)"
tar --numeric-owner -xpf "$TAR" -C "$WORK"
# overlay the repo configs (session launcher, USB debug service, ...)
if [ -d "$REPO_ROOT/configs/rootfs" ]; then
    cp -a "$REPO_ROOT/configs/rootfs/." "$WORK"/
    chmod 755 "$WORK/usr/local/bin/mango-session" "$WORK/usr/local/bin/usb-net-up.sh" 2>/dev/null || true
fi

# The tar was created by a normal user, so every entry is owned by uid 1000.
# systemd ignores unit files that are not owned by root, and setuid binaries
# only work when owned by root.  Normalise ownership here (we are root), then
# restore the setuid/setgid bits that chown clears, using the modes recorded
# in the tar itself.
echo "[4b/6] normalise ownership (root:root, /home/magma stays 1000:1000)"
tar --numeric-owner -tvf "$TAR" | awk '
  {
    mode=$1; path=$6;
    suid = (substr(mode,4,1) == "s" || substr(mode,4,1) == "S");
    sgid = (substr(mode,7,1) == "s" || substr(mode,7,1) == "S");
    if (suid || sgid) print (suid ? "u+s" : "") (sgid ? "g+s" : "") " " path;
  }' > /tmp/elish-special-bits.list 2>/dev/null || true
chown -R 0:0 "$WORK"
chown -R 1000:1000 "$WORK/home/magma"
chmod 600 "$WORK/etc/shadow" "$WORK/etc/gshadow" 2>/dev/null || true
while read -r bits path; do
    [ -n "$path" ] || continue
    chmod "$bits" "$WORK/${path#./}" 2>/dev/null || true
done < /tmp/elish-special-bits.list
echo "     special bits restored: $(wc -l < /tmp/elish-special-bits.list)"
sync
echo "[5/6] unmount"
umount "$WORK"
rmdir "$WORK"
echo "[6/6] patch f2fs sector-size words to 12/0 in both superblocks"
printf '\x0c\x00\x00\x00\x00\x00\x00\x00' | dd of="$NEW" bs=1 seek=1032 conv=notrunc status=none
printf '\x0c\x00\x00\x00\x00\x00\x00\x00' | dd of="$NEW" bs=1 seek=5128 conv=notrunc status=none
# ---------------------------------------------------------------------------
# Grow the image to the size of the linux partition and resize the f2fs
# filesystem offline (the only mode resize.f2fs supports).  Verified on a
# copy of the real image: fsck (5 s) + resize (7 s) for 16 GiB -> 195 GiB.
# The file stays sparse, so it occupies only the data it actually holds.
# ---------------------------------------------------------------------------
RESIZE_TARGET=${RESIZE_TARGET:-209545494528}
if [ -n "$RESIZE_TARGET" ] && [ "$RESIZE_TARGET" -gt 0 ]; then
    echo "[7/8] grow image to $RESIZE_TARGET bytes (sparse)"
    truncate -s "$RESIZE_TARGET" "$NEW"
    echo "[8/8] fsck + resize.f2fs + fsck (offline; -F skips the [y/n] dialogue)"
    fsck.f2fs -fp "$NEW" >/dev/null 2>&1 || true
    printf 'y\n' | resize.f2fs -f -F "$NEW" >/dev/null 2>&1 || true
    fsck.f2fs -fp "$NEW" >/dev/null 2>&1 || true
    echo "block_count after resize: $(fsck.f2fs -l "$NEW" 2>/dev/null | awk '/block_count/{print $2}')"
    echo "apparent size: $(stat -c %s "$NEW")  disk usage: $(du -h --apparent-size "$NEW" | cut -f1)/$(du -h "$NEW" | cut -f1)"
fi

[ -f "$IMG.sload.bak" ] || mv "$IMG" "$IMG.sload.bak"
mv "$NEW" "$IMG"
sha256sum "$IMG"
echo "done: $IMG"
