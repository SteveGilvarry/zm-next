// Validate cuda_motion_bbox_gpudiff (fully GPU-resident) against the host
// cuda_motion_bbox_cpudiff on synthetic luma frames with known motion, then time both on
// a realistic grid. Uses the REAL plugin functions (links detect_cuda.{cu,cpp}).
#include "detect_cuda.hpp"
#include <cuda_runtime.h>
#include <cstdio>
#include <cstdint>
#include <vector>
#include <cstdlib>
#include <chrono>
#include <algorithm>

using namespace zm::detect;
using clk = std::chrono::high_resolution_clock;
static double us(clk::time_point a, clk::time_point b){
    return std::chrono::duration<double, std::micro>(b - a).count();
}

// Make a width*height luma frame: flat background, optional bright rectangle.
static std::vector<uint8_t> make_frame(int w, int h, int rx, int ry, int rw, int rh, uint8_t base, uint8_t fg){
    std::vector<uint8_t> f(static_cast<size_t>(w)*h, base);
    for (int y = ry; y < ry+rh && y < h; ++y)
        for (int x = rx; x < rx+rw && x < w; ++x) f[static_cast<size_t>(y)*w + x] = fg;
    return f;
}

static bool eq(const MotionRoi& a, const MotionRoi& b){
    return a.active==b.active && a.x==b.x && a.y==b.y && a.w==b.w && a.h==b.h && a.changed==b.changed;
}

int main(){
    const int W=3840, H=2160;
    uint8_t* d_y; cudaMalloc(&d_y, static_cast<size_t>(W)*H);
    int fails = 0, cases = 0;

    // ---- correctness: across ds, frame B moves a block; host vs device must match ----
    printf("=== correctness: host cuda_motion_bbox_cpudiff vs cuda_motion_bbox_gpudiff ===\n");
    for (int ds : {64,32,16,8}) {
        for (int jump : {0, 40}) {
            auto A = make_frame(W,H, 0,0,0,0, 100, 100);                 // flat
            auto B = make_frame(W,H, 1200,800, 600,500, 100, 220);       // moved bright block
            // host path
            std::vector<uint8_t> prev; float hm=0; MotionRoi h0,h1;
            cudaMemcpy(d_y, A.data(), A.size(), cudaMemcpyHostToDevice);
            h0 = cuda_motion_bbox_cpudiff(reinterpret_cast<uint64_t>(d_y), W, W, H, prev, ds, 18, 4, jump, &hm);
            cudaMemcpy(d_y, B.data(), B.size(), cudaMemcpyHostToDevice);
            h1 = cuda_motion_bbox_cpudiff(reinterpret_cast<uint64_t>(d_y), W, W, H, prev, ds, 18, 4, jump, &hm);
            // device path
            GpuDiffState* st = gpudiff_state_create(); MotionRoi d0,d1;
            cudaMemcpy(d_y, A.data(), A.size(), cudaMemcpyHostToDevice);
            d0 = cuda_motion_bbox_gpudiff(reinterpret_cast<uint64_t>(d_y), W, W, H, st, ds, 18, 4, jump);
            cudaMemcpy(d_y, B.data(), B.size(), cudaMemcpyHostToDevice);
            d1 = cuda_motion_bbox_gpudiff(reinterpret_cast<uint64_t>(d_y), W, W, H, st, ds, 18, 4, jump);
            gpudiff_state_destroy(st);
            bool ok = eq(h1,d1);
            ++cases; if(!ok) ++fails;
            printf("  ds=%-3d jump=%-3d | host{act=%d x=%d y=%d w=%d h=%d chg=%d} dev{act=%d x=%d y=%d w=%d h=%d chg=%d} %s\n",
                   ds, jump, h1.active,h1.x,h1.y,h1.w,h1.h,h1.changed,
                   d1.active,d1.x,d1.y,d1.w,d1.h,d1.changed, ok?"MATCH":"*** MISMATCH ***");
        }
    }

    // ---- timing on a realistic grid (ds=16 -> 240x135) over many frames ----
    printf("\n=== timing (ds=16, 240x135 grid, 2000 frames, alternating frames) ===\n");
    const int ds=16, ITER=2000;
    auto A = make_frame(W,H, 0,0,0,0, 100,100);
    auto B = make_frame(W,H, 1200,800, 600,500, 100,220);
    // host
    { std::vector<uint8_t> prev; float hm=0; double t=0;
      for(int i=0;i<ITER;++i){ auto& F=(i&1)?B:A; cudaMemcpy(d_y,F.data(),F.size(),cudaMemcpyHostToDevice);
        auto t0=clk::now(); cuda_motion_bbox_cpudiff(reinterpret_cast<uint64_t>(d_y),W,W,H,prev,ds,18,4,0,&hm);
        auto t1=clk::now(); if(i>50) t+=us(t0,t1); }
      printf("  HOST  (downsample+grid copy+CPU diff): %.2f us/frame\n", t/(ITER-51));
    }
    // device
    { GpuDiffState* st=gpudiff_state_create(); double t=0;
      for(int i=0;i<ITER;++i){ auto& F=(i&1)?B:A; cudaMemcpy(d_y,F.data(),F.size(),cudaMemcpyHostToDevice);
        auto t0=clk::now(); cuda_motion_bbox_gpudiff(reinterpret_cast<uint64_t>(d_y),W,W,H,st,ds,18,4,0);
        auto t1=clk::now(); if(i>50) t+=us(t0,t1); }
      gpudiff_state_destroy(st);
      printf("  DEVICE(downsample+diff on GPU, 24B back): %.2f us/frame\n", t/(ITER-51));
    }
    cudaFree(d_y);
    printf("\n%d/%d cases matched.\n", cases-fails, cases);
    return fails ? 1 : 0;
}
