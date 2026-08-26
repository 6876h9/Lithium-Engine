#!/usr/bin/env bash
#
# Assembles Lithium Engine into a single self-contained .AppImage.
#
#   ./packaging/build_appimage.sh [build-dir]
#
# Produces Lithium_Engine-x86_64.AppImage in the repository root.
set -euo pipefail

REPO="$(cd -P "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${1:-$REPO/build}"
APPDIR="$REPO/AppDir"
OUT="$REPO/Lithium_Engine-x86_64.AppImage"

[ -x "$BUILD_DIR/Lithium_Engine" ] || { echo "No engine binary in $BUILD_DIR - build first."; exit 1; }

echo ">> Assembling AppDir"
rm -rf "$APPDIR"
mkdir -p "$APPDIR/usr/bin" "$APPDIR/usr/lib/lithium" \
         "$APPDIR/usr/share/applications" \
         "$APPDIR/usr/share/icons/hicolor/scalable/apps"

cp "$BUILD_DIR/Lithium_Engine" "$APPDIR/usr/bin/"
[ -x "$BUILD_DIR/Lithium_Game" ] && cp "$BUILD_DIR/Lithium_Game" "$APPDIR/usr/bin/"

# Runtime content sits beside the binary because the engine anchors its working
# directory to the executable and loads everything with relative paths.
for d in EngineContent Content fonts; do
    [ -d "$BUILD_DIR/$d" ] && cp -r "$BUILD_DIR/$d" "$APPDIR/usr/bin/"
done

# Libraries the engine ships itself (Embree + its SYCL/TBB runtime). These are not
# available from distro repositories in the versions we link against, so they must
# travel with the build.
if [ -d "$BUILD_DIR/lib" ]; then
    cp -a "$BUILD_DIR"/lib/*.so* "$APPDIR/usr/lib/lithium/" 2>/dev/null || true
fi

cp "$REPO/packaging/AppRun" "$APPDIR/AppRun"
chmod +x "$APPDIR/AppRun"
cp "$REPO/packaging/lithium-engine.desktop" "$APPDIR/usr/share/applications/"
cp "$REPO/packaging/lithium-engine.desktop" "$APPDIR/lithium-engine.desktop"
if [ -f "$REPO/lithium-engine.svg" ]; then
    cp "$REPO/lithium-engine.svg" "$APPDIR/usr/share/icons/hicolor/scalable/apps/"
    cp "$REPO/lithium-engine.svg" "$APPDIR/lithium-engine.svg"
    ln -sf lithium-engine.svg "$APPDIR/.DirIcon"
fi

# The engine's RUNPATH is $ORIGIN/lib, so keep a lib/ next to the binary pointing at
# the bundled libraries. This makes the AppDir work when run directly too, not only
# once packed into an image.
ln -sfn ../lib/lithium "$APPDIR/usr/bin/lib"

echo ">> Bundling system dependencies"
# linuxdeploy walks the binary's dependencies and copies them in, while excluding the
# libraries that must come from the host: glibc, the GL/driver stack, X11 and Wayland
# client libs. Bundling those is the classic AppImage mistake - a bundled libGL cannot
# reach the host's kernel driver, and a bundled glibc newer than the host's will not load.
if command -v linuxdeploy >/dev/null 2>&1; then
    LD=linuxdeploy
elif [ -x "$REPO/linuxdeploy-x86_64.AppImage" ]; then
    LD="$REPO/linuxdeploy-x86_64.AppImage"
else
    LD=""
    echo "   linuxdeploy not found - skipping automatic dependency bundling."
    echo "   Get it: https://github.com/linuxdeploy/linuxdeploy/releases"
fi

if [ -n "$LD" ]; then
    export LDAI_UPDATE_INFORMATION=""
    # The bundled Embree libraries carry their own RUNPATH pointing at the original
    # build tree, so linuxdeploy cannot resolve them from inside the AppDir (it fails
    # on libsycl -> libur_loader). Give it an explicit search path for the originals.
    export LD_LIBRARY_PATH="$REPO/thirdparty/embree:$APPDIR/usr/lib/lithium${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
    "$LD" --appdir "$APPDIR" \
          --executable "$APPDIR/usr/bin/Lithium_Engine" \
          --desktop-file "$APPDIR/usr/share/applications/lithium-engine.desktop" \
          ${ICON_ARG:-} || echo "   linuxdeploy reported problems; continuing."
fi

echo ">> Packing AppImage"
if command -v appimagetool >/dev/null 2>&1; then
    ARCH=x86_64 appimagetool "$APPDIR" "$OUT"
elif [ -x "$REPO/appimagetool-x86_64.AppImage" ]; then
    ARCH=x86_64 "$REPO/appimagetool-x86_64.AppImage" "$APPDIR" "$OUT"
else
    echo "   appimagetool not found."
    echo "   Get it:  wget https://github.com/AppImage/AppImageKit/releases/download/continuous/appimagetool-x86_64.AppImage"
    echo "   The AppDir is ready at: $APPDIR"
    echo "   Then run: ARCH=x86_64 ./appimagetool-x86_64.AppImage AppDir $OUT"
    exit 0
fi

chmod +x "$OUT"
echo ">> Done: $OUT"
