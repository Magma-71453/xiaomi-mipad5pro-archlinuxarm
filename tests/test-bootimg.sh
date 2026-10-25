#!/usr/bin/env bash
# (a) boot image v0: build from 1 MiB kernel + ~4 KB ramdisk + two dtbs, then
# --verify; every header field and every segment SHA256 must match the inputs.
set -uo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
. "$HERE/lib.sh"
TOOL="$REPO/scripts/make-bootimg.py"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

align() { local v=$1 p=$2; printf '%s' $(( (v + p - 1) / p * p )); }
field() { sed -n "s/^$1 = //p" "$2" | head -1; }

emit "test-bootimg: Android boot image v0 build + verify"

head -c 1048576 /dev/urandom > "$WORK/kernel"
head -c 4000 /dev/urandom | gzip -n -9 > "$WORK/ramdisk.gz"
head -c 8192 /dev/urandom > "$WORK/dtb-boe"
head -c 4096 /dev/urandom > "$WORK/dtb-csot"

KERNEL_SIZE=$(stat -c %s "$WORK/kernel")
RAMDISK_SIZE=$(stat -c %s "$WORK/ramdisk.gz")
assert_eq "kernel blob is 1 MiB" 1048576 "$KERNEL_SIZE"
assert_between "ramdisk blob around 4 KiB" 3800 4400 "$RAMDISK_SIZE"

# Expected kernel segment per the documented rule: dtbs are appended at 2048-byte
# alignment, computed here independently of the generator.
{
    cat "$WORK/kernel"
    head -c $(( (2048 - KERNEL_SIZE % 2048) % 2048 )) /dev/zero
    cat "$WORK/dtb-boe"
} > "$WORK/seg1"
SEG1_SIZE=$(stat -c %s "$WORK/seg1")
{
    cat "$WORK/seg1"
    head -c $(( (2048 - SEG1_SIZE % 2048) % 2048 )) /dev/zero
    cat "$WORK/dtb-csot"
} > "$WORK/expected-kernel-segment"
EXPECT_KERNEL_SIZE=$(stat -c %s "$WORK/expected-kernel-segment")
RAMDISK_OFF=$(align $((4096 + EXPECT_KERNEL_SIZE)) 4096)
EXPECT_TOTAL=$(align $((RAMDISK_OFF + RAMDISK_SIZE)) 4096)

python3 "$TOOL" --kernel "$WORK/kernel" --ramdisk "$WORK/ramdisk.gz" \
    --dtb "$WORK/dtb-boe" --dtb "$WORK/dtb-csot" \
    --page-size 4096 --out "$WORK/boot.img" > "$WORK/build.log" 2>&1
assert_eq "generator exit status" 0 "$?"
IMAGE_SIZE=$(stat -c %s "$WORK/boot.img")
assert_eq "image size equals page-aligned layout" "$EXPECT_TOTAL" "$IMAGE_SIZE"
assert_eq "image size is a multiple of 4096" 0 $(( IMAGE_SIZE % 4096 ))

python3 "$TOOL" --verify --input "$WORK/boot.img" > "$WORK/verify.log" 2>"$WORK/verify.err"
assert_eq "verify exit status" 0 "$?"

assert_eq "header.magic" "ANDROID!" "$(field header.magic "$WORK/verify.log")"
assert_eq "header.header_version" 0 "$(field header.header_version "$WORK/verify.log")"
assert_eq "header.page_size" 4096 "$(field header.page_size "$WORK/verify.log")"
assert_eq "header.kernel_addr (deviceinfo 0x8000)" 0x00008000 "$(field header.kernel_addr "$WORK/verify.log")"
assert_eq "header.ramdisk_addr (deviceinfo 0x01000000)" 0x01000000 "$(field header.ramdisk_addr "$WORK/verify.log")"
assert_eq "header.tags_addr (deviceinfo 0x00000100)" 0x00000100 "$(field header.tags_addr "$WORK/verify.log")"
assert_eq "header.cmdline pins the rootfs by PARTLABEL" "quiet rootwait root=PARTLABEL=linux console=tty0 loglevel=4" "$(field header.cmdline "$WORK/verify.log")"
assert_eq "header.kernel_size includes both dtbs" "$EXPECT_KERNEL_SIZE" "$(field header.kernel_size "$WORK/verify.log")"
assert_eq "header.ramdisk_size" "$RAMDISK_SIZE" "$(field header.ramdisk_size "$WORK/verify.log")"
assert_eq "header.second_size" 0 "$(field header.second_size "$WORK/verify.log")"
assert_eq "layout.kernel_offset" 4096 "$(field layout.kernel_offset "$WORK/verify.log")"
assert_eq "layout.ramdisk_offset" "$RAMDISK_OFF" "$(field layout.ramdisk_offset "$WORK/verify.log")"
assert_eq "layout.file_size" "$EXPECT_TOTAL" "$(field layout.file_size "$WORK/verify.log")"
assert_eq "layout.file_size_page_aligned" yes "$(field layout.file_size_page_aligned "$WORK/verify.log")"
assert_eq "verify.status" OK "$(field verify.status "$WORK/verify.log")"
assert_eq "default size limit is the 192 MiB boot_b partition" 201326592 "$(field layout.size_limit "$WORK/verify.log")"

assert_eq "kernel segment sha256 matches inputs" "$(sha256sum "$WORK/expected-kernel-segment" | cut -d' ' -f1)" \
    "$(field sha256.kernel_segment "$WORK/verify.log")"
assert_eq "ramdisk segment sha256 matches input" "$(sha256sum "$WORK/ramdisk.gz" | cut -d' ' -f1)" \
    "$(field sha256.ramdisk_segment "$WORK/verify.log")"
assert_eq "image sha256 matches file on disk" "$(sha256sum "$WORK/boot.img" | cut -d' ' -f1)" \
    "$(field sha256.file "$WORK/verify.log")"

assert_eq "header dump prints 15 header.* fields" 15 "$(grep -c '^header\.' "$WORK/verify.log")"
assert_eq "header dump prints 6 layout.* fields" 6 "$(grep -c '^layout\.' "$WORK/verify.log")"

# negative paths: corrupted padding and corrupted magic must be rejected
cp "$WORK/boot.img" "$WORK/bad-padding.img"
PAD_POS=$((RAMDISK_OFF + RAMDISK_SIZE))
if [ "$PAD_POS" -lt "$EXPECT_TOTAL" ]; then
    printf '\177' | dd of="$WORK/bad-padding.img" bs=1 seek="$PAD_POS" conv=notrunc status=none
    python3 "$TOOL" --verify --input "$WORK/bad-padding.img" > "$WORK/bad-pad.log" 2>"$WORK/bad-pad.err"
    assert_eq "verify rejects non-zero section padding" 3 "$?"
    assert_contains "padding failure is explained" "padding" "$WORK/bad-pad.err"
else
    bad "image had no tail padding byte to corrupt"
fi

cp "$WORK/boot.img" "$WORK/bad-magic.img"
printf 'XAMIRODN' | dd of="$WORK/bad-magic.img" bs=1 seek=0 conv=notrunc status=none
python3 "$TOOL" --verify --input "$WORK/bad-magic.img" > "$WORK/bad-magic.log" 2>&1
assert_eq "verify rejects a bad magic" 1 "$?"

# negative: the 192 MiB boot_b cap is enforced when building and when verifying
python3 "$TOOL" --kernel "$WORK/kernel" --ramdisk "$WORK/ramdisk.gz" --dtb "$WORK/dtb-csot" \
    --page-size 4096 --max-size 2048 --out "$WORK/too-big.img" > "$WORK/cap.log" 2>"$WORK/cap.err"
assert_eq "build refuses an image above the partition limit (exit 1)" 1 "$?"
assert_contains "the cap error explains the fix" "exceeds the" "$WORK/cap.err"
assert_contains "the cap error suggests compressing the initramfs" "compress the initramfs" "$WORK/cap.err"
assert_file_absent "no oversized image was written" "$WORK/too-big.img"

python3 "$TOOL" --verify --input "$WORK/boot.img" --max-size 2048 > "$WORK/capv.log" 2>"$WORK/capv.err"
assert_eq "verify rejects an image above the partition limit (exit 3)" 3 "$?"
assert_contains "verify names the limit" "boot_b partition limit" "$WORK/capv.err"

summary "test-bootimg"
