#!/bin/bash
# Package built programs for release.  Run after src/build-dos.sh and/or src/build-linux.sh.
#   tools/package.sh dos|linux VERSION     ->  dist/vantool-VERSION-dos.zip | dist/vantool-VERSION-linux-x86_64.tar.gz
# DOS text files get 8.3 names and CRLF line endings.
set -e
cd "$(dirname "$0")/.."
KIND=${1:?usage: package.sh dos|linux VERSION}
VER=${2:?usage: package.sh dos|linux VERSION}
mkdir -p dist
STAGE=$(mktemp -d)
trap 'rm -rf "${STAGE:?}"' EXIT
case "$KIND" in
dos)
    cp src/VANTOOL.EXE src/VANPLAY.EXE "$STAGE"
    sed 's/$/\r/' README.md > "$STAGE/README.TXT"
    sed 's/$/\r/' docs/VAN_SPEC.md > "$STAGE/VANSPEC.TXT"
    [ -f LICENSE ] && sed 's/$/\r/' LICENSE > "$STAGE/LICENSE.TXT"
    OUT="$PWD/dist/vantool-$VER-dos.zip"
    rm -f "$OUT"
    (cd "$STAGE" && zip -q -X "$OUT" *)
    ;;
linux)
    D="$STAGE/vantool-$VER-linux-x86_64"
    mkdir "$D"
    cp src/vantool src/vanplay README.md docs/VAN_SPEC.md "$D"
    [ -f LICENSE ] && cp LICENSE "$D"
    OUT="$PWD/dist/vantool-$VER-linux-x86_64.tar.gz"
    tar -C "$STAGE" -czf "$OUT" "$(basename "$D")"
    ;;
*)
    echo "unknown kind: $KIND" >&2; exit 1 ;;
esac
echo "$OUT"
