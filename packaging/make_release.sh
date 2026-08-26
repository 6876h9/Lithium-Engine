#!/usr/bin/env bash
#
# Builds the distributable release archive:  build/Lithium_Engine_v<VERSION>.zip
#
#   ./packaging/make_release.sh [version] [build-dir]
#
# Contents: portable engine folder, the smart launcher, the AppImage, and docs.
set -euo pipefail

REPO="$(cd -P "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VERSION="${1:-1.0.0}"
BUILD_DIR="${2:-$REPO/build}"
STAGE="$BUILD_DIR/release_stage/Lithium_Engine_v$VERSION"
OUT="$BUILD_DIR/Lithium_Engine_v$VERSION.zip"

[ -x "$BUILD_DIR/Lithium_Engine" ] || { echo "No engine binary in $BUILD_DIR - build first."; exit 1; }

echo ">> Staging $STAGE"
rm -rf "$BUILD_DIR/release_stage"
mkdir -p "$STAGE/Engine" "$STAGE/Documentation"

# --- Portable engine folder -------------------------------------------------
cp "$BUILD_DIR/Lithium_Engine" "$STAGE/Engine/"
[ -x "$BUILD_DIR/Lithium_Game" ] && cp "$BUILD_DIR/Lithium_Game" "$STAGE/Engine/"
cp "$REPO/launch_engine.sh" "$STAGE/Engine/"
chmod +x "$STAGE/Engine/launch_engine.sh" "$STAGE/Engine/Lithium_Engine"

cp -r "$BUILD_DIR/lib" "$STAGE/Engine/"
for d in EngineContent Content fonts; do
    [ -d "$BUILD_DIR/$d" ] && cp -r "$BUILD_DIR/$d" "$STAGE/Engine/"
done

# Build artefacts and per-user state must not ship.
rm -f "$STAGE/Engine/engine_config.json" \
      "$STAGE/Engine/LithiumEngine_Startup.log" \
      "$STAGE/Engine/imgui.ini"
rm -rf "$STAGE/Engine/Content/Bakes"

# --- AppImage ---------------------------------------------------------------
if [ -f "$REPO/Lithium_Engine-x86_64.AppImage" ]; then
    cp "$REPO/Lithium_Engine-x86_64.AppImage" "$STAGE/"
    chmod +x "$STAGE/Lithium_Engine-x86_64.AppImage"
else
    echo "   WARNING: no AppImage found - run packaging/build_appimage.sh first."
fi

# --- Documentation ----------------------------------------------------------
for f in README_Launcher_Instructions Engine_Limitations Language_Syntax_Manual \
         System_Requirements Troubleshooting_Error_Manual; do
    cp "$REPO/docs/$f.md" "$STAGE/Documentation/"
done

cat > "$STAGE/START_HERE.txt" <<'TXT'
Lithium Engine
==============

Quick start
-----------
  cd Engine
  chmod +x launch_engine.sh Lithium_Engine
  ./launch_engine.sh

Or run the single-file build with no extraction:
  chmod +x Lithium_Engine-x86_64.AppImage
  ./Lithium_Engine-x86_64.AppImage

If you get "Permission denied", your extraction tool dropped the executable
bit - the chmod above restores it.

Documentation/
  README_Launcher_Instructions.md ... running the engine, permissions, AppImage
  System_Requirements.md ............ supported hardware, required libraries
  Language_Syntax_Manual.md ......... C-Minus and V-Script scripting
  Engine_Limitations.md ............. what works, what does not, known bugs
  Troubleshooting_Error_Manual.md ... missing libraries, GPU timeouts, paths

Read Engine_Limitations.md before starting a project. This is a hobby engine
and that file states plainly which features are real and which are stubs.
TXT

# --- Pack -------------------------------------------------------------------
echo ">> Packing $OUT"
rm -f "$OUT"
( cd "$BUILD_DIR/release_stage" && zip -q -r -y "$OUT" "Lithium_Engine_v$VERSION" )
rm -rf "$BUILD_DIR/release_stage"

echo ">> Done: $OUT  ($(du -h "$OUT" | cut -f1))"
