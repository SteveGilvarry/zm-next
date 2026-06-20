#!/usr/bin/env python3
"""Resource comparison: run each detection METHOD as its own process and sample
CPU%, RSS (system RAM), GPU utilization and VRAM, plus throughput. Emits a CSV, a
human-readable markdown report, and a PNG chart.

Methods compared (axes): CPU-EP vs GPU; 720p vs 4K; full vs motion-gated vs ROI.
"""
import subprocess, time, re, os, statistics as st, csv

ROOT = os.path.expanduser("~/code/zm-next")
VCPKG = os.path.expanduser("~/vcpkg"); CUDA = "/usr/local/cuda-12.9"; ORT = os.path.expanduser("~/onnxruntime")
env = dict(os.environ)
env["LD_LIBRARY_PATH"] = f"{VCPKG}/installed/x64-linux-dynamic/lib:{ORT}/lib:{CUDA}/lib64:" + env.get("LD_LIBRARY_PATH", "")
MODEL = f"{ROOT}/bench/models/yolo26n.onnx"; PLUG = f"{ROOT}/build/plugins"
GPU = f"{ROOT}/build/bench/bench_gpu_roi"; CPU = f"{ROOT}/build/bench/bench_roi_cascade"
C720 = f"{ROOT}/bench/clips/sync_720p.mp4"; C4K = f"{ROOT}/bench/clips/sync_4k.mp4"
OUT = f"{ROOT}/bench/samples"

def gpu(inp, only, loops): return [GPU, "--input", inp, "--model", MODEL, "--plugins", PLUG, "--only", only, "--loops", str(loops)]
def cpu(inp): return [CPU, "--input", inp, "--model", MODEL, "--cpu"]   # CPU execution provider

CONFIGS = [
    {"label": "CPU-EP · 720p · full",  "cmd": cpu(C720),          "parse": "full-frame"},
    {"label": "GPU · 720p · full",     "cmd": gpu(C720, "full", 8), "parse": "full-frame"},
    {"label": "GPU · 4K · full",       "cmd": gpu(C4K, "full", 4),  "parse": "full-frame"},
    {"label": "GPU · 4K · gated",      "cmd": gpu(C4K, "gated", 4), "parse": "motion-gated"},
    {"label": "GPU · 4K · ROI-crop",   "cmd": gpu(C4K, "roi", 4),   "parse": "roi-crop"},
]

import psutil
try:
    import pynvml; pynvml.nvmlInit(); H = pynvml.nvmlDeviceGetHandleByIndex(0); NVML = True
except Exception:
    NVML = False

def gpu_sample(pids):
    if not NVML: return 0.0, 0.0
    util = pynvml.nvmlDeviceGetUtilizationRates(H).gpu
    vram = 0
    try:
        for p in pynvml.nvmlDeviceGetComputeRunningProcesses(H):
            if p.pid in pids and p.usedGpuMemory: vram += p.usedGpuMemory
    except Exception: pass
    return float(util), vram / 1e6

def run(cfg):
    p = subprocess.Popen(cfg["cmd"], env=env, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
    proc = psutil.Process(p.pid); proc.cpu_percent(None)
    cpu_s, rss_s, gu_s, gv_s = [], [], [], []
    while p.poll() is None:
        try:
            kids = proc.children(recursive=True)
            pids = {proc.pid} | {k.pid for k in kids}
            c = proc.cpu_percent(None) + sum(k.cpu_percent(None) for k in kids)
            r = proc.memory_info().rss + sum(k.memory_info().rss for k in kids)
            u, v = gpu_sample(pids)
            cpu_s.append(c); rss_s.append(r / 1e6); gu_s.append(u); gv_s.append(v)
        except (psutil.NoSuchProcess, psutil.AccessDenied): break
        time.sleep(0.15)
    out, _ = p.communicate()
    ms = ips = None
    for line in out.splitlines():                      # prefer the uniform METHOD line
        if line.startswith("METHOD"):
            m1 = re.search(r"ms_per_inf=([\d.]+)", line); m2 = re.search(r"inf_per_s=([\d.]+)", line)
            if m1: ms = float(m1.group(1))
            if m2: ips = float(m2.group(1))
    if ms is None:                                      # CPU tool: parse its strategy line
        for line in out.splitlines():
            if line.strip().startswith(cfg["parse"]):
                n = re.findall(r"[\d.]+", line)
                if n: ms = float(n[-1])
    mean = lambda a: round(st.mean(a), 1) if a else 0.0
    peak = lambda a: round(max(a), 1) if a else 0.0
    # drop the first couple of samples (warmup) from CPU mean
    cpu_steady = cpu_s[2:] or cpu_s
    return {"label": cfg["label"], "cpu_mean_pct": mean(cpu_steady), "cpu_peak_pct": peak(cpu_s),
            "rss_peak_mb": peak(rss_s), "gpu_util_mean_pct": mean(gu_s), "gpu_util_peak_pct": peak(gu_s),
            "vram_peak_mb": peak(gv_s), "ms_per_inf": ms,
            "inf_per_s": round(ips) if ips else (round(1000 / ms) if ms else None),
            "samples": len(cpu_s)}

print("running resource matrix (each method = its own process)...")
rows = []
for c in CONFIGS:
    print("  ", c["label"], "...", flush=True)
    rows.append(run(c))

# CSV
with open(f"{OUT}/resources.csv", "w", newline="") as f:
    w = csv.DictWriter(f, fieldnames=list(rows[0].keys())); w.writeheader(); w.writerows(rows)

# Markdown
hdr = ["method", "CPU% (mean/peak)", "RAM peak MB", "GPU% (mean/peak)", "VRAM peak MB", "ms/inf", "inf/s"]
with open(f"{OUT}/resources.md", "w") as f:
    f.write("# Detection method resource comparison\n\n")
    f.write("| " + " | ".join(hdr) + " |\n|" + "---|" * len(hdr) + "\n")
    for r in rows:
        f.write(f"| {r['label']} | {r['cpu_mean_pct']:.0f} / {r['cpu_peak_pct']:.0f} | {r['rss_peak_mb']:.0f} "
                f"| {r['gpu_util_mean_pct']:.0f} / {r['gpu_util_peak_pct']:.0f} | {r['vram_peak_mb']:.0f} "
                f"| {r['ms_per_inf']} | {r['inf_per_s']} |\n")

# Chart
try:
    import matplotlib; matplotlib.use("Agg"); import matplotlib.pyplot as plt
    labels = [r["label"].replace(" · ", "\n") for r in rows]
    metrics = [("CPU % (mean)", [r["cpu_mean_pct"] for r in rows], "#d9534f"),
               ("GPU util % (mean)", [r["gpu_util_mean_pct"] for r in rows], "#5cb85c"),
               ("VRAM peak (MB)", [r["vram_peak_mb"] for r in rows], "#5bc0de"),
               ("RAM peak (MB)", [r["rss_peak_mb"] for r in rows], "#f0ad4e"),
               ("throughput (inf/s)", [r["inf_per_s"] or 0 for r in rows], "#337ab7")]
    fig, axes = plt.subplots(1, 5, figsize=(20, 4.6))
    for ax, (title, vals, col) in zip(axes, metrics):
        ax.bar(range(len(vals)), vals, color=col)
        ax.set_title(title, fontsize=11); ax.set_xticks(range(len(labels)))
        ax.set_xticklabels(labels, fontsize=7, rotation=0)
        for i, v in enumerate(vals): ax.text(i, v, f"{v:.0f}", ha="center", va="bottom", fontsize=7)
    fig.suptitle("zm-next detection methods — resource usage per process", fontsize=13)
    fig.tight_layout(rect=[0, 0, 1, 0.95]); fig.savefig(f"{OUT}/resources.png", dpi=110)
    print("wrote chart resources.png")
except Exception as e:
    print("chart skipped:", e)

print("\n=== results ===")
for r in rows:
    print(f"{r['label']:24s} CPU {r['cpu_mean_pct']:5.0f}%  GPU {r['gpu_util_mean_pct']:4.0f}%  "
          f"VRAM {r['vram_peak_mb']:5.0f}MB  RAM {r['rss_peak_mb']:5.0f}MB  {r['inf_per_s']} inf/s")
