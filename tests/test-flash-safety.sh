#!/usr/bin/env bash
# (b) flash safety: a mock fastboot on PATH records every argument. Asserts
# dry-run never invokes it, --execute only ever touches boot_b and linux, the
# single D-009 exception (erase dtbo_b) needs --allow-dtbo-erase plus a second
# confirmation, and every other partition / operation is refused outright.
set -uo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
. "$HERE/lib.sh"
SCRIPT="$REPO/scripts/flash-elish.sh"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$WORK/bin"

cat > "$WORK/bin/fastboot" <<'MOCK'
#!/bin/sh
printf '%s\n' "$*" >> "$MOCK_LOG"
case "$1" in
    getvar)
        part=$(printf '%s' "$2" | sed 's/.*://')
        printf 'partition-size:%s: %s\n' "$part" "$MOCK_CAPACITY" >&2
        exit 0
        ;;
    flash|erase|reboot-bootloader)
        exit 0
        ;;
    *)
        printf 'mock fastboot: unexpected operation %s\n' "$1" >&2
        exit 9
        ;;
esac
MOCK
chmod +x "$WORK/bin/fastboot"

head -c 10240 /dev/urandom > "$WORK/boot.img"
head -c 4096 /dev/urandom > "$WORK/root.img"
head -c 20000000 /dev/urandom > "$WORK/huge.img"
BOOT_SHA=$(sha256sum "$WORK/boot.img" | cut -d' ' -f1)
ROOT_SHA=$(sha256sum "$WORK/root.img" | cut -d' ' -f1)
HUGE_SHA=$(sha256sum "$WORK/huge.img" | cut -d' ' -f1)
FLASH_LOG="$WORK/FLASH-LOG"

run_flash() {
    local log=$1; shift
    rm -f "$WORK/$log"
    MOCK_LOG="$WORK/$log" MOCK_CAPACITY=${MOCK_CAPACITY:-0x1000000} \
        PATH="$WORK/bin:$PATH" bash "$SCRIPT" --manifest "$WORK/no-manifest" "$@" \
        > "$WORK/$log.out" 2> "$WORK/$log.err"
}
mock_calls() {
    if [ -f "$WORK/$1" ]; then wc -l < "$WORK/$1"; else printf '0'; fi
}

# Same, but through the default manifest search (no --manifest): MANIFEST_DIR
# points at a temp dir so the repository manifests never leak into the test.
run_flash_default() {
    local log=$1; shift
    rm -f "$WORK/$log"
    MOCK_LOG="$WORK/$log" MOCK_CAPACITY=${MOCK_CAPACITY:-0x1000000} MANIFEST_DIR="$WORK/md" \
        PATH="$WORK/bin:$PATH" bash "$SCRIPT" "$@" \
        > "$WORK/$log.out" 2> "$WORK/$log.err"
}
mock_line() {
    if [ -f "$WORK/$1" ]; then sed -n "${2}p" "$WORK/$1"; fi
}

emit "test-flash-safety: mock fastboot on PATH, no device present"

# --- 1: default dry-run never invokes fastboot -------------------------------
run_flash dry.log --boot "$WORK/boot.img" --root "$WORK/root.img"
assert_eq "dry-run with --boot/--root exits 0" 0 "$?"
assert_eq "dry-run invoked mock fastboot 0 times" 0 "$(mock_calls dry.log)"
assert_contains "dry-run prints the boot_b flash command" "[dry-run] fastboot flash boot_b $WORK/boot.img" "$WORK/dry.log.out"
assert_contains "dry-run prints the linux flash command" "[dry-run] fastboot flash linux $WORK/root.img" "$WORK/dry.log.out"
assert_contains "dry-run prints the partition-size check" "getvar partition-size:boot_b" "$WORK/dry.log.out"
assert_file_absent "dry-run produced no mock log" "$WORK/dry.log"
assert_file_absent "dry-run wrote no FLASH-LOG" "$FLASH_LOG"

# --- 2: --execute only touches the two whitelisted partitions ---------------
run_flash exec.log --execute --boot "$WORK/boot.img" --root "$WORK/root.img" \
    --boot-sha256 "$BOOT_SHA" --root-sha256 "$ROOT_SHA" --flash-log "$WORK/LOG-flash"
assert_eq "--execute with both images exits 0" 0 "$?"
assert_eq "--execute issued 4 fastboot calls (2 getvar + 2 flash)" 4 "$(mock_calls exec.log)"
assert_contains "--execute flashes boot_b" "flash boot_b $WORK/boot.img" "$WORK/exec.log"
assert_contains "--execute flashes linux" "flash linux $WORK/root.img" "$WORK/exec.log"
assert_not_contains "--execute never issues erase" "erase" "$WORK/exec.log"
assert_eq "--execute flash targets are only boot_b/linux" "boot_b linux" \
    "$(awk '$1=="flash" {print $2}' "$WORK/exec.log" | tr '\n' ' ' | sed 's/ *$//')"
assert_contains "executed steps are appended to the flash log" "fastboot flash boot_b" "$WORK/LOG-flash"

# --- 3: flash whitelist ------------------------------------------------------
for forbidden in system_b dtbo_b userdata vendor_boot super boot_a modem persist; do
    run_flash "deny-$forbidden.log" --flash "$forbidden=$WORK/boot.img"
    assert_eq "flash $forbidden refused with exit 2" 2 "$?"
    assert_eq "flash $forbidden made no fastboot call" 0 "$(mock_calls "deny-$forbidden.log")"
    assert_contains "flash $forbidden names the partition" "$forbidden" "$WORK/deny-$forbidden.log.err"
done
run_flash deny-partarg.log --flash "system_b=$WORK/boot.img" --flash "boot_b=$WORK/boot.img"
assert_eq "system_b mixed with a legal partition is still refused" 2 "$?"
assert_eq "mixed request made no fastboot call" 0 "$(mock_calls deny-partarg.log)"

# --- 4: D-009 erase exception -----------------------------------------------
for target in dtbo_b boot_b system_b userdata vendor_boot super; do
    run_flash "erase-$target.log" --erase "$target"
    assert_eq "erase $target without --allow-dtbo-erase refused with exit 2" 2 "$?"
    assert_eq "erase $target without switch made no fastboot call" 0 "$(mock_calls "erase-$target.log")"
done
assert_contains "the refusal cites the missing switch" "no --allow-dtbo-erase switch given" "$WORK/erase-dtbo_b.log.err"

run_flash erase-ok.log --execute --allow-dtbo-erase --confirm-erase dtbo_b \
    --erase dtbo_b --flash-log "$FLASH_LOG"
assert_eq "gated erase dtbo_b executes (exit 0)" 0 "$?"
assert_eq "gated erase issues exactly one fastboot call" 1 "$(mock_calls erase-ok.log)"
assert_eq "gated erase sends exactly 'erase dtbo_b'" "erase dtbo_b" "$(mock_line erase-ok.log 1)"
assert_contains "gated erase is logged for auditing" "fastboot erase dtbo_b -> ok" "$FLASH_LOG"

for target in system_b userdata vendor_boot super boot_b; do
    run_flash "erase-sw-$target.log" --allow-dtbo-erase --execute --confirm-erase "$target" --erase "$target"
    assert_eq "erase $target refused even with the switch (exit 2)" 2 "$?"
    assert_eq "erase $target with switch made no fastboot call" 0 "$(mock_calls "erase-sw-$target.log")"
done
run_flash erase-op-sw.log --allow-dtbo-erase --execute --confirm-erase userdata --op erase=userdata
assert_eq "--op erase=userdata refused even with the switch" 2 "$?"
assert_eq "--op erase=userdata made no fastboot call" 0 "$(mock_calls erase-op-sw.log)"

run_flash flash-dtbo-sw.log --allow-dtbo-erase --flash "dtbo_b=$WORK/boot.img"
assert_eq "flash dtbo_b refused even with the switch on (exit 2)" 2 "$?"
assert_eq "flash dtbo_b made no fastboot call" 0 "$(mock_calls flash-dtbo-sw.log)"

run_flash erase-noconfirm.log --execute --allow-dtbo-erase --erase dtbo_b --flash-log "$WORK/LOG-none"
assert_eq "gated erase without --confirm-erase refused (exit 1)" 1 "$?"
assert_eq "unconfirmed erase made no fastboot call" 0 "$(mock_calls erase-noconfirm.log)"
assert_contains "the error demands the second confirmation" "second confirmation" "$WORK/erase-noconfirm.log.err"
run_flash erase-badconfirm.log --execute --allow-dtbo-erase --confirm-erase system_b --erase dtbo_b
assert_eq "mismatched confirmation token refused (exit 1)" 1 "$?"
assert_eq "mismatched confirmation made no fastboot call" 0 "$(mock_calls erase-badconfirm.log)"

run_flash erase-dry.log --allow-dtbo-erase --erase dtbo_b --flash-log "$WORK/LOG-dry"
assert_eq "dry-run with switch exits 0" 0 "$?"
assert_contains "dry-run prints the gated erase command" "[dry-run] fastboot erase dtbo_b" "$WORK/erase-dry.log.out"
assert_eq "dry-run gated erase made no fastboot call" 0 "$(mock_calls erase-dry.log)"
assert_file_absent "dry-run wrote no FLASH-LOG" "$WORK/LOG-dry"

run_flash erase-op-ok.log --execute --allow-dtbo-erase --confirm-erase dtbo_b \
    --op erase=dtbo_b --flash-log "$WORK/LOG-op"
assert_eq "--op erase=dtbo_b with switch executes (exit 0)" 0 "$?"
assert_eq "--op erase=dtbo_b sends exactly one erase" "erase dtbo_b" "$(mock_line erase-op-ok.log 1)"

# --- 5: other write operations are refused ----------------------------------
for op in "format=userdata" "wipe=super" "flashall=" "oem=unlock"; do
    run_flash "op-$(printf '%s' "$op" | tr '=' '-').log" --op "$op"
    assert_eq "--op $op refused with exit 2" 2 "$?"
    assert_eq "--op $op made no fastboot call" 0 "$(mock_calls "op-$(printf '%s' "$op" | tr '=' '-').log")"
done

# --- 5b: MANIFEST digest resolution ------------------------------------------
printf 'boot.img boot_b %s 2026-10-24T00:00:00Z %s\n' "$BOOT_SHA" "$(stat -c %s "$WORK/boot.img")" > "$WORK/manifest-ok"
run_flash manifest-ok.log --execute --boot "$WORK/boot.img" --manifest "$WORK/manifest-ok"
assert_eq "digest resolved from an explicit MANIFEST enables --execute" 0 "$?"
assert_contains "the manifest entry is used for boot_b" "sha256 ok (boot_b)" "$WORK/manifest-ok.log.out"
run_flash manifest-isolated.log --execute --boot "$WORK/boot.img" --manifest "$WORK/no-such-manifest"
assert_eq "an explicit --manifest disables the repo MANIFEST fallback (exit 1)" 1 "$?"

# --- 5c: digest binding is by PARTITION ROLE, not by CLI form ---------------
run_flash role-root.log --flash "linux=$WORK/root.img" --root-sha256 "$ROOT_SHA"
assert_eq "--flash linux=IMG honours --root-sha256 (exit 0)" 0 "$?"
assert_contains "--flash linux=IMG reports sha256 ok (linux)" "sha256 ok (linux): $ROOT_SHA" "$WORK/role-root.log.out"
assert_not_contains "no sha256 WARN for the --flash + --root-sha256 path" "no expected sha256" "$WORK/role-root.log.out"

run_flash role-boot.log --flash "boot_b=$WORK/boot.img" --boot-sha256 "$BOOT_SHA"
assert_eq "--flash boot_b=IMG honours --boot-sha256 (exit 0)" 0 "$?"
assert_contains "--flash boot_b=IMG reports sha256 ok (boot_b)" "sha256 ok (boot_b): $BOOT_SHA" "$WORK/role-boot.log.out"

run_flash role-exec.log --execute --flash "linux=$WORK/root.img" --root-sha256 "$ROOT_SHA"     --flash-log "$WORK/LOG-role"
assert_eq "--flash linux=IMG + --root-sha256 passes --execute" 0 "$?"
assert_contains "--flash linux=IMG really issued the flash" "flash linux $WORK/root.img" "$WORK/role-exec.log"

run_flash role-both.log --flash "linux=$WORK/root.img" --flash "boot_b=$WORK/boot.img"     --root-sha256 "$ROOT_SHA" --boot-sha256 "$BOOT_SHA"
assert_eq "both roles get their own digest in one run" 0 "$?"
assert_contains "root role digest used" "sha256 ok (linux): $ROOT_SHA" "$WORK/role-both.log.out"
assert_contains "boot role digest used" "sha256 ok (boot_b): $BOOT_SHA" "$WORK/role-both.log.out"

# --- 5d: MANIFEST parsing and default search order ---------------------------
mkdir -p "$WORK/md"
ROOT_SIZE=$(stat -c %s "$WORK/root.img")
printf '# aggregate manifest (pipe separated, like scripts/collect-manifest.sh)\nroot.img | linux | %s | %s | 2026-10-24T00:00:00Z\n' \
    "$ROOT_SHA" "$ROOT_SIZE" > "$WORK/md/MANIFEST"
printf 'root.img boot_b %s 2026-10-24T00:00:00Z %s\n' \
    "0000000000000000000000000000000000000000000000000000000000000000" "$ROOT_SIZE" > "$WORK/md/MANIFEST.boot"
run_flash_default pipe.log --flash "linux=$WORK/root.img"
assert_eq "pipe-separated aggregate MANIFEST resolves the digest (exit 0)" 0 "$?"
assert_contains "pipe MANIFEST yields sha256 ok" "sha256 ok (linux): $ROOT_SHA" "$WORK/pipe.log.out"
assert_eq "MANIFEST is consulted before MANIFEST.boot (the wrong digest was not used)" 0 "$?"

rm -f "$WORK/md/MANIFEST"
printf 'root.img | linux | %s | %s | 2026-10-24T00:00:00Z\n' "$ROOT_SHA" "$ROOT_SIZE" > "$WORK/md/MANIFEST.rootfs"
rm -f "$WORK/md/MANIFEST.boot"
run_flash_default fragment.log --flash "linux=$WORK/root.img"
assert_eq "MANIFEST.* fragment is used when MANIFEST/MANIFEST.boot are absent" 0 "$?"
assert_contains "fragment yields sha256 ok" "sha256 ok (linux): $ROOT_SHA" "$WORK/fragment.log.out"

# --- 5e: actionable WARN + sidecar convention --------------------------------
rm -rf "$WORK/md"; mkdir -p "$WORK/md"
run_flash_default warn.log --flash "linux=$WORK/root.img"
assert_eq "unknown digest stays a WARN in dry-run (exit 0)" 0 "$?"
assert_contains "the WARN offers the copy-paste flag" "--root-sha256 $ROOT_SHA" "$WORK/warn.log.out"
assert_contains "the WARN offers the sidecar command" ".sha256" "$WORK/warn.log.out"
assert_contains "the WARN points at collect-manifest.sh" "scripts/collect-manifest.sh" "$WORK/warn.log.out"

run_flash_default warn-exec.log --execute --flash "linux=$WORK/root.img"
assert_eq "--execute without any digest is refused (exit 1)" 1 "$?"
assert_contains "the execution refusal repeats the hint" "--root-sha256 $ROOT_SHA" "$WORK/warn-exec.log.out"

cp "$WORK/root.img" "$WORK/side.img"
printf '%s  %s\n' "$ROOT_SHA" "$WORK/side.img" > "$WORK/side.img.sha256"
run_flash side.log --flash "linux=$WORK/side.img"
assert_eq "<image>.sha256 sidecar works for --flash (exit 0)" 0 "$?"
assert_contains "sidecar yields sha256 ok (linux)" "sha256 ok (linux): $ROOT_SHA" "$WORK/side.log.out"

# --- 6: size check aborts before writing ------------------------------------
MOCK_CAPACITY=0x1000000 run_flash huge.log --execute --boot "$WORK/huge.img" --boot-sha256 "$HUGE_SHA"
assert_eq "oversized image aborts with exit 1" 1 "$?"
assert_contains "oversized image explains the limit" "exceeds partition size" "$WORK/huge.log.err"
assert_eq "oversized image issued only the getvar call" 1 "$(mock_calls huge.log)"
assert_not_contains "oversized image never reached flash" "flash" "$WORK/huge.log"

# --- 7: preflight refuses bad digests / unknown digests ----------------------
run_flash badsha.log --execute --boot "$WORK/boot.img" --boot-sha256 0000000000000000000000000000000000000000000000000000000000000000
assert_eq "wrong sha256 refused with exit 1" 1 "$?"
assert_eq "wrong sha256 made no fastboot call" 0 "$(mock_calls badsha.log)"
assert_contains "wrong sha256 is reported" "sha256 mismatch" "$WORK/badsha.log.err"

run_flash nosha.log --execute --boot "$WORK/boot.img" --manifest "$WORK/no-such-manifest"
assert_eq "missing expected sha256 refused with exit 1" 1 "$?"
assert_eq "missing sha256 made no fastboot call" 0 "$(mock_calls nosha.log)"
assert_contains "missing sha256 is reported" "refusing --execute without a verifiable sha256" "$WORK/nosha.log.err"
assert_contains "missing sha256 WARN names the partition" "no expected sha256 known" "$WORK/nosha.log.out"

run_flash missing.log --execute --boot "$WORK/nope.img" --boot-sha256 "$BOOT_SHA"
assert_eq "missing image refused with exit 1" 1 "$?"
assert_eq "missing image made no fastboot call" 0 "$(mock_calls missing.log)"

# --- 8: reboot-bootloader is the only allowed non-flash op -------------------
run_flash reboot.log --op reboot-bootloader
assert_eq "--op reboot-bootloader accepted (exit 0)" 0 "$?"
assert_contains "reboot-bootloader is printed in dry-run" "[dry-run] fastboot reboot-bootloader" "$WORK/reboot.log.out"
assert_eq "dry-run reboot-bootloader made no fastboot call" 0 "$(mock_calls reboot.log)"

run_flash reboot-exec.log --execute --boot "$WORK/boot.img" --boot-sha256 "$BOOT_SHA" --reboot-bootloader --flash-log "$WORK/LOG-reboot"
assert_eq "--execute with reboot-bootloader exits 0" 0 "$?"
assert_contains "reboot-bootloader issued after flash" "reboot-bootloader" "$WORK/reboot-exec.log"

# --- 9: static guarantees ---------------------------------------------------
assert_eq "flash whitelist constant lists only boot_b and linux" 1 "$(grep -c 'ALLOWED_PARTITIONS="boot_b linux"' "$SCRIPT")"
assert_eq "erase whitelist constant is dtbo_b" 'ERASE_ALLOWPART=dtbo_b' "$(grep -o 'ERASE_ALLOWPART=dtbo_b' "$SCRIPT")"
assert_eq "exactly one run_fastboot erase call site" 1 "$(grep -c 'run_fastboot erase' "$SCRIPT")"
assert_eq "no /dev/block path outside comments" "" \
    "$(grep -n '/dev/block' "$SCRIPT" | grep -v '^[0-9]*: *#' | cut -d: -f1 | tr '\n' ' ' | sed 's/ *$//')"
stray_ops=$(grep -oE 'run_fastboot [a-z-]+' "$SCRIPT" | awk '{print $2}' | grep -vE '^(flash|getvar|reboot-bootloader|erase)$' | tr '\n' ' ')
assert_eq "every run_fastboot call uses an allowlisted operation" "" "$stray_ops"
assert_eq "erase runs only through the confirmation helper" 2 "$(grep -c 'confirm_erase_execution' "$SCRIPT")"
assert_eq "the erase gate is enforced in 4 places (def, parse, guard, execute)" 4 "$(grep -c 'require_erase_allowed' "$SCRIPT")"
assert_contains "the header cites the D-009 decision" "decisions.md D-009" "$SCRIPT"

summary "test-flash-safety"
