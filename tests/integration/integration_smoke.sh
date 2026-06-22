#!/usr/bin/env bash
# Integration smoke test for the zm-next per-monitor worker.
#
# Drives the built zm-core + plugins through capture_file scenarios and asserts
# decode / record / worker-socket behavior end to end. Codifies the bugs found by
# the integration-test workflow so they can't regress:
#   - baseline H264 cascade records a valid mkv (and no duplicate first keyframe)
#   - HEVC (large extradata) decodes and records              [B1 regression]
#   - loop replay keeps recording past the first pass         [B2 regression]
#   - store creates NO orphan file when it gets no muxable frame [B3 regression]
#   - a dead input surfaces a CONNECTION_FAILED health event  [B6 regression]
#
# Usage: integration_smoke.sh <build_dir>
# Skips gracefully (exit 0) if ffmpeg / zm-core / wl_dump are unavailable, so a
# minimal CI box keeps `ctest` green.
set -u

BUILD="${1:-${BUILD_DIR:-}}"
BUILD="$(cd "$BUILD" 2>/dev/null && pwd)"
ZMCORE="$BUILD/zm-core"
WLDUMP="$BUILD/wl_dump"

command -v ffmpeg  >/dev/null 2>&1 || { echo "SKIP: ffmpeg not found";  exit 0; }
command -v ffprobe >/dev/null 2>&1 || { echo "SKIP: ffprobe not found"; exit 0; }
[ -x "$ZMCORE" ] || { echo "SKIP: zm-core not built at $ZMCORE"; exit 0; }
[ -x "$WLDUMP" ] || { echo "SKIP: wl_dump not built at $WLDUMP"; exit 0; }

# zm-core resolves plugins as plugins/<kind>/<kind>.dylib relative to CWD, so run
# from the build dir. All other paths in this script are absolute.
cd "$BUILD" || { echo "SKIP: cannot cd to build dir"; exit 0; }

WORK="$(mktemp -d)"
cleanup() { for p in "${PIDS[@]:-}"; do kill "$p" 2>/dev/null; done; rm -rf "$WORK"; }
trap cleanup EXIT
PIDS=()
fails=0
pass() { echo "  PASS: $1"; }
fail() { echo "  FAIL: $1"; fails=$((fails + 1)); }

wait_sock() { for _ in $(seq 1 100); do [ -S "$1" ] && return 0; sleep 0.1; done; return 1; }
pkt_count() { ffprobe -v error -count_packets -select_streams v:0 \
              -show_entries stream=nb_read_packets -of csv=p=0 "$1" 2>/dev/null; }
rec_file()  { find "$1" -name '*.mkv' -type f 2>/dev/null | head -1; }

# ---------------------------------------------------------------------------
echo "[1] baseline H264 cascade"
ffmpeg -y -f lavfi -i testsrc=duration=5:size=640x480:rate=15 \
       -c:v libx264 -bf 0 -tune zerolatency -g 15 -pix_fmt yuv420p \
       "$WORK/h264.mp4" >/dev/null 2>&1
SRC_PKTS=$(pkt_count "$WORK/h264.mp4")
cat > "$WORK/base.json" <<JSON
{"name":"base","root":true,"plugins":[
 {"id":"cap","kind":"capture_file","cfg":{"path":"$WORK/h264.mp4","stream_id":0,"loop":false,"realtime":false},
  "children":[
   {"id":"dec","kind":"decode_ffmpeg","cfg":{"output_format":"rgb24"},"queue_depth":8},
   {"id":"st","kind":"store","cfg":{"mode":"continuous","root":"$WORK/base_rec","monitor_id":701},"queue_depth":120}]}]}
JSON
"$ZMCORE" --pipeline "$WORK/base.json" --socket "$WORK/s701.sock" --monitor-id 701 >"$WORK/c701.log" 2>&1 &
WPID=$!; PIDS+=("$WPID"); wait_sock "$WORK/s701.sock"; sleep 4; kill $WPID 2>/dev/null; wait $WPID 2>/dev/null
REC=$(rec_file "$WORK/base_rec")
if [ -n "$REC" ]; then
  P=$(pkt_count "$REC")
  [ "$P" = "$SRC_PKTS" ] && pass "recorded $P pkts == source $SRC_PKTS (no dup keyframe)" \
                         || fail "recorded $P pkts != source $SRC_PKTS"
else fail "no recording produced"; fi
grep -q "Error writing frame" "$WORK/c701.log" && fail "store write errors present" || pass "no store write errors"
grep -q "send_packet failed" "$WORK/c701.log" && fail "decode errors present" || pass "no decode errors"

# ---------------------------------------------------------------------------
echo "[2] HEVC decode + record [B1]"
if ffmpeg -y -f lavfi -i testsrc=duration=4:size=640x480:rate=15 \
          -c:v libx265 -x265-params bframes=0:log-level=error -tag:v hvc1 \
          -g 15 -pix_fmt yuv420p "$WORK/hevc.mp4" >/dev/null 2>&1; then
  cat > "$WORK/hevc.json" <<JSON
{"name":"hevc","root":true,"plugins":[
 {"id":"cap","kind":"capture_file","cfg":{"path":"$WORK/hevc.mp4","stream_id":0,"loop":false,"realtime":false},
  "children":[
   {"id":"dec","kind":"decode_ffmpeg","cfg":{"output_format":"rgb24"},"queue_depth":8},
   {"id":"st","kind":"store","cfg":{"mode":"continuous","root":"$WORK/hevc_rec","monitor_id":702},"queue_depth":120}]}]}
JSON
  "$ZMCORE" --pipeline "$WORK/hevc.json" --socket "$WORK/s702.sock" --monitor-id 702 >"$WORK/c702.log" 2>&1 &
  WPID=$!; PIDS+=("$WPID"); wait_sock "$WORK/s702.sock"; sleep 4; kill $WPID 2>/dev/null; wait $WPID 2>/dev/null
  grep -q "decoder ready codec=hevc" "$WORK/c702.log" && pass "decoder selected hevc (not h264 fallback)" \
                                                      || fail "decoder did not select hevc"
  grep -q "send_packet failed\|No start code" "$WORK/c702.log" && fail "hevc decode errors" || pass "no hevc decode errors"
  REC=$(rec_file "$WORK/hevc_rec")
  if [ -n "$REC" ] && [ "$(ffprobe -v error -select_streams v:0 -show_entries stream=codec_name -of csv=p=0 "$REC")" = "hevc" ]; then
    pass "valid hevc recording"
  else fail "no valid hevc recording"; fi
else echo "  SKIP: libx265 unavailable"; fi

# ---------------------------------------------------------------------------
echo "[3] loop replay continuity [B2]"
ffmpeg -y -f lavfi -i testsrc=duration=2:size=640x480:rate=15 \
       -c:v libx264 -bf 0 -tune zerolatency -g 15 -pix_fmt yuv420p "$WORK/loop.mp4" >/dev/null 2>&1
LOOP_SRC=$(pkt_count "$WORK/loop.mp4")
cat > "$WORK/loop.json" <<JSON
{"name":"loop","root":true,"plugins":[
 {"id":"cap","kind":"capture_file","cfg":{"path":"$WORK/loop.mp4","stream_id":0,"loop":true,"realtime":true},
  "children":[
   {"id":"st","kind":"store","cfg":{"mode":"continuous","root":"$WORK/loop_rec","monitor_id":703},"queue_depth":120}]}]}
JSON
"$ZMCORE" --pipeline "$WORK/loop.json" --socket "$WORK/s703.sock" --monitor-id 703 >"$WORK/c703.log" 2>&1 &
WPID=$!; PIDS+=("$WPID"); wait_sock "$WORK/s703.sock"; sleep 7; kill $WPID 2>/dev/null; wait $WPID 2>/dev/null
grep -q "seeking to start (loop)" "$WORK/c703.log" && pass "loop seek occurred" || fail "no loop seek"
grep -q "Error writing frame" "$WORK/c703.log" && fail "EINVAL write errors across loop" || pass "no write errors across loop"
REC=$(rec_file "$WORK/loop_rec")
if [ -n "$REC" ]; then
  P=$(pkt_count "$REC")
  [ "${P:-0}" -gt "$((LOOP_SRC + LOOP_SRC / 2))" ] && pass "recorded $P pkts > one pass ($LOOP_SRC) — recording continued past loop" \
                                                   || fail "recorded $P pkts <= one pass ($LOOP_SRC) — stalled after first loop"
else fail "no loop recording"; fi

# ---------------------------------------------------------------------------
echo "[4] store creates no orphan file for non-muxable input [B3]"
cat > "$WORK/b3.json" <<JSON
{"name":"b3","root":true,"plugins":[
 {"id":"cap","kind":"capture_file","cfg":{"path":"$WORK/h264.mp4","stream_id":0,"loop":false,"realtime":false},
  "children":[
   {"id":"dec","kind":"decode_ffmpeg","cfg":{"output_format":"rgb24"},
    "children":[{"id":"st","kind":"store","cfg":{"mode":"continuous","root":"$WORK/b3_rec","monitor_id":704}}]}]}]}
JSON
"$ZMCORE" --pipeline "$WORK/b3.json" --socket "$WORK/s704.sock" --monitor-id 704 >"$WORK/c704.log" 2>&1 &
WPID=$!; PIDS+=("$WPID"); wait_sock "$WORK/s704.sock"; sleep 3; kill $WPID 2>/dev/null; wait $WPID 2>/dev/null
N=$(find "$WORK/b3_rec" -type f 2>/dev/null | wc -l | tr -d ' ')
[ "$N" = "0" ] && pass "no orphan file created (store got only decoded frames)" \
               || fail "$N orphan file(s) created"

# ---------------------------------------------------------------------------
echo "[5] dead input surfaces a health event [B6]"
cat > "$WORK/b6.json" <<JSON
{"name":"b6","root":true,"plugins":[
 {"id":"cap","kind":"capture_file","cfg":{"path":"$WORK/does_not_exist.mp4","stream_id":0,"loop":false,"realtime":false},
  "children":[{"id":"st","kind":"store","cfg":{"mode":"continuous","root":"$WORK/b6_rec","monitor_id":705}}]}]}
JSON
"$ZMCORE" --pipeline "$WORK/b6.json" --socket "$WORK/s705.sock" --monitor-id 705 >"$WORK/c705.log" 2>&1 &
WPID=$!; PIDS+=("$WPID")
if wait_sock "$WORK/s705.sock"; then
  sleep 0.5
  "$WLDUMP" "$WORK/s705.sock" 2 >"$WORK/wl705.log" 2>&1
  # CONNECTION_FAILED is event code 0x0101 (canonical wire); replayed via the
  # connect snapshot and printed by wl_dump as "EVENT code=0x101".
  grep -q "EVENT code=0x101" "$WORK/wl705.log" && pass "CONNECTION_FAILED surfaced on socket" \
                                               || fail "no health event for dead input"
else fail "worker socket never came up"; fi
kill -0 $WPID 2>/dev/null && pass "worker still alive (no crash on dead input)" || fail "worker crashed on dead input"
kill $WPID 2>/dev/null; wait $WPID 2>/dev/null

# ---------------------------------------------------------------------------
echo "[7] segment rotation (keyframe-aligned, no frame loss)"
# 5s clip, max_secs=2 -> ~3 keyframe-aligned segments that together hold every
# frame, each a valid mkv (regression for the rotation bug chain).
cat > "$WORK/rot.json" <<JSON
{"name":"rot","root":true,"plugins":[
 {"id":"cap","kind":"capture_file","cfg":{"path":"$WORK/h264.mp4","stream_id":0,"loop":false,"realtime":false},
  "children":[
   {"id":"st","kind":"store","cfg":{"mode":"continuous","root":"$WORK/rot_rec","monitor_id":707,"max_secs":2},"queue_depth":120}]}]}
JSON
"$ZMCORE" --pipeline "$WORK/rot.json" --socket "$WORK/s707.sock" --monitor-id 707 >"$WORK/c707.log" 2>&1 &
WPID=$!; PIDS+=("$WPID"); wait_sock "$WORK/s707.sock"; sleep 4; kill $WPID 2>/dev/null; wait $WPID 2>/dev/null
NSEG=$(find "$WORK/rot_rec" -name '*.mkv' -type f 2>/dev/null | wc -l | tr -d ' ')
TOT=0; BAD=0
for f in $(find "$WORK/rot_rec" -name '*.mkv' -type f 2>/dev/null); do
  P=$(pkt_count "$f"); TOT=$((TOT + ${P:-0})); ffprobe -v error "$f" >/dev/null 2>&1 || BAD=$((BAD + 1))
done
[ "$NSEG" -ge 2 ] && pass "rotated into $NSEG segments" || fail "expected multiple segments, got $NSEG"
[ "$TOT" = "$SRC_PKTS" ] && pass "segments hold all $TOT frames (no loss across rotation)" \
                         || fail "segments hold $TOT frames != source $SRC_PKTS"
[ "$BAD" = "0" ] && pass "all segments are valid mkv" || fail "$BAD corrupt/empty segment(s)"
grep -q "Error writing frame" "$WORK/c707.log" && fail "write errors during rotation" || pass "no write errors during rotation"

# ---------------------------------------------------------------------------
echo "[6] audio carried end-to-end [B4]"
if ffmpeg -y -f lavfi -i testsrc=duration=4:size=640x480:rate=15 \
          -f lavfi -i sine=frequency=1000:duration=4 \
          -c:v libx264 -bf 0 -tune zerolatency -g 15 -pix_fmt yuv420p \
          -c:a aac -shortest "$WORK/av.mp4" >/dev/null 2>&1; then
  cat > "$WORK/av.json" <<JSON
{"name":"av","root":true,"plugins":[
 {"id":"cap","kind":"capture_file","cfg":{"path":"$WORK/av.mp4","stream_id":0,"loop":false,"realtime":false,"forward_audio":true},
  "children":[
   {"id":"st","kind":"store","cfg":{"mode":"continuous","root":"$WORK/av_rec","monitor_id":706},"queue_depth":120}]}]}
JSON
  "$ZMCORE" --pipeline "$WORK/av.json" --socket "$WORK/s706.sock" --monitor-id 706 >"$WORK/c706.log" 2>&1 &
  WPID=$!; PIDS+=("$WPID"); wait_sock "$WORK/s706.sock"; sleep 4; kill $WPID 2>/dev/null; wait $WPID 2>/dev/null
  REC=$(rec_file "$WORK/av_rec")
  if [ -n "$REC" ]; then
    STREAMS=$(ffprobe -v error -show_entries stream=codec_type -of csv=p=0 "$REC" 2>/dev/null | sort | tr '\n' ',')
    echo "$STREAMS" | grep -q "audio" && echo "$STREAMS" | grep -q "video" \
      && pass "recording has both audio and video tracks ($STREAMS)" \
      || fail "recording missing audio track ($STREAMS)"
  else fail "no a/v recording"; fi
else echo "  SKIP: aac encoder unavailable"; fi

# ---------------------------------------------------------------------------
echo "----------------------------------------"
if [ "$fails" -eq 0 ]; then echo "INTEGRATION SMOKE: ALL PASS"; exit 0
else echo "INTEGRATION SMOKE: $fails FAILURE(S)"; exit 1; fi
