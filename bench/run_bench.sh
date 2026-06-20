#!/usr/bin/env bash
# Run the decode/detect benchmark across the synthetic clips and (if present)
# the live RTSP camera. Credentials come from the untracked bench/camera.local.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${BUILD:-$ROOT/build}"
VCPKG_ROOT="${VCPKG_ROOT:-$HOME/vcpkg}"
CUDA_HOME="${CUDA_HOME:-/usr/local/cuda-12.9}"
MODEL="${MODEL:-$ROOT/bench/models/yolo26n.onnx}"
BIN="$BUILD/bench/bench_decode_detect"
PLUGINS="$BUILD/plugins"

export LD_LIBRARY_PATH="$VCPKG_ROOT/installed/x64-linux-dynamic/lib:$HOME/onnxruntime/lib:$CUDA_HOME/lib64:${LD_LIBRARY_PATH:-}"

if [[ ! -x "$BIN" ]]; then echo "benchmark not built: $BIN (configure with -DZM_BUILD_BENCH=ON)"; exit 1; fi

run() { echo; echo "########################## $1 ##########################"; "$BIN" --input "$2" --model "$MODEL" --plugins "$PLUGINS" "${@:3}"; }

for c in 1080p_h264 1080p_hevc 4k_h264 4k_hevc; do
  [[ -f "$ROOT/bench/clips/$c.mp4" ]] && run "clip: $c" "$ROOT/bench/clips/$c.mp4"
done

if [[ -f "$ROOT/bench/camera.local" ]]; then
  # shellcheck disable=SC1090
  source "$ROOT/bench/camera.local"
  [[ -n "${ZM_RTSP_URL:-}" ]] && run "live RTSP camera" "$ZM_RTSP_URL" --max-frames 200 --passes 2
fi
