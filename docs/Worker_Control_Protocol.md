# Worker control protocol (zm-api ↔ zm-core)

Status: **Phase 1 in progress** (zm-next), 2026-09-14: authentication, hello, schemas, configure,
redaction, status codes and exit codes are built; contract transcripts remain. Proposed 2026-09-13. Sections marked
*Implemented* are built; the rest is still proposal. It replaces how zm-api configures,
supervises and talks to zm-next workers; media and analysis events keep the canonical stream-socket
protocol unchanged.

## Summary

zm-api configures a zm-next worker once, at spawn, by writing a plugin graph to its stdin, and learns
about it only from events and its exit status. That works, but the two sides each keep their own idea
of the contract and nothing checks one against the other. This proposal turns the existing worker
socket into a proper control session:

1. The worker starts **unconfigured**, with only `--monitor-id` and `--socket`.
2. It **authenticates** the connecting peer by kernel credentials.
3. It sends a **worker hello**: versions, installed plugins with config schemas, hardware backends,
   current state.
4. zm-api validates its graph against those schemas and sends **configure**, with secrets in a
   separate map. The worker answers with structured errors or applies it.
5. The worker reports typed **status** (streaming, auth failed, camera unreachable, degraded) on the
   canonical health codes.
6. zm-api **owns worker lifetime**: workers are its children, not services of their own, and stop
   with it (decided 2026-09-14, see Worker lifetime).

## How it works today

Checked against zm-api `03e30b2` and zm-next `9217bdf`.

| Channel | Today |
|---|---|
| Spawn | zm-api's daemon manager runs `zm-core --monitor-id <id> --pipeline - --socket <path>` and writes the full plugin graph, camera credentials included, to stdin (`zm-api src/daemon/manager.rs`). |
| Socket | zm-core listens on `stream_<id>.sock` (`chmod 0660`); zm-api connects. Out: HELLO / MEDIA / KEYFRAME / STATS / EVENT (canonical, same as zmc). In: zm-next extension types Subscribe `0x10`, Command `0x11` (→ Response `0x12`), Talkback `0x13`. |
| Files | `store` writes clips into the ZoneMinder events tree after the `recording_opening` → `assign_recording` handshake. |
| Reload | "Reload" regenerates the graph from the database and restarts the worker. There is no in-process reconfigure. |
| Errors | zm-core exits `1`–`5` for bad arguments or a pipeline that fails to load. zm-api logs the status and restarts with 5 s – 15 min backoff. |
| Lifetime | zm-api's systemd unit has `KillMode=mixed`, and `kill_orphan_daemons()` runs `pkill -9` on `zm-core` (and zmc etc.) at startup, so restarting zm-api restarts every worker. |

## Problems this fixes

Each of these is a property of the interface, not a one-off bug.

- **Contract drift.** zm-api keeps a hand-maintained `KNOWN_KINDS` list of zm-next plugins. At the
  time of writing it still lists `output_webrtc` and `output_mse` (removed from zm-next 2026-09-13)
  and `plate_export` (never existed). The same drift left zm-next unable to read `--pipeline -` or
  the `username`/`password` fields zm-api has sent since June (fixed in zm-next `2148992`).
- **Every failure looks like a crash.** A graph with a typo restart-loops forever with backoff
  instead of being reported once as invalid.
- **Camera lockout.** `capture_rtsp_multi` retries a `401` like any other failure. With
  `max_retry_attempts: -1` a wrong password is retried indefinitely, which locks many camera accounts.
  It publishes `StreamDisconnected` but none of the canonical health codes zmc uses.
- **No introspection.** zm-api cannot ask a worker its version, which plugins are installed, which
  hardware backend it found, or whether it is actually streaming.
- **Unauthenticated control.** Any process in the socket's group can send `stop`, `describe_now` or
  talkback audio. There is no peer credential check.
- **Config changes cost recording.** Changing a zone, a threshold or a password restarts the worker,
  losing its pre-event buffer.
- **Secrets are redacted by pattern.** zm-next scrubs `scheme://user:pass@` from logs, but the MQTT
  password, webhook `auth_header` and LLM `api_key` have no protection, and any plugin that logs its
  config would leak them.

## Goals and non-goals

Goals: one authoritative contract that both sides test against; configuration errors reported, not
crash-looped; typed worker and camera status; secrets scrubbed by value everywhere; room for in-place
reconfiguration.

Not goals: changing the media or analysis event wire (zmc and zm-api must keep sharing it); moving
the database into zm-next; putting several monitors in one worker (rejected in
`zm-api/docs/ZMNEXT_SHARED_INFERENCE_PLAN.md` for fault isolation); a network API on the worker;
running zm-next as a service of its own, outside zm-api.

## Design

### Transport

Same Unix socket, same 24-byte header, same additive rule: new types and codes are skipped by
consumers that don't know them. zm-core stays the listener (zm-api already reads zmc sockets as a
client, and a worker that listens can be reconnected to after a zm-api restart).

| Type | Direction | Status | Payload |
|---|---|---|---|
| `0x10` Subscribe | client → worker | exists | JSON `{video, audio, events}` |
| `0x11` Command | client → worker | exists | JSON `{cmd, request_id, ...}` |
| `0x12` Response | worker → client | exists | JSON `{request_id, ok, message, data}` |
| `0x13` Talkback | client → worker | exists | `u32 codec` + audio |
| **`0x14` WorkerHello** | worker → client | **new** | JSON, see below |

Session state and camera health travel as EVENT (`0x06`) frames, reusing canonical codes where they
exist and adding a `0x04xx` worker range.

### Authentication

On accept, zm-core reads the peer's uid with `SO_PEERCRED` (Linux) / `getpeereid` (macOS).

- **Control peers**: uid equal to the worker's own euid, or a uid passed as `--control-uid` (zm-api's
  service user). They may send any Command, Configure and Talkback.
- **Observers**: any other peer allowed by the socket's file mode. They receive media and events.
  Their Commands and Talkback get `{"ok":false,"message":"forbidden"}` and are not dispatched.

The socket stays `0660`; the uid check is what separates observing from controlling.

*Implemented 2026-09-14:* `WorkerLink::Config::control_uids` (empty = own euid) and
`zm-core --control-uid <uid>` (repeatable; own euid always included). A peer whose uid can't be read
is an observer. It gates every Command, including configure, and Talkback.

### Worker hello

Sent to every peer right after accept, before the cached HELLO/snapshot/keyframe replay.

```json
{
  "protocol": {"canonical": 1, "control": 1},
  "zm_next": {"version": "0.1.0", "commit": "525a391", "plugin_abi": 1},
  "monitor_id": 21,
  "state": "running",
  "pipeline_hash": "sha256:fa7a5e2e…",
  "plugins": [
    {"kind": "capture_rtsp_multi", "version": "1.0.0", "schema_sha256": "sha256:9ab2…"},
    {"kind": "capture_file", "version": null, "schema_sha256": null}
  ],
  "hw": {"backends": ["metal"]},
  "secrets_fingerprint": "sha256:8489f816…",
  "control_peer": true
}
```

- `state`: `unconfigured` | `configuring` | `running` | `failed` (the last configure validated but
  its pipeline would not load; the worker has no pipeline until the next configure).
- `pipeline_hash`: SHA-256 of the active pipeline (keys sorted, compact) with every secret value
  replaced by `"<secret>"`. Secret values are those at keys marked `x-secret` in the plugin's schema,
  at `password`, `username`, `auth_header`, `api_key`, `token`, `secret` for any plugin, and wherever
  configure gave a `$secret` reference. Safe for any peer; unchanged by a password change.
- `secrets_fingerprint` (**control peers only**): SHA-256 of `salt` + the pipeline's secret values
  in a stable order, or `null` if the pipeline has none. It tells zm-api whether a running worker
  already has the secrets it would send. The salt is `secrets_salt` from the last configure (empty
  when the worker was started with `--pipeline`). Observers never receive it: unsalted, a hash of
  a short password can be brute-forced.
- `plugins`: every plugin in this build's `plugins/manifest.json` (written by CMake), whether or not
  the pipeline uses it; `version` and `schema_sha256` are `null` for plugins without a schema.
  Libraries left over in the plugin directory from older builds are not listed. Schemas are fetched
  on demand (below) so the hello stays small.
- `hw.backends`: hardware detection backends compiled in (`cuda`, `metal`, `vaapi`, `vulkan`,
  `openvino`). Available decoders aren't reported yet.
- `catalog_error`: present only if `manifest.json` couldn't be read.

*Implemented 2026-09-14:* message type `0x14`, `zm/WorkerHello.{hpp,cpp}`, `WorkerLink::setWorkerHello`.
zm-core rebuilds it on every state change; peers already connected see the change as a
`worker_state` event. `commit` is taken when CMake configures, so it can lag a rebuild without
reconfiguring.

### Plugin schemas

Each plugin ships `plugins/<kind>/<kind>.schema.json`, a JSON Schema (2020-12 subset: `type`,
`properties`, `required`, `enum`, `minimum`/`maximum`, `items`, `additionalProperties`, `default`,
`description`, `oneOf`), copied next to the built library and installed with it. Extra keywords:
`x-plugin-kind`, `x-plugin-version`, `"x-secret": true` on secret properties, and `x-not-config`,
the keys the plugin's source reads that aren't config (event fields, server responses).
`tools/check_plugin_schemas.py` (ctest `PluginSchemaCheck`) fails when a schema property isn't read
by the source or the source reads a key the schema doesn't account for.

`{"cmd":"describe_plugins","kinds":["tracker"]}` returns `data: {"tracker": {"version": "…",
"schema": {…}}}`; omitting `kinds` returns every plugin with a schema, and a kind without one maps
to `null`. zm-api caches by `schema_sha256` and replaces `KNOWN_KINDS` and its hand-written
validation with schema validation.

*Implemented 2026-09-14:* schemas for the 16 kinds zm-api's generator emits: `analytics_rules`,
`capture_rtsp_multi`, `decode_detect`, `decode_ffmpeg`, `detect_onnx`, `encode_ffmpeg`,
`llm_event_review`, `motion_pixel_diff`, `output_mqtt`, `output_webhook`, `privacy_mask`,
`review_export`, `store`, `store_snapshot`, `tracker`, `zones`.

### Configure

```json
{
  "cmd": "configure",
  "request_id": 17,
  "pipeline": {
    "plugins": [
      {"id": "capture", "kind": "capture_rtsp_multi",
       "cfg": {"streams": [{"url": "rtsp://10.0.0.5/Streaming/Channels/102",
                            "username": {"$secret": "cam.user"},
                            "password": {"$secret": "cam.pass"}}]},
       "children": [ … ]}
    ]
  },
  "secrets": {"cam.user": "admin", "cam.pass": "p@ss:w/d"},
  "secrets_salt": "c2FsdC1mcm9tLXptLWFwaQ",
  "apply": "restart"
}
```

The worker:

1. **Validates** every node against its plugin's schema, checks that every `$secret` reference exists
   and that every `x-secret` key is given as a reference, not a literal. On failure it changes
   nothing and responds `ok:false` with `data.errors`:
   ```json
   [{"path": "plugins[0].children[1].cfg.iou_threshold", "message": "must be <= 1"},
    {"path": "plugins[0].cfg.streams[0].password", "message": "secret must be a $secret reference"}]
   ```
2. **Resolves** references into the config each plugin receives, so the plugin ABI is unchanged,
   and keeps `secrets_salt` (chosen by zm-api, stored with the monitor) for the hello's
   `secrets_fingerprint`.
3. **Registers** every secret value with core's redaction set (below).
4. **Applies**. `apply: "restart"` stops the running pipeline and starts the new one inside the same
   process: the socket, its clients and the worker's state survive. A later `apply: "hot"` lets
   plugins that implement an optional `reconfigure` hook change settings without stopping.
5. Responds `ok:true` with `data.pipeline_hash`, then emits `worker_state` `running`.

The same message rotates a password: configure again with a new `secrets` map.

*Implemented 2026-09-14* (`zm/WorkerConfig.{hpp,cpp}`, `src/zm-core.cpp`), with these specifics:

- `zm-core --socket <path> --monitor-id <id>` without `--pipeline` starts `unconfigured`.
  `--pipeline <file|->` still starts `running`; a later configure replaces that pipeline.
- The validator covers the keywords the plugin schemas use (`type`, `enum`, `minimum`, `maximum`,
  `exclusiveMinimum`, `exclusiveMaximum`, `properties`, `required`, `additionalProperties`,
  `items`); others are ignored. A plugin without a schema only has its `$secret` references checked.
  Node keys are limited to `id`, `kind`, `cfg`/`config`, `children`, `queue_depth`; `path` is
  rejected, so a control peer can't make the worker load an arbitrary library. Error messages never
  include the rejected value.
- Other failures: `message` `"apply_failed"` with `data.error` when the pipeline validates but a
  plugin library won't load (state becomes `failed`), and `"stopping"` if a stop is in progress.
- Events: `worker_state` `configuring` (reason `configure`), then `running` (reason `configured`)
  or `failed` (reason = the error). They are enqueued before the Response.
- Apply restarts the plugins in the same process; the socket and its clients stay connected. The
  capture connection and pre-event buffer do not survive yet (Open questions).
- `apply` accepts only `"restart"`.

### Secret redaction

Core already sits on every plugin output path: the host `log` function, `publish_evt`, and
`WorkerLink::publishEventJson`. Each passes its text through one filter that replaces:

- every registered secret value (exact match, including its percent-encoded form), and
- any `scheme://user:pass@` userinfo (the existing `redact_text`).

This covers all plugins, including ones not written yet, and the MQTT, webhook and LLM keys. The
per-call-site redaction in `capture_rtsp_multi` stays as a second layer.

*Implemented 2026-09-14:* `zm/Redactor.{hpp,cpp}`, applied in the host `log` and `publish_evt`
(`PluginManager.cpp`) and `WorkerLink::publishEventJson`. Registered values: every secret the
pipeline holds by key name or reference, from configure or `--pipeline`, each in raw,
percent-encoded and JSON-escaped form, replaced by `***`. Limits:

- Values shorter than 4 characters are not matched in free text (replacing every `"1"` would
  corrupt events). They still never reach the hello, and URL userinfo is still scrubbed.
- Text written straight to stdout/stderr instead of through the host log is not filtered. Plugins
  log through the host; the remaining source is libraries such as FFmpeg's own `av_log` output.

### Status

EVENT frames, `StreamId::Monitor`. The detail is a JSON object: in the JSON detail TLV `0x10` for
the zm-next `0x04xx` codes, and in the message TLV `0x02` for the canonical lifecycle codes (as
zmc-compatible consumers already read them). Status codes, unlike `0x03xx` analysis events, replace
the on-connect snapshot.

*Implemented 2026-09-14:* the `0x0101`/`0x0102`/`0x0105`/`0x0106`/`0x0402` rows, emitted by
`capture_rtsp_multi` (`plugins/capture_rtsp_multi/reconnect_policy.hpp`), the `0x0401`–`0x0403`
constants and WorkerLink mapping, and `0x0401` worker_state from zm-core (reasons `started`,
`command_line`, `configure`, `configured`, or the apply error). A connecting client gets the latest
worker_state and then the latest other status event, so one doesn't hide the other. `connection_failed` also carries `attempt`; it is sent once per
outage and then at most once a minute.

| Code | Name | When | Detail |
|---|---|---|---|
| `0x0001` | snapshot | on connect (exists) | now also includes `worker_state` |
| `0x0101` | connection_failed | a stream cannot connect (canonical, now emitted by zm-next) | `stream_id`, `error`, `retry_in_sec` |
| `0x0102` | connection_restored | canonical | `stream_id` |
| `0x0105` | capture_failed | stream dropped after streaming | `stream_id`, `error` |
| `0x0106` | capture_resumed | canonical | `stream_id` |
| **`0x0401`** | worker_state | state changes | `state`, `pipeline_hash`, `reason` |
| **`0x0402`** | stream_auth_failed | camera answered 401/403 | `stream_id`, `retry_in_sec` |
| **`0x0403`** | worker_degraded | a dependency is down (e.g. shared inference daemon) and a stage is skipped | `component`, `effect` |

**Auth backoff.** On 401/403 capture stops normal reconnect backoff and waits 60 s, doubling to 15
min, publishing `stream_auth_failed` each time. A successful login, or (once built) a configure with
new secrets, resets it. `max_retry_attempts` counts consecutive failures of both kinds; `-1` retries
forever. Measured on a mediamtx camera with a wrong password: 2 logins in 126 s (60 s apart), where
the network backoff would have made one every 1-30 s.

### Worker lifetime

zm-next is not a service of its own. zm-api spawns each worker as its child and is its only
supervisor; a zm-api restart or upgrade restarts the workers, and recording pauses for that long.
An earlier draft of this document proposed separate systemd units that outlive zm-api and are
adopted on restart; that was dropped on 2026-09-14.

What the protocol still provides within that model:

- `pipeline_hash` and `secrets_fingerprint` let zm-api check that a running worker has the
  configuration it expects (after a configure, or when a monitor's settings change) without
  restarting it.
- `configure` changes a running worker's pipeline in place, so a settings change no longer needs a
  new process.
- zm-api stops workers it spawned by pid (`stop` command, then a signal), not by `pkill` pattern.

### Exit codes

With configuration moved to the socket, the worker exits only on `stop`, a signal, or a fatal startup
error. Fixed codes so a supervisor can decide whether a restart can help:

| Code | Meaning | Restart? |
|---|---|---|
| 0 | stopped on request | no |
| 64 | bad command line, or a `--pipeline` that doesn't parse or whose plugins don't load | no |
| 69 | socket path unusable (permissions, in use) | after operator action |
| 70 | internal error / crash | yes, with backoff |

*Implemented 2026-09-14.* Unknown command-line arguments are now an error (64) instead of being
ignored. A crash by signal still shows as a signal, not 70.

## Migration

**Phase 1, zm-next** (backward compatible):
- peer credential check and the `forbidden` response;
- `0x14` WorkerHello, `describe_plugins`, schemas for every plugin (start with the ones zm-api's
  generator emits);
- `configure` with `apply: "restart"`, `$secret` resolution, core-wide redaction;
- capture: canonical health codes, `0x0402` and auth backoff;
- `--pipeline <file|->` still accepted: the worker starts `running` with that pipeline, exactly as today.

**Phase 2, zm-api:**
- read the worker hello; validate graphs against schemas; drop `KNOWN_KINDS`;
- send `configure` instead of stdin, falling back to `--pipeline -` when no hello arrives (older zm-next);
- surface `worker_state`, `stream_auth_failed`, `worker_degraded` in the API and UI;
- stop workers by the pid it spawned, not by `pkill` pattern.

**Phase 3:** hot reconfigure for plugins that support it; remove the stdin path.

## Testing

- zm-next ships `tests/contract/` with the schemas and golden transcripts: hello; configure accepted;
  configure rejected with each error kind; secret references; status sequences for auth failure and
  recovery.
- zm-api's CI replays the transcripts against its client and runs its generated graphs through
  schema validation. zm-next's CI runs the same graphs through a real `zm-core` against a
  password-protected test camera.
- Integration (see the ZoneMinder-install CI discussion): reconfigure a recording zm-next monitor
  through zm-api; the socket stays connected and recording resumes without a new worker process.

## ZoneMinder upstream

Message type `0x14` and EVENT codes `0x0401`–`0x0403` must be reserved in the canonical spec
(`docs/stream_socket.rst` on the fork branch) alongside the existing zm-next extensions. zmc could
adopt the worker hello and typed health codes later; nothing here requires it.

## Alternatives considered

| Option | Why not |
|---|---|
| Keep stdin, add schemas only | Fixes drift but not error reporting, reconfigure, credential rotation or lifetime. |
| gRPC or HTTP control API in zm-core | Adds a large dependency and a network surface to a deliberately small C++ worker. |
| Separate control socket | Two connections to authenticate and supervise per monitor for no functional gain. Worth revisiting only if ZoneMinder upstream objects to zm-next types in the shared protocol. |
| Worker pulls config from zm-api or the database | Breaks "zm-next has no database" and gives the worker a credential of its own to protect. |
| Secrets over a separate fd or systemd credentials | Secure, but ties zm-next to systemd and still leaves errors, reconfigure and drift unsolved. |
| One worker for many monitors | Widens the blast radius of a crash; already rejected in the shared inference plan. |

## Open questions

- Whether `apply: "restart"` keeps the capture connection and pre-event buffer when only downstream
  stages change (desirable; needs a pipeline diff).
- Schema coverage for nested plugin configs that are free-form today (`analytics_rules.rules`,
  `privacy_mask.dynamic`).
- More than one control peer at once (zm-api plus an admin tool): serialise configures, or reject a
  second controller.
- How the shared inference daemon (`zm-infer`) reports into `worker_degraded`, and whether it gets the
  same hello/configure session.
