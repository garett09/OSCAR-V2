#!/bin/bash
# OSCAR Build Script
# Default: incremental build. Use --clean for full rebuild.
set -e

SOURCE_DIR="$HOME/oscar-code"
BUILD_DIR="$SOURCE_DIR/build-qt6"
OSCAR_BUILD_DIR="$BUILD_DIR/oscar"

CLEAN=true

echo "=== OSCAR Build ($([ "$CLEAN" = true ] && echo "clean" || echo "incremental")) ==="

killall OSCAR20 2>/dev/null || true
killall OSCAR 2>/dev/null || true

if [ "$CLEAN" = true ]; then
    echo "Cleaning build directory..."
    rm -rf "$BUILD_DIR"
    mkdir -p "$BUILD_DIR"
    echo "Configuring with qmake6..."
    cd "$BUILD_DIR"
    qmake6 "$SOURCE_DIR/OSCAR_QT.pro"
fi

CORES=$(sysctl -n hw.logicalcpu)
echo "Building with $CORES cores..."
if [ -d "$OSCAR_BUILD_DIR" ]; then
    make -C "$OSCAR_BUILD_DIR" -j"$CORES"
else
    make -C "$BUILD_DIR" -j"$CORES"
fi

if [ -d "$OSCAR_BUILD_DIR/OSCAR20.app" ]; then
    APP_PATH="$OSCAR_BUILD_DIR/OSCAR20.app"
elif [ -d "$OSCAR_BUILD_DIR/OSCAR.app" ]; then
    APP_PATH="$OSCAR_BUILD_DIR/OSCAR.app"
else
    echo "Error: app bundle not found in $OSCAR_BUILD_DIR"
    exit 1
fi

echo "Done. Launching built app directly (bypasses LaunchServices)..."
# Launch the inner Mach-O directly so macOS can't redirect to another
# OSCAR20.app sharing the same bundle id (org.oscar-team.OSCAR20).
APP_EXEC="$APP_PATH/Contents/MacOS/$(basename "$APP_PATH" .app)"
"$APP_EXEC" >/dev/null 2>&1 &
