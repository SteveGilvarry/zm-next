#!/usr/bin/env bash
# Start a local OpenAI-compatible mlx-vlm server for describe_vlm, serving a small
# Qwen vision model on Apple silicon (ANE/GPU via MLX).
#
# Default model: Qwen3.5-2B (natively multimodal). NOTE: the mlx-community Qwen3.5
# builds bundle MTP (multi-token-prediction) speculative-decode weights in a
# separate mtp.safetensors, which mlx-vlm 0.6.3 rejects on load ("N parameters not
# in model"). We work around it by serving a local copy of the cached snapshot with
# mtp.safetensors excluded — the main model + vision tower load fine. Swap MODEL_ID
# back to the plain repo once mlx-vlm handles Qwen3.5's MTP head.
#
# Usage: run_mlx_vlm_server.sh [port=8080] [model_repo=mlx-community/Qwen3.5-2B-OptiQ-4bit]
set -euo pipefail
cd "$(dirname "$0")/.."          # bench/
PORT="${1:-8080}"
MODEL_ID="${2:-mlx-community/Qwen3.5-2B-OptiQ-4bit}"
VENV="$PWD/.venv-mlx"

[ -d "$VENV" ] || { echo "creating venv + installing mlx-vlm..."; python3.13 -m venv "$VENV"; "$VENV/bin/pip" -q install mlx-vlm; }
. "$VENV/bin/activate"

# Build a local model dir with mtp.safetensors stripped (Qwen3.5 MTP workaround).
SERVE="$MODEL_ID"
case "$MODEL_ID" in
  *Qwen3.5*)
    CACHE="$HOME/.cache/huggingface/hub/models--${MODEL_ID//\//--}/snapshots"
    SRC="$(ls -d "$CACHE"/*/ 2>/dev/null | head -1 || true)"
    if [ -z "$SRC" ]; then
        echo "model not cached; downloading $MODEL_ID ..."
        "$VENV/bin/huggingface-cli" download "$MODEL_ID" >/dev/null
        SRC="$(ls -d "$CACHE"/*/ | head -1)"
    fi
    DST="$PWD/models/$(basename "$MODEL_ID")-nomtp"
    rm -rf "$DST"; mkdir -p "$DST"
    for f in "$SRC"*; do b="$(basename "$f")"; [ "$b" = "mtp.safetensors" ] && continue; ln -sf "$f" "$DST/$b"; done
    SERVE="$DST"
    echo "serving local (mtp-stripped): $SERVE"
    ;;
esac

echo "mlx-vlm server: model=$SERVE  port=$PORT"
exec env HF_HUB_OFFLINE=1 python -m mlx_vlm.server \
    --model "$SERVE" --port "$PORT" --trust-remote-code --max-tokens 128
