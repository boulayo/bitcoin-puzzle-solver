#!/bin/bash
# SPDX-License-Identifier: AGPL-3.0-or-later
# Exports a clean copy of the project for a new public repository: the tracked source tree without binaries, the
# archive of old releases, the unrelated work folders and the git history, as a single initial commit.
#
#   tools/export_public.sh [DEST_DIR]        (default: ../pzsolve-public)
#
# Then, from DEST_DIR:  git remote add origin git@github.com:YOU/bitcoin-puzzle-solver.git && git push -u origin main
# Release binaries are built by GitHub Actions (.github/workflows/release.yml) when a tag vX.Y.Z is pushed.
set -euo pipefail
cd "$(dirname "$0")/.."
DEST=${1:-../pzsolve-public}
if [ -e "$DEST" ] && [ "$(ls -A "$DEST" | grep -vx '.git' | wc -l)" != 0 ]; then
  echo "$DEST already exists and is not empty: remove it or choose another folder"; exit 2
fi
if [ -n "$(git status --porcelain --untracked-files=no)" ]; then
  echo "the working tree has uncommitted changes: commit them first"; exit 2
fi
VERSION=$(cat VERSION)
mkdir -p "$DEST"
# tracked files only, minus what a public repository must not carry
git ls-files -z | grep -zvE '^(archive|dist|lavoro_rilevatore|_en)/|^tools/tag_releases\.sh$' | xargs -0 -I{} cp --parents {} "$DEST"/
cd "$DEST"
[ -d .git ] || git init -q -b main      # an existing empty clone keeps its remote and branch name
git add -A
git -c user.name="${GIT_AUTHOR_NAME:-pzsolve}" -c user.email="${GIT_AUTHOR_EMAIL:-noreply@example.invalid}" \
    commit -q -m "pzsolve $VERSION" -m "First public commit of the English edition (AGPL-3.0-or-later)."
echo "exported $(git ls-files | wc -l) files to $DEST (version $VERSION)"
du -sh --exclude=.git . | cat
echo "next: set your git identity if needed ('git commit --amend --reset-author'), add the remote and push; tag v$VERSION to publish the binaries."
