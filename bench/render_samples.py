#!/usr/bin/env python3
"""Draw the detect_onnx plugin's detection events onto a clip (overlay only).

This does NO inference — it consumes the JSONL emitted by bench_events (the real
decode_ffmpeg -> detect_onnx pipeline) and renders the boxes, exactly as a viewer
would overlay the worker's Event.DETECTION metadata onto the untouched stream.

  python render_samples.py <clip> <events.jsonl> <out_dir>
"""
import sys, os, json, cv2

clip, jsonl, outdir = sys.argv[1], sys.argv[2], sys.argv[3]
os.makedirs(outdir, exist_ok=True)

by_frame = {}
for line in open(jsonl):
    o = json.loads(line)
    by_frame[o["frame"]] = o["event"]["detections"]

cap = cv2.VideoCapture(clip)
fps = cap.get(cv2.CAP_PROP_FPS) or 25.0
W, H = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH)), int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT))
vw = cv2.VideoWriter(os.path.join(outdir, "annotated.mp4"),
                     cv2.VideoWriter_fourcc(*"mp4v"), fps, (W, H))

idx = saved = last = 0
best = (0, None)
while True:
    ok, frame = cap.read()
    if not ok:
        break
    dets = by_frame.get(idx, [])
    for d in dets:
        x, y, w, h = (int(v) for v in d["bbox"])
        cv2.rectangle(frame, (x, y), (x + w, y + h), (0, 230, 0), 3)
        label = f'{d["label"]} {d["confidence"]:.2f}'
        (tw, th), _ = cv2.getTextSize(label, cv2.FONT_HERSHEY_SIMPLEX, 0.9, 2)
        cv2.rectangle(frame, (x, y - th - 8), (x + tw + 4, y), (0, 230, 0), -1)
        cv2.putText(frame, label, (x + 2, y - 6), cv2.FONT_HERSHEY_SIMPLEX, 0.9, (0, 0, 0), 2)
    vw.write(frame)
    if dets:
        if len(dets) > best[0]:
            best = (len(dets), frame.copy())
        if saved < 8 and idx - last >= 8:
            cv2.imwrite(os.path.join(outdir, f"sample_{saved:02d}.jpg"), frame)
            saved, last = saved + 1, idx
    idx += 1

if best[1] is not None:
    cv2.imwrite(os.path.join(outdir, "busiest.jpg"), best[1])
cap.release()
vw.release()
annotated = sum(1 for i in range(idx) if by_frame.get(i))
print(f"frames={idx} frames_with_detections={annotated} samples={saved}")
