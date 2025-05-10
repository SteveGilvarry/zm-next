# Sample Video Files

This directory should contain sample video files for testing the RTSP plugin.

During CI testing, you can use ffmpeg to create a test RTSP stream like this:

```bash
# Stream a sample video as an RTSP source
ffmpeg -re -stream_loop -1 -i sample.mp4 -c copy -f rtsp -rtsp_transport tcp rtsp://localhost:8554/test
```

Or with an HLS live stream:

```bash
# Create HLS segments
ffmpeg -re -stream_loop -1 -i sample.mp4 -c copy -f hls -hls_time 2 -hls_list_size 10 -hls_flags delete_segments -method PUT http://localhost:8080/stream/sample.m3u8
```

To run tests with a real camera, set the RTSP_TEST_URL environment variable:

```bash
export RTSP_TEST_URL=rtsp://username:password@camera-ip:port/stream
ctest -R RtspPluginTest
```
