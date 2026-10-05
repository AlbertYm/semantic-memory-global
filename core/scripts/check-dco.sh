#!/usr/bin/env bash
set -euo pipefail

# DCO enforcement: every commit in the given range must carry a
# Signed-off-by trailer whose email matches the commit author
# (Developer Certificate of Origin 1.1 — see the DCO file).
#
# Usage: check-dco.sh <range>          e.g. origin/main..HEAD, sha1..sha2
#
# Exemptions (same as the standard DCO checks): merge commits and
# bot-authored commits (author name ending in [bot]).

RANGE="${1:?usage: check-dco.sh <commit-range>}"

# Materialise the commit list before the loop. Feeding the loop directly from
# `< <(git rev-list "$RANGE")` makes this check fail *open*: the process
# substitution runs in a subshell, so a rev-list failure (bad range, shallow
# clone, base sha not fetched) is invisible to `set -e` — the loop simply reads
# zero lines and the script reports "OK: 0 commit(s)" and exits 0. Assigning to
# a variable puts the exit status back on the main shell where `if` can see it.
if ! COMMITS=$(git rev-list "$RANGE" 2>&1); then
    echo "BLOCKED: cannot enumerate commits in range '$RANGE'"
    printf '%s\n' "$COMMITS" | sed 's/^/  /'
    echo "  A bad range, a shallow clone, or an unfetched base commit causes this."
    echo "=== DCO CHECK FAILED — the range could not be read, so nothing was verified ==="
    exit 1
fi

FAIL=0
CHECKED=0
while IFS= read -r sha; do
    # An empty range yields one empty line from the here-string; skip it.
    [ -n "$sha" ] || continue
    # Skip merge commits
    nparents=$(git rev-list --no-walk --parents -n1 "$sha" | wc -w)
    if [ "$nparents" -gt 2 ]; then
        continue
    fi
    author_name=$(git log -1 --format='%an' "$sha")
    case "$author_name" in
        *"[bot]") continue ;;
    esac
    CHECKED=$((CHECKED + 1))
    author_email=$(git log -1 --format='%ae' "$sha")
    trailers=$(git log -1 --format='%(trailers:key=Signed-off-by,valueonly)' "$sha")
    if ! printf '%s' "$trailers" | grep -qiF "<$author_email>"; then
        echo "BLOCKED: $sha lacks a Signed-off-by matching its author:"
        git log -1 --format='  author: %an <%ae>%n  subject: %s' "$sha"
        echo "  fix: git commit --amend -s   (or: git rebase --signoff <base>)"
        FAIL=1
    fi
done <<< "$COMMITS"

if [ "$FAIL" -ne 0 ]; then
    echo "=== DCO CHECK FAILED — every commit must be signed off (git commit -s) ==="
    echo "=== See the DCO file and CONTRIBUTING.md ==="
    exit 1
fi
echo "OK: $CHECKED commit(s) in $RANGE carry a valid Signed-off-by"
