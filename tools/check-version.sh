#!/bin/bash
# Check that the version in src/version.h matches the release tag (when run for one) and the top entry of
# debian/changelog (when that exists).  CI runs it on every push; run it by hand before tagging:
#   tools/check-version.sh            check against the files only
#   tools/check-version.sh v1.2.3     check against this tag
set -e
cd "$(dirname "$0")/.."
VER=$(sed -n 's/^#define VERSION "\(.*\)"$/\1/p' src/version.h)
[ -n "$VER" ] || { echo "no VERSION found in src/version.h" >&2; exit 1; }
case "$VER" in
[0-9]*.[0-9]*.[0-9]*) ;;
*) echo "src/version.h: version '$VER' is not X.Y.Z" >&2; exit 1 ;;
esac
STATUS=0
TAG=${1:-}
if [ -n "$TAG" ] && [ "${TAG#v}" != "$VER" ]; then
    echo "tag $TAG does not match src/version.h ($VER)" >&2; STATUS=1
fi
if [ -f debian/changelog ]; then
    DEB=$(dpkg-parsechangelog -S Version 2>/dev/null || sed -n '1s/^[^(]*(\([^)]*\)).*/\1/p' debian/changelog)
    if [ "${DEB%%-*}" != "$VER" ]; then
        echo "debian/changelog is $DEB but src/version.h is $VER" >&2; STATUS=1
    fi
fi
[ $STATUS = 0 ] && echo "version $VER ok"
exit $STATUS
