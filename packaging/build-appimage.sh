#!/usr/bin/env bash
#
# Builds manifest-x86_64.AppImage from a release CMake build.
#
# Downloads linuxdeploy + linuxdeploy-plugin-qt on first run into
# packaging/.tools/, caches them there so repeat runs are fast. The
# script is idempotent — rerun after editing source and it produces
# a fresh AppImage.
#
# Output: <repo>/dist/manifest-x86_64.AppImage
#
set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$REPO/build-appimage"
APPDIR="$BUILD/AppDir"
DIST="$REPO/dist"
TOOLS="$REPO/packaging/.tools"
ICON_SRC="$REPO/resource/Copilot_20260415_212406.png"
DESKTOP_SRC="$REPO/packaging/manifest.desktop"

mkdir -p "$TOOLS" "$DIST"

# --- fetch linuxdeploy + Qt plugin (continuous builds) -------------------
fetch() {
    local name url dest
    name="$1"; url="$2"; dest="$TOOLS/$name"
    if [[ ! -x "$dest" ]]; then
        echo "Fetching $name..."
        curl -sSL -o "$dest" "$url"
        chmod +x "$dest"
    fi
}
fetch linuxdeploy-x86_64.AppImage \
    https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage
fetch linuxdeploy-plugin-qt-x86_64.AppImage \
    https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-x86_64.AppImage

# Running AppImages from inside another AppImage build often needs
# --appimage-extract-and-run on CI, but a local dev box with FUSE is fine.
LD="$TOOLS/linuxdeploy-x86_64.AppImage --appimage-extract-and-run"
LDQT="$TOOLS/linuxdeploy-plugin-qt-x86_64.AppImage --appimage-extract-and-run"

# --- build manifest (release) --------------------------------------------
echo "Configuring release build at $BUILD"
cmake -S "$REPO" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$BUILD" --target manifest -j >/dev/null

# --- assemble AppDir -----------------------------------------------------
rm -rf "$APPDIR"
mkdir -p \
    "$APPDIR/usr/bin" \
    "$APPDIR/usr/share/applications" \
    "$APPDIR/usr/share/metainfo" \
    "$APPDIR/usr/share/icons/hicolor/256x256/apps" \
    "$APPDIR/usr/share/manifest/data"

cp "$BUILD/manifest" "$APPDIR/usr/bin/manifest"

# Icon needs to be called `manifest.png` (matches Icon=manifest in .desktop).
# 256×256 is the AppImage-recommended resolution; source is 1024×1024
# so we resize down via ImageMagick if available, fall back to copying
# the source as-is (larger but functional).
if command -v convert >/dev/null; then
    convert "$ICON_SRC" -resize 256x256 \
        "$APPDIR/usr/share/icons/hicolor/256x256/apps/manifest.png"
else
    cp "$ICON_SRC" "$APPDIR/usr/share/icons/hicolor/256x256/apps/manifest.png"
fi
# AppImage also looks for a top-level icon alongside the .desktop.
cp "$APPDIR/usr/share/icons/hicolor/256x256/apps/manifest.png" "$APPDIR/manifest.png"

cp "$DESKTOP_SRC" "$APPDIR/usr/share/applications/manifest.desktop"

# Ship the seed data bundle so the AppImage can enrich out of the box —
# catalog + TOSEC titles JSON if present.
for d in menu_disk_contents.json tosec_titles.json; do
    if [[ -f "$REPO/data/$d" ]]; then
        cp "$REPO/data/$d" "$APPDIR/usr/share/manifest/data/$d"
    fi
done

# Ship user-facing docs so Help ▸ Instructions works inside the AppImage.
for f in INSTRUCTIONS.md README.md LICENSE; do
    if [[ -f "$REPO/$f" ]]; then
        cp "$REPO/$f" "$APPDIR/usr/share/manifest/$f"
    fi
done

# --- bundle Qt + libs via linuxdeploy -----------------------------------
cd "$BUILD"
QMAKE="$(command -v qmake6 || command -v qmake)"
export QMAKE
$LD --appdir "$APPDIR" --plugin qt \
    --desktop-file "$APPDIR/usr/share/applications/manifest.desktop" \
    --icon-file    "$APPDIR/manifest.png" \
    --output appimage

OUT="$BUILD/"*.AppImage
mv $OUT "$DIST/manifest-x86_64.AppImage"

echo ""
echo "=== Done ==="
ls -lh "$DIST/manifest-x86_64.AppImage"
echo ""
echo "Run with:  $DIST/manifest-x86_64.AppImage"
