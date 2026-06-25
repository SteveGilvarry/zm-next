# Motion Synopsis — Design

## Overview & goal

**Video synopsis** condenses a long recording into a short clip in which moving objects that
occurred at *different* times are replayed *simultaneously* over a static background plate, so
an operator reviews an hour (or a night) of activity in seconds. This is the BriefCam-style
"smart review" some NVRs ship.

The user-facing goal: **"show me everything that moved, on one background, fast."** The MVP
target is the full synopsis video; a per-event **composite still** falls out of the same
ingredients along the way (Phase 1) as a cheaper secondary review mode and as the development
smoke test.

This doc covers the architecture and the **zm-next (C++) side** — producing the *ingredients*.
The temporal optimisation, rendering, caching and serving live in **zm-api**; that contract is
the hand-off spec in the **zm-api** repo at `docs/MOTION_SYNOPSIS_API_SPEC.md`.

## Why this split (and the anti-recompute rule)

The expensive part of synopsis is **foreground extraction**: decode every frame, isolate the
moving subject, cut it out. zm-next *already does this in-pipeline, on GPU/ANE, in real time* —
it decodes frames, runs `detect_seg` (masks), and `tracker` (persistent ids). If zm-api tried to
build the synopsis later it would have to **re-decode the clip and re-run segmentation per
view** — duplicating the most expensive work the C++ side already did, every time, and breaking
once the source clip is rotated out of storage.

> **Rule:** zm-next emits the cheap *ingredients* (object "tubes" + background plates) as a
> byproduct of work it is already doing; zm-api does the cheap-but-presentation-heavy
> *optimisation + compositing + serving*. Pixel blobs are written to the events tree as side
> files and referenced by **path** in a small metadata event — never pushed over the worker
> socket.

The split is clean: zm-api never re-decodes or re-detects. The only design care needed is
*where inside zm-next* the per-pixel work happens (resolved below).

```
   zm-api (Rust): optimiser + compositor + mp4 render + cache + serve   <-- presentation
        ▲  0x0306 review_assets EVENT (metadata: paths to tubes+plates)
        │  (one Unix socket per monitor, canonical stream protocol)
   zm-next (C++): capture → decode → detect_seg → tracker → store
                                          └────────────────────┐
                                   review_export (PROCESS node) ┘  <-- emits tubes
                  motion_pixel_diff ── background plates (side files)
```

## The tube data model

A **tube** is one tracked object's spatiotemporal trail: a *sampled* sequence of
`{premultiplied RGB cutout, mask, bbox, pts}` keyed by `track_id`, plus the background plate(s)
in effect. Every field already exists in the pipeline:

| Tube field | Source (existing) | Evidence |
|---|---|---|
| identity (one tube / object) | `tracker` → `track_id` | `plugins/tracker/tracker.cpp:140-159` |
| object outline (→ matte) | `detect_seg` coarse `polygon` | `plugins/detect_seg/seg_postprocess.hpp:164-228` |
| position / size | detection `bbox [x,y,w,h]` | `plugins/decode_detect/decode_detect.cpp:54-64` |
| time | `pts_usec` | `core/include/zm_plugin.h:80` |
| label / class | `label`, `class_id` | detection event |
| cutout pixels | decoded RGB24 frame × matte | `decode_ffmpeg` RGB24 (`ZM_FRAME_RGB24=101`) |
| background plate | `motion_pixel_diff` `background_` | `plugins/motion_pixel_diff/motion_pixel_diff.cpp:84-269` |

**Coordinate space.** All `bbox`/`polygon`/`cutout` coordinates are in the **decoded RGB24 frame**
`review_export` consumes (which `decode_ffmpeg` may have downscaled, e.g. `frame_width:1280`). The
manifest records that frame's `source_w`/`source_h`. The plate is produced at
`motion_pixel_diff`'s own (independently downscaled) resolution and records its own `w`/`h`;
zm-api rescales the plate to `source_w`/`source_h` at render.

### On-disk layout (written next to the event clip)

```
{event_dir}/                         # = dirname(EventClip.path) after zm-api assigns the id
  512-video.mkv                      # the recording (existing)
  512-video.mkv.json                 # existing descriptions sidecar
  synopsis/                          # review assets for this event
    manifest.json                    # the review_assets manifest (schema below)
    plate-1782129180.jpg             # background plate(s), referenced by manifest
    t17/000001.jpg t17/000002.jpg …  # per-track premultiplied RGB cutouts
    t23/…
```

Plates are camera-level and refreshed on a timer; `review_export` references the plate(s)
nearest each tube's time. Plates may be shared/symlinked or copied into the event dir — see
*Open decisions*.

### The `review_assets` manifest (and EVENT payload)

The published EVENT JSON detail **is** `manifest.json` (or a thin pointer to it — see emission).
Identical schema on both sides:

```jsonc
{
  "type": "review_assets",          // discriminator read by WorkerLink::map_event_code
  "schema": 1,
  "monitor_id": 3,
  "event_id": 512,                  // 0 if the recording_opening→assign handshake didn't complete
  "clip_token": "3-1782129185-7",   // always present; fallback key when event_id == 0
  "clip_path": "/data/3/2026-06-25/512/512-video.mkv",
  "path_base": "synopsis",          // asset dir, ALWAYS relative to dirname(clip_path)
                                    //   (review_export writes synopsis/ next to the actual clip,
                                    //    so this resolves whether clip_path is the ZM tree or
                                    //    store's own-naming fallback)
  "t_start_us": 1782129185000000,   // event media-clock span (pts_usec)
  "t_end_us":   1782129260000000,
  "source_w": 1280,                 // decoded-frame coords for all bbox/polygon/cutout
  "source_h": 720,
  "sample_fps": 4,
  "cause": "detection",             // ZM event cause (trigger type, or "continuous")
  "plates": [
    { "path": "plate-1782129180.jpg", "wallclock_ms": 1782129180000,
      "w": 640, "h": 360, "illum": "day" }
  ],
  "tubes": [
    {
      "track_id": 17, "label": "person", "class_id": 0,
      "t_start_us": 1782129186000000, "t_end_us": 1782129200000000,
      "samples": [
        {
          "pts_us": 1782129186250000, "wallclock_ms": 1782129186250,
          "bbox": [840, 120, 96, 220],            // source coords
          "cutout": "t17/000001.jpg",             // premultiplied RGB, relative to path_base
          "cutout_w": 96, "cutout_h": 220,
          "mask": { "format": "polygon", "points": [[x,y], …] }
          //  or  { "format": "rle", "w": 96, "h": 220, "counts": [..] } (bbox-local)
        }
      ]
    }
  ]
}
```

`mask` polygon points are in **source-frame pixels** (same space as `bbox`); zm-api rasterises
them inside the cutout to recover an alpha. RLE, when used, is **bbox-local** (`w`/`h` = bbox
size). Premultiplied cutouts have background pixels driven to black, so even a hard mask edge
composites cleanly.

## Detector topology (the load-bearing decision)

`tracker` only consumes events with `type=="detection"` (`tracker.cpp:114`); `detect_seg` today
emits only `type=="segmentation"` (`detect_seg.cpp:349`). Naïvely "adding detect_seg" would feed
the tracker nothing and every tube would get `track_id=0` (which `review_export` drops). And the
soft `build_mask()` (`seg_postprocess.hpp:132`) needs the raw `proto` tensor + per-object
`coeffs`, which never leave `detect_seg`.

**Locked topology — single inference:** on synopsis cameras, configure `detect_seg` to emit a
`type:"detection"` event whose per-object entries **carry the coarse `polygon`** (in addition to
`label`/`confidence`/`bbox`/`class_id`). Then:

```
decode_ffmpeg(RGB24) → detect_seg (type:"detection", incl. polygon)
                          → tracker (attaches track_id, passes polygon through → "tracked_detection")
                          → review_export (consumes tracked_detection: track_id + bbox + polygon, in one event)
                       ⤷ also a normal frame-chain child for RGB24 pixels
```

`tracker` already copies original fields through and attaches `track_id`
(`tracker.cpp:140-159`), so the `polygon` rides along untouched — **no tracker change**, **one
inference**, **no join** in `review_export`. (Alternatives considered: running `detect_onnx` +
`detect_seg` = 2× detector cost, rejected; teaching `tracker` to consume `segmentation` = more
coupling. If a deployment *does* run two separate detectors, `review_export` can fall back to a
`(pts_us, bbox-IoU)` join between `tracked_detection` and `segmentation`.)

The matte source is therefore the **coarse polygon** (rasterised + feathered), not the soft
sigmoid mask. That is honest MVP quality; a soft-matte upgrade is P4 (below).

## zm-next work

### 1. `detect_seg` — emit a tracker-compatible detection (additive)
Add a config (`event_type:"detection"`, default keeps `"segmentation"`) so on synopsis cameras
`detect_seg` publishes `type:"detection"` with `polygon` embedded per object. `mask_format` must
be `"polygon"` (not `"none"`) — the daemon enforces this (see API spec). One inference, two
consumers (tracker for ids, review_export for masks via the enriched event).

### 2. `motion_pixel_diff` — own the background plate export (additive)
Add config: `plate_export` (bool), `plate_refresh_secs` (default 120), `plate_dir`,
`plate_on_illum_change` (bool). On the timer / on a significant mean-luma change, write
`background_` as a JPEG side file and publish an **internal** bus event
`{"type":"background_plate","path":…,"wallclock_ms":…,"w":…,"h":…,"illum":"day"|"night"|…}`.
This keeps pixels off the bus (only a path crosses) and needs **no host-API/ABI change** —
`zm_host_api_t` has no state-pull and adding one would bump the ABI. `background_` is grayscale
EMA-updated at the detector's downscale; record its true dims. (`background_` currently has no
getter — add a small accessor + the periodic writer inside the plugin.)

> Note: this `background_plate` event is **bus-internal**; `review_export` consumes it. It is not
> part of the zm-api contract — plate references travel to zm-api *inside* the `review_assets`
> manifest. (It will also leak to the worker socket as an unspecified-code message event, which
> zm-api ignores via skip-on-unknown; harmless.)

### 3. New plugin `review_export` (PROCESS) — assemble + emit tubes
**Placement (important):** frames are delivered *topologically* (chain `on_frame` →
`StageRunner::forwardToChildren`, `core/src/PluginManager.cpp:24-26`), **not** over the bus. So
`review_export` is a PROCESS node wired **downstream of `decode_ffmpeg` (RGB24)** in the pipeline
tree so it receives decoded frames; it *also* subscribes to the EventBus for
`tracked_detection`, `background_plate`, and `EventClip`. Its own `StageRunner` thread has a
bounded drop-oldest queue (`StageRunner.cpp:25-34`) so a slow encoder **drops frames rather than
stalling the detector** — but that means a sampled frame can be missed; tolerate it (use the
nearest buffered frame within a small window, else skip that sample).

Responsibilities:
- **Frame ring:** keep a short ring of recent decoded RGB24 frames keyed by `pts_usec`.
- **pts→wallclock:** capture one `(pts_anchor, wallclock_anchor)` pair at stream start / each
  HELLO; `wallclock_ms = wallclock_anchor + (pts_us - pts_anchor)/1000` (approximate for VOD).
- **Sampling:** for each `tracked_detection` with `track_id != 0`, at ≤ `sample_fps` (default 4)
  per track, locate the matching frame in the ring, **rasterise the polygon → binary mask,
  erode+blur 1–2px (feather), premultiply RGB by the matte (bg→black), downscale to ≤256px max
  edge, MJPEG-encode** the cutout. Buffer the cutout + sample metadata under `clip_token`.
- **Plates:** track the latest `background_plate`(s); pick the one(s) nearest the event span.
- **Materialise on `EventClip`:** the event dir is only known *after* zm-api assigns the id and
  `store` renames the clip (`store.cpp:578-605`); `EventClip.path` is the final path *iff* the
  rename succeeded. So on `EventClip`, write the buffered assets under
  `dirname(EventClip.path)/synopsis/` and set `path_base` = `"synopsis"` (relative). Because the
  assets are written next to the actual clip, the relative `path_base` resolves correctly whether
  `path` is the ZM tree or store's own-naming fallback — no absolute path needed. Then emit the
  `review_assets` event. (`event_id` may be 0; always include `clip_token`.)
- Drop tubes whose only samples are `track_id==0` or that never matched a frame.

### 4. Shared encoder helper
`encode_rgb24_to_jpeg(...)` exists as a file-static in `describe_vlm.cpp:101-163` (FFmpeg MJPEG,
no alpha). Promote it to `plugins/common/image_encode.hpp` (create `plugins/common/`), refactor
`describe_vlm` to consume it, and link FFmpeg into `review_export`. This is the encode path used
for premultiplied cutouts — **no new alpha/PNG/WebP encoder is needed at MVP** (the mask travels
as polygon/RLE in the manifest).

### 5. WorkerLink — three edits + one constant (additive, version stays 1)
1. `core/include/zm/stream_socket_protocol.hpp`: add
   `constexpr uint16_t kEventReviewAssets = 0x0306;` (note: `0x0305` is reserved for the future
   `reasoning` event, see `docs/Research_Motion_and_LLM_Review.md`).
2. `WorkerLink.cpp` `map_event_code`: `if (type == "review_assets") return ss::kEventReviewAssets;`
3. `WorkerLink.cpp` `is_ai_code`: add `kEventReviewAssets` so the manifest JSON rides in **TLV
   0x10** (`kTlvJsonDetail`), not the human-readable message tag 0x02. (`is_health` then
   correctly excludes it, so it won't clobber the snapshot cache.)

Without edit (2)+(3) the manifest silently lands in the wrong TLV and the zm-api ingest breaks.

### 6. Build / registration
Add `plugins/review_export/{CMakeLists.txt,review_export.cpp}` (mirror an existing PROCESS plugin
CMake; link `${ZM_FFMPEG_LIBS}`, `zmcore`, `nlohmann_json`), `add_subdirectory(review_export)` in
`plugins/CMakeLists.txt`, and `add_subdirectory(common)` for the shared header if it needs a
target. Lifecycle is the standard `start/stop/on_frame` (`zm_plugin_init`,
`core/include/zm_plugin.h`).

## Phased plan

| Phase | Repo | Scope | Demo / verification |
|---|---|---|---|
| **P0** Tube export | zm-next | `detect_seg` detection-emit; `motion_pixel_diff` plate export; `review_export` plugin; `0x0306`; shared encoder | `wl_dump` shows a `0x0306` EVENT; `synopsis/manifest.json` + cutouts + plate exist on disk for a clip |
| **P1** Composite still | zm-api | Ingest `0x0306`; blit all tube cutouts onto the plate → one image; serve | `GET /events/{id}/review` returns a glanceable still; validates the whole ingredient path |
| **P2** Temporal optimiser | zm-api | Tube time-shift packing (greedy first), collision + interaction handling | layout preview (still frames at synopsis times) |
| **P3** Render + serve | zm-api | Composite time-shifted tubes → mp4 (shell ffmpeg), cache, REST + queue | `GET /events/{id}/synopsis` returns the clip |
| **P4** Polish | both | soft-matte (detect_seg soft-mask side file → true alpha PNG); time-varying plate selection; retention/budget; per-camera tuning; Poisson blend | quality + storage benchmarks |

Each phase is independently demoable; P1 is the first shippable feature.

## Open design decisions (resolved)

- **(a) Store cutout pixels, not polygons + re-decode.** Emit sampled, downscaled, premultiplied
  cutouts at event time so render is pure compositing. Re-decode-at-render couples synopsis to
  clip retention, pays seek + re-matte per render, and dies when the clip rotates out. Keep the
  polygon/RLE mask in the manifest too (near-free, drives alpha + is the visualisation fallback).
- **(b) `detect_seg` mandatory on synopsis cameras, gated per camera.** Alpha-matted cutouts (the
  locked quality bar) require the mask; bbox-only crops cause rectangular ghosting. With the
  single-inference topology this is *one* detector, not two — but it's still a real per-camera
  cost vs a box-only detector, so it's opt-in per camera (daemon-enforced).
- **(c) Time-varying plate.** Snapshot `background_` every 1–5 min + on illumination change +
  forced day/night samples; zm-api picks the plate nearest each tube's time. This is the single
  biggest realism lever for day↔night and slow-light drift.
- **(d) Retention.** Tube assets are derived/regenerable and bounded; keep them for the review
  window (e.g. 7–30 days) and drop after, independent of (longer) source-event retention.
  **Storage estimate:** ~4 fps × ~3 KB/cutout-sample → a busy camera (~30 active object-minutes/
  hour) is order *tens of MB/hour* of tube assets vs the full clip. Benchmark one real camera
  before P0 ships and bake a per-camera daily budget (P4).

## Risks & failure modes (and mitigations)

| Failure mode | Mitigation |
|---|---|
| **Ghosting / background bleed** | tight matte from polygon + 1–2px feather; premultiply RGB by matte; time-matched plate |
| **Collisions / crowds** (overlapping tubes unreadable) | optimiser collision-area budget (~5% default); cap simultaneous tubes/frame, overflow → longer synopsis or 2nd pass; detect "too crowded" → fall back to plain fast-forward |
| **Flicker** | temporally smooth per-object mask/scale (penalise 2nd-order diffs); EMA the cutout edge |
| **Label clutter** | show per-tube timestamp labels on hover/selection or a few at a time, not all |
| **Missed samples under load** | StageRunner drop-oldest → nearest-frame fallback or skip sample; tolerate gaps |
| **Polygon-grade mattes look rough** | P4: `detect_seg` writes a downscaled soft-alpha side file (it has proto+coeffs+frame) → true alpha |
| **Split interactions** (hand-off, person+bag) | zm-api groups co-occurring tubes and shifts each group as a rigid unit |

## Smoke test / verification (P0)

Mirror `docs/End_To_End_Proof.md`:

```bash
# pipeline: capture_file → decode_ffmpeg(rgb24) → detect_seg(event_type=detection) → tracker
#                                                → review_export ;  motion_pixel_diff(plate_export)
cd build
./zm-core --pipeline ../pipelines/synopsis_smoke.json --socket /tmp/s.sock --monitor-id 3 &
./wl_dump /tmp/s.sock 10 | grep "EVENT code=0x306"     # the review_assets event
cat /tmp/.../synopsis/manifest.json | jq '.tubes | length'   # >0 tubes
ls /tmp/.../synopsis/t*/                                # cutout JPEGs exist
```

**Tests (gtest):** the matte unit must target the **polygon-rasterisation** path (not the
unreachable `build_mask`): polygon→binary mask→feather; premultiply correctness; sampling cadence
(≤ `sample_fps`/track); manifest shape + `event_id==0`/own-naming `path_base` handling. Keep
`./build.sh test` green.
