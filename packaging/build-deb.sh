#!/usr/bin/env bash
set -euo pipefail

PACKAGE="terminal2"
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/build}"
DIST_DIR="${DIST_DIR:-$ROOT_DIR/dist}"
ARCH="${ARCH:-$(dpkg --print-architecture)}"
GIT_SHORT="$(git -C "$ROOT_DIR" rev-parse --short HEAD 2>/dev/null || true)"
VERSION="${VERSION:-0.1.0+git${GIT_SHORT:-local}-1}"
JOBS="${JOBS:-$(nproc)}"

DEB="$DIST_DIR/${PACKAGE}_${VERSION}_${ARCH}.deb"

mkdir -p "$BUILD_DIR" "$DIST_DIR"
PKGROOT="$(mktemp -d "$DIST_DIR/.${PACKAGE}_${VERSION}_${ARCH}.pkgroot.XXXXXX")"
chmod 755 "$PKGROOT"

cd "$BUILD_DIR"
qmake "$ROOT_DIR/terminal2.pro" CONFIG+=release
make -j"$JOBS"

cd "$ROOT_DIR"
install -Dm755 "$BUILD_DIR/terminal2" "$PKGROOT/usr/bin/terminal2"
strip --strip-unneeded "$PKGROOT/usr/bin/terminal2" 2>/dev/null || true
install -Dm644 "$ROOT_DIR/packaging/terminal2.desktop" \
    "$PKGROOT/usr/share/applications/terminal2.desktop"
install -Dm644 "$ROOT_DIR/images/terminal.png" \
    "$PKGROOT/usr/share/icons/hicolor/256x256/apps/terminal2.png"
install -Dm644 "$ROOT_DIR/packaging/copyright" \
    "$PKGROOT/usr/share/doc/$PACKAGE/copyright"

CHANGELOG="$PKGROOT/usr/share/doc/$PACKAGE/changelog.Debian"
cat > "$CHANGELOG" <<EOF
terminal2 ($VERSION) stable; urgency=medium

  * Local package build.

 -- Local build <root@localhost>  $(date -R)
EOF
gzip -9 "$CHANGELOG"
chmod 644 "$CHANGELOG.gz"

INSTALLED_SIZE="$(du -ks "$PKGROOT/usr" | awk '{print $1}')"
mkdir -p "$PKGROOT/DEBIAN"
cat > "$PKGROOT/DEBIAN/control" <<EOF
Package: $PACKAGE
Version: $VERSION
Section: misc
Priority: optional
Architecture: $ARCH
Maintainer: Local build <root@localhost>
Installed-Size: $INSTALLED_SIZE
Depends: libc6 (>= 2.35), libstdc++6, libgcc-s1, libqt5core5a, libqt5gui5, libqt5widgets5, libqt5serialport5, libqt5svg5, libqt5opengl5, libqt5printsupport5, libqt5concurrent5, libqwt-qt5-6
Description: Terminal2 Qt telemetry application
 Terminal2 is a Qt 5 desktop application for serial telemetry and antenna
 control.
EOF

dpkg-deb --build --root-owner-group "$PKGROOT" "$DEB"
echo "$DEB"
