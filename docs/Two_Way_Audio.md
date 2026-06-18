# Two-Way Audio (Talkback)

How a user talks *to* a camera that has a speaker. Decision + architecture +
what's implemented vs. remaining.

## Is it frontend or backend?

**Both.** The frontend captures the mic and plays camera audio; the backend must
(a) negotiate the camera's audio backchannel and (b) relay the client's audio to
it. The browser cannot speak the camera's protocol directly, so the relay is a
real backend responsibility.

## Decision: ONVIF Profile T RTSP audio backchannel

The standard mechanism for sending audio to a camera speaker is the **ONVIF
Profile T RTSP backchannel** (Profile S is receive-only; Profile T adds two-way):

- The client `DESCRIBE`s the RTSP URL with `Require: www.onvif.org/ver20/backchannel`.
- The camera's SDP then has a `recvonly` audio media (camera→client) **and** a
  `sendonly` audio media (client→camera).
- After `SETUP`/`PLAY`, the client sends RTP audio (commonly **G.711 PCMA/PCMU**,
  sometimes AAC/Opus) up the sendonly channel, interleaved over the RTSP TCP
  connection.
- Cameras lacking Profile T need vendor fallbacks (e.g. Axis VAPIX
  `transmit.cgi`); `go2rtc` is the reference implementation of all this.

Because the backchannel rides the **same RTSP connection** zm-next already opens
to the camera, it belongs in the **capture plugin** (`capture_rtsp_multi`), which
owns that connection.

## End-to-end path

```
browser mic ──► zm-api (WebRTC/REST) ──► worker socket (Talkback frame)
   ──► WorkerLink (talkback handler) ──► capture plugin ──► camera RTSP backchannel
```

## What's implemented now

- **Contract:** `worker_link.proto` has an inbound `Talkback { codec, pts_us, data }`
  message in the `Frame` oneof (client→server). The bidirectional worker socket
  already supports inbound messages (Subscribe/Command), so no new transport.
- **Routing hook:** `WorkerLink` parses inbound `Talkback` frames and invokes a
  `TalkbackHandler` (`setTalkbackHandler`). `zm-core` registers one today that
  logs receipt, so the path from API → worker is exercisable end-to-end.

## Remaining (the camera-specific part)

1. **Deliver talkback to the capture plugin.** The handler in `zm-core` must reach
   `capture_rtsp_multi`. Add a host→plugin control hook (the plugin ABI currently
   has only start/stop/on_frame). Note: the in-process `EventBus` is **not** a
   safe path here — plugins are `dlopen`'d and may not share the executable's
   `EventBus` singleton across the dylib boundary (`-fvisibility=hidden`); a host
   API callback (function pointer into the worker) is the correct mechanism.
2. **Open the backchannel.** In `capture_rtsp_multi`, add the
   `Require: www.onvif.org/ver20/backchannel` option to the RTSP `DESCRIBE`
   (FFmpeg: `av_dict_set(&opts, "...", ...)` is insufficient — FFmpeg's RTSP
   demuxer doesn't natively send a sendonly backchannel, so this needs manual RTSP
   SDP handling à la go2rtc, or a vendor API). Detect the `551 Option not
   supported` response and fall back / disable talkback for that camera.
3. **Send the audio.** Transcode the inbound `Talkback` audio to the camera's
   negotiated backchannel codec (usually G.711) and packetize as RTP up the
   sendonly channel.

This step is **camera-specific and needs real hardware to validate**, so it is
deliberately left as the next implementation task rather than written blind.

## Codec note

Default to **G.711 (PCMA/PCMU)** for the backchannel — the most widely supported
camera speaker codec. Negotiate from the camera's SDP; transcode the client audio
(Opus from WebRTC) to it in the worker or API.
