#!/bin/zsh
# This script starts a local RTSP server using ffmpeg, runs the GTest, and cleans up.

set -e

VIDEO_FILE="sample.mp4"
RTSP_PORT=8554
RTSP_URL="rtsp://localhost:$RTSP_PORT/test"
FFMPEG_LOG="/tmp/ffmpeg_rtsp_test.log"

# Download sample video if not present
if [ ! -f "$VIDEO_FILE" ]; then
  echo "Sample video $VIDEO_FILE not found. Downloading Big Buck Bunny sample..."
  curl -L -o "$VIDEO_FILE" "https://test-videos.co.uk/vids/bigbuckbunny/mp4/h264/720/Big_Buck_Bunny_720_1min.mp4"
fi

# Start ffmpeg RTSP server in background
ffmpeg -re -stream_loop -1 -i "$VIDEO_FILE" -c copy -f rtsp -rtsp_transport tcp "$RTSP_URL" > "$FFMPEG_LOG" 2>&1 &
FFMPEG_PID=$!

# Wait for ffmpeg to start
sleep 2

# Set env var and run tests
export RTSP_TEST_URL="$RTSP_URL"
ctest -R RtspPluginTest --output-on-failure

# Kill ffmpeg after tests
kill $FFMPEG_PID
wait $FFMPEG_PID 2>/dev/null || true
