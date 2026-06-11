#!/bin/bash
# Build a self-contained Linux x86_64 AppImage for VibeDolphin.
#
# Usage:  Distribution/build-appimage.sh [SOURCE_DIR]
#   SOURCE_DIR  defaults to the repo root (parent of this script)
# Output:  VibeDolphin.AppImage in SOURCE_DIR (unversioned; the GitHub release tag carries the
#          version, and a stable filename makes in-place updates clean).
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
BUILD="$SRC/build-appimage"
APPDIR="$SRC/AppDir"
TOOLS="${APPIMAGE_TOOLS_DIR:-$HOME/.cache/vibedolphin-appimage-tools}"
OUT="$SRC/VibeDolphin.AppImage"

# --- AppImage tooling. Each tool is verified against a pinned sha256 (the exact bytes known
#     to build VibeDolphin) before it is executed, so a moved 'continuous' upstream build or a
#     tampered/corrupt download fails the build loudly instead of running unverified. To bump a
#     tool: download it, run `sha256sum`, and update the hash here. ---
LINUXDEPLOY_URL="https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage"
LINUXDEPLOY_SHA256="514d4ffe2a2f757369b41863a4f63fbbb222c429652803ebc081cb16ba21ac25"
LINUXDEPLOY_QT_URL="https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-x86_64.AppImage"
LINUXDEPLOY_QT_SHA256="be1b7e166bf9975cfb694ebe6759ba40502ffc6196440d3e64aa90c4dbd67e9f"
APPIMAGETOOL_URL="https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage"
APPIMAGETOOL_SHA256="a6d71e2b6cd66f8e8d16c37ad164658985e0cf5fcaa950c90a482890cb9d13e0"

mkdir -p "$TOOLS"
# $1=url $2=filename $3=expected-sha256. Download if absent, then ALWAYS verify (cached or
# fresh) before the tool is trusted.
dl() {
  [ -f "$TOOLS/$2" ] || wget -qO "$TOOLS/$2" "$1"
  if ! echo "$3  $TOOLS/$2" | sha256sum -c - >/dev/null 2>&1; then
    echo "ERROR: $2 failed sha256 verification (expected $3)." >&2
    echo "       Upstream 'continuous' likely changed; review the new build and update the pin." >&2
    rm -f "$TOOLS/$2"
    exit 1
  fi
}
dl "$LINUXDEPLOY_URL" linuxdeploy-x86_64.AppImage "$LINUXDEPLOY_SHA256"
dl "$LINUXDEPLOY_QT_URL" linuxdeploy-plugin-qt-x86_64.AppImage "$LINUXDEPLOY_QT_SHA256"
dl "$APPIMAGETOOL_URL" appimagetool-x86_64.AppImage "$APPIMAGETOOL_SHA256"
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

# --- bundle Qt (linuxdeploy's own appimage-output step is buggy on icons, so we run
#     appimagetool ourselves below; '|| true' tolerates that step erroring). We do NOT trust
#     the exit code -- instead we assert the Qt libs + platform plugin actually landed, so a
#     real bundling failure can't silently ship an AppImage that crashes on a clean machine. ---
PATH="$TOOLS:$PATH" APPIMAGE_EXTRACT_AND_RUN=1 QMAKE="$(command -v qmake6)" EXTRA_QT_PLUGINS=svg \
  "$TOOLS/linuxdeploy-x86_64.AppImage" --appdir "$APPDIR" --plugin qt || true
if ! ls "$APPDIR"/usr/lib/libQt6Core.so* >/dev/null 2>&1 ||
   [ ! -e "$APPDIR/usr/plugins/platforms/libqxcb.so" ]; then
  echo "ERROR: Qt was not bundled into the AppDir (libQt6Core / qxcb platform plugin missing)." >&2
  echo "       The AppImage would crash on launch; aborting." >&2
  exit 1
fi

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
# installed, so launching from Steam drops you right into the channel grid. Pass --gui (or set
# VIBEDOLPHIN_FORCE_GUI=1) to force the normal Dolphin interface; any explicit arguments also
# bypass kiosk mode. First run (no System Menu yet) -> the GUI. A one-line note goes to stderr
# (captured by Steam) so the chosen mode is diagnosable.
# 00000001/00000002 is the Wii System Menu title; require a non-empty, readable TMD so a
# half-installed/corrupt menu falls through to the GUI rather than a doomed kiosk boot.
SYSMENU_TMD="${DOLPHIN_EMU_USERPATH}/Wii/title/00000001/00000002/content/title.tmd"
EXTRA=()
if [ "${1:-}" = "--gui" ]; then
  shift
  echo "VibeDolphin: --gui -> starting the normal interface" >&2
elif [ -n "${VIBEDOLPHIN_FORCE_GUI:-}" ]; then
  echo "VibeDolphin: VIBEDOLPHIN_FORCE_GUI set -> starting the normal interface" >&2
elif [ "$#" -eq 0 ] && [ -s "$SYSMENU_TMD" ] && [ -r "$SYSMENU_TMD" ]; then
  echo "VibeDolphin: System Menu found -> booting the Wii Menu (kiosk). Use --gui for settings." >&2
  EXTRA=(--wii-menu)
else
  echo "VibeDolphin: no System Menu (or arguments given) -> starting the normal interface" >&2
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
