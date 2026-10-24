#!/bin/bash
# SPDX-License-Identifier: AGPL-3.0-or-later
# Release of pzsolve. Usage: tools/release.sh [NEW_VERSION]
# Without an argument it releases the version already written in VERSION (first release).
# See docs/en/RELEASING.md.
#
# What it does: checks the preconditions, bumps VERSION and commits, runs tools/verify.sh and creates the annotated tag vX.Y.Z.
# It does not build or commit any binary and it does not push: the binaries are built and published by the GitHub Actions
# workflow .github/workflows/release.yml when the tag is pushed. The last line printed is the push command to run.
set -euo pipefail
cd "$(dirname "$0")/.."

# COMMIT_TRAILERS (optional): lines appended to the message of the commit created by this script (e.g. "Co-Authored-By: ...").
TRAILERS=${COMMIT_TRAILERS:-}
OLD=$(cat VERSION)
NEW=${1:-$OLD}
[[ "$NEW" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || { echo "invalid version: $NEW (expected X.Y.Z)"; exit 2; }

if [ -n "$(git status --porcelain --untracked-files=no)" ]; then
  echo "the git tree has uncommitted changes: commit or discard them before releasing"; exit 2
fi
grep -q "^## \[$NEW\]" CHANGELOG.md || { echo "CHANGELOG.md has no '## [$NEW]' section"; exit 2; }
if git rev-parse -q --verify "refs/tags/v$NEW" >/dev/null; then
  echo "the tag v$NEW already exists"; exit 2
fi
if [ "$NEW" != "$OLD" ]; then
  # the new number must be greater than the old one
  if [ "$(printf '%s\n%s\n' "$OLD" "$NEW" | sort -V | tail -1)" != "$NEW" ]; then
    echo "the new version ($NEW) must be greater than the current one ($OLD)"; exit 2
  fi
  echo "$NEW" > VERSION
  git add VERSION
  git commit -q -m "Version $NEW" ${TRAILERS:+-m "$TRAILERS"}
fi

echo "== full verification (clean build, tests, end-to-end, sanitizers, valgrind)"
tools/verify.sh "${VERIFY_MODE:-quick}"      # VERIFY_MODE=full for the complete end-to-end run

if [ -n "$(git status --porcelain --untracked-files=no)" ]; then
  echo "the verification changed tracked files: the tag would not match what was tested"; git status --short --untracked-files=no; exit 2
fi
git tag -a "v$NEW" -m "pzsolve $NEW"
echo
echo "Release v$NEW is ready at commit $(git rev-parse --short HEAD). Nothing was pushed. Publish it with:"
echo "  git push origin $(git rev-parse --abbrev-ref HEAD) --follow-tags"
echo "The tag push starts the GitHub Actions release workflow, which builds the binaries and publishes the release."
