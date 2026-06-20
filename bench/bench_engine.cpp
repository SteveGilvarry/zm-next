// bench_engine — prove the shared batched InferenceEngine. Spawns N producer
// threads (simulating N cameras), each repeatedly submitting a preprocessed GPU
// tensor to ONE shared engine. Reports throughput, the average batch size the
// dispatcher coalesced, and the VRAM of the single shared session/context.
//
// Compare --max-batch 1 (shared session, no batching ~ today's per-thread model)
// vs --max-batch 8 (concurrent requests fused into one Run).
//
//   bench_engine --model dyn.onnx --threads 8 --seconds 5 --max-batch 8 --wait-us 2000

#include "detect_engine.hpp"

#include <cuda_runtime.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

using namespace zm::detect;

int main(int argc, char** argv) {
    std::string model;
    int net = 640, threads = 4, secs = 5, maxb = 8, waitus = 2000;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i]; auto nx = [&] { return (i + 1 < argc) ? argv[++i] : ""; };
        if (a == "--model") model = nx(); else if (a == "--threads") threads = std::atoi(nx());
        else if (a == "--seconds") secs = std::atoi(nx()); else if (a == "--max-batch") maxb = std::atoi(nx());
        else if (a == "--wait-us") waitus = std::atoi(nx()); else if (a == "--net") net = std::atoi(nx());
    }
    if (model.empty()) { fprintf(stderr, "need --model (dynamic-batch onnx)\n"); return 2; }

    auto& eng = InferenceEngine::get(model, net, maxb, waitus);
    const Letterbox lb = compute_letterbox(net, net, net);

    std::atomic<long> total{0};
    std::atomic<bool> running{true};
    std::vector<std::thread> ts;
    for (int i = 0; i < threads; ++i)
        ts.emplace_back([&] {
            float* d = nullptr;
            if (cudaMalloc(&d, static_cast<size_t>(3) * net * net * sizeof(float)) != cudaSuccess) return;
            cudaMemset(d, 0, static_cast<size_t>(3) * net * net * sizeof(float));
            std::vector<int> allow;
            while (running.load()) { auto b = eng.infer(d, lb, 0.25f, allow); (void)b; total.fetch_add(1); }
            cudaFree(d);
        });

    std::this_thread::sleep_for(std::chrono::milliseconds(300));  // warmup
    eng.runs(); // (counters already moving; we time a clean window next)
    const long runs0 = eng.runs(), items0 = eng.items();
    const long total0 = total.load();
    auto t0 = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(std::chrono::seconds(secs));
    const double dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const long did = total.load() - total0;
    const long runs = eng.runs() - runs0, items = eng.items() - items0;
    running = false;
    for (auto& t : ts) t.join();

    size_t freeB = 0, totB = 0; cudaMemGetInfo(&freeB, &totB);
    printf("threads=%d  max_batch=%d  wait=%dus\n", threads, maxb, waitus);
    printf("throughput     : %.0f inf/s  (%ld inferences in %.1fs)\n", did / dt, did, dt);
    printf("batched Runs   : %ld   avg batch size: %.2f\n", runs, runs ? static_cast<double>(items) / runs : 0.0);
    printf("VRAM (1 shared session+context): %.0f MB\n", (totB - freeB) / 1e6);
    return 0;
}
