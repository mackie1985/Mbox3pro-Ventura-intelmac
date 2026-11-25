#!/bin/sh
# SPDX-FileCopyrightText: 2026 Mackie
# SPDX-License-Identifier: LGPL-3.0-or-later
# Build an installer package (macOS only; needs the Xcode command line tools):
#   scripts/make_pkg.sh      ->  build/Mbox3pro-Ventura-intelmac-<version>.pkg
# The binaries are universal (x86_64 + arm64), built one architecture at a time and joined with lipo, whatever Mac this runs on.
# Built to start on macOS 12 and later (MINVER). The package is not signed or notarised. The arm64 part is a beta: it has not been run on an Apple Silicon Mac yet.
set -e
cd "$(dirname "$0")/.."
[ "$(uname)" = Darwin ] || { echo "run this on a Mac"; exit 1; }
VER=$(cat VERSION)
OUT=build/Mbox3pro-Ventura-intelmac-$VER.pkg

MINVER=12.0      # the oldest macOS the binaries are built to start on (arm64 needs 11.0 or later anyway)
TMPB=$(mktemp -d)
trap 'rm -rf "$TMPB"' EXIT
for A in x86_64 arm64; do
    make clean
    make CC="cc -arch $A -mmacosx-version-min=$MINVER" daemon driver
    mkdir -p "$TMPB/$A"
    cp build/mbox3d "$TMPB/$A/mbox3d"
    cp build/Mbox3.driver/Contents/MacOS/Mbox3Driver "$TMPB/$A/Mbox3Driver"
done
make clean
make CC="cc -arch x86_64 -mmacosx-version-min=$MINVER" daemon driver          # the bundle layout (Info.plist ...) from the Intel build
lipo -create "$TMPB/x86_64/mbox3d" "$TMPB/arm64/mbox3d" -output build/mbox3d
lipo -create "$TMPB/x86_64/Mbox3Driver" "$TMPB/arm64/Mbox3Driver" -output build/Mbox3.driver/Contents/MacOS/Mbox3Driver
codesign --force --sign - build/Mbox3.driver     # arm64 code must be signed (ad hoc is enough)
codesign --force --sign - build/mbox3d

STAGE=build/pkgroot
RES=build/pkgres
rm -rf "$STAGE" "$RES"
mkdir -p "$STAGE/usr/local/libexec" "$STAGE/usr/local/share/mbox3pro/ui" "$STAGE/Library/LaunchDaemons" \
         "$STAGE/Library/Audio/Plug-Ins/HAL" "$RES"
install -m 755 build/mbox3d "$STAGE/usr/local/libexec/mbox3d"
install -m 644 ui/index.html "$STAGE/usr/local/share/mbox3pro/ui/index.html"
install -m 755 scripts/uninstall.sh "$STAGE/usr/local/share/mbox3pro/uninstall.sh"
install -m 644 scripts/com.mbox3pro.driver.daemon.plist "$STAGE/Library/LaunchDaemons/com.mbox3pro.driver.daemon.plist"
ditto build/Mbox3.driver "$STAGE/Library/Audio/Plug-Ins/HAL/Mbox3.driver"
xattr -cr "$STAGE" 2>/dev/null || true
find "$STAGE" -name '*.dSYM' -prune -exec rm -rf {} +      # debug symbols from -g: not needed on the user's Mac

cp LICENSE "$RES/license.txt"
cp scripts/pkg-gui/readme.txt "$RES/readme.txt"
sed "s/@VERSION@/$VER/" scripts/pkg-gui/distribution.xml > build/distribution.xml
chmod 755 scripts/pkg/preinstall scripts/pkg/postinstall

# the plug-in bundle must go to /Library/Audio/Plug-Ins/HAL and nowhere else: without this the installer may "relocate" it onto
# another copy of the same bundle it finds on the disk (for example the build/ folder of the sources)
pkgbuild --analyze --root "$STAGE" build/components.plist >/dev/null
i=0
while /usr/libexec/PlistBuddy -c "Set :$i:BundleIsRelocatable false" build/components.plist 2>/dev/null; do i=$((i + 1)); done
pkgbuild --root "$STAGE" --component-plist build/components.plist --identifier com.mbox3pro.driver --version "$VER" \
         --ownership recommended --scripts scripts/pkg --install-location / build/component.pkg
productbuild --distribution build/distribution.xml --resources "$RES" --package-path build "$OUT"

echo
echo "built $OUT"
pkgutil --payload-files build/component.pkg | grep -v "/Contents/" | head -20
file build/mbox3d build/Mbox3.driver/Contents/MacOS/Mbox3Driver | sed 's/^/  /'
