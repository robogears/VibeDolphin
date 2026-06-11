#!/bin/bash
# Build a self-contained Linux x86_64 AppImage for VibeDolphin.
#
# Usage:  Distribution/build-appimage.sh [SOURCE_DIR] [VERSION]
#   SOURCE_DIR  defaults to the repo root (parent of this script)
#   VERSION     defaults to 0.1.4 (used only for the output filename)
#
# Run on Linux x86_64 with the Dolphin build dependencies installed, e.g. on Ubuntu:
#   sudo apt install build-essential cmake ninja-build pkg-config qt6-base-dev \
#     qt6-base-private-dev libqt6svg6-dev libevdev-dev libudev-dev libusb-1.0-0-dev \
#     libsystemd-dev libbluetooth-dev libasound2-dev libpulse-dev libgl1-mesa-dev \
#     libxi-dev libxrandr-dev libx11-dev libxext-dev libxxf86vm-dev libcurl4-openssl-dev \
#     libhidapi-dev libpng-dev liblzo2-dev libzstd-dev zlib1g-dev libavcodec-dev \
#     libavformat-dev libavutil-dev libswscale-dev libfuse2 desktop-file-utils gettext wget
set -euo pipefail

SRC="${1:-$(cd "$(dirname "$0")/.." && pwd)}"
VERSION="${2:-0.1.4}"
BUILD="$SRC/build-appimage"
APPDIR="$SRC/AppDir"
TOOLS="${APPIMAGE_TOOLS_DIR:-$HOME/.cache/vibedolphin-appimage-tools}"
OUT="$SRC/VibeDolphin.AppImage"

# --- fetch the AppImage tooling (cached) ---
mkdir -p "$TOOLS"
dl() { [ -f "$TOOLS/$2" ] || wget -qO "$TOOLS/$2" "$1"; }
dl https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage linuxdeploy-x86_64.AppImage
dl https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-x86_64.AppImage linuxdeploy-plugin-qt-x86_64.AppImage
dl https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage appimagetool-x86_64.AppImage
chmod +x "$TOOLS"/*.AppImage

# --- build (LINUX_LOCAL_DEV => Sys is resolved relative to the binary, required for AppImage) ---
cmake -S "$SRC" -B "$BUILD" -GNinja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr \
  -DENABLE_TESTS=OFF -DDISTRIBUTOR=VibeDolphin -DLINUX_LOCAL_DEV=true
ninja -C "$BUILD"

# --- install into the AppDir; put Sys next to the binary (exe-relative lookup) ---
rm -rf "$APPDIR"
DESTDIR="$APPDIR" cmake --install "$BUILD"
mv "$APPDIR/usr/share/dolphin-emu/sys" "$APPDIR/usr/bin/Sys"

# --- icon to AppDir root + strip any CRLF from the .desktop (Windows-checkout artifact) ---
cp -f "$APPDIR/usr/share/icons/hicolor/256x256/apps/dolphin-emu.png" "$APPDIR/dolphin-emu.png"
sed -i 's/\r$//' "$APPDIR/usr/share/applications/dolphin-emu.desktop"
# Brand the app name "VibeDolphin" (shown by Steam / desktop launchers and the AppImage name).
sed -i 's/^Name=.*/Name=VibeDolphin/' "$APPDIR/usr/share/applications/dolphin-emu.desktop"

# --- bundle Qt (linuxdeploy's own appimage-output step is buggy on icons, so we
#     run appimagetool ourselves below; '|| true' tolerates that step erroring) ---
PATH="$TOOLS:$PATH" APPIMAGE_EXTRACT_AND_RUN=1 QMAKE="$(command -v qmake6)" EXTRA_QT_PLUGINS=svg \
  "$TOOLS/linuxdeploy-x86_64.AppImage" --appdir "$APPDIR" --plugin qt || true

# --- AppRun: VibeDolphin keeps its OWN user dir (persists across updates, never shares
#     config/NAND with a stock Dolphin install). linuxdeploy leaves AppRun as a symlink to
#     the binary, so remove it first or the heredoc would overwrite the binary. ---
rm -f "$APPDIR/AppRun"
cat > "$APPDIR/AppRun" <<'EOF'
#!/bin/bash
HERE="$(dirname "$(readlink -f "${0}")")"
export LD_LIBRARY_PATH="${HERE}/usr/lib:${LD_LIBRARY_PATH}"
export QT_QPA_PLATFORM_PLUGIN_PATH="${HERE}/usr/plugins/platforms"
export QT_PLUGIN_PATH="${HERE}/usr/plugins"
export DOLPHIN_EMU_USERPATH="${DOLPHIN_EMU_USERPATH:-${XDG_DATA_HOME:-$HOME/.local/share}/vibedolphin}"
# VibeDolphin kiosk: with no arguments, boot straight into the Wii System Menu once one is
# installed, so launching from Steam drops you right into the channel grid. Pass --gui to
# force the normal Dolphin interface (first-run setup, settings, installing the System Menu);
# any explicit arguments also bypass kiosk mode. First run (no System Menu yet) -> the GUI.
EXTRA=()
if [ "${1:-}" = "--gui" ]; then
  shift
elif [ "$#" -eq 0 ] && \
     [ -f "${DOLPHIN_EMU_USERPATH}/Wii/title/00000001/00000002/content/title.tmd" ]; then
  EXTRA=(--wii-menu)
fi
exec "${HERE}/usr/bin/dolphin-emu" "${EXTRA[@]}" "$@"
EOF
chmod +x "$APPDIR/AppRun"
cp -f "$APPDIR/dolphin-emu.png" "$APPDIR/.DirIcon"
sed -i 's/\r$//' "$APPDIR/usr/share/applications/dolphin-emu.desktop"

# --- package ---
rm -f "$OUT"
APPIMAGE_EXTRACT_AND_RUN=1 ARCH=x86_64 "$TOOLS/appimagetool-x86_64.AppImage" "$APPDIR" "$OUT"
echo "Built: $OUT"
ls -la "$OUT"
