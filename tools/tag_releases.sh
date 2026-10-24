#!/bin/bash
# SPDX-License-Identifier: AGPL-3.0-or-later
# Recreates missing tags vX.Y.Z on commits whose subject is "Release vX.Y.Z" (the form used by the older release script).
# New releases do not need it: tools/release.sh creates the tag, and the maintainer pushes it. Use this only on a clone whose
# history has such commits but not their tags. It creates the tags locally and prints the command to publish them.
#   tools/tag_releases.sh            create the missing tags
#   tools/tag_releases.sh --dry-run  only show what it would do
set -euo pipefail
cd "$(dirname "$0")/.."
DRY=${1:-}
git fetch origin --tags --quiet || true
new=0
while read -r sha subject; do
  tag=${subject#Release }
  if git rev-parse -q --verify "refs/tags/$tag" >/dev/null; then echo "already exists: $tag"; continue; fi
  echo "to create : $tag -> ${sha:0:7}"
  [ "$DRY" = --dry-run ] || git tag -a "$tag" "$sha" -m "pzsolve ${tag#v}"
  new=$((new+1))
done < <(git log --format='%H %s' --reverse | grep -E ' Release v[0-9]+\.[0-9]+\.[0-9]+$')
if [ "$DRY" = --dry-run ]; then echo "(dry run: no tag created, $new to create)"; exit 0; fi
echo "done: $new new tags created locally"
[ "$new" -eq 0 ] || echo "Publish them with:  git push origin --tags"
