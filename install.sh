#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# cvtLTXAlphaGen install.sh
#
# From Creative Video Tips (CVT) - LTX Alpha Gen for DaVinci Resolve, a port
# of Flame_LTXAlphaGen_OFX (MPL 2.0, see src/LICENSE and src/CVT_PORT.md).
#
# What it does:
#   1. Copies build/LTXAlphaGen.ofx.bundle into /Library/OFX/Plugins, where
#      Resolve looks for OFX plugins (asks for your password: sudo).
#   2. Removes the quarantine flag so macOS lets Resolve load it.
#
#   ./install.sh              install (replaces an earlier copy)
#   ./install.sh --uninstall  remove it
#
# Quit Resolve first; it finds the plugin at its next start. Models, the LTX
# runtime and the matte cache are not touched (see README.md).
# ---------------------------------------------------------------------------
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
NAME=LTXAlphaGen.ofx.bundle
DEST=/Library/OFX/Plugins

if pgrep -x Resolve >/dev/null; then
  echo "Resolve is running: quit it first, then run this again. Nothing was changed." >&2
  exit 1
fi
if [ "$(id -u)" -ne 0 ]; then exec sudo "$0" "$@"; fi

if [ "${1:-}" = "--uninstall" ]; then
  rm -rf "${DEST:?}/$NAME"
  echo "Removed $DEST/$NAME."
  exit 0
fi

SRC="$HERE/build/$NAME"
[ -f "$SRC/Contents/MacOS/LTXAlphaGen.ofx" ] || { echo "No build found: run ./build.sh first." >&2; exit 1; }
mkdir -p "$DEST"
rm -rf "${DEST:?}/$NAME"
ditto "$SRC" "$DEST/$NAME"
xattr -dr com.apple.quarantine "$DEST/$NAME" 2>/dev/null || true
chmod -R go-w "$DEST/$NAME"
echo "Installed $DEST/$NAME. Start Resolve: Fusion page > Effects > OpenFX > Creative Video Tips > LTX Alpha Gen."
