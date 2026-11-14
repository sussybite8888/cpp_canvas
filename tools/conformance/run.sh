#!/bin/bash
# Draws every scene in tests/scenes.cpp with the native backend and, built
# with emscripten, with headless Chrome's own canvas; then compares the two
# and checks that culling on and off give identical pixels on both backends.
#
#   tools/conformance/run.sh [output-dir]
#
# Needs: clang++, SDL2 (pkg-config), em++, Google Chrome, python3.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${1:-$ROOT/build-conformance}
CHROME=${CHROME:-"/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"}
mkdir -p "$OUT/native" "$OUT/web" "$OUT/png"
SRC="$ROOT/canvas.cpp $ROOT/font.cpp $ROOT/svg.cpp"
TESTS="$ROOT/tools/conformance/harness.cpp $ROOT/tests/scenes.cpp"
clang++ -std=c++17 -O2 $(pkg-config --cflags sdl2) -I "$ROOT" $TESTS $SRC -o "$OUT/harness" $(pkg-config --libs sdl2) -lpthread
"$OUT/harness" "$OUT/native"
em++ -std=c++17 -O2 -I "$ROOT" $TESTS $SRC -sALLOW_MEMORY_GROWTH=1 -o "$OUT/web/harness.js"
echo '<!doctype html><html><body><pre id="out"></pre><script src="harness.js"></script></body></html>' > "$OUT/web/index.html"
"$CHROME" --headless=new --no-first-run --disable-gpu-sandbox --allow-file-access-from-files \
  --user-data-dir="$OUT/chrome-profile" --virtual-time-budget=20000 --dump-dom "file://$OUT/web/index.html" 2>/dev/null \
  | sed -n '/<pre id="out">/,/<\/pre>/p' | sed -e 's/.*<pre id="out">//' -e 's/<\/pre>.*//' > "$OUT/web/dump.txt"
python3 "$ROOT/tools/conformance/compare.py" "$OUT"
echo "side-by-side images (native | Chrome): $OUT/png"
