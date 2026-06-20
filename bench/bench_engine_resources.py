#!/usr/bin/env python3
"""Full performance comparison of the per-camera-session model vs the shared
batched engine, across camera counts. Samples CPU%, GPU util, VRAM, RAM and
throughput per process; emits a table and a chart."""
import subprocess, time, re, os, statistics as st, psutil
try:
    import pynvml; pynvml.nvmlInit(); H = pynvml.nvmlDeviceGetHandleByIndex(0); NVML = True
except Exception:
    NVML = False

ROOT = os.path.expanduser("~/code/zm-next"); VCPKG = os.path.expanduser("~/vcpkg")
CUDA = "/usr/local/cuda-12.9"; ORT = os.path.expanduser("~/onnxruntime")
env = dict(os.environ)
env["LD_LIBRARY_PATH"] = f"{VCPKG}/installed/x64-linux-dynamic/lib:{ORT}/lib:{CUDA}/lib64:" + env.get("LD_LIBRARY_PATH", "")
BIN = f"{ROOT}/build/bench/bench_engine"; MODEL = f"{ROOT}/bench/models/yolo26n.onnx"; OUT = f"{ROOT}/bench/samples"
COUNTS = [1, 2, 4, 8]

def run(cmd):
    p = subprocess.Popen(cmd, env=env, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
    proc = psutil.Process(p.pid); proc.cpu_percent(None)
    cpu, rss, gu, gv = [], [], [], []
    while p.poll() is None:
        try:
            kids = proc.children(recursive=True); pids = {proc.pid} | {k.pid for k in kids}
            cpu.append(proc.cpu_percent(None) + sum(k.cpu_percent(None) for k in kids))
            rss.append((proc.memory_info().rss + sum(k.memory_info().rss for k in kids)) / 1e6)
            if NVML:
                gu.append(pynvml.nvmlDeviceGetUtilizationRates(H).gpu)
                v = 0
                for pr in pynvml.nvmlDeviceGetComputeRunningProcesses(H):
                    if pr.pid in pids and pr.usedGpuMemory: v += pr.usedGpuMemory
                gv.append(v / 1e6)
        except Exception: break
        time.sleep(0.2)
    out, _ = p.communicate()
    m = re.search(r"throughput\s*:\s*([\d.]+)", out)
    ips = float(m.group(1)) if m else 0
    pk = lambda a: max(a) if a else 0; mn = lambda a: st.mean(a) if a else 0
    cs = cpu[6:] or cpu  # drop the (longer) warmup samples
    return dict(cpu=mn(cs), rss=pk(rss), gu=mn(gu), vram=pk(gv), ips=ips)

rows = []
print(f"{'cameras':8} {'mode':11} {'CPU%':>6} {'GPU%':>6} {'VRAM MB':>8} {'RAM MB':>8} {'inf/s':>7}")
for n in COUNTS:
    for mode in ["per-thread", "shared"]:
        cmd = [BIN, "--model", MODEL, "--threads", str(n), "--seconds", "5"]
        cmd += ["--per-thread", "--max-batch", "1"] if mode == "per-thread" else ["--max-batch", "8", "--wait-us", "2000"]
        r = run(cmd); r["n"] = n; r["mode"] = mode; rows.append(r)
        print(f"{n:<8} {mode:11} {r['cpu']:6.0f} {r['gu']:6.0f} {r['vram']:8.0f} {r['rss']:8.0f} {r['ips']:7.0f}")

try:
    import matplotlib; matplotlib.use("Agg"); import matplotlib.pyplot as plt
    pt = [r for r in rows if r["mode"] == "per-thread"]; sh = [r for r in rows if r["mode"] == "shared"]
    fig, ax = plt.subplots(1, 3, figsize=(15, 4.4))
    for a, key, title in [(ax[0], "vram", "VRAM (MB)"), (ax[1], "rss", "RAM (MB)"), (ax[2], "ips", "throughput (inf/s)")]:
        a.plot(COUNTS, [r[key] for r in pt], "o-", color="#d9534f", label="per-camera session")
        a.plot(COUNTS, [r[key] for r in sh], "s-", color="#337ab7", label="shared engine")
        a.set_title(title); a.set_xlabel("cameras (threads)"); a.set_xticks(COUNTS); a.grid(alpha=0.3); a.legend()
    fig.suptitle("Per-camera sessions vs shared batched engine", fontsize=13)
    fig.tight_layout(rect=[0, 0, 1, 0.94]); fig.savefig(f"{OUT}/engine_scaling.png", dpi=110)
    print("wrote chart engine_scaling.png")
except Exception as e:
    print("chart skipped:", e)
