# Plugin Configuration Reference

Every plugin is configured via the JSON object on its pipeline node (`"config"`,
or `"cfg"`), passed to the plugin's `start()` as a JSON string. All keys are
optional with the defaults shown. Common keys:
- `queue_depth` (node-level, any stage): bounded input-queue depth for the stage's
  own thread; drop-oldest when full (default 16). Use a small value (2–4) for
  low-latency detectors and a large value (e.g. 120) for recorders that shouldn't
  drop. Each non-input plugin runs on its own thread, so a slow stage drops its
  own backlog instead of stalling capture, recording, or sibling branches.
- `stream_filter`: array of stream ids; empty/absent = all streams.
- `frame_width` / `frame_height`: required by plugins that read decoded pixels
  (the frame header has no dimensions), set to the decoder's output size.
- `ep`: ONNX execution provider — `"cpu"` (default) or `"coreml"` (CUDA via the
  `-DZM_WITH_CUDA` build).

## Inputs
- **capture_rtsp_multi** — `streams` (or single `url`), `transport` ("tcp"),
  `hw_decode` (false), `forward_audio` (true), `max_retry_attempts` (5),
  `retry_delay_ms` (2000).
- **capture_file** — `path` (required), `stream_id` (0), `loop` (true),
  `realtime` (true).

## Decode / Encode (codec + hardware configurable)
- **decode_ffmpeg** — input codec is **auto-detected** from the capture plugin's
  StreamMetadata (so H264/HEVC/etc. cameras just work). `codec` is an OPTIONAL
  override ("h264" | "hevc"/"h265" | "mjpeg" | "av1" | "vp8"/"vp9", or any FFmpeg
  decoder name; only set it to force a codec). `output_format`
  ("yuv420p" | "rgb24" | "gray"), `scale` ("orig" | "720p" | "WxH"), `threads`
  (0), `hwaccel` ("none" | "auto" | "cuda" | "videotoolbox" | "vaapi" | "qsv" |
  "d3d11va"/"dxva2"). CUDA → zero-copy GPU surface; other hw → decode on GPU then
  download to CPU; all fall back to software if the device is unavailable.
- **encode_ffmpeg** — `codec` (output: "h264" | "hevc"/"h265", default "h264"),
  `hwaccel` ("none" | "nvenc" | "videotoolbox" | "vaapi" | "qsv" | "amf") which
  resolves to the encoder (e.g. h265+nvenc → `hevc_nvenc`); `encoder` (explicit
  FFmpeg encoder name, overrides codec/hwaccel); `bitrate` (4000000), `gop` (50),
  `fps` (0 = µs clock), `preset` ("veryfast"), `tune` ("zerolatency"),
  `frame_width`/`frame_height`, `stream_filter`. (HW encoders use NV12 input;
  vaapi/qsv may need a hw frames context — future.)

## Pre-filter / motion
- **motion_gate** — `downscale` (4), `pixel_threshold` (20),
  `min_changed_pixels` (50), `cooldown_frames` (15), `gate` (true),
  `frame_width`/`frame_height`, `stream_filter`.
- **zones** — zone definitions (ZoneMinder-format: `coords`, `type`,
  thresholds, ...).
- **motion_pixel_diff** — `frame_width`/`frame_height`, `out_width`/`out_height`,
  pixel/blob thresholds, zone-aware options.

## Detect / recognize
- **detect_onnx** — `model_path`, `input_size` (640), `conf_threshold` (0.25),
  `class_filter`, `class_names` (COCO-80), `frame_width`/`frame_height`, `ep`,
  `stream_filter`.
- **detect_openvocab** — `model_path`, `prompts` (class names baked into export),
  `input_size`, `conf_threshold`, `frame_width`/`frame_height`, `ep`,
  `stream_filter`.
- **detect_pose** — `model_path`, `input_size`, `conf_threshold`,
  `iou_threshold` (0.45), `keypoint_names` (COCO-17), dims, `ep`, `stream_filter`.
- **detect_seg** — `model_path`, `input_size`, `conf_threshold`,
  `iou_threshold` (0.45), `mask_dim` (32), `num_classes`, `class_names`,
  `mask_format` ("polygon" | "none"), dims, `ep`, `stream_filter`.
- **recognize_face** — `detector_model_path`, `embedder_model_path`,
  `gallery` (`[{name, embedding[]}]`), `match_threshold` (0.5), `conf_threshold`,
  `embed_size` (112), `embed_mean` (127.5), `embed_scale` (128), dims, `ep`.
- **lpr** — `detector_model_path`, `ocr_model_path`, `charset`, `watchlist`,
  `ocr_width` (168), `ocr_height` (48), `ocr_grayscale` (false), `ctc_blank` (-1),
  `conf_threshold`, dims, `ep`, `stream_filter`.
- **audio_detect** — `model_path`, `codec` ("aac"), `audio_stream_id` (-1=any),
  `sample_rate` (16000), `window_sec` (1.0), `hop_sec` (0.5),
  `conf_threshold` (0.4), `top_k` (3), `labels`.

## Track / analytics / understand
- **tracker** — `iou_threshold` (0.3), `max_age` (30), `min_hits` (3).
- **analytics_rules** — `rules`: array of
  `{name, type:"intrusion"|"linecross"|"loiter", polygon|line, direction, seconds, classes, stream_id}`.
- **describe_vlm** — `server_url` (OpenAI-compatible VLM), `model`,
  `prompt`, `interval_sec` (10), `frame_width`/`frame_height`, `stream_filter`,
  `trigger_types` (e.g. `["detection"]`): when set, the VLM describes a frame
  only after a matching upstream event fires (the YOLO→VLM cascade gate),
  throttled to once per `interval_sec`; empty = legacy fixed-interval.

## Outputs / store
- **output_mqtt** — `host` ("localhost"), `port` (1883), `base_topic`
  ("zm-next"), `client_id`, `username`, `password`, `qos` (0).
- **output_webhook** — `url`, `timeout_ms` (2000), `auth_header`,
  `event_types` (filter; empty = all).
- **output_webrtc** / **output_mse** — `port`, `stream_filter`, client limits
  (video output is being superseded by the zm-api front door).
- **store** — unified recorder. `mode` (`continuous` | `event` | `both`, default
  `continuous`), `root`, `monitor_id`, `stream_filter`. Continuous: `max_secs` (300,
  segment rotation). Event/both: `pre_roll_sec` (5), `post_roll_sec` (10),
  `trigger_types` (["motion","detection","audio_event","tracked_detection"]),
  `max_buffer_sec` (15). Each clip/segment is a ZM event assigned an id by zm-api
  via the recording_opening → assign_recording → EventClip handshake.
- **store_snapshot** — `root`, `trigger_types`, `min_interval_ms` (2000),
  `jpeg_quality` (2–31, lower=better), `frame_width`/`frame_height`,
  `stream_filter`.

## Event flow (what produces/consumes what)

Detectors publish events (`detection`, `pose`, `segmentation`, `face`, `lpr`,
`audio_event`); **tracker** consumes `detection` → emits `tracked_detection`
(adds `track_id`); **analytics_rules** consumes `tracked_detection` → emits
`analytics`; **store** (mode=event/both) / **store_snapshot** / **output_mqtt** /
**output_webhook** consume any of these as triggers. All cross-plugin events flow
through the host event API (`subscribe_evt`/`publish_evt`).
