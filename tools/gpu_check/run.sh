#!/bin/bash
# Draws every scene in tests/scenes.cpp in software and on the GPU (a hidden
# window's OpenGL context) and prints how far apart they are.
#
#   tools/gpu_check/run.sh [output-dir]
#
# Writes <scene>.ppm side by side (software | GPU | difference x8) into the
# output directory. Needs clang++, SDL2 (pkg-config) and OpenGL 3.3.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${1:-$ROOT/build-gpu-check}
mkdir -p "$OUT"
SRC="$ROOT/canvas.cpp $ROOT/font.cpp $ROOT/svg.cpp $ROOT/window.cpp $ROOT/touch_gesture.cpp"
clang++ -std=c++17 -O2 $(pkg-config --cflags sdl2) -I "$ROOT" "$ROOT/tools/gpu_check/gpu_check.cpp" "$ROOT/tests/scenes.cpp" $SRC \
  -o "$OUT/gpu_check" $(pkg-config --libs sdl2) -lpthread $( [ "$(uname)" = Darwin ] && echo -framework CoreText -framework CoreFoundation -framework CoreGraphics )
"$OUT/gpu_check" "$OUT"
