# ONVIF Integration — a zm-api responsibility

Decision: ONVIF (both directions) lives in **zm-api**, the control plane. zm-next
is the per-monitor media/AI worker and stays ONVIF-unaware — it is simply handed
an RTSP URL + config and runs the pipeline. This is the same boundary as PTZ
(see docs/AI_Architecture.md) and the whole role split: zm-api owns DB, auth,
camera inventory + credentials, orchestration, and the public surface.

There are TWO distinct ONVIF roles; they're easy to conflate.

## 1. ONVIF *client* — managing cameras (the common NVR job)

The system acts as an ONVIF client to the cameras. All of this is control-plane
and belongs in zm-api (it has the network access, credentials, and DB):

- **WS-Discovery** — multicast `Probe` on the LAN to find ONVIF cameras; show
  them in the dashboard for one-click add.
- **Device/media services** — `GetDeviceInformation`, `GetCapabilities`,
  `GetProfiles`, and crucially **`GetStreamUri`** to obtain the RTSP URL (per
  profile/substream) that zm-api then writes into the pipeline JSON it hands a
  zm-next worker.
- **PTZ service** — `ContinuousMove` / `Stop` / `GotoPreset` (real-time control).
- **Imaging** — brightness/focus/IR config.
- **Event service** — subscribe (PullPoint / base notification) to camera-side
  events (motion, tampering, line-cross) and fold them into the event model
  alongside zm-next's AI events.

Flow: `zm-api WS-Discovery → GetStreamUri → store camera + RTSP URL in DB →
render pipeline JSON → spawn zm-next worker`. The worker never speaks ONVIF.

## 2. ONVIF *device/server* — being managed by other systems (optional)

The system presents *itself* as an ONVIF device so external VMSes can discover
and consume it:

- **Profile S/T device** — advertise our streams (answer WS-Discovery, serve
  `GetStreamUri` pointing at zm-api's RTSP/HLS).
- **Profile G** — recording search & replay so a third-party VMS can pull our
  recordings.

This is also zm-api (it is the front door that already owns the viewer protocols
and the recording model). zm-next is not involved.

## What zm-next does (and doesn't)

- Does NOT do ONVIF discovery, management, PTZ, or device-server. None of it is a
  pipeline/frame concern.
- The ONLY camera-protocol thing a worker does is the **two-way-audio RTSP
  backchannel** (docs/Two_Way_Audio.md) — because that audio must ride the same
  RTSP connection the capture plugin already holds. That is unrelated to ONVIF
  management.

## Status

Not implemented as ONVIF here. zm-api has a `ptz/bridge.rs` (PTZ path); WS-
Discovery, GetStreamUri-driven onboarding, the ONVIF event service, and the
device-server side are zm-api work items. A small Rust ONVIF/SOAP + WS-Discovery
layer (or an existing crate) in zm-api covers the client side; the device-server
side is a later, optional add.
