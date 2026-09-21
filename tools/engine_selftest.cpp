// engine_selftest.cpp -- the check that was missing.
//
// The first round of testing only proved that the engine RUNS (frame counters
// advance, ms/frame is non-zero, playback hits its frame rate). It never proved
// the engine CHANGES the picture -- and it did not: the RGBA8->NV12 pass was
// reading the wrong texture, so the output was a faithful round trip of the
// input. Frame rate looked perfect and the effect was absent.
//
// This test closes that gap: it feeds a noisy NV12 frame through the engine and
// asserts that (a) the output differs from the input, and (b) changing a
// parameter changes the output again.

#include "dlssnr_engine.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <vector>
#include <algorithm>

static void makeNv12(std::vector<unsigned char>& y, std::vector<unsigned char>& uv,
                     int w, int h, int seed) {
    y.resize((size_t)w * h);
    uv.resize((size_t)w * h / 2);
    unsigned s = (unsigned)seed;
    for (int j = 0; j < h; ++j) {
        for (int i = 0; i < w; ++i) {
            double v = 128.0
                     + 30.0 * sin(i / 37.0) * sin(j / 41.0)
                     + 25.0 * sin((i + j) / 13.0);
            s = s * 1103515245u + 12345u;
            v += ((int)(s >> 16) % 41) - 20;              // grain, so a denoiser has work
            int c = (int)(v + 0.5);
            y[(size_t)j * w + i] = (unsigned char)(c < 0 ? 0 : (c > 255 ? 255 : c));
        }
    }
    for (int j = 0; j < h / 2; ++j)
        for (int i = 0; i < w / 2; ++i) {
            uv[((size_t)j * w / 2 + i) * 2 + 0] = (unsigned char)(128 + 20 * sin(i / 50.0));
            uv[((size_t)j * w / 2 + i) * 2 + 1] = (unsigned char)(128 + 20 * cos(j / 45.0));
        }
}

struct Diff { double meanAbs; int maxAbs; double pctGt2; };

static Diff compare(const std::vector<unsigned char>& a, const std::vector<unsigned char>& b,
                    size_t n) {
    Diff d{ 0.0, 0, 0.0 };
    size_t big = 0;
    for (size_t i = 0; i < n; ++i) {
        int delta = abs((int)a[i] - (int)b[i]);
        d.meanAbs += delta;
        if (delta > d.maxAbs) d.maxAbs = delta;
        if (delta > 2) ++big;
    }
    d.meanAbs /= (double)n;
    d.pctGt2 = 100.0 * (double)big / (double)n;
    return d;
}

int main(int argc, char** argv) {
    const int W = 1920, H = 1080;
    const wchar_t* appDir = (argc > 1) ? nullptr : L"D:\\VSCODE\\dlssnr-filter\\app";

    std::vector<unsigned char> inY, inUV, outY, outUV;
    makeNv12(inY, inUV, W, H, 12345);
    outY.assign((size_t)W * H, 0);          // the engine writes into these
    outUV.assign((size_t)W * H / 2, 0);

    if (!dlssnr::Init(W, H, appDir, "engine_selftest.log")) {
        printf("FAIL: dlssnr::Init returned false (see engine_selftest.log)\n");
        return 2;
    }
    printf("engine ready at %dx%d\n\n", dlssnr::Width(), dlssnr::Height());

    dlssnr::Options o;
    o.style = 1; o.intensity = 1.0f; o.localTone = 1.0f; o.localStruct = 1.0f;
    o.skinStruct = 0.0f; o.autoMask = 0;
    dlssnr::SetOptions(o);

    // a few frames so the temporal history is warm, then keep the last output
    for (int k = 0; k < 4; ++k) {
        if (!dlssnr::ProcessNv12(inY.data(), W, inUV.data(), W,
                                 outY.data(), W, outUV.data(), W, false)) {
            printf("FAIL: ProcessNv12 returned false\n");
            return 3;
        }
    }
    std::vector<unsigned char> style1 = outY;

    Diff vsInput = compare(inY, style1, (size_t)W * H);
    printf("style=1 output vs INPUT   : mean|d| %.2f   max %d   %.1f%% of pixels differ by >2\n",
           vsInput.meanAbs, vsInput.maxAbs, vsInput.pctGt2);

    o.style = 0;
    dlssnr::SetOptions(o);
    for (int k = 0; k < 4; ++k)
        dlssnr::ProcessNv12(inY.data(), W, inUV.data(), W, outY.data(), W, outUV.data(), W, false);
    Diff vsStyle1 = compare(style1, outY, (size_t)W * H);
    printf("style=0 output vs style=1 : mean|d| %.2f   max %d   %.1f%% of pixels differ by >2\n",
           vsStyle1.meanAbs, vsStyle1.maxAbs, vsStyle1.pctGt2);

    printf("\n");
    bool applied = (vsInput.meanAbs > 0.5) && (vsInput.pctGt2 > 1.0);
    bool paramsWork = (vsStyle1.meanAbs > 0.05) || (vsStyle1.pctGt2 > 0.1);
    printf("VERDICT: the engine changes the picture ....... %s\n", applied ? "YES" : "NO  <-- effect is absent");
    printf("         parameters reach the network .......... %s\n", paramsWork ? "YES" : "NO  <-- params ignored");
    return (applied && paramsWork) ? 0 : 1;
}
