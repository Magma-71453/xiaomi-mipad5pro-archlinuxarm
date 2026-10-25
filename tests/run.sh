#!/usr/bin/env bash
# Aggregate test entry point: any failing step makes this script exit non-zero.
#   (a) tests/test-bootimg.sh       boot image v0 build + verify
#   (b) tests/test-flash-safety.sh  flash whitelist / dry-run / size gate
#   (c) tests/test-hygiene.sh       repo cleanliness audit
#   (d) static checks: bash -n for every shell file, py_compile for the generator
#   (e) scripts/build-initramfs.sh --test (mocked mount, no device)
set -uo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
BUILD_ROOT=${BUILD_ROOT:-$(cd "$REPO/.." && pwd)/.build}
STEPS=0
FAILED=0

step() {
    local name=$1
    shift
    STEPS=$((STEPS + 1))
    printf '\n########## %s ##########\n' "$name"
    if "$@"; then
        printf '[run] PASS %s\n' "$name"
    else
        local rc=$?
        FAILED=$((FAILED + 1))
        printf '[run] FAIL %s (rc=%d)\n' "$name" "$rc"
    fi
}

static_checks() {
    local rc=0 file
    for file in "$REPO"/scripts/*.sh "$REPO"/tests/*.sh "$REPO"/configs/initramfs/init; do
        if bash -n "$file"; then
            printf '  ok  bash -n %s\n' "${file#$REPO/}"
        else
            printf '  FAIL bash -n %s\n' "${file#$REPO/}"
            rc=1
        fi
    done
    for file in "$REPO"/scripts/*.py; do
        if PYTHONPYCACHEPREFIX="$BUILD_ROOT/pycache" python3 -m py_compile "$file"; then
            printf '  ok  py_compile %s\n' "${file#$REPO/}"
        else
            printf '  FAIL py_compile %s\n' "${file#$REPO/}"
            rc=1
        fi
    done
    return "$rc"
}

printf '== elish test suite: %s ==\n' "$REPO"
step "static checks (bash -n, py_compile)" static_checks
step "(a) boot image v0 build + verify" bash "$HERE/test-bootimg.sh"
step "(b) flash whitelist / dry-run / size gate" bash "$HERE/test-flash-safety.sh"
step "(c) repository hygiene audit" bash "$HERE/test-hygiene.sh"
step "(e) initramfs build logic (mocked mount)" bash "$REPO/scripts/build-initramfs.sh" --test

printf '\n== run.sh summary: %d step(s), %d failed ==\n' "$STEPS" "$FAILED"
[ "$FAILED" -eq 0 ] || exit 1
printf 'ALL TESTS PASSED\n'
