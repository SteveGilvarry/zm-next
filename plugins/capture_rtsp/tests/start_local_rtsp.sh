#!/bin/zsh
# Start a local RTSP server using ffmpeg and a sample video
# Usage: ./start_local_rtsp.sh [sample.mp4] [rtsp_port]

VIDEO_FILE=${1:-sample.mp4}
RTSP_PORT=${2:-8554}

if [ ! -f "$VIDEO_FILE" ]; then
  echo "Sample video $VIDEO_FILE not found. Downloading Big Buck Bunny sample..."
  curl -L -o "$VIDEO_FILE" "https://test-videos.co.uk/vids/bigbuckbunny/mp4/h264/720/Big_Buck_Bunny_720_1min.mp4"
fi

# Start ffmpeg RTSP server
ffmpeg -re -stream_loop -1 -i "$VIDEO_FILE" -c copy -f rtsp -rtsp_transport tcp "rtsp://localhost:$RTSP_PORT/test"
