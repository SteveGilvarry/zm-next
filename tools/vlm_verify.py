#!/usr/bin/env python3
"""vlm_verify: second-opinion pass over scan_recordings.py hits using a local VLM.

For every segment in a scan's report.json, sends the boxed snapshot to an
OpenAI-compatible vision endpoint (the ~/llm vLLM server by default) and asks a
BLIND question about what is inside the red box: no label hint is given, so a
YOLO false positive (IR blob, bin bag, possum) is rejected on the model's own
judgement. Writes the verdict back into report.json and regenerates report.md
with a verdict column. Safe to re-run; already-verified segments are skipped
unless --redo.

  tools/vlm_verify.py ~/catscan/laneway_north [--label cat] [--base http://localhost:8000]
"""

import argparse
import base64
import datetime as dt
import json
import re
import sys
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from scan_recordings import write_markdown  # noqa: E402


def served_model(base: str) -> str:
    with urllib.request.urlopen(base + "/v1/models", timeout=10) as r:
        return json.load(r)["data"][0]["id"]


def ask(base: str, model: str, jpeg: bytes, prompt: str, max_tokens: int = 80) -> str:
    body = {
        "model": model, "max_tokens": max_tokens, "temperature": 0,
        "chat_template_kwargs": {"enable_thinking": False},
        "messages": [{"role": "user", "content": [
            {"type": "image_url", "image_url": {"url": "data:image/jpeg;base64," + base64.b64encode(jpeg).decode()}},
            {"type": "text", "text": prompt}]}],
    }
    req = urllib.request.Request(base + "/v1/chat/completions", data=json.dumps(body).encode(),
                                 headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=120) as r:
        return json.load(r)["choices"][0]["message"]["content"].strip()


def downscale_jpeg(path: Path, max_pixels: int = 1003520) -> bytes:
    """Cap at ~1 MP like the plugin does, so a 4 MP frame doesn't blow the token budget."""
    try:
        from PIL import Image
        import io
        im = Image.open(path).convert("RGB")
        w, h = im.size
        if w * h > max_pixels:
            s = (max_pixels / (w * h)) ** 0.5
            im = im.resize((int(w * s), int(h * s)))
        buf = io.BytesIO()
        im.save(buf, "JPEG", quality=88)
        return buf.getvalue()
    except ImportError:
        # No PIL: let ffmpeg do the same cap (keeps the image under the server's token budget).
        import subprocess
        r = subprocess.run(["ffmpeg", "-v", "error", "-i", str(path), "-vf",
                            f"scale='min(iw,sqrt({max_pixels}*iw/ih))':-2", "-q:v", "4",
                            "-f", "image2", "-vcodec", "mjpeg", "-"], capture_output=True)
        return r.stdout if r.returncode == 0 and r.stdout else path.read_bytes()


# One blind question, no label hint. The verdict is derived in code from the noun
# phrase: a small model answers "is it a cat?" unreliably but names what it sees well.
PROMPT = ("This is a frame from a security camera. Look ONLY inside the red rectangle and say "
          "what is there, as a short noun phrase of 1-4 words (for example: 'a cat', 'a person', "
          "'a dog', 'a bin bag', 'a car', 'empty ground', 'too dark to tell'). No other text.")

SYNONYMS = {
    "cat": ["cat", "kitten", "feline"],
    "dog": ["dog", "puppy", "canine"],
    "person": ["person", "man", "woman", "people", "human", "pedestrian", "child", "boy", "girl"],
    "bird": ["bird", "magpie", "crow", "pigeon", "cockatoo"],
}
UNSURE_WORDS = ["dark", "unclear", "cannot", "can't", "unsure", "not sure", "blurry", "indistinct",
                "shape", "blob", "unknown", "hard to tell", "nothing", "empty",
                "animal", "creature", "figure", "silhouette"]


def verdict_for(label: str, answer: str) -> str:
    a = answer.lower()
    if any(re.search(r"\b" + w + r"s?\b", a) for w in SYNONYMS.get(label, [label])):
        return "YES"
    if any(w in a for w in UNSURE_WORDS):
        return "UNSURE"
    return "NO"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("scan_dir", type=Path, help="scan output dir containing report.json")
    ap.add_argument("--label", default=None, help="target label (default: the scan's label)")
    ap.add_argument("--base", default="http://localhost:8000", help="OpenAI-compatible server base URL")
    ap.add_argument("--redo", action="store_true", help="re-ask for segments that already have a verdict")
    ap.add_argument("--max-box-frac", type=float, default=0.5, help="reject without asking when the box covers more than this fraction of the frame")
    a = ap.parse_args()

    rp = a.scan_dir / "report.json"
    report = json.loads(rp.read_text())
    label = a.label or (report.get("labels_wanted") or [report.get("label", "cat")])[0]
    model = served_model(a.base)
    print(f"model {model}; verifying '{label}' hits in {rp}", file=sys.stderr)

    n = yes = 0
    for c in report["clips"]:
        for s in c["segments"]:
            if s.get("vlm") and not a.redo:
                continue
            snap = Path(s["snapshot"])
            if not snap.exists():
                s["vlm"] = {"verdict": "NO_SNAPSHOT"}
                continue
            fw, fh = c.get("width", 0), c.get("height", 0)
            if not (fw and fh):
                try:
                    from PIL import Image
                    fw, fh = Image.open(snap).size
                except Exception:  # noqa: BLE001
                    pass
            b = s.get("peak_bbox")
            if b and fw and fh and b[2] * b[3] > a.max_box_frac * fw * fh:
                s["vlm"] = {"verdict": "NO", "what": f"box covers {b[2] * b[3] / (fw * fh):.0%} of frame (filtered)"}
                print(f"  {c['event_id']} {s['start'][11:19]} {s.get('label', label)} conf={s['peak_conf']:.2f} -> NO     (box too large, not asked)",
                      file=sys.stderr)
                continue
            t0 = dt.datetime.now()
            txt = ask(a.base, model, downscale_jpeg(snap), PROMPT, max_tokens=24)
            lines = [l.strip() for l in txt.splitlines() if l.strip()]
            what = (lines[0] if lines else txt).strip(" .\"'")
            verdict = verdict_for(s.get("label", label), what)
            s["vlm"] = {"verdict": verdict, "what": what, "raw": txt, "model": model,
                        "ms": int((dt.datetime.now() - t0).total_seconds() * 1000)}
            n += 1
            yes += verdict == "YES"
            print(f"  {c['event_id']} {s['start'][11:19]} {s.get('label', label)} conf={s['peak_conf']:.2f} -> {verdict:6} {what}",
                  file=sys.stderr)
            rp.write_text(json.dumps(report, indent=2))  # checkpoint after every answer

    write_markdown(report, a.scan_dir)
    print(f"asked {n}, YES {yes}; report: {a.scan_dir / 'report.md'}", file=sys.stderr)


if __name__ == "__main__":
    main()
