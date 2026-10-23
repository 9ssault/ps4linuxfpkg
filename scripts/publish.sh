#!/usr/bin/env bash
# Publishes the source and the built package to GitHub.
#
#   scripts/publish.sh <owner>/ps4linuxfpkg [tag]
#
# Needs an authenticated gh (gh auth login) with permission to create the repo.
# Nothing here runs automatically; review it before you do.
set -euo pipefail

REPO="${1:-}"
TAG="${2:-v1.0.0}"

if [ -z "$REPO" ]; then
  echo "usage: $0 <owner>/<repo> [tag]" >&2
  exit 2
fi

cd "$(dirname "$0")/.."

if [ ! -f dist/ps4-linux-installer.pkg ]; then
  echo "building the package first"
  ./scripts/build.sh
  mkdir -p dist
  cp out/ps4-linux-installer.pkg dist/
  ( cd dist && sha256sum ps4-linux-installer.pkg > SHA256SUMS.txt )
fi

if ! git remote get-url origin >/dev/null 2>&1; then
  echo "creating $REPO"
  gh repo create "$REPO" --public --source=. --remote=origin --push
else
  git push -u origin HEAD
fi

if gh release view "$TAG" >/dev/null 2>&1; then
  gh release upload "$TAG" dist/ps4-linux-installer.pkg dist/SHA256SUMS.txt --clobber
else
  gh release create "$TAG" \
    dist/ps4-linux-installer.pkg dist/SHA256SUMS.txt \
    --title "$TAG" --notes-file docs/RELEASE_NOTES.md
fi

echo "published $REPO $TAG"
