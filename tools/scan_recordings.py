#!/usr/bin/env python3
"""scan_recordings: offline object search over ZoneMinder passthrough recordings.

Points at a ZoneMinder event store reachable over SSH (e.g. the NAS that holds
`events/<monitor_id>/<YYYY-MM-DD>/<event_id>/<event_id>-video*.mp4`), selects the
clips that overlap a wall-clock window, pulls each one over rsync, runs the real
decode_ffmpeg -> detect_onnx plugins on it via `bench_events` (GPU, motion-ROI
cascade), and reports every sighting of a target COCO label ("cat" by default)
as a merged time segment with a boxed JPEG of the best frame.

Event start time is derived as clip mtime (= write finish) minus the clip's
duration, so no ZoneMinder database access is needed.

Example:
  tools/scan_recordings.py --monitor "Front Gate - Record" \
      --from "2026-09-10 18:00" --to "2026-09-10 20:00" --label cat
"""

import argparse
import datetime as dt
import json
import os
import shlex
import shutil
import concurrent.futures
import queue
import subprocess
import sys
import threading
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
HOME = Path.home()


def parse_when(s: str) -> dt.datetime:
    for fmt in ("%Y-%m-%d %H:%M:%S", "%Y-%m-%d %H:%M", "%Y-%m-%dT%H:%M:%S", "%Y-%m-%dT%H:%M", "%Y-%m-%d"):
        try:
            return dt.datetime.strptime(s, fmt).astimezone()
        except ValueError:
            pass
    raise argparse.ArgumentTypeError(f"unrecognised time: {s!r} (use 'YYYY-MM-DD HH:MM')")


def ssh(host: str, cmd: str, timeout: int = 120) -> str:
    r = subprocess.run(["ssh", "-o", "BatchMode=yes", host, cmd], capture_output=True, text=True, timeout=timeout)
    if r.returncode != 0:
        raise RuntimeError(f"ssh {host} {cmd!r} failed: {r.stderr.strip()}")
    return r.stdout


def resolve_monitor(host: str, root: str, monitor: str) -> tuple[int, str]:
    """Accept a numeric id or a monitor-name symlink in the events root."""
    if monitor.isdigit():
        return int(monitor), monitor
    out = ssh(host, f"ls -l {shlex.quote(root)}")
    for line in out.splitlines():
        if " -> " not in line:
            continue
        left, target = line.rsplit(" -> ", 1)
        name = left.split(None, 8)[-1]
        if name == monitor:
            mid = os.path.basename(target.strip().rstrip("/"))
            if mid.isdigit():
                return int(mid), monitor
    raise SystemExit(f"monitor {monitor!r} not found under {host}:{root}")


def list_clips(host: str, root: str, mid: int, t_from: dt.datetime, t_to: dt.datetime, max_seg_s: int):
    """Return [(mtime_epoch, size, remote_path)] for clips whose write-finish falls in the window."""
    day = (t_from - dt.timedelta(seconds=max_seg_s)).date()
    dirs = []
    while day <= t_to.date():
        dirs.append(f"{root}/{mid}/{day.isoformat()}")
        day += dt.timedelta(days=1)
    cmd = "for d in " + " ".join(shlex.quote(d) for d in dirs) + \
          "; do [ -d \"$d\" ] && find \"$d\" -mindepth 2 -maxdepth 2 -name '*-video*.mp4' -printf '%T@ %s %p\\n'; done; true"
    lo, hi = t_from.timestamp(), t_to.timestamp() + max_seg_s
    clips = []
    for line in ssh(host, cmd, timeout=300).splitlines():
        mt, sz, path = line.split(" ", 2)
        mt = float(mt)
        if lo <= mt <= hi:
            clips.append((mt, int(sz), path))
    clips.sort()
    return clips


def ffprobe(path: Path) -> tuple[float, float, int, int, int, str]:
    """-> (duration_s, fps, nb_frames, width, height, codec)"""
    out = subprocess.run(
        ["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries",
         "stream=avg_frame_rate,nb_frames,width,height,codec_name:format=duration", "-of", "json", str(path)],
        capture_output=True, text=True, check=True).stdout
    j = json.loads(out)
    st = j["streams"][0]
    dur = float(j["format"]["duration"])
    nb = int(st.get("nb_frames") or 0)
    num, den = st.get("avg_frame_rate", "0/1").split("/")
    fps = (float(num) / float(den)) if float(den) else 0.0
    if nb and dur:
        fps = nb / dur  # most faithful to how bench_events counts frames
    return dur, fps, nb, int(st.get("width") or 0), int(st.get("height") or 0), st.get("codec_name", "h264")


def probe_rotation(path: Path) -> int:
    """Rotation the container asks players to apply (display matrix), 0 when none."""
    out = subprocess.run(["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries",
                          "stream_side_data=rotation:stream_tags=rotate", "-of", "json", str(path)],
                         capture_output=True, text=True).stdout
    try:
        st = json.loads(out)["streams"][0]
        for sd in st.get("side_data_list", []):
            if "rotation" in sd:
                return int(round(float(sd["rotation"])))
        return int(st.get("tags", {}).get("rotate", 0))
    except (KeyError, IndexError, ValueError, json.JSONDecodeError):
        return 0


def prepare_clip(src: Path, width: int, height: int, codec: str, max_long: int = 1920,
                 crop=None, fps: float = 0) -> Path:
    """Re-encode a clip on the GPU before scanning, when the source needs fixing or trimming:
      - rotation: the decode plugin ignores the container's rotation flag, so a sideways-mounted
        camera would be scanned sideways (and snapshot boxes would land in the wrong place);
        ffmpeg's autorotate applies the display matrix here and NVENC writes an unflagged file.
      - crop (W, H, X, Y in stored-frame pixels): restrict the scan to a region (e.g. the footpath,
        not the road). Done inside NVDEC so it is free, and the region then fills the detector's
        640-px input instead of being a sliver of a 4K frame.
      - fps: drop to N frames/s for a cheaper pass; timestamps stay correct.
    NVDEC also downscales so the long side is at most max_long. ~10 s per 10-min 4K clip."""
    dst = src.with_name(src.stem + ".prep.mp4")
    dec = {"hevc": "hevc_cuvid", "h265": "hevc_cuvid"}.get(codec, "h264_cuvid")
    cmd = ["ffmpeg", "-v", "error", "-y", "-hwaccel", "cuda", "-c:v", dec]
    cw, ch = width, height
    if crop:
        w, h, x, y = crop
        cmd += ["-crop", f"{y}x{max(0, height - y - h)}x{x}x{max(0, width - x - w)}"]  # cuvid: top x bottom x left x right
        cw, ch = w, h
    scale = min(1.0, max_long / max(cw, ch))
    rw, rh = (int(cw * scale) // 2) * 2, (int(ch * scale) // 2) * 2
    cmd += ["-resize", f"{rw}x{rh}", "-i", str(src), "-an"]
    if fps:
        cmd += ["-vf", f"fps={fps}"]
    cmd += ["-c:v", "h264_nvenc", "-preset", "p1", "-cq", "23", str(dst)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0 or not dst.exists():
        raise RuntimeError(f"prepare transcode failed for {src.name}: {r.stderr[-800:]}")
    return dst


def run_bench(bench: Path, clip: Path, model: Path, plugins: Path, out: Path, env: dict) -> int:
    r = subprocess.run([str(bench), "--input", str(clip), "--model", str(model), "--plugins", str(plugins),
                        "--roi", "--out", str(out)], capture_output=True, text=True, env=env)
    if r.returncode != 0:
        raise RuntimeError(f"bench_events failed on {clip.name}:\n{r.stderr[-2000:]}")
    for line in r.stderr.splitlines():
        if line.startswith("frames="):
            return int(line.split()[0].split("=")[1])
    return 0


def merge_segments(hits, fps: float, gap_s: float):
    """hits: [(frame, conf, bbox)] sorted -> [{start_f,end_f,peak_conf,peak_frame,peak_bbox,n}]"""
    segs = []
    for f, c, b in hits:
        if segs and (f - segs[-1]["end_f"]) / fps <= gap_s:
            s = segs[-1]
            s["end_f"] = f
            s["n"] += 1
            if c > s["peak_conf"]:
                s.update(peak_conf=c, peak_frame=f, peak_bbox=b)
        else:
            segs.append(dict(start_f=f, end_f=f, n=1, peak_conf=c, peak_frame=f, peak_bbox=b))
    return segs


def snapshot(clip: Path, t_s: float, bbox, out: Path):
    x, y, w, h = [int(v) for v in bbox]
    subprocess.run(["ffmpeg", "-v", "error", "-y", "-ss", f"{t_s:.3f}", "-i", str(clip), "-frames:v", "1",
                    "-vf", f"drawbox=x={x}:y={y}:w={w}:h={h}:color=red@0.9:t=4", "-q:v", "3", str(out)],
                   capture_output=True, text=True)


def load_hits(dets: Path, labels_wanted, conf: float, frame_area: int = 0, max_box_frac: float = 0.5):
    """Read a clip's detections JSONL -> (label counts at >= conf, {label: [(frame, conf, bbox)]}).
    Boxes covering more than max_box_frac of the frame are dropped: on a fixed camera those are
    exposure flips, rain on the lens, or headlight washes, never an object."""
    labels: dict[str, int] = {}
    hits: dict[str, list] = {l: [] for l in labels_wanted}
    with open(dets) as fh:
        for line in fh:
            j = json.loads(line)
            f = j["frame"]
            for d in j["event"].get("detections", []):
                if d["confidence"] < conf:
                    continue
                if frame_area and d["bbox"][2] * d["bbox"][3] > max_box_frac * frame_area:
                    continue
                labels[d["label"]] = labels.get(d["label"], 0) + 1
                if d["label"] in hits:
                    hits[d["label"]].append((f, d["confidence"], d["bbox"]))
    for v in hits.values():
        v.sort()
    return labels, hits


def build_segments(centry: dict, hits: dict, clip, hits_dir: Path, gap_s: float) -> int:
    """Merge per-label hits into segments on a clip entry, snapshotting the peak frame
    when the clip file is available. Returns the number of segments added."""
    start = dt.datetime.fromisoformat(centry["start"])
    fps = centry["fps"]
    eid = centry["event_id"]
    n = 0
    for label, lh in hits.items():
        for s in merge_segments(lh, fps, gap_s):
            t0 = start + dt.timedelta(seconds=s["start_f"] / fps)
            t1 = start + dt.timedelta(seconds=s["end_f"] / fps)
            jpg = hits_dir / f"{eid}_{t0:%H%M%S}_{label}.jpg"
            if clip is not None and clip.exists():
                snapshot(clip, s["peak_frame"] / fps, s["peak_bbox"], jpg)
            centry["segments"].append(dict(label=label, start=t0.isoformat(), end=t1.isoformat(),
                                           offset_s=round(s["start_f"] / fps, 1), frames=s["n"],
                                           peak_conf=round(s["peak_conf"], 3), peak_frame=s["peak_frame"],
                                           peak_bbox=[round(v, 1) for v in s["peak_bbox"]], snapshot=str(jpg)))
            n += 1
    centry["segments"].sort(key=lambda x: x["start"])
    return n


def write_markdown(report: dict, out: Path, path: Path | None = None) -> str:
    """Render report.md (used by the scanner and the post-hoc report/verify tools)."""
    mname, mid = report["monitor"], report["monitor_id"]
    t_from, t_to = (dt.datetime.fromisoformat(t) for t in report["window"])
    wanted = report.get("labels_wanted") or [report.get("label", "cat")]
    total = sum(len(c["segments"]) for c in report["clips"])
    verified = any(s.get("vlm") for c in report["clips"] for s in c["segments"])
    confirmed = sum(1 for c in report["clips"] for s in c["segments"] if s.get("vlm", {}).get("verdict") == "YES")
    lines = [f"# {'/'.join(wanted)} scan: {mname} (id {mid}), {t_from:%Y-%m-%d %H:%M} to {t_to:%Y-%m-%d %H:%M}", "",
             f"{len(report['clips'])} clip(s) scanned, {total} segment(s) at conf >= {report['conf']}"
             + (f", {confirmed} confirmed by VLM." if verified else "."), "",
             "| time | label | dur | event | offset | frames | peak |" + (" VLM | VLM says |" if verified else "") + " snapshot |",
             "|---|---|---|---|---|---|---|" + ("---|---|" if verified else "") + "---|"]
    for c in report["clips"]:
        for s in c["segments"]:
            t0 = dt.datetime.fromisoformat(s["start"]); t1 = dt.datetime.fromisoformat(s["end"])
            v = s.get("vlm", {})
            row = (f"| {t0:%Y-%m-%d %H:%M:%S} | {s.get('label', report.get('label'))} | {(t1 - t0).total_seconds():.0f}s | "
                   f"{c['event_id']} | {s['offset_s']}s | {s['frames']} | {s['peak_conf']:.2f} |")
            if verified:
                row += f" {v.get('verdict', '')} | {v.get('what', '').replace('|', '/')} |"
            lines.append(row + f" {Path(s['snapshot']).name} |")
    lines += ["", "## Activity per clip (frames with a detection, by label)", ""]
    for c in report["clips"]:
        t0 = dt.datetime.fromisoformat(c["start"])
        lab = ", ".join(f"{k} {v}" for k, v in sorted(c["labels"].items(), key=lambda kv: -kv[1])) or "nothing"
        lines.append(f"- {t0:%Y-%m-%d %H:%M:%S} event {c['event_id']}: {lab}")
    text = "\n".join(lines) + "\n"
    (path or out / "report.md").write_text(text)
    return text


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="192.168.0.40", help="SSH host holding the event store")
    ap.add_argument("--root", default="/mnt/zoneminder/videodata/events", help="events root on --host")
    ap.add_argument("--monitor", required=True, help="monitor id or name symlink (e.g. 'Front Gate - Record')")
    ap.add_argument("--from", dest="t_from", required=True, type=parse_when, help="window start, local time")
    ap.add_argument("--to", dest="t_to", required=True, type=parse_when, help="window end, local time")
    ap.add_argument("--label", default="cat", help="COCO label(s) to report, comma-separated (e.g. cat,person)")
    ap.add_argument("--conf", type=float, default=0.35, help="min confidence for a hit")
    ap.add_argument("--merge-gap", type=float, default=5.0, help="seconds between hits to merge into one segment")
    ap.add_argument("--max-box-frac", type=float, default=0.5, help="drop boxes covering more than this fraction of the frame")
    ap.add_argument("--max-seg", type=int, default=900, help="longest expected clip in seconds (window slack)")
    ap.add_argument("--model", type=Path, default=ROOT / "bench/models/yolo26n.onnx")
    ap.add_argument("--plugins", type=Path, default=ROOT / "build/plugins")
    ap.add_argument("--bench", type=Path, default=ROOT / "build/bench/bench_events")
    ap.add_argument("--out", type=Path, default=None, help="output dir (default ~/catscan/<monitor>_<from>_<to>)")
    ap.add_argument("--keep-clips", action="store_true", help="keep pulled clips instead of deleting after scan")
    ap.add_argument("--workers", type=int, default=3, help="concurrent GPU scan workers (transfer is pipelined)")
    ap.add_argument("--rotate", choices=["auto", "off"], default="auto",
                    help="auto: re-encode clips whose container carries a 90/270 rotation flag upright before scanning")
    ap.add_argument("--max-long", type=int, default=1920, help="long-side pixel cap for the re-encode")
    ap.add_argument("--crop", type=lambda v: tuple(int(x) for x in v.split(":")), default=None,
                    help="W:H:X:Y region of the stored frame to scan (e.g. the footpath only)")
    ap.add_argument("--fps", type=float, default=0, help="re-encode at this frame rate before scanning (skip frames)")
    ap.add_argument("--dry-run", action="store_true", help="list matching clips and exit")
    a = ap.parse_args()

    if a.t_to <= a.t_from:
        sys.exit("--to must be after --from")

    labels_wanted = [l.strip() for l in a.label.split(",") if l.strip()]
    mid, mname = resolve_monitor(a.host, a.root, a.monitor)
    clips = list_clips(a.host, a.root, mid, a.t_from, a.t_to, a.max_seg)
    print(f"monitor {mname!r} (id {mid}): {len(clips)} candidate clip(s) finishing in "
          f"{a.t_from:%Y-%m-%d %H:%M} .. {a.t_to:%Y-%m-%d %H:%M} (+{a.max_seg}s slack)", file=sys.stderr)
    if a.dry_run:
        for mt, sz, p in clips:
            print(f"  {dt.datetime.fromtimestamp(mt):%Y-%m-%d %H:%M:%S}  {sz/1e6:8.1f} MB  {p}")
        return

    tag = f"{mid}_{a.t_from:%Y%m%d-%H%M}_{a.t_to:%Y%m%d-%H%M}"
    out = a.out or (HOME / "catscan" / tag)
    (out / "clips").mkdir(parents=True, exist_ok=True)
    (out / "dets").mkdir(exist_ok=True)
    (out / "hits").mkdir(exist_ok=True)

    vcpkg = os.environ.get("VCPKG_ROOT", str(HOME / "vcpkg"))
    cuda = os.environ.get("CUDA_HOME", "/usr/local/cuda")
    env = dict(os.environ)
    env["LD_LIBRARY_PATH"] = ":".join([f"{vcpkg}/installed/x64-linux-dynamic/lib", str(HOME / "onnxruntime/lib"),
                                       f"{cuda}/lib64", env.get("LD_LIBRARY_PATH", "")])

    report = dict(monitor=mname, monitor_id=mid, label=a.label, labels_wanted=labels_wanted, conf=a.conf,
                  host=a.host, window=[a.t_from.isoformat(), a.t_to.isoformat()], clips=[])
    total_hits = 0
    lock = threading.Lock()

    def fetch(item):
        """Producer step: rsync one clip (network-bound, serialised so the NAS link is not thrashed)."""
        mt, sz, rpath = item
        local = out / "clips" / Path(rpath).name
        subprocess.run(["rsync", "-q", "--inplace", f"{a.host}:{rpath}", str(local)], check=True)
        return item, local

    def process(item, local):
        """Worker step: probe, detect, segment, snapshot. GPU-bound; several run concurrently."""
        nonlocal total_hits
        mt, sz, rpath = item
        eid = Path(rpath).parent.name
        try:
            dur, fps, nb, fw, fh, codec = ffprobe(local)
            start = dt.datetime.fromtimestamp(mt - dur).astimezone()
            end = dt.datetime.fromtimestamp(mt).astimezone()
            if start > a.t_to or end < a.t_from:
                print(f"[{eid}] {start:%H:%M:%S}-{end:%H:%M:%S} outside window, skipped", file=sys.stderr)
                return
            rot = probe_rotation(local) if a.rotate == "auto" else 0
            upright = False
            if rot % 180 != 0 or a.crop or a.fps:
                up = prepare_clip(local, fw, fh, codec, a.max_long, a.crop, a.fps)
                local.unlink(missing_ok=True)
                local = up
                _, fps2, _, fw, fh, _ = ffprobe(local)
                fps = fps2 or fps
                upright = True
            dets = out / "dets" / f"{eid}.jsonl"
            frames = run_bench(a.bench, local, a.model, a.plugins, dets, env)
            labels, hits = load_hits(dets, labels_wanted, a.conf, fw * fh, a.max_box_frac)
            centry = dict(event_id=eid, remote=rpath, start=start.isoformat(), end=end.isoformat(),
                          duration_s=dur, fps=fps, frames=frames, width=fw, height=fh, upright=upright,
                          crop=list(a.crop) if a.crop else None,
                          labels=labels, segments=[])
            nseg = build_segments(centry, hits, local, out / "hits", a.merge_gap)
            others = ", ".join(f"{k}:{v}" for k, v in sorted(labels.items(), key=lambda kv: -kv[1]))
            with lock:
                total_hits += nseg
                report["clips"].append(centry)
                report["clips"].sort(key=lambda c: c["start"])
                (out / "report.json").write_text(json.dumps(report, indent=2))  # checkpoint per clip
                done = len(report["clips"])
            print(f"[{eid}] {start:%m-%d %H:%M:%S}-{end:%H:%M:%S} {sz/1e6:.0f} MB{' prep' if upright else ''} {frames} frames, "
                  f"{nseg} {a.label} segment(s); labels: {others or 'none'}   ({done}/{len(clips)})",
                  file=sys.stderr, flush=True)
        finally:
            if not a.keep_clips:
                local.unlink(missing_ok=True)

    # Pipeline: one fetcher thread keeps up to workers+1 clips ahead on local disk; a pool of
    # GPU workers scans them concurrently (one process does not saturate the GPU or NVDEC).
    q: "queue.Queue" = queue.Queue(maxsize=a.workers + 1)
    errors: list = []

    def fetcher():
        try:
            for item in clips:
                q.put(fetch(item))
        except Exception as e:  # noqa: BLE001
            errors.append(e)
        finally:
            for _ in range(a.workers):
                q.put(None)

    threading.Thread(target=fetcher, daemon=True).start()
    with concurrent.futures.ThreadPoolExecutor(max_workers=a.workers) as pool:
        def worker():
            while True:
                got = q.get()
                if got is None:
                    return
                try:
                    process(*got)
                except Exception as e:  # noqa: BLE001
                    errors.append(e)
                    print(f"[{Path(got[0][2]).parent.name}] FAILED: {e}", file=sys.stderr)
        list(pool.map(lambda _: worker(), range(a.workers)))
    if errors:
        print(f"{len(errors)} clip(s) failed; first: {errors[0]}", file=sys.stderr)

    (out / "report.json").write_text(json.dumps(report, indent=2))
    print("\n" + write_markdown(report, out))
    print(f"\nreport: {out}/report.md  (json: report.json, snapshots: hits/)", file=sys.stderr)
    if not shutil.disk_usage(out).free > 1 << 30:
        print("warning: under 1 GB free in output dir", file=sys.stderr)


if __name__ == "__main__":
    main()
