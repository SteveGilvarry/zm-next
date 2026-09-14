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
- **capture_rtsp_multi** — `streams` (or single `url`); per stream `url`,
  `stream_id`, `username` / `password` (kept out of the URL, logs and events),
  `transport` ("tcp"), `hw_decode` (false), `forward_audio` (true),
  `max_retry_attempts` (5; -1 = forever; counts consecutive failures). Network
  failures back off 1 s → 30 s; a 401/403 backs off 60 s → 15 min so a wrong
  password can't lock the camera account. Publishes `connection_failed` /
  `connection_restored` / `capture_failed` / `capture_resumed` (canonical codes
  0x0101/0x0102/0x0105/0x0106) on transitions and `stream_auth_failed` (0x0402)
  per rejected login. (`retry_delay_ms` is parsed but the backoff above governs.)
- **capture_file** — `path` (required), `stream_id` (0), `loop` (true),
  `realtime` (true).

## Decode / Encode (codec + hardware configurable)
- **decode_ffmpeg** — input codec is **auto-detected** from the capture plugin's
  StreamMetadata (so H264/HEVC/etc. cameras just work). `codec` is an OPTIONAL
  override ("h264" | "hevc"/"h265" | "mjpeg" | "av1" | "vp8"/"vp9", or any FFmpeg
  decoder name; only set it to force a codec). `output_format`
  ("yuv420p" | "rgb24" | "gray"), `scale` ("orig" | "720p" | "WxH"), `threads`
  (0), `hwaccel` ("none" | "auto" | "cuda" | "videotoolbox" | "vaapi" | "qsv" |
  "d3d11va"/"dxva2"): decode on the GPU, then download to CPU so every downstream
  plugin gets normal pixels; falls back to software if the device is unavailable.
  `gpu_output` (false) emits CUDA / VideoToolbox frames as GPU descriptors
  instead. Those are only valid inside the decoder's own `on_frame` call, so don't
  set it in a pipeline (a queued child reads a freed frame); `decode_detect` sets
  it for its internal decoder. For zero-copy detection use `decode_detect`.
- **decode_detect** — fused hardware decode + on-GPU motion gate + detect in one
  synchronous stage (see `docs/GPU_Pipeline.md`). `model_path`, `input_size` (640),
  `conf_threshold` (0.25), `hw` ("auto" | "cuda" | "metal" | "vaapi" | "vulkan" |
  "openvino"; auto picks the platform's backend and sets the inner decoder's
  hwaccel to match), `roi_motion` (false; true = run the gate and infer only on
  frames/regions that moved), `motion` (gate tunables, every key optional, omit to
  keep the backend default): `downsample` (cell size px, 8), `pixel_threshold`
  (per-cell luma diff, 25; Vulkan 18), `min_cells` (max(8, cells/400)),
  `luma_jump` (suppress whole-scene exposure jumps above this, off; Metal),
  `max_regions` (CUDA multi-region path, 8). `class_filter` ([ids]),
  `stream_filter`, `codec` (override), `decode_path` (inner decode_ffmpeg
  library). Logs `decode_detect stats` (frames/gated/infers/detections) every 250
  frames and on stop.
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
- **privacy_mask** — obscures regions in decoded RGB24/gray frames, never the
  caller's buffer. `frame_width`/`frame_height` (required), `mode` ("black" |
  "blur" | "pixelate"; pixelate is the usual choice for identities),
  `blur_size` (16; blur radius or pixelate block px), `stream_filter`.
  Static: `regions` (array of polygons `[[x,y],...]`); place right after decode
  so detection never sees them. Dynamic: `dynamic` object masks what detectors
  found on each frame; place **downstream** of the detectors and before
  encode/store/output (detectors publish before forwarding, so the boxes for a
  frame arrive before the frame). Keys: `enabled` (true), `sources`
  (["detection","tracked_detection","face","lpr"]), `classes` (["person"],
  labels taken from detection events; empty = all), `min_confidence` (0.25,
  detections only; faces and plates are always masked), `padding` (0.15 of box
  size per side), `hold_ms` (400; reuse boxes from frames this close, covering
  skipped or missed detections), `person_region` ("body" | "head"; head masks
  the top `head_fraction`, 0.3, of person boxes). Logs `privacy_mask stats`
  (frames/masked/boxes) every 500 frames and on stop. A frame the detectors
  miss for longer than `hold_ms` passes unmasked.

## Detect / recognize
- **detect_onnx** — `model_path`, `input_size` (640), `conf_threshold` (0.25),
  `class_filter`, `class_names` (COCO-80), `frame_width`/`frame_height`, `ep`,
  `stream_filter`. ReID (appearance embedding for the tracker): `reid` (false);
  `reid_model_path` (optional OSNet-style ONNX — when set, emits a learned
  embedding per box, else falls back to an HSV colour histogram),
  `reid_input_w` (128) / `reid_input_h` (256).
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
  `conf_threshold` (0.4), `top_k` (3), `labels`. `input_type` ("waveform"
  YAMNet-style | "logmel" for CED/EfficientAT) — with a log-mel front-end:
  `n_fft` (512, power of 2), `hop_length` (160), `n_mels` (64), `fmin` (0),
  `fmax` (0=sr/2), `mel_log_offset` (1e-6), `mel_log10` (false), `mel_slaney` (false).

## Track / analytics / understand
- **tracker** — `iou_threshold` (0.3), `max_age` (30), `min_hits` (3),
  `class_gated` (true), `appearance_threshold` (0=off) / `appearance_weight` (0.3) /
  `embed_alpha` (0.1) for ReID; OC-SORT/ByteTrack: `det_high_thresh` (0.5,
  high/low confidence split; 0 = single-stage), `low_iou_threshold` (0.2),
  `ocm_weight` (0.2). Kalman motion + observation-centric recovery are always on.
- **analytics_rules** — `rules`: array of
  `{name, type:"intrusion"|"linecross"|"loiter"|"fall", polygon|line, direction, seconds, classes, stream_id}`.
  intrusion/linecross/loiter consume the tracker's `tracked_detection`. **fall**
  consumes detect_pose's `pose` events (needs detect_pose upstream, no extra
  model) and fires once when a person goes from upright to down and stays down
  and still: `seconds` (2.0, down this long), `polygon` (optional; only people
  whose feet are inside), `tilt_deg` (55, torso angle from vertical),
  `aspect` (1.0, bbox w/h), `min_signals` (2 of torso tilt / wide bbox / head at
  or below hips), `keypoint_conf` (0.5), `max_drift` (0.5 bbox heights of
  movement allowed while down; more restarts the clock, so crawling doesn't
  fire), `require_upright` (true; must have been seen upright within
  `upright_window_sec`, 5.0, so someone already lying down doesn't fire).
  Emits `rule_type:"fall"` with `down_sec`, `drift`, `tilt_deg`, `bbox`;
  `track_id` is the rule's own person id. Re-arms when the person is upright again.
- **describe_vlm** — `server_url` (OpenAI-compatible VLM), `model`,
  `prompt`, `interval_sec` (10), `frame_width`/`frame_height`, `stream_filter`,
  `trigger_types` (e.g. `["detection"]`): when set, the VLM describes a frame
  only after a matching upstream event fires (the YOLO→VLM cascade gate),
  throttled to once per `interval_sec`; empty = legacy fixed-interval.
  Multi-frame + structured output: `frames` (1; >1 sends that many keyframes with
  per-frame timestamps so the VLM reasons over a sequence), `frame_interval_ms`
  (700, spacing of the sampled keyframe ring), `json_output` (false; when true,
  requests a strict JSON schema and emits `threat_level` + `scene_confidence` on
  the event), `max_tokens` (128). Best with a temporally-aware model
  (Qwen3-VL-4B-Instruct). Answers the `describe_now` worker command (see
  "On-demand commands" below).

## Outputs / store
- **output_mqtt** — `host` ("localhost"), `port` (1883), `base_topic`
  ("zm-next"), `client_id`, `username`, `password`, `qos` (0).
- **output_webhook** — `url`, `timeout_ms` (2000), `auth_header`,
  `event_types` (filter; empty = all).
- Live video is not a zm-next plugin. The worker socket carries compressed
  media to zm-api, which serves WebRTC / HLS / MSE (`/api/v3/live/...`). The old
  `output_webrtc` / `output_mse` plugins and their Node signaling bridge were
  removed on 2026-09-13.
- **store** — unified recorder. `mode` (`continuous` | `event` | `both`, default
  `continuous`), `root`, `monitor_id`, `stream_filter`. Continuous: `max_secs` (300,
  segment rotation). Event/both: `pre_roll_sec` (5), `post_roll_sec` (10),
  `trigger_types` (["motion","detection","audio_event","tracked_detection"]),
  `max_buffer_sec` (15). Each clip/segment is a ZM event assigned an id by zm-api
  via the recording_opening → assign_recording → EventClip handshake.
- **store_snapshot** — `root`, `trigger_types`, `min_interval_ms` (2000),
  `jpeg_quality` (2–31, lower=better), `frame_width`/`frame_height`,
  `stream_filter`. Each snapshot publishes `EventSnapshot` (EVENT `0x0307`) with
  `path`, `width`, `height`, `bytes`, `pts_usec`. Answers the `snapshot_now`
  worker command. `trigger_types: ["none"]` gives a snapshot-on-command-only
  instance.

## On-demand commands (worker socket)

zm-api can ask a running worker for work now, for an agent/MCP tool or a UI
button. Send a Command (`0x11`) with a JSON body; zm-core replies at once with a
Response (`0x12`) `{"ok":true,"message":"dispatched","request_id":N}`, then the
plugin that owns the command publishes exactly one result EVENT carrying the same
`request_id`, `on_demand:true` and `ok` (with `error` when `ok` is false). The
result can arrive before or after the Response; match on `request_id`. Instances
with a `stream_filter` only answer commands naming one of their streams; a
command without `stream_id` goes to every instance. If no plugin in the pipeline
owns the command, only the Response arrives, so callers should time out.

| Command | Keys | Result EVENT |
|---|---|---|
| `snapshot_now` | `request_id`, `stream_id`?, `inline`? (true adds `jpeg_base64`) | `0x0307` `EventSnapshot`: `path`, `width`, `height`, `bytes`, `pts_usec` (store_snapshot). Ignores trigger list and throttle. |
| `describe_now` | `request_id`, `stream_id`?, `prompt`? (one-off override) | `0x0302` `description`: `text`, `prompt`, `model`, `frames`, `pts_usec` (describe_vlm). Ignores trigger gate and cooldown and does not reset it. |

Try it with `./wl_dump /tmp/zm.sock 5 '{"cmd":"snapshot_now","request_id":1}'`.

## Event flow (what produces/consumes what)

Detectors publish events (`detection`, `pose`, `segmentation`, `face`, `lpr`,
`audio_event`); **tracker** consumes `detection` → emits `tracked_detection`
(adds `track_id`); **analytics_rules** consumes `tracked_detection` → emits
`analytics`; **store** (mode=event/both) / **store_snapshot** / **output_mqtt** /
**output_webhook** consume any of these as triggers. All cross-plugin events flow
through the host event API (`subscribe_evt`/`publish_evt`).
