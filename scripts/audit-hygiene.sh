#!/usr/bin/env bash
# audit-hygiene.sh - repository cleanliness gate for elish-archlinux.
#
# Fails (exit 1) when:
#   1. build artifacts (*.img *.ko *.o *.dtb *.dtbo Image *.apk *.deb *.pkg.tar.*
#      *.pyc) or artifact dirs (abuild/ chroot/ .src/ .output/ pkg/ .build/
#      pmbootstrap-work/ __pycache__/) exist anywhere in the repo outside output/;
#   2. a file listed in output/<date>/MANIFEST.* has a wrong size/SHA256, or the
#      MANIFEST points at a file that does not exist;
#   3. output/<date>/FLASH-LOG exists but is malformed, empty, or records an
#      erase outside the single D-009 exception (erase dtbo_b);
#   4. git status shows paths outside the expected set.
#
# Missing MANIFEST / FLASH-LOG files are WARN, never a crash (other modules may
# not have produced their output and no real flashing has happened yet). Files
# under output/ that no MANIFEST covers are a WARN too.
#
# Usage: scripts/audit-hygiene.sh [--repo DIR] [--date YYYYMMDD] [--strict] [--quiet]
#        [--ignore-dir RELPATH ...]   # loud WARN: skip an in-progress foreign build dir
set -uo pipefail

REPO_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
DATE=${OUTPUT_DATE:-20261024}
QUIET=0
STRICT=0
IGNORE_DIRS=()

ARTIFACT_GLOBS='*.img *.img.gz *.img.xz *.img.zst *.ko *.o *.dtb *.dtbo *.apk *.deb *.pkg.tar.* *.pkg.tar.zst Image Image.gz zImage vmlinuz *.pyc'
ARTIFACT_DIRS='abuild chroot pmbootstrap-work .src .output pkg .build __pycache__'
# Paths git status may legitimately show: the repo layout declared in README.md.
# Anything outside it (stray binaries, build dirs, scratch files at the root) fails.
ALLOWED_STATUS_PREFIXES=${HYGIENE_ALLOWED_STATUS:-scripts/ tests/ configs/ patches/ perf/ docs/ output/ bug.md README.md .gitignore}
FLASH_LOG_PATTERN='^[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9:]+Z  fastboot [a-z-]+ .* -> ok$'
ERASE_ALLOWED_PATTERN='fastboot erase dtbo_b -> ok'

say()  { [ "$QUIET" -eq 1 ] || printf '%s\n' "$*"; }
ok()   { say "  ok    $*"; }
warn() { printf '  WARN  %s\n' "$*"; WARNINGS=$((WARNINGS + 1)); }
fail() { printf '  FAIL  %s\n' "$*"; FAILURES=$((FAILURES + 1)); }

FAILURES=0
WARNINGS=0

while [ $# -gt 0 ]; do
    case "$1" in
        --repo) REPO_ROOT=$2; shift 2 ;;
        --date) DATE=$2; shift 2 ;;
        --ignore-dir) IGNORE_DIRS+=("${2%/}"); shift 2 ;;
        --strict) STRICT=1; shift ;;
        --quiet) QUIET=1; shift ;;
        -h|--help) sed -n '2,22p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) printf 'error: unknown argument %s\n' "$1" >&2; exit 1 ;;
    esac
done

[ -d "$REPO_ROOT" ] || { printf 'error: repo root not found: %s\n' "$REPO_ROOT" >&2; exit 1; }
OUTPUT_DIR="$REPO_ROOT/output"
DATE_DIR="$OUTPUT_DIR/$DATE"

say "== hygiene audit: $REPO_ROOT (date $DATE) =="

# ------------------------------------------------------------ 1. artifacts --
say "-- artifacts outside output/"
FIND_EXCLUDES=( -path "$REPO_ROOT/.git" -prune -o )
if [ -d "$OUTPUT_DIR" ]; then
    FIND_EXCLUDES+=( -path "$OUTPUT_DIR" -prune -o )
fi
for ignored in ${IGNORE_DIRS[@]+"${IGNORE_DIRS[@]}"}; do
    warn "skipping caller-requested path: $ignored (owned by another module and mid-build; remove the flag for the real verdict)"
    FIND_EXCLUDES+=( -path "$REPO_ROOT/$ignored" -prune -o )
done

artifact_args=()
for glob in $ARTIFACT_GLOBS; do
    [ "${#artifact_args[@]}" -eq 0 ] || artifact_args+=(-o)
    artifact_args+=(-name "$glob")
done

scan_hits=$(find "$REPO_ROOT" "${FIND_EXCLUDES[@]}" -type f \( "${artifact_args[@]}" \) -print 2>/dev/null)
if [ -n "$scan_hits" ]; then
    while IFS= read -r hit; do
        [ -n "$hit" ] && fail "artifact file in repo: ${hit#$REPO_ROOT/}"
    done <<< "$scan_hits"
else
    ok "no artifact files (*.img/*.ko/*.o/*.dtb/*.dtbo/Image/*.apk/*.deb/pkg.tar.*/*.pyc) outside output/"
fi

dir_hits=
for dirname in $ARTIFACT_DIRS; do
    while IFS= read -r hit; do
        [ -n "$hit" ] && dir_hits="$dir_hits $hit"
    done <<< "$(find "$REPO_ROOT" "${FIND_EXCLUDES[@]}" -type d -name "$dirname" -print 2>/dev/null)"
done
if [ -n "${dir_hits# }" ]; then
    for hit in $dir_hits; do
        fail "artifact directory in repo: ${hit#$REPO_ROOT/}"
    done
else
    ok "no artifact directories (abuild/chroot/.src/.output/pkg/.build/pmbootstrap-work/__pycache__)"
fi

# --------------------------------------------------- 2. MANIFEST digests ----
say "-- output/$DATE/MANIFEST.* digests"
manifest_seen=0
if [ ! -d "$DATE_DIR" ]; then
    warn "output/$DATE does not exist yet; nothing to verify"
else
    shopt -s nullglob
    manifests=("$DATE_DIR"/MANIFEST.*)
    shopt -u nullglob
    if [ "${#manifests[@]}" -eq 0 ]; then
        warn "no output/$DATE/MANIFEST.* found (allowed while other modules are still building)"
    fi
    for manifest in ${manifests[@]+"${manifests[@]}"}; do
        [ -f "$manifest" ] || continue
        manifest_seen=1
        entries=0
        while IFS= read -r raw_line; do
            case "$raw_line" in ''|\#*) continue ;; esac
            entries=$((entries + 1))
            normalized=$(printf '%s' "$raw_line" | tr '|' ' ')
            name=$(printf '%s' "$normalized" | awk '{print $1}')
            target=$(printf '%s' "$normalized" | awk '{print $2}')
            want_sha=$(printf '%s' "$normalized" | tr ' ' '\n' | grep -Ex '[0-9a-f]{64}' | head -1)
            want_size=$(printf '%s' "$normalized" | tr ' ' '\n' |
                awk 'length($0) > 0 && length($0) < 20 && $0 ~ /^[0-9]+$/ {print; exit}')
            candidate="$DATE_DIR/$name"
            if [ ! -f "$candidate" ] && [ -f "$REPO_ROOT/$name" ]; then
                candidate="$REPO_ROOT/$name"
            fi
            if [ ! -f "$candidate" ]; then
                candidate=$(find "$DATE_DIR" -type f -name "$(basename "$name")" -print -quit 2>/dev/null)
            fi
            if [ -z "$candidate" ] || [ ! -f "$candidate" ]; then
                fail "$(basename "$manifest"): file '$name' listed but not found under output/$DATE/ or in the repo"
                continue
            fi
            if [ -z "$want_sha" ]; then
                fail "$(basename "$manifest"): entry '$name' has no sha256 field"
                continue
            fi
            actual_sha=$(sha256sum "$candidate" | cut -d' ' -f1)
            if [ "$actual_sha" != "$want_sha" ]; then
                fail "$(basename "$candidate"): sha256 mismatch (manifest $want_sha, disk $actual_sha)"
            elif [ -n "$want_size" ] && [ "$(stat -c %s "$candidate")" != "$want_size" ]; then
                fail "$(basename "$candidate"): size mismatch (manifest $want_size, disk $(stat -c %s "$candidate"))"
            else
                ok "$(basename "$candidate") sha256+size verified for target '$target'"
            fi
        done < "$manifest"
        [ "$entries" -gt 0 ] || warn "$(basename "$manifest") contains no entries"
    done
    if [ "$manifest_seen" -eq 1 ]; then
        unlisted=$(comm -23 \
            <(find "$DATE_DIR" -type f ! -name 'MANIFEST.*' ! -name 'FLASH-LOG' ! -name '.gitkeep' -printf '%f\n' 2>/dev/null | sort -u) \
            <(cat "$DATE_DIR"/MANIFEST.* 2>/dev/null | grep -v '^#' | awk '{print $1}' | xargs -r -n1 basename 2>/dev/null | sort -u))
        if [ -n "$unlisted" ]; then
            warn "files under output/$DATE not covered by any MANIFEST: $(printf '%s ' $unlisted)"
        fi
    fi
fi

# ------------------------------------------------------ 3. FLASH-LOG trail --
say "-- output/$DATE/FLASH-LOG (D-009 gated erase trail)"
FLASH_LOG_PATH="$DATE_DIR/FLASH-LOG"
if [ ! -f "$FLASH_LOG_PATH" ]; then
    warn "no output/$DATE/FLASH-LOG yet (no --execute flash run has happened, so the D-009 erase trail is unverified)"
else
    if [ ! -s "$FLASH_LOG_PATH" ]; then
        fail "output/$DATE/FLASH-LOG exists but is empty"
    fi
    malformed=$(grep -v -E "$FLASH_LOG_PATTERN" "$FLASH_LOG_PATH" 2>/dev/null | grep -v '^$' | head -3)
    if [ -n "$malformed" ]; then
        fail "FLASH-LOG has malformed line(s): $(printf '[%s] ' $malformed)"
    fi
    erase_lines=$(grep -c '  fastboot erase ' "$FLASH_LOG_PATH" 2>/dev/null) || erase_lines=0
    flash_lines=$(grep -c '  fastboot flash ' "$FLASH_LOG_PATH" 2>/dev/null) || flash_lines=0
    forbidden_erase=$(grep '  fastboot erase ' "$FLASH_LOG_PATH" 2>/dev/null | grep -vF -- "$ERASE_ALLOWED_PATTERN" | head -3)
    if [ -n "$forbidden_erase" ]; then
        fail "FLASH-LOG records an erase outside the D-009 exception: $(printf '[%s] ' $forbidden_erase)"
    elif [ "${erase_lines:-0}" -gt 0 ]; then
        ok "FLASH-LOG records ${erase_lines} gated 'erase dtbo_b' entr(y/ies) and ${flash_lines} flash step(s)"
    else
        warn "FLASH-LOG records ${flash_lines} flash step(s) but no erase record (D-009 exception unused so far)"
    fi
fi

# ------------------------------------------------------ 4. git status ------
say "-- git status expected paths"
if ! git -C "$REPO_ROOT" rev-parse --git-dir >/dev/null 2>&1; then
    warn "$REPO_ROOT is not a git repository; skipping git status check"
else
    status_out=$(git -C "$REPO_ROOT" status --porcelain 2>/dev/null)
    if [ -z "$status_out" ]; then
        ok "git status is clean"
    else
        unexpected=0
        while IFS= read -r line; do
            [ -n "$line" ] || continue
            path="${line:3}"
            case "$path" in
                *' -> '*) path="${path##* -> }" ;;
            esac
            allowed=0
            for prefix in $ALLOWED_STATUS_PREFIXES; do
                case "$path" in
                    "$prefix"*) allowed=1; break ;;
                esac
            done
            if [ "$allowed" -eq 0 ]; then
                fail "git status shows unexpected path: $path"
                unexpected=1
            fi
        done <<< "$status_out"
        [ "$unexpected" -eq 0 ] && ok "git status only touches: $ALLOWED_STATUS_PREFIXES"
    fi
fi

# ------------------------------------------------------------- summary -----
say "== summary: $FAILURES failure(s), $WARNINGS warning(s) =="
if [ "$FAILURES" -ne 0 ]; then
    exit 1
fi
if [ "$STRICT" -eq 1 ] && [ "$WARNINGS" -ne 0 ]; then
    printf 'strict mode: %d warning(s) treated as failure\n' "$WARNINGS" >&2
    exit 1
fi
exit 0
