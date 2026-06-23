# End-to-End Proof (file → cascade → worker socket + recording)

This is a runnable, camera-free proof that the whole worker actually works: capture,
per-stage threading, decode, motion detection, recording, and the canonical worker
socket — all in one process, verifiable on a laptop with no RTSP camera.

## What it exercises

```
capture_file (MP4, AVCC H.264)
 ├─ decode_ffmpeg (auto codec + extradata → RGB24)
 │   └─ motion_gate ──> motion DETECTION events
 │       └─ detect_onnx (pass-through; no model needed)
 └─ store (mode=continuous) ──> /tmp/zm_e2e_rec/.../<HH-MM-SS>.mkv
WorkerLink (--socket) ──> Hello + Media + Event + Stats to any connecting client
```

Every non-input stage runs on its own `StageRunner` thread with a bounded
drop-oldest queue (`queue_depth` per node), so the analysis branch can never stall
recording or the socket.

## Run it

```bash
# 1. make a camera-like clip (no B-frames, like an IP camera's zerolatency stream)
ffmpeg -y -f lavfi -i testsrc=duration=5:size=1280x720:rate=15 \
       -c:v libx264 -bf 0 -tune zerolatency -g 15 -pix_fmt yuv420p /tmp/zm_e2e.mp4

# 2. run the worker from build/ (plugin paths resolve relative to CWD)
cd build
./zm-core --pipeline ../pipelines/e2e_file_cascade.template.json \
          --socket /tmp/zm_e2e.sock --monitor-id 1 &

# 3. observe the worker socket (Hello/Media/Event/Stats) with the probe tool
./wl_dump /tmp/zm_e2e.sock 7

# 4. confirm a valid recording was written
ffprobe -count_packets -show_entries stream=codec_name,width,height,nb_read_packets \
        /tmp/zm_e2e_rec/*/Monitor-1/*.mkv
```

`wl_dump` (built from `tools/wl_dump.cpp`) is a ~100-line reference consumer: it
connects and prints each framed message (a canonical consumer receives Hello +
Media + Event + Stats by default). It is the smallest possible stand-in for the
zm-api consumer.

## Expected result (clean run)

- `wl_dump`: `hello=1`, `media≈75 (~160 KB)`, `events=4` (motion, with monotonic
  `pts_us` = 1s/2s/3s/4s), `stats=1`, `dropped=0`. Hello arrives even when the
  client connects seconds after startup.
- recording: a valid Matroska, `ffprobe` reports `h264 1280x720`, ~76 packets,
  ~5 s, **0 write errors**.
- clean shutdown on `SIGTERM` (no orphaned ShmRing segment).

## Bugs this proof surfaced and fixed

1. **Decode of file (AVCC) sources** — `decode_ffmpeg` never set the codec
   `extradata`, so MP4/MOV H.264/HEVC (length-prefixed NALs, out-of-band SPS/PPS)
   failed with "no start code found". Fix: carry `extradata` in the StreamMetadata
   handshake and apply it before `avcodec_open2`. RTSP (Annex-B, in-band SPS/PPS)
   is unaffected. Packets are now forwarded in their **native** container form — no
   in-pipeline bitstream conversion — so the decoder and the muxer both see one
   consistent stream.
2. **the recorder skipped all frames** — it treated the compressed bitstream
   (`hw_type=ZM_FRAME_COMPRESSED`) as a GPU surface and dropped it. Fix: accept
   `ZM_FRAME_COMPRESSED`, reject GPU surfaces and raw pixel buffers.
3. **the recorder never got video codecpar** — the core delivers input-plugin
   metadata on the EventBus, but store consumed only *audio* StreamMetadata, so the
   muxer header was never written (0-byte files). Fix: consume video StreamMetadata
   from the bus and build the muxer codecpar from it.
4. **Decoded frames carried no timestamp** — `decode_ffmpeg` didn't set `pkt->pts`,
   so every decoded frame (and thus every motion/detection event) reported
   `AV_NOPTS_VALUE`. Fix: propagate `pts_us` into the packet.
5. **Hello not delivered to late subscribers** — `WorkerLink` sent `Hello` once at
   startup; a client connecting later couldn't initialize a decoder. Fix: cache the
   latest `Hello` per stream kind and replay it on `Subscribe`.

## Known limitations

- **B-frames**: `zm_frame_hdr_t` carries only `pts_usec`, no `dts`. With B-frame
  reordering, store sets `pts=dts` and the muxer rejects the non-monotonic DTS.
  Surveillance cameras stream zero-latency (no B-frames), so this is unhit in
  practice; supporting B-frame sources would need a `dts` field in the frame header
  (an ABI addition).
