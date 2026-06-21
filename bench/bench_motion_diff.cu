// bench_motion_diff: A/B the motion-gate DIFF step (everything after the shared
// launch_luma_grid downsample, which both approaches share and is excluded).
//
//   A (current): cudaMemcpy(grid D2H)  +  CPU diff loop (count changed + bbox)
//   B (on-GPU):  diff kernel on device +  cudaMemcpy(verdict D2H, ~20 B)
//                prev_grid stays on the device (ping-pong, no copy)
//
// Reports per-frame time (median) and PCIe bytes for each, across grid sizes
// matching real 4K downsample factors. Honest about kernel-launch overhead.
#include <cuda_runtime.h>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <vector>
#include <algorithm>
#include <string>
#include <chrono>

using clk = std::chrono::high_resolution_clock;
static double ms(clk::time_point a, clk::time_point b){
    return std::chrono::duration<double, std::milli>(b - a).count();
}

struct Verdict { int cnt, minx, miny, maxx, maxy; };

// One thread per cell; atomics accumulate changed-count + bbox into a device verdict.
__global__ void diff_kernel(const uint8_t* cur, const uint8_t* prev,
                            int sw, int sh, int thr, Verdict* v){
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    int total = sw * sh;
    if (i >= total) return;
    int d = abs((int)cur[i] - (int)prev[i]);
    if (d > thr){
        int x = i % sw, y = i / sw;
        atomicAdd(&v->cnt, 1);
        atomicMin(&v->minx, x); atomicMin(&v->miny, y);
        atomicMax(&v->maxx, x); atomicMax(&v->maxy, y);
    }
}

static Verdict cpu_diff(const uint8_t* cur, const uint8_t* prev, int sw, int sh, int thr){
    Verdict v{0, sw, sh, -1, -1};
    for (int j = 0; j < sh; ++j)
        for (int i = 0; i < sw; ++i){
            int d = abs((int)cur[j*sw+i] - (int)prev[j*sw+i]);
            if (d > thr){ ++v.cnt;
                v.minx = std::min(v.minx,i); v.miny = std::min(v.miny,j);
                v.maxx = std::max(v.maxx,i); v.maxy = std::max(v.maxy,j); }
        }
    return v;
}

static double median(std::vector<double>& v){
    std::sort(v.begin(), v.end()); return v[v.size()/2];
}

int main(){
    const int thr = 18, ITERS = 3000;
    // grid sizes = 4K (3840x2160) at downsample ds = 64,32,16,8
    int dss[] = {64,32,16,8};
    printf("4K source, diff step only (shared downsample excluded). thr=%d, %d iters\n\n", thr, ITERS);
    printf("%-12s %8s | %-22s | %-26s | %s\n","grid","cells",
           "A: CPU diff (+grid copy)","B: GPU diff (+verdict copy)","winner");
    printf("%s\n", std::string(95,'-').c_str());

    for (int ds : dss){
        int sw = 3840/ds, sh = 2160/ds, n = sw*sh;
        std::vector<uint8_t> hcur(n), hprev(n);
        srand(1234);
        for (int k=0;k<n;++k){ hprev[k]=rand()&0xFF;
            hcur[k] = (rand()%100 < 12) ? (uint8_t)(hprev[k]^0x60) : hprev[k]; } // ~12% changed

        uint8_t *dcur,*dprev; Verdict *dv;
        cudaMalloc(&dcur,n); cudaMalloc(&dprev,n); cudaMalloc(&dv,sizeof(Verdict));
        cudaMemcpy(dcur,hcur.data(),n,cudaMemcpyHostToDevice);
        cudaMemcpy(dprev,hprev.data(),n,cudaMemcpyHostToDevice);
        Verdict vinit{0,sw,sh,-1,-1};

        // ---- A: copy grid D2H + CPU diff ----
        std::vector<uint8_t> back(n); Verdict va{};
        std::vector<double> ta;
        for (int it=-200; it<ITERS; ++it){
            auto t0=clk::now();
            cudaMemcpy(back.data(), dcur, n, cudaMemcpyDeviceToHost);
            va = cpu_diff(back.data(), hprev.data(), sw, sh, thr);
            auto t1=clk::now();
            if (it>=0) ta.push_back(ms(t0,t1));
        }

        // ---- B: device diff kernel + verdict copy ----
        Verdict vb{}; std::vector<double> tb;
        int blk=256, grd=(n+blk-1)/blk;
        for (int it=-200; it<ITERS; ++it){
            auto t0=clk::now();
            cudaMemcpy(dv,&vinit,sizeof(Verdict),cudaMemcpyHostToDevice);
            diff_kernel<<<grd,blk>>>(dcur,dprev,sw,sh,thr,dv);
            cudaMemcpy(&vb,dv,sizeof(Verdict),cudaMemcpyDeviceToHost); // blocks => syncs
            auto t1=clk::now();
            if (it>=0) tb.push_back(ms(t0,t1));
        }

        double ma=median(ta)*1000.0, mb=median(tb)*1000.0; // us
        bool match = (va.cnt==vb.cnt && va.minx==vb.minx && va.maxx==vb.maxx);
        const char* win = ma<mb ? "CPU" : "GPU";
        printf("%dx%-9d %8d | %7.1f us  %6d B | %7.1f us  %5zu B | %s%s\n",
               sw,sh,n, ma,n, mb,sizeof(Verdict), win, match?"":"  [MISMATCH!]");
        cudaFree(dcur); cudaFree(dprev); cudaFree(dv);
    }
    printf("\nPCIe per frame: A copies the whole grid back; B copies only a %zu-byte verdict.\n", sizeof(Verdict));
    return 0;
}
