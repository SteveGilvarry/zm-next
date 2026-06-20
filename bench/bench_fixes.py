#!/usr/bin/env python3
"""Prove the two CPU fixes on GPU-4K-full: dynamic model -> fixed-shape model ->
fixed-shape + blocking sync. Samples CPU%, GPU%, RAM and throughput per variant."""
import subprocess, time, re, os, statistics as st, psutil
try:
    import pynvml; pynvml.nvmlInit(); H = pynvml.nvmlDeviceGetHandleByIndex(0); NVML = True
except Exception:
    NVML = False

ROOT = os.path.expanduser("~/code/zm-next"); VCPKG = os.path.expanduser("~/vcpkg")
CUDA = "/usr/local/cuda-12.9"; ORT = os.path.expanduser("~/onnxruntime")
env = dict(os.environ)
env["LD_LIBRARY_PATH"] = f"{VCPKG}/installed/x64-linux-dynamic/lib:{ORT}/lib:{CUDA}/lib64:" + env.get("LD_LIBRARY_PATH", "")
GPU = f"{ROOT}/build/bench/bench_gpu_roi"; PLUG = f"{ROOT}/build/plugins"; C4K = f"{ROOT}/bench/clips/sync_4k.mp4"
DYN = f"{ROOT}/bench/models/yolo26n.onnx"; STAT = f"{ROOT}/bench/models/yolo26n_static.onnx"

def run(cmd):
    p = subprocess.Popen(cmd, env=env, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
    proc = psutil.Process(p.pid); proc.cpu_percent(None)
    cpu, rss, gu = [], [], []
    while p.poll() is None:
        try:
            kids = proc.children(recursive=True)
            cpu.append(proc.cpu_percent(None) + sum(k.cpu_percent(None) for k in kids))
            rss.append((proc.memory_info().rss + sum(k.memory_info().rss for k in kids)) / 1e6)
            if NVML: gu.append(pynvml.nvmlDeviceGetUtilizationRates(H).gpu)
        except Exception: break
        time.sleep(0.15)
    out, _ = p.communicate()
    ms = None
    for l in out.splitlines():
        if l.startswith("METHOD"):
            m = re.search(r"ms_per_inf=([\d.]+)", l)
            if m: ms = float(m.group(1))
    cs = cpu[2:] or cpu
    return (st.mean(cs) if cs else 0, st.mean(gu) if gu else 0, max(rss) if rss else 0, round(1000 / ms) if ms else None)

def cmd(model, blocking):
    c = [GPU, "--input", C4K, "--model", model, "--plugins", PLUG, "--only", "full", "--loops", "8"]
    if blocking: c.append("--blocking-sync")
    return c

print(f"{'variant':34s} {'CPU%':>6} {'GPU%':>6} {'RAM MB':>8} {'inf/s':>7}")
for label, model, bl in [("dynamic model (baseline)", DYN, False),
                          ("fixed-shape model", STAT, False),
                          ("fixed-shape + blocking-sync", STAT, True)]:
    cpu, gu, rss, ips = run(cmd(model, bl))
    print(f"{label:34s} {cpu:6.0f} {gu:6.0f} {rss:8.0f} {str(ips):>7}")
