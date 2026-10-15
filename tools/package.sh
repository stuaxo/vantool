#!/bin/bash
# Package built programs for release.  Run after src/build-dos.sh and/or src/build-linux.sh.
#   tools/package.sh dos VERSION    ->  dist/vantool-VERSION-dos.zip
#   tools/package.sh linux VERSION  ->  dist/vantool-VERSION-linux-x86_64.tar.gz
#   tools/package.sh deb VERSION    ->  dist/vantool_DEBVERSION_ARCH.deb (needs dpkg-dev)
# DOS text files get 8.3 names and CRLF line endings.  A deb version must start with a digit, so a
# leading "v" is dropped (v1.0 -> 1.0) and anything else, such as a branch name, becomes 0~name.
set -e
cd "$(dirname "$0")/.."
KIND=${1:?usage: package.sh dos|linux|deb VERSION}
VER=${2:?usage: package.sh dos|linux|deb VERSION}
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
deb)
    case "$VER" in
    v[0-9]*|[0-9]*) DEBVER=${VER#v} ;;
    *) DEBVER=0~$VER ;;
    esac
    DEBVER=$(echo "$DEBVER" | tr -c 'A-Za-z0-9.+~\n' '.')
    ARCH=$(dpkg --print-architecture)
    chmod 755 "$STAGE"
    install -D -s -m 755 src/vantool "$STAGE/usr/bin/vantool"
    install -D -s -m 755 src/vanplay "$STAGE/usr/bin/vanplay"
    DOC="$STAGE/usr/share/doc/vantool"
    install -D -m 644 README.md "$DOC/README.md"
    install -D -m 644 docs/VAN_SPEC.md "$DOC/VAN_SPEC.md"
    [ -f LICENSE ] && install -D -m 644 LICENSE "$DOC/copyright"
    # dpkg-shlibdeps works out the library dependencies, with versions, from the binaries
    mkdir -p "$STAGE/debian" "$STAGE/DEBIAN"
    printf 'Source: vantool\n\nPackage: vantool\nArchitecture: any\n' > "$STAGE/debian/control"
    DEPENDS=$(cd "$STAGE" && dpkg-shlibdeps -O usr/bin/vantool usr/bin/vanplay 2> >(grep -v 'should already be installed' >&2) | sed 's/^shlibs:Depends=//')
    rm -r "$STAGE/debian"
    cat > "$STAGE/DEBIAN/control" <<CONTROL
Package: vantool
Version: $DEBVER
Architecture: $ARCH
Maintainer: ${DEBMAINTAINER:-Stuart Axon <stuaxo2@yahoo.com>}
Depends: $DEPENDS
Section: graphics
Priority: optional
Homepage: https://github.com/stuaxo/vantool
Installed-Size: $(du -sk --exclude=DEBIAN "$STAGE" | cut -f1)
Description: tools for VistaPro .VAN animation files
 vantool inspects, extracts, builds, cuts and repairs .VAN animations made
 by VistaPro 3.20 for DOS. vanplay plays them.
CONTROL
    OUT="$PWD/dist/vantool_${DEBVER}_$ARCH.deb"
    dpkg-deb --root-owner-group --build "$STAGE" "$OUT" >/dev/null
    ;;
*)
    echo "unknown kind: $KIND" >&2; exit 1 ;;
esac
echo "$OUT"
