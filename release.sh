#!/usr/bin/env bash
# Build the source tarball attached to a GitHub release.
#
# The tarball is written to the repository root and is intentionally not
# committed. It is reproducible: the same commit always produces the same
# bytes. Sign it with your release key, then upload the tarball and the
# detached signature to the matching GitHub release.
set -euo pipefail

cd "$(dirname "$0")"

version=$(sed -n 's/^project(hyd VERSION \([0-9][0-9.]*\).*/\1/p' CMakeLists.txt)
if [ -z "$version" ]; then
    echo "error: could not read the project version from CMakeLists.txt" >&2
    exit 1
fi

if [ -n "$(git status --porcelain)" ]; then
    echo "error: the working tree is dirty; commit or stash changes first" >&2
    echo "       the tarball is built from HEAD, not from the working tree" >&2
    exit 1
fi

tag="v$version"
name="hyd-$version"
archive="$name.tar.gz"
signature="$archive.asc"

if git rev-parse -q --verify "refs/tags/$tag" >/dev/null; then
    if [ "$(git rev-parse "$tag^{commit}")" != "$(git rev-parse HEAD)" ]; then
        echo "error: tag $tag does not point at HEAD" >&2
        exit 1
    fi
else
    echo "note: tag $tag does not exist yet; create and sign it with:" >&2
    echo "      git tag -s $tag" >&2
    echo
fi

rm -f -- "$archive" "$signature"
git archive --format=tar --prefix="$name/" HEAD | gzip -n -9 > "$archive"

sha256=$(sha256sum -- "$archive" | cut -d' ' -f1)

echo "Created $archive"
echo
echo "Sign the tarball:"
echo "  gpg --detach-sign --armor $archive"
echo
echo "PKGBUILD sha256sums entry:"
echo "  $sha256"
