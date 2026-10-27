#!/usr/bin/env bash
# Rebase stealth/<N> branch onto a new upstream Firefox tag.
# Mirrors Brave's "Chromium rebase" workflow + Tor Browser's ESR rebase issues.
#
# Usage:
#   ./scripts/rebase_to.sh FIREFOX_152_0_RELEASE 152
#
# Args:
#   $1 = upstream tag to rebase onto (e.g. FIREFOX_152_0_RELEASE)
#   $2 = new major version (used for branch + tag names: stealth/<N>)
#
# Pre-conditions:
#   - git rerere is enabled (git config rerere.enabled true) — REQUIRED
#   - origin remote points at feder-cr/invisible_firefox
#   - upstream remote points at mozilla-firefox/firefox

set -e
cd "$(dirname "$0")/.."

NEW_TAG="${1:?Usage: $0 <upstream-tag> <new-major>}"
NEW_MAJOR="${2:?Usage: $0 <upstream-tag> <new-major>}"

# Discover current branch + base tag
CUR_BRANCH=$(git rev-parse --abbrev-ref HEAD)
if [[ ! "$CUR_BRANCH" =~ ^stealth/([0-9]+)$ ]]; then
    echo "ERROR: must be on a stealth/<N> branch (currently: $CUR_BRANCH)"
    exit 1
fi
CUR_MAJOR="${BASH_REMATCH[1]}"
CUR_BASE_TAG="stealth-base/v${CUR_MAJOR}.0.1"

if ! git rev-parse --verify "$CUR_BASE_TAG" >/dev/null 2>&1; then
    echo "ERROR: base tag $CUR_BASE_TAG missing — cannot determine commit range"
    exit 1
fi

NEW_BRANCH="stealth/${NEW_MAJOR}"
NEW_BASE_TAG="stealth-base/v${NEW_MAJOR}.0.0"  # or .0.1 — adjust at tag time

# Verify rerere enabled
if [[ "$(git config rerere.enabled)" != "true" ]]; then
    echo "WARNING: git rerere is not enabled. Conflict resolutions will not"
    echo "be remembered. Enable with: git config rerere.enabled true"
    read -p "Continue anyway? (y/N) " -n 1 -r
    echo
    [[ ! $REPLY =~ ^[Yy]$ ]] && exit 1
fi

echo "=== rebase_to.sh ==="
echo "Current:  $CUR_BRANCH (base: $CUR_BASE_TAG)"
echo "Target:   $NEW_BRANCH (new base: $NEW_TAG → will be tagged $NEW_BASE_TAG)"
echo ""

# Fetch upstream
echo "--- Fetching upstream ---"
if ! git remote get-url upstream >/dev/null 2>&1; then
    echo "Adding upstream remote → mozilla-firefox/firefox"
    git remote add upstream https://github.com/mozilla-firefox/firefox.git
fi
git fetch upstream --tags

if ! git rev-parse --verify "$NEW_TAG" >/dev/null 2>&1; then
    echo "ERROR: tag $NEW_TAG not found in upstream"
    exit 1
fi

# Tag the new base
echo "--- Tagging new base ---"
git tag -f "$NEW_BASE_TAG" "$NEW_TAG"
echo "Tagged $NEW_BASE_TAG → $(git rev-parse --short $NEW_TAG)"

# Create new branch
echo "--- Creating $NEW_BRANCH from $CUR_BRANCH ---"
if git rev-parse --verify "$NEW_BRANCH" >/dev/null 2>&1; then
    echo "Branch $NEW_BRANCH already exists. Delete it first:"
    echo "  git branch -D $NEW_BRANCH"
    exit 1
fi
git checkout -b "$NEW_BRANCH" "$CUR_BRANCH"

# Rebase
echo ""
echo "--- Rebasing onto $NEW_BASE_TAG ---"
echo "    (conflicts will need manual resolution; rerere will remember them)"
echo ""

if ! git rebase --onto "$NEW_BASE_TAG" "$CUR_BASE_TAG"; then
    cat <<EOF

REBASE INTERRUPTED — manual conflict resolution needed.

Next steps:
  1. Edit conflicted files, then 'git add' them
  2. 'git rebase --continue'
  3. Repeat until rebase completes
  4. Run './scripts/rebase_gate.sh' to verify smoke tests pass

Useful commands during conflict resolution:
  git diff                 # see current conflict
  git log --oneline upstream...HEAD -- <file>   # see upstream churn on a file
  git checkout --ours <f>  # keep our side
  git checkout --theirs <f># keep their side
  git rebase --skip        # skip this commit (lose its changes)
  git rebase --abort       # back out completely

EOF
    exit 1
fi

echo ""
echo "REBASE COMPLETE without manual conflicts."
echo ""
echo "Next: run ./scripts/rebase_gate.sh to verify smoke tests pass."
echo "If green, tag and push:"
echo "    git tag -f stealth-head/v${NEW_MAJOR}.0.0 HEAD"
echo "    git push origin $NEW_BRANCH stealth-base/v${NEW_MAJOR}.0.0 stealth-head/v${NEW_MAJOR}.0.0"
