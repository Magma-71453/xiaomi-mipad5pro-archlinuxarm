#!/usr/bin/env bash
# Shared assertion helpers for the elish test suite. Source, do not execute.
CHECKED=0
FAILED=0

emit() { printf '\n== %s ==\n' "$*"; }
pass() { CHECKED=$((CHECKED + 1)); printf '  PASS %s\n' "$*"; }
bad()  { CHECKED=$((CHECKED + 1)); FAILED=$((FAILED + 1)); printf '  FAIL %s\n' "$*"; }

assert_eq() {
    local what=$1 want=$2 got=${3-}
    if [ "$want" = "$got" ]; then
        pass "$what [$got]"
    else
        bad "$what: expected [$want] got [$got]"
    fi
}

assert_between() {
    local what=$1 low=$2 high=$3 got=${4-}
    if [ -n "$got" ] && [ "$got" -ge "$low" ] && [ "$got" -le "$high" ]; then
        pass "$what [$got in $low..$high]"
    else
        bad "$what: expected $low..$high, got [$got]"
    fi
}

assert_contains() {
    local what=$1 needle=$2 haystack_file=$3
    if [ -f "$haystack_file" ] && grep -qF -- "$needle" "$haystack_file"; then
        pass "$what (found: $needle)"
    else
        bad "$what: '$needle' not found in $haystack_file"
    fi
}

assert_not_contains() {
    local what=$1 needle=$2 haystack_file=$3
    if [ ! -f "$haystack_file" ]; then
        pass "$what (output file absent: $haystack_file)"
    elif grep -qF -- "$needle" "$haystack_file"; then
        bad "$what: forbidden '$needle' present in $haystack_file"
    else
        pass "$what (no '$needle')"
    fi
}

assert_file_absent() {
    local what=$1 path=$2
    if [ -e "$path" ]; then
        bad "$what: $path should not exist"
    else
        pass "$what ($path absent)"
    fi
}

summary() {
    local label=${1:-tests}
    if [ "$FAILED" -eq 0 ]; then
        printf '== %s: %d checks, all passed ==\n' "$label" "$CHECKED"
        return 0
    fi
    printf '== %s: %d checks, %d FAILED ==\n' "$label" "$CHECKED" "$FAILED"
    return 1
}
