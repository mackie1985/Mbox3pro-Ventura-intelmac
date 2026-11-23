#!/bin/sh
# SPDX-FileCopyrightText: 2026 Mackie
# SPDX-License-Identifier: LGPL-3.0-or-later
# Build an installer package (macOS only; needs the Xcode command line tools):
#   scripts/make_pkg.sh      ->  build/Mbox3pro-Ventura-intelmac-<version>.pkg
# The binaries are built for Intel (x86_64) whatever Mac this runs on. The package is not signed or notarised.
set -e
cd "$(dirname "$0")/.."
[ "$(uname)" = Darwin ] || { echo "run this on a Mac"; exit 1; }
VER=$(cat VERSION)
OUT=build/Mbox3pro-Ventura-intelmac-$VER.pkg

make clean
make CC="cc -arch x86_64" daemon driver

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
file build/mbox3d | sed 's/^/  /'
