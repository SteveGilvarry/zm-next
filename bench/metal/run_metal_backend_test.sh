#!/usr/bin/env bash
# Build + run the wired Metal HwBackend integration test (compiles the actual
# plugin sources hw_backend.cpp + hw_backend_metal.mm).
set -e
cd "$(dirname "$0")"
ORT=/opt/homebrew/opt/onnxruntime
SRC=../../plugins/detect_onnx
CORE=../../core/include
FFC="$(pkg-config --cflags libavformat libavcodec libavutil)"
FFL="$(pkg-config --libs libavformat libavcodec libavutil)"

# hw_backend.cpp is plain C++ (no ARC); the .mm TUs are ObjC++ with ARC.
clang++ -std=c++17 -O2 -DZM_WITH_METAL -I$SRC -I$CORE -I$ORT/include/onnxruntime $FFC \
  -c $SRC/hw_backend.cpp -o /tmp/hwb.o
clang++ -std=c++17 -fobjc-arc -O2 -DZM_WITH_METAL -I$SRC -I$CORE -I$ORT/include/onnxruntime $FFC \
  -c $SRC/hw_backend_metal.mm -o /tmp/hwb_metal.o
clang++ -std=c++17 -fobjc-arc -O2 -DZM_WITH_METAL -I$SRC -I$CORE -I$ORT/include/onnxruntime $FFC \
  -c metal_backend_test.mm -o /tmp/hwb_test.o

clang++ -std=c++17 /tmp/hwb_test.o /tmp/hwb_metal.o /tmp/hwb.o -o metal_backend_test \
  -framework Metal -framework CoreVideo -framework VideoToolbox -framework CoreMedia \
  -framework Foundation -framework QuartzCore \
  -L$ORT/lib -lonnxruntime $FFL

echo "built metal_backend_test"
./metal_backend_test "$@"
