
# Sample Video Files & RTSP Test Setup

This directory can contain sample video files for local RTSP plugin testing.

## Option 1: Use a Public RTSP Test Stream

The test will automatically use a public RTSP stream (Big Buck Bunny) if `RTSP_TEST_URL` is not set:

```
rtsp://wowzaec2demo.streamlock.net/vod/mp4:BigBuckBunny_115k.mov
```

## Option 2: Run a Local RTSP Server with ffmpeg

You can use the provided script to start a local RTSP server streaming a sample video:

```bash
cd plugins/capture_rtsp/tests
./start_local_rtsp.sh [sample.mp4] [rtsp_port]
```
This will download a sample video if needed and stream it to `rtsp://localhost:8554/test` by default.

Set the environment variable for the test:

```bash
export RTSP_TEST_URL=rtsp://localhost:8554/test
ctest -R RtspPluginTest
```

## Option 3: HLS (for reference)

```bash
# Create HLS segments
ffmpeg -re -stream_loop -1 -i sample.mp4 -c copy -f hls -hls_time 2 -hls_list_size 10 -hls_flags delete_segments -method PUT http://localhost:8080/stream/sample.m3u8
```

To run tests with a real camera, set the RTSP_TEST_URL environment variable:

```bash
export RTSP_TEST_URL=rtsp://username:password@camera-ip:port/stream
ctest -R RtspPluginTest
```

To run tests with a real camera, set the RTSP_TEST_URL environment variable:

```bash
export RTSP_TEST_URL=rtsp://username:password@camera-ip:port/stream
ctest -R RtspPluginTest
```
