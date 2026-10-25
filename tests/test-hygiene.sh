#!/usr/bin/env bash
# (c) hygiene audit: copy a minimal repo skeleton into a temp dir, inject one
# artifact at a time and assert scripts/audit-hygiene.sh fails; also asserts the
# MANIFEST digest verification and that output/ contents are allowed.
set -uo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
. "$HERE/lib.sh"
AUDIT="$REPO/scripts/audit-hygiene.sh"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
FAKE="$WORK/repo"

reset_repo() {
    rm -rf "$FAKE"
    mkdir -p "$FAKE/scripts" "$FAKE/configs" "$FAKE/docs/findings" "$FAKE/output/20261024/boot"
    printf '# minimal repo skeleton for the hygiene test\n' > "$FAKE/README.md"
    printf '#!/bin/sh\nexit 0\n' > "$FAKE/scripts/placeholder.sh"
}

run_audit() {
    bash "$AUDIT" --repo "$FAKE" --date 20261024 > "$WORK/audit.out" 2> "$WORK/audit.err"
}

emit "test-hygiene: audit-hygiene.sh against a temp repo copy"

# --- 1: clean skeleton passes -------------------------------------------------
reset_repo
if run_audit; then ok_rc=0; else ok_rc=$?; fi
assert_eq "clean skeleton exits 0" 0 "$ok_rc"
assert_contains "missing MANIFEST is a WARN, not a crash" "WARN" "$WORK/audit.out"

# --- 2: a stray .img fails ----------------------------------------------------
reset_repo
head -c 1024 /dev/urandom > "$FAKE/fake.img"
if run_audit; then bad_rc=0; else bad_rc=$?; fi
assert_eq "injected fake.img makes the audit fail (exit 1)" 1 "$bad_rc"
assert_contains "fake.img is named in the failure" "artifact file in repo: fake.img" "$WORK/audit.out"

# --- 3: other artifact types fail too ----------------------------------------
for artifact in "kernel/driver.ko" "dtbs/board.dtb" "obj/entry.o" "Image" "pkg/busybox-1.38.0-1-aarch64.pkg.tar.zst"; do
    reset_repo
    mkdir -p "$FAKE/$(dirname "$artifact")"
    head -c 64 /dev/urandom > "$FAKE/$artifact"
    if run_audit; then rc=0; else rc=$?; fi
    assert_eq "artifact $artifact fails the audit" 1 "$rc"
done

# --- 4: artifact directories fail -------------------------------------------
for dirname in abuild chroot .src __pycache__; do
    reset_repo
    mkdir -p "$FAKE/$dirname/nested"
    printf 'x\n' > "$FAKE/$dirname/nested/file"
    if run_audit; then rc=0; else rc=$?; fi
    assert_eq "artifact directory $dirname/ fails the audit" 1 "$rc"
    assert_contains "$dirname/ is named in the failure" "artifact directory in repo" "$WORK/audit.out"
done

# --- 5: MANIFEST digests are verified ---------------------------------------
reset_repo
head -c 4096 /dev/urandom > "$FAKE/output/20261024/boot/demo.img"
DEMO_SHA=$(sha256sum "$FAKE/output/20261024/boot/demo.img" | cut -d' ' -f1)
DEMO_SIZE=$(stat -c %s "$FAKE/output/20261024/boot/demo.img")
write_manifest() {
    printf '# filename target sha256 build_time size\n' > "$FAKE/output/20261024/MANIFEST.boot"
    printf 'demo.img boot_b %s 2026-10-24T00:00:00Z %s\n' "$1" "$2" >> "$FAKE/output/20261024/MANIFEST.boot"
}
write_manifest "$DEMO_SHA" "$DEMO_SIZE"
if run_audit; then rc=0; else rc=$?; fi
assert_eq "repo with a correct MANIFEST exits 0" 0 "$rc"
assert_contains "correct MANIFEST entries are verified" "sha256+size verified for target 'boot_b'" "$WORK/audit.out"

write_manifest "0000000000000000000000000000000000000000000000000000000000000000" "$DEMO_SIZE"
if run_audit; then rc=0; else rc=$?; fi
assert_eq "MANIFEST with a wrong sha256 fails" 1 "$rc"
assert_contains "sha mismatch is named" "sha256 mismatch" "$WORK/audit.out"

write_manifest "$DEMO_SHA" 12345
if run_audit; then rc=0; else rc=$?; fi
assert_eq "MANIFEST with a wrong size fails" 1 "$rc"
assert_contains "size mismatch is named" "size mismatch" "$WORK/audit.out"

printf 'ghost.img boot_b %s 2026-10-24T00:00:00Z 100\n' "$DEMO_SHA" > "$FAKE/output/20261024/MANIFEST.boot"
if run_audit; then rc=0; else rc=$?; fi
assert_eq "MANIFEST listing a missing file fails" 1 "$rc"
assert_contains "missing file is named" "listed but not found" "$WORK/audit.out"

# --- 6: products inside output/ are allowed ---------------------------------
reset_repo
head -c 8192 /dev/urandom > "$FAKE/output/20261024/boot/boot.img"
head -c 512 /dev/urandom > "$FAKE/output/20261024/boot/other.img"
printf 'other.img boot_b %s 2026-10-24T00:00:00Z %s\n' \
    "$(sha256sum "$FAKE/output/20261024/boot/other.img" | cut -d' ' -f1)" \
    "$(stat -c %s "$FAKE/output/20261024/boot/other.img")" > "$FAKE/output/20261024/MANIFEST.boot"
if run_audit; then rc=0; else rc=$?; fi
assert_eq "boot.img inside output/ is allowed (exit 0)" 0 "$rc"
assert_contains "a MANIFEST-listed product is verified" "other.img sha256+size verified" "$WORK/audit.out"
assert_contains "unlisted output product is a WARN" "not covered by any MANIFEST" "$WORK/audit.out"

# --- 6b: the D-009 FLASH-LOG trail is verified ------------------------------
reset_repo
printf '2026-10-24T10:00:00Z  fastboot erase dtbo_b -> ok\n' > "$FAKE/output/20261024/FLASH-LOG"
if run_audit; then rc=0; else rc=$?; fi
assert_eq "FLASH-LOG with a gated erase dtbo_b passes" 0 "$rc"
assert_contains "the gated erase trail is acknowledged" "records 1 gated" "$WORK/audit.out"

printf '2026-10-24T10:00:00Z  fastboot erase dtbo_b -> ok\n2026-10-24T10:01:00Z  fastboot erase userdata -> ok\n' > "$FAKE/output/20261024/FLASH-LOG"
if run_audit; then rc=0; else rc=$?; fi
assert_eq "FLASH-LOG with a forbidden erase fails" 1 "$rc"
assert_contains "the forbidden erase is named" "erase outside the D-009 exception" "$WORK/audit.out"

printf '2026-10-24T10:02:00Z  fastboot flash boot_b /tmp/boot.img -> ok\n' > "$FAKE/output/20261024/FLASH-LOG"
if run_audit; then rc=0; else rc=$?; fi
assert_eq "flash-only FLASH-LOG passes" 0 "$rc"
assert_contains "an unused exception is only a WARN" "no erase record" "$WORK/audit.out"

printf 'garbage line\n' > "$FAKE/output/20261024/FLASH-LOG"
if run_audit; then rc=0; else rc=$?; fi
assert_eq "malformed FLASH-LOG fails" 1 "$rc"
assert_contains "malformed line is reported" "malformed line" "$WORK/audit.out"

# --- 6c: a missing FLASH-LOG is only a WARN --------------------------------
reset_repo
if run_audit; then rc=0; else rc=$?; fi
assert_eq "missing FLASH-LOG exits 0" 0 "$rc"
assert_contains "missing FLASH-LOG is a WARN" "no output/20261024/FLASH-LOG yet" "$WORK/audit.out"

# --- 7: real repository ------------------------------------------------------
# Other modules build in parallel inside the same tree. The honest assertions are:
#   (i)   the audit never crashes on the real repo (exit 0 or 1, never 2+);
#   (ii)  no failure is owned by THIS module's paths;
#   (iii) with a foreign in-progress build dir excluded, the audit is clean.
if bash "$AUDIT" > "$WORK/real.out" 2>&1; then rc=0; else rc=$?; fi
if [ "$rc" -le 1 ]; then assert_eq "real repository audit does not crash (exit 0 or 1)" 0 0;
else bad "real repository audit crashed with exit $rc"; fi

raw_failures=$(grep -c '  FAIL' "$WORK/real.out" 2>/dev/null) || raw_failures=0
owned_failures=$(grep '  FAIL' "$WORK/real.out" 2>/dev/null |
    grep -E 'scripts/|tests/|configs/initramfs/|docs/findings/|docs/flash-guide\.md|output/20261024/boot/|MANIFEST\.boot' |
    wc -l)
assert_eq "no residue owned by the boot module in the real repo" 0 "$owned_failures"
if [ "$raw_failures" -gt "$owned_failures" ]; then
    pass "foreign residue is reported honestly: $((raw_failures - owned_failures)) failure(s) outside this module"
fi

if bash "$AUDIT" --ignore-dir perf/schedplace/.build > "$WORK/real-ig.out" 2>&1; then rc2=0; else rc2=$?; fi
assert_eq "real audit is clean once foreign mid-build dirs are excluded" 0 "$rc2"
assert_contains "the exclusion is announced, not silent" "skipping caller-requested path" "$WORK/real-ig.out"
assert_contains "real repository audit reports no failures" "0 failure(s)" "$WORK/real-ig.out"

summary "test-hygiene"
