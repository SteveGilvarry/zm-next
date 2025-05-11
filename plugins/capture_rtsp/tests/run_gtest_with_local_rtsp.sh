#!/bin/zsh
# This script starts a local RTSP server using ffmpeg, runs the GTest, and cleans up.

set -e






# Start mediamtx with custom config (assumes mediamtx is installed and in PATH)
mediamtx /opt/homebrew/etc/mediamtx/mediamtx.yml > /tmp/mediamtx.log 2>&1 &
MTX_PID=$!

# Wait for mediamtx to start (try up to 10s)
for i in {1..20}; do
  nc -z localhost 8554 && break
  sleep 0.5
done

# Start ffmpeg to push a test pattern with regular keyframes to mediamtx
# Keyframe every 30 frames (1s at 30fps)
ffmpeg -re \
  -f lavfi -i testsrc2=size=1280x720:rate=30 \
  -f lavfi -i sine=frequency=1000:sample_rate=48000 \
  -c:v libx264 -preset ultrafast -tune zerolatency -g 30 -keyint_min 30 \
  -c:a aac -b:a 128k \
  -f rtsp rtsp://localhost:8554/mystream > /tmp/ffmpeg_rtsp_test.log 2>&1 &
FFMPEG_PID=$!

# Wait for ffmpeg to start streaming
sleep 2

# Run the test binary from the build directory if present
if [ -f "../../../build/plugins/capture_rtsp/tests/test_capture_rtsp" ]; then
  TEST_BIN="../../../build/plugins/capture_rtsp/tests/test_capture_rtsp"
elif [ -f "./test_capture_rtsp" ]; then
  TEST_BIN="./test_capture_rtsp"
else
  echo "test_capture_rtsp binary not found!"
  kill $FFMPEG_PID $MTX_PID
  exit 1
fi

$TEST_BIN

# Clean up
kill $FFMPEG_PID $MTX_PID
wait $FFMPEG_PID 2>/dev/null || true
wait $MTX_PID 2>/dev/null || true
