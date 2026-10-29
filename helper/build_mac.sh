#!/bin/bash
# Builds "Plane Radar Deck.app" and a .dmg on a Mac: arm64 on an Apple M runner, x86_64 on
# an Intel one (CI: macos-14 / macos-15-intel). Usage: build_mac.sh [dmg name]
# The now-playing reader (MediaRemote Adapter, BSD 3-Clause) is built from a fixed commit.
set -euo pipefail
cd "$(dirname "$0")"
MRA_COMMIT=0e82bf1c44ec39554fbfc402ec88a48c6ae2a0fd
APP="dist/Plane Radar Deck.app"
DMG="dist/${1:-PlaneRadarDeck-mac.dmg}"
# Older macOS versions too (Intel Macs often stay on 11 - 13).
export MACOSX_DEPLOYMENT_TARGET=11.0

rm -rf build/mra-src mra
git clone -q https://github.com/ungive/mediaremote-adapter build/mra-src
git -C build/mra-src checkout -q "$MRA_COMMIT"
cmake -S build/mra-src -B build/mra-src/build -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0 >/dev/null
cmake --build build/mra-src/build --config Release -j 4 >/dev/null
mkdir -p mra
cp build/mra-src/bin/mediaremote-adapter.pl mra/
ditto build/mra-src/build/MediaRemoteAdapter.framework mra/MediaRemoteAdapter.framework
cp build/mra-src/LICENSE mra/LICENSE-mediaremote-adapter.txt

python3 -c "import app; img = app.tray_image().resize((512, 512)); img.save('icon.icns')" || true
ICON=(); [ -f icon.icns ] && ICON=(--icon icon.icns)
cp ../web/tz.json ../web/tz-LICENSE.txt web/  # time zones of the setup page (Radar page)
pyinstaller --noconfirm --windowed --name "Plane Radar Deck" "${ICON[@]}" \
  --osx-bundle-identifier com.planeradar.deck \
  --add-data "web:web" --add-data "browser-extension:browser-extension" --collect-all webview \
  --collect-submodules websockets \
  --hidden-import AppKit --hidden-import Foundation --hidden-import Quartz \
  --hidden-import ApplicationServices --hidden-import WebKit --hidden-import PyObjCTools.AppHelper \
  app.py

# The reader goes in as is (ditto keeps the framework's links), then the bundle is signed
# again (ad hoc) as a whole.
ditto mra "$APP/Contents/Resources/mra"
PL="$APP/Contents/Info.plist"
plutil -replace LSUIElement -bool true "$PL"
plutil -replace CFBundleDisplayName -string "Plane Radar Deck" "$PL"
plutil -replace NSHumanReadableCopyright -string "Plane Radar 3D" "$PL"
VER=$(python3 -c "import re;print(re.search(r'VERSION = \"([^\"]+)\"', open('app.py').read()).group(1))")
plutil -replace CFBundleShortVersionString -string "$VER" "$PL"
plutil -replace CFBundleVersion -string "$VER" "$PL"
codesign --force --deep --sign - "$APP"
codesign --verify --deep "$APP"

rm -rf build/dmg && mkdir -p build/dmg
ditto "$APP" "build/dmg/Plane Radar Deck.app"
ln -s /Applications build/dmg/Applications
hdiutil create -quiet -volname "Plane Radar Deck" -srcfolder build/dmg -ov -format UDZO "$DMG"
ls -la dist
