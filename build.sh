#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# cvtLTXAlphaGen build.sh
#
# From Creative Video Tips (CVT) - LTX Alpha Gen for DaVinci Resolve and
# Fusion Studio, a port of Flame_LTXAlphaGen_OFX (MPL 2.0, see src/LICENSE
# and src/CVT_PORT.md).
#
# What it does:
#   1. Makes a small test Python with numpy in ~/.cvt/ltx-alphagen-testpy
#      (only once), for the unit tests and the dummy worker.
#   2. Builds the OFX bundle into build/LTXAlphaGen.ofx.bundle.
#   3. Runs the unit tests; with --systest also the fast system test, on a
#      separate test build in build-dev/.
#
#   ./build.sh [--dev] [--systest]
#   --dev      show the Prototype section (Use Dummy test mode) in the bundle
#
# cvt1.05: the normal build hides the Prototype section (it used to be shown
# unless --release was given).
#
# Nothing is installed: run ./install.sh afterwards.
# ---------------------------------------------------------------------------
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
BUILD="$HERE/build"
DEVBUILD="$HERE/build-dev"
TESTPY="$HOME/.cvt/ltx-alphagen-testpy"
PORT_VERSION="cvt1.50"   # the version: Version label, and (as 1.12) the plugin version hosts store
DEV=OFF
SYSTEST=0
for a in "$@"; do
  case $a in
    --dev) DEV=ON ;;
    --systest) SYSTEST=1 ;;
    *) echo "unknown option $a" >&2; exit 2 ;;
  esac
done

command -v cmake >/dev/null || { echo "cmake is missing: brew install cmake" >&2; exit 1; }

if [ ! -x "$TESTPY/bin/python" ]; then
  echo "== making the test Python ($TESTPY)"
  mkdir -p "$HOME/.cvt"
  /usr/bin/python3 -m venv "$TESTPY"
  "$TESTPY/bin/python" -m pip install -q numpy
fi

build() {  # build <folder> <ON|OFF>
  cmake -S "$HERE/src" -B "$1" -DCMAKE_BUILD_TYPE=Release -DLTXAG_DEV_BUILD=$2 \
    -DLTXAG_GIT="$PORT_VERSION" -DLTXAG_VERSION="${PORT_VERSION#cvt}" -DWORKER_TEST_PYTHON="$TESTPY/bin/python" >/dev/null
  cmake --build "$1" -j "$(sysctl -n hw.ncpu)" 2>&1 | grep -E "error|warning:" || true
  [ -f "$1/LTXAlphaGen.ofx.bundle/Contents/MacOS/LTXAlphaGen.ofx" ] || { echo "build FAILED" >&2; exit 1; }
}

echo "== building (Prototype section: $DEV)"
build "$BUILD" $DEV
echo "== unit tests"
(cd "$BUILD" && ctest --output-on-failure | tail -4)

if [ $SYSTEST = 1 ]; then
  if [ $DEV = ON ]; then SYS="$BUILD"; else SYS="$DEVBUILD"; echo "== test build"; build "$SYS" ON; fi
  echo "== fast system test (about 90 s)"
  rm -rf "$SYS/systest_work"
  "$HERE/src/systest/run_systest.sh" --tier fast --bundle "$SYS/LTXAlphaGen.ofx.bundle" \
    --host "$SYS/ltxag_systest" --work "$SYS/systest_work" --job-python "$TESTPY/bin/python" \
    | grep -E "PASS|FAIL|tests,"
fi
echo "Built $BUILD/LTXAlphaGen.ofx.bundle ($PORT_VERSION). Next: ./install.sh"
