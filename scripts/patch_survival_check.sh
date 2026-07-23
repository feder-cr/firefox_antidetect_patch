#!/usr/bin/env bash
# Patch-survival check — run after rebasing the stealth stack onto a new base.
#
# WHY THIS EXISTS
# ---------------
# The FF150->151 rebase silently dropped an entire upstream contribution (the
# TTC font fix, PR #2): FindSfntOffset was gone and two signatures had reverted
# to upstream. Everything still built, every smoke test passed, and the font
# gate stayed green because it only checked that the 72 families were *listed* -
# not that their faces still loaded. The loss surfaced only weeks later, by
# hand. These two checks would have caught it in seconds:
#
#   1. LINE SURVIVAL  - every line the previous stealth branch ADDED must still
#      exist somewhere in the corresponding file on the new branch. Line numbers
#      and surrounding context are allowed to move; the content is not allowed
#      to vanish.
#   2. CALL-SITE COUNT - every stealth identifier must appear at least as many
#      times as before. This catches the nastier shape: a helper survives as a
#      definition while its call sites disappear, so a plain grep for the name
#      still finds it and looks fine.
#
# Intentional removals and refactors are expected: put those lines/identifiers
# in scripts/patch_survival_allow.txt (one substring per line, '#' comments ok)
# so the gate stays honest instead of being switched off.
#
# Usage:
#   ./scripts/patch_survival_check.sh <prev-base-tag> <prev-branch>
#   ./scripts/patch_survival_check.sh FIREFOX_150_0_1_RELEASE stealth/150
#
# Exit 0 iff nothing was lost (or everything lost is allow-listed).

set -uo pipefail
cd "$(dirname "$0")/.."

PREV_BASE="${1:-${PREV_BASE:-}}"
PREV_BRANCH="${2:-${PREV_BRANCH:-}}"

if [[ -z "$PREV_BASE" || -z "$PREV_BRANCH" ]]; then
    echo "usage: $0 <prev-base-tag> <prev-branch>   (e.g. FIREFOX_150_0_1_RELEASE stealth/150)" >&2
    exit 2
fi
for ref in "$PREV_BASE" "$PREV_BRANCH"; do
    git rev-parse --verify --quiet "$ref" >/dev/null || {
        echo "[GATE FAIL] unknown git ref: $ref" >&2; exit 2; }
done

ALLOW_FILE="scripts/patch_survival_allow.txt"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# Allow-listed substrings (intentional removals / refactors).
if [[ -f "$ALLOW_FILE" ]]; then
    grep -vE '^\s*(#|$)' "$ALLOW_FILE" > "$TMP/allow" || true
else
    : > "$TMP/allow"
fi
allowed() {  # $1 = text; true if any allow-list entry is a substring of it
    [[ -s "$TMP/allow" ]] || return 1
    grep -qFf "$TMP/allow" <<<"$1"
}

# Normalise for comparison: strip leading/trailing space, collapse runs of space.
norm() { sed -e 's/[[:space:]]\+/ /g' -e 's/^ //' -e 's/ $//'; }

mapfile -t FILES < <(git diff --name-only "$PREV_BASE".."$PREV_BRANCH")
echo "=== patch_survival_check: ${#FILES[@]} files patched on $PREV_BRANCH ==="

LOST_LINES=0
LOST_CALLS=0
MISSING_FILES=0

for f in "${FILES[@]}"; do
    # File deleted on the new branch: nothing can have survived.
    if [[ ! -f "$f" ]]; then
        if allowed "$f"; then continue; fi
        echo "[LOST FILE] $f  (patched before, absent now)"
        MISSING_FILES=$((MISSING_FILES+1))
        continue
    fi

    norm < "$f" | sort -u > "$TMP/cur"

    # --- check 1: line survival -------------------------------------------
    git diff "$PREV_BASE".."$PREV_BRANCH" -- "$f" \
        | grep '^+' | grep -v '^+++' | cut -c2- | norm \
        | grep -vE '^$' | sort -u > "$TMP/added"

    while IFS= read -r line; do
        [[ -z "$line" ]] && continue
        if ! grep -qxF "$line" "$TMP/cur"; then
            allowed "$line" && continue
            echo "[LOST LINE] $f"
            echo "            $line"
            LOST_LINES=$((LOST_LINES+1))
        fi
    done < "$TMP/added"

    # --- check 2: call-site count -----------------------------------------
    # Only identifiers that are ours; upstream churn is not our business.
    git show "$PREV_BRANCH:$f" 2>/dev/null \
        | grep -oE '\b(Stealth[A-Za-z0-9_]+|[A-Za-z0-9_]*[Ss]tealthfox[A-Za-z0-9_]*|juggler[A-Za-z0-9_]+)\b' \
        | sort -u > "$TMP/ids" || true
    while IFS= read -r id; do
        [[ -z "$id" ]] && continue
        before=$(git show "$PREV_BRANCH:$f" 2>/dev/null | grep -cF "$id" || true)
        after=$(grep -cF "$id" "$f" || true)
        if (( after < before )); then
            allowed "$id" && continue
            echo "[LOST CALL-SITES] $f: '$id' appears $after time(s), was $before"
            LOST_CALLS=$((LOST_CALLS+1))
        fi
    done < "$TMP/ids"
done

echo ""
echo "=== patch_survival_check summary ==="
TOTAL=$((LOST_LINES + LOST_CALLS + MISSING_FILES))
if (( TOTAL == 0 )); then
    echo "OK - every added line and every stealth identifier survived the rebase."
    exit 0
fi
echo "lost lines: $LOST_LINES | identifiers with fewer occurrences: $LOST_CALLS | missing files: $MISSING_FILES"
echo ""
echo "Each finding is either a patch the rebase dropped (re-apply it) or a"
echo "deliberate removal (add a distinctive substring to $ALLOW_FILE, with a"
echo "comment saying why). Do not silence this gate by disabling it."
exit 1
