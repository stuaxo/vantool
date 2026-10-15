#!/bin/bash
# Package built programs for release.  Run after src/build-dos.sh and/or src/build-linux.sh.
#   tools/package.sh dos VERSION    ->  dist/vantool-VERSION-dos.zip
#   tools/package.sh freedos VERSION  ->  dist/vantool-VERSION-freedos.zip (FreeDOS package; install with FDNPKG)
#   tools/package.sh linux VERSION  ->  dist/vantool-VERSION-linux-x86_64.tar.gz
#   tools/package.sh deb VERSION    ->  dist/vantool_DEBVERSION_ARCH.deb (needs dpkg-dev)
#   tools/package.sh win32|win64 VERSION  ->  dist/vantool-VERSION-win32.zip | -win64.zip (after src/build-windows.sh)
# DOS and Windows text files get CRLF line endings, and DOS ones 8.3 names.  A deb version must start with a digit, so a
# leading "v" is dropped (v1.0 -> 1.0) and anything else, such as a branch name, becomes 0~name.
set -e
cd "$(dirname "$0")/.."
KIND=${1:?usage: package.sh dos|freedos|linux|deb|win32|win64 VERSION}
VER=${2:?usage: package.sh dos|freedos|linux|deb|win32|win64 VERSION}
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
freedos)
    # FreeDOS layout for non-core programs: APPINFO\VANTOOL.LSM, PROGS\VANTOOL\*, LINKS\*.BAT
    mkdir -p "$STAGE/APPINFO" "$STAGE/PROGS/VANTOOL" "$STAGE/LINKS"
    P="$STAGE/PROGS/VANTOOL"
    cp src/VANTOOL.EXE src/VANPLAY.EXE "$P"
    sed 's/$/\r/' README.md > "$P/README.TXT"
    sed 's/$/\r/' docs/VAN_SPEC.md > "$P/VANSPEC.TXT"
    [ -f LICENSE ] && sed 's/$/\r/' LICENSE > "$P/LICENSE.TXT"
    # LINKS batch files only name the target; the package manager rewrites them on install
    printf 'PROGS\\VANTOOL\\VANTOOL.EXE\r\n' > "$STAGE/LINKS/VANTOOL.BAT"
    printf 'PROGS\\VANTOOL\\VANPLAY.EXE\r\n' > "$STAGE/LINKS/VANPLAY.BAT"
    sed 's/$/\r/' > "$STAGE/APPINFO/VANTOOL.LSM" <<LSM
Begin3
Title:          vantool
Version:        ${VER#v}
Entered-date:   $(date -u +%Y-%m-%d)
Description:    Inspect, convert, repair and play VistaPro .VAN animations
Summary:        vantool extracts .VAN frames to BMP or PCX, builds .VAN files from images, cuts them and repairs them (including the VistaPro 3.20 continue fault). vanplay plays them in VGA or VESA modes, with an XMS frame cache.
Keywords:       vistapro, van, animation, graphics, video, vesa
Author:         Stuart Axon
Maintained-by:  Stuart Axon
Primary-site:   https://github.com/stuaxo/vantool
Platforms:      DOS, FreeDOS
Copying-policy: ${COPYING_POLICY:-Not yet specified}
End
LSM
    OUT="$PWD/dist/vantool-$VER-freedos.zip"
    rm -f "$OUT"
    (cd "$STAGE" && zip -q -9 -r -k -X "$OUT" APPINFO PROGS LINKS)
    ;;
linux)
    D="$STAGE/vantool-$VER-linux-x86_64"
    mkdir "$D"
    cp src/vantool src/vanplay README.md docs/VAN_SPEC.md "$D"
    [ -f LICENSE ] && cp LICENSE "$D"
    OUT="$PWD/dist/vantool-$VER-linux-x86_64.tar.gz"
    tar -C "$STAGE" -czf "$OUT" "$(basename "$D")"
    ;;
win32|win64)
    cp build/$KIND/vantool.exe build/$KIND/vanplay.exe build/$KIND/SDL2.dll "$STAGE"
    sed 's/$/\r/' README.md > "$STAGE/README.txt"
    sed 's/$/\r/' docs/VAN_SPEC.md > "$STAGE/VAN_SPEC.txt"
    [ -f LICENSE ] && sed 's/$/\r/' LICENSE > "$STAGE/LICENSE.txt"
    [ -n "$SDL2_DIR" ] && sed 's/$/\r/' "$SDL2_DIR/LICENSE.txt" > "$STAGE/SDL2-LICENSE.txt"
    OUT="$PWD/dist/vantool-$VER-$KIND.zip"
    rm -f "$OUT"
    (cd "$STAGE" && zip -q -X "$OUT" *)
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
