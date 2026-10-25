// engine_selftest.cpp -- the check that was missing, plus the layered-engine
// checks the chain needs.
//
// Round 1 proved only that the engine RUNS. It never proved the engine CHANGES
// the picture -- and it did not, because the RGBA8->NV12 pass read the wrong
// texture. That is the "convert the input back" trap the engine's descriptor
// layout warns about; the end-to-end comparison still guards it here.
//
// Round 2 adds layers. What has to hold now:
//   1. the chain is REAL: 2 layers differ from 1 on the same input, and changing
//      ONLY layer 2's parameters changes the output again -- so layer 2 is an
//      independent pass on its own feature, not a no-op;
//   2. ONE FEATURE PER LAYER is stable: the flat-area level instability for 1
//      and 2 layers is measured, which is the number the docs quote;
//   3. the COST curve is sane: ms/frame for 1..4 layers (relative, short run).
//
// Everything runs in ONE process on purpose: NGX may only be initialised once
// per process, and the engine grows its feature set lazily, so changing the layer
// count between sections is exactly the runtime path the panel drives.

#define NOMINMAX            // windows.h min/max macros vs std::min/std::max
#include "../src/dlssnr_engine.h"

#include <windows.h>   // MultiByteToWideChar
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <chrono>
#include <vector>
#include <algorithm>
#include <string>

// ------------------------------------------------------------------ helpers

static std::wstring Widen(const char* s) {
    if (!s || !s[0]) return std::wstring();
    const int n = MultiByteToWideChar(CP_ACP, 0, s, -1, nullptr, 0);
    if (n <= 1) return std::wstring();
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_ACP, 0, s, -1, &w[0], n);
    w.resize((size_t)n - 1);
    return w;
}

static double MsSince(const std::chrono::steady_clock::time_point& t0) {
    return std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t0).count();
}

// A noisy test frame, so the denoiser has something to remove.
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

// A test frame that is ~70% FLAT. The flat rectangle carries luma 128 plus a
// FIXED +-1 dither, so the denoiser's temporal recursion has a static noisy
// neighbourhood to smooth -- which is exactly where a doubled loop gain shows up.
// The high-contrast band around the rectangle changes with `phase`; the flat
// interior does not depend on `phase`, so one `phase` = a static input.
static void makeFlatNv12(std::vector<unsigned char>& y, std::vector<unsigned char>& uv,
                         int w, int h, int phase,
                         int fx0, int fy0, int fx1, int fy1) {
    y.resize((size_t)w * h);
    uv.resize((size_t)w * h / 2);
    for (int j = 0; j < h; ++j) {
        for (int i = 0; i < w; ++i) {
            unsigned hsh = (unsigned)i * 2246822519u ^ (unsigned)j * 3266489917u;
            hsh ^= hsh >> 15;
            const bool flat = (i >= fx0 && i < fx1 && j >= fy0 && j < fy1);
            int c;
            if (flat) {
                c = 128 + (int)(hsh % 3u) - 1;            // fixed, zero-mean +-1
            } else {
                unsigned b = hsh + (unsigned)phase * 2654435761u;
                b ^= b >> 13;
                c = 24 + (int)(b % 208);
            }
            y[(size_t)j * w + i] = (unsigned char)(c < 0 ? 0 : (c > 255 ? 255 : c));
        }
    }
    for (size_t k = 0; k < uv.size(); ++k) uv[k] = 128;
}

static double RegionMean(const std::vector<unsigned char>& y, int w,
                         int x0, int y0, int x1, int y1) {
    double sum = 0.0;
    long long n = 0;
    for (int j = y0; j < y1; ++j)
        for (int i = x0; i < x1; ++i) { sum += y[(size_t)j * w + i]; ++n; }
    return n ? sum / (double)n : 0.0;
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

// One layer, in the engine's float units. Style is the only knob the tests vary.
static dlssnr::LayerOptions Layer(int style) {
    dlssnr::LayerOptions L;
    L.style = style; L.intensity = 1.0f; L.localTone = 1.0f;
    L.localStruct = 1.0f; L.skinStruct = 0.0f;
    L.autoMask = 0; L.uiCorrection = 0;
    return L;
}

// The whole chain, every layer on `style` (the cost/instability sections only
// care about the number of passes).
static dlssnr::Options OptsAllLayers(int n, int style) {
    dlssnr::Options o;
    o.layerCount = n;
    for (int i = 0; i < dlssnr::kMaxLayers; ++i) o.layers[i] = Layer(style);
    return o;
}

// Two layers, layer 1 fixed on style 1 and layer 2 on `layer2Style`.
static dlssnr::Options Opts2(int layer2Style) {
    dlssnr::Options o = OptsAllLayers(2, 1);
    o.layers[1] = Layer(layer2Style);
    return o;
}

// Runs `frames` frames and leaves the LAST output in outY/outUV. Frame 0 resets
// the temporal history so every capture starts from the same state.
static bool RunCapture(const std::vector<unsigned char>& inY,
                       const std::vector<unsigned char>& inUV, int W, int H, int frames,
                       std::vector<unsigned char>& outY,
                       std::vector<unsigned char>& outUV) {
    outY.assign((size_t)W * H, 0);
    outUV.assign((size_t)W * H / 2, 0);
    for (int i = 0; i < frames; ++i) {
        if (!dlssnr::ProcessNv12(inY.data(), W, inUV.data(), W,
                                 outY.data(), W, outUV.data(), W, i == 0)) return false;
    }
    return true;
}

int main(int argc, char** argv) {
    const int W = 1920, H = 1080;

    // argv[1] is the folder holding nvngx_dlssnr.dll. The old code had this
    // BACKWARDS: it ignored an argument that was present and used the default
    // when it was absent. Default to this repo's own app\ folder.
    std::wstring appDirBuf;
    if (argc > 1) appDirBuf = Widen(argv[1]);
    else          appDirBuf = L"D:\\VSCODE\\dlssnr-filter\\app";
    const wchar_t* appDir = appDirBuf.c_str();

    if (!dlssnr::Init(W, H, appDir, "engine_selftest.log")) {
        printf("FAIL: dlssnr::Init returned false (see engine_selftest.log)\n");
        return 2;
    }
    printf("engine ready at %dx%d (network %dx%d, scale %d%%)\n\n",
           dlssnr::Width(), dlssnr::Height(),
           dlssnr::ProcWidth(), dlssnr::ProcHeight(), dlssnr::ProcScalePercent());

    bool ok = true;
    auto verdict = [&](bool pass, const char* what) {
        printf("VERDICT: %-44s %s\n", what, pass ? "YES" : "NO  <-- FAILED");
        ok = ok && pass;
    };

    std::vector<unsigned char> inY, inUV, outY, outUV;
    makeNv12(inY, inUV, W, H, 12345);

    // ---------------------------------------------------------------- picture
    // Regression for the original bug: the engine must change the picture, and a
    // parameter change must change it again.
    printf("--- picture / parameters ---\n");
    dlssnr::SetOptions(OptsAllLayers(1, 1));
    if (!RunCapture(inY, inUV, W, H, 6, outY, outUV)) { printf("FAIL: ProcessNv12\n"); return 3; }
    std::vector<unsigned char> outStyle1 = outY;

    Diff vsInput = compare(inY, outStyle1, (size_t)W * H);
    printf("style=1 output vs INPUT   : mean|d| %.2f   max %d   %.1f%% of pixels differ by >2\n",
           vsInput.meanAbs, vsInput.maxAbs, vsInput.pctGt2);

    dlssnr::SetOptions(OptsAllLayers(1, 0));
    if (!RunCapture(inY, inUV, W, H, 6, outY, outUV)) { printf("FAIL: ProcessNv12\n"); return 3; }
    Diff vsStyle1 = compare(outStyle1, outY, (size_t)W * H);
    printf("style=0 output vs style=1 : mean|d| %.2f   max %d   %.1f%% of pixels differ by >2\n",
           vsStyle1.meanAbs, vsStyle1.maxAbs, vsStyle1.pctGt2);
    verdict(vsInput.meanAbs > 0.5 && vsInput.pctGt2 > 1.0,
            "the engine changes the picture");
    verdict(vsStyle1.meanAbs > 0.05 || vsStyle1.pctGt2 > 0.1,
            "parameters reach the network");

    // --------------------------------------------------------------- chaining
    printf("\n--- chaining (2 layers vs 1, layer 2 independence) ---\n");
    dlssnr::SetOptions(OptsAllLayers(1, 1));
    if (!RunCapture(inY, inUV, W, H, 8, outY, outUV)) { printf("FAIL: ProcessNv12\n"); return 3; }
    std::vector<unsigned char> outOne = outY;

    dlssnr::SetOptions(Opts2(2));                      // grow to 2 layers
    if (!RunCapture(inY, inUV, W, H, 8, outY, outUV)) { printf("FAIL: ProcessNv12\n"); return 3; }
    std::vector<unsigned char> outTwoA = outY;
    const int liveAfterGrow = dlssnr::ActiveLayers();

    dlssnr::SetOptions(Opts2(0));                      // change ONLY layer 2
    if (!RunCapture(inY, inUV, W, H, 8, outY, outUV)) { printf("FAIL: ProcessNv12\n"); return 3; }
    std::vector<unsigned char> outTwoB = outY;

    Diff twoVsOne = compare(outOne,  outTwoA, (size_t)W * H);
    Diff onlyL2   = compare(outTwoA, outTwoB, (size_t)W * H);
    printf("2 layers vs 1 layer       : mean|d| %.2f   max %d   %.1f%% of pixels differ by >2\n",
           twoVsOne.meanAbs, twoVsOne.maxAbs, twoVsOne.pctGt2);
    printf("only layer 2 changed      : mean|d| %.2f   max %d   %.1f%% of pixels differ by >2\n",
           onlyL2.meanAbs, onlyL2.maxAbs, onlyL2.pctGt2);
    printf("ActiveLayers() after growing to 2: %d (requested 2)\n", liveAfterGrow);
    verdict(twoVsOne.meanAbs > 0.10,
            "2 layers differ from 1");
    verdict(onlyL2.meanAbs > 0.05 || onlyL2.pctGt2 > 0.1,
            "layer 2 is an independent pass");
    verdict(liveAfterGrow == 2, "ActiveLayers() matches the live chain");

    // ------------------------------------------------------------ instability
    // Static input: the flat interior never changes, so any frame-to-frame
    // movement of its mean is the temporal recursion talking. Run 31 warm frames
    // (reset on the first), then measure 60 steady-state ones (reset=0).
    printf("\n--- flat-area instability, 60 steady-state frames (reset=0) ---\n");
    const int FX0 = 120, FY0 = 108, FX1 = 1800, FY1 = 972;   // ~70% of the frame
    std::vector<unsigned char> flatY, flatUV, foutY, foutUV;
    makeFlatNv12(flatY, flatUV, W, H, 7, FX0, FY0, FX1, FY1);

    double instStd[3] = {0,0,0}, instPP[3] = {0,0,0};
    for (int n = 1; n <= 2; ++n) {
        dlssnr::SetOptions(OptsAllLayers(n, 1));
        foutY.assign((size_t)W * H, 0);
        foutUV.assign((size_t)W * H / 2, 0);
        for (int i = 0; i < 61; ++i) {
            if (!dlssnr::ProcessNv12(flatY.data(), W, flatUV.data(), W,
                                     foutY.data(), W, foutUV.data(), W, i == 0)) {
                printf("FAIL: ProcessNv12 (instability)\n"); return 3;
            }
        }
        std::vector<double> m;
        m.reserve(60);
        for (int i = 0; i < 60; ++i) {
            if (!dlssnr::ProcessNv12(flatY.data(), W, flatUV.data(), W,
                                     foutY.data(), W, foutUV.data(), W, false)) {
                printf("FAIL: ProcessNv12 (instability)\n"); return 3;
            }
            m.push_back(RegionMean(foutY, W, FX0, FY0, FX1, FY1));
        }
        double sum = 0.0, mn = m[0], mx = m[0];
        for (double v : m) { sum += v; mn = std::min(mn, v); mx = std::max(mx, v); }
        const double mean = sum / (double)m.size();
        double var = 0.0;
        for (double v : m) var += (v - mean) * (v - mean);
        var /= (double)m.size();
        instStd[n] = sqrt(var);
        instPP[n]  = mx - mn;
        double first10 = 0.0, last10 = 0.0;
        for (int i = 0; i < 10; ++i) { first10 += m[i]; last10 += m[50 + i]; }
        printf("layers=%d: region mean %.3f   frame-to-frame std %.3f   peak-to-peak %.3f  (8-bit levels)\n",
               n, mean, instStd[n], instPP[n]);
        printf("           mean of first 10 / last 10 measured frames: %.3f / %.3f   (steady-state check)\n",
               first10 / 10.0, last10 / 10.0);
    }
    // One feature per layer must NOT multiply the loop gain. 3x the single-layer
    // value plus a small floor is generous: the toolkit measured 2-4x on one
    // feature, which fails this, while independent features stay near 1x.
    const double stdLimit = 0.05 + 3.0 * instStd[1];
    const double ppLimit  = 0.20 + 3.0 * instPP[1];
    printf("2-layer limit (3x 1-layer + floor): std <= %.3f, peak-to-peak <= %.3f\n",
           stdLimit, ppLimit);
    verdict(instStd[2] <= stdLimit && instPP[2] <= ppLimit,
            "one feature per layer stays stable");

    // ---------------------------------------------------------------- cost
    printf("\n--- cost curve, 1920x1080 (warm >=2 s, time 60 frames) ---\n");
    double ms[5] = {0,0,0,0,0};
    for (int n = 1; n <= 4; ++n) {
        dlssnr::SetOptions(OptsAllLayers(n, 1));
        // Warm for >= 2 s; the first ProcessNv12 also pays for creating any
        // missing feature. Short run with no long clock-ramp warmup, so these are
        // a RELATIVE curve, not the absolute table.
        auto tw = std::chrono::steady_clock::now();
        int warmFrames = 0;
        while (MsSince(tw) < 2000.0) {
            if (!dlssnr::ProcessNv12(inY.data(), W, inUV.data(), W,
                                     outY.data(), W, outUV.data(), W, warmFrames == 0)) {
                printf("FAIL: ProcessNv12 (warmup)\n"); return 3;
            }
            ++warmFrames;
        }
        auto t1 = std::chrono::steady_clock::now();
        for (int i = 0; i < 60; ++i) {
            if (!dlssnr::ProcessNv12(inY.data(), W, inUV.data(), W,
                                     outY.data(), W, outUV.data(), W, false)) {
                printf("FAIL: ProcessNv12 (timed)\n"); return 3;
            }
        }
        ms[n] = MsSince(t1) / 60.0;
        printf("layers=%d  %6.2f ms/frame   (%d warmup frames, %d live features)\n",
               n, ms[n], warmFrames, dlssnr::ActiveLayers());
    }
    verdict(ms[1] > 0.0 && ms[2] > ms[1] && ms[3] > ms[2] && ms[4] > ms[3],
            "cost grows with layer count");

    // ---------------------------------------------------------------- ceiling
    // The clamp is 1..kMaxLayers; the top of the range must actually run, since a
    // hand-edited ini can ask for it.
    printf("\n--- ceiling (layerCount=%d) ---\n", (int)dlssnr::kMaxLayers);
    dlssnr::SetOptions(OptsAllLayers((int)dlssnr::kMaxLayers, 1));
    if (!RunCapture(inY, inUV, W, H, 3, outY, outUV)) {
        printf("FAIL: %d-layer ProcessNv12\n", (int)dlssnr::kMaxLayers); ok = false;
    } else {
        Diff dN = compare(inY, outY, (size_t)W * H);
        printf("%d layers output vs INPUT : mean|d| %.2f   max %d   %.1f%% of pixels differ by >2\n",
               (int)dlssnr::kMaxLayers, dN.meanAbs, dN.maxAbs, dN.pctGt2);
        printf("ActiveLayers(): %d\n", dlssnr::ActiveLayers());
        verdict(dlssnr::ActiveLayers() == (int)dlssnr::kMaxLayers,
                "the 8-layer chain is live");
        verdict(dN.meanAbs > 0.5 && dN.pctGt2 > 1.0,
                "the 8-layer chain changes the picture");
    }

    // ------------------------------------------------------------ scaled path
    // The reduced-resolution path must run the WHOLE chain at the NETWORK size
    // while the frame in/out stays at the video size. Re-init at 50%: the engine
    // supports changing geometry in-process, and all layers run at 960x540.
    printf("\n--- scaled path (scalePercent=50, 2 layers) ---\n");
    // Set the chain length BEFORE Init: Init builds exactly g.opt.layerCount
    // features, and Shutdown inside Init releases the 1080p set first.
    dlssnr::SetOptions(OptsAllLayers(2, 1));
    if (!dlssnr::Init(W, H, appDir, "engine_selftest.log", 50)) {
        printf("FAIL: re-Init at 50%% failed (see engine_selftest.log)\n");
        ok = false;
    } else {
        printf("engine ready at %dx%d (network %dx%d, scale %d%%)\n",
               dlssnr::Width(), dlssnr::Height(),
               dlssnr::ProcWidth(), dlssnr::ProcHeight(), dlssnr::ProcScalePercent());
        if (!RunCapture(inY, inUV, W, H, 6, outY, outUV)) {
            printf("FAIL: scaled ProcessNv12\n"); ok = false;
        } else {
            Diff d = compare(inY, outY, (size_t)W * H);
            printf("scale=50 output vs INPUT : mean|d| %.2f   max %d   %.1f%% of pixels differ by >2\n",
                   d.meanAbs, d.maxAbs, d.pctGt2);
            printf("ActiveLayers() at 50%%: %d\n", dlssnr::ActiveLayers());
            verdict(dlssnr::ProcScalePercent() == 50, "scalePercent=50 was applied");
            verdict(dlssnr::ActiveLayers() == 2, "scaled chain has 2 features");
            verdict(d.meanAbs > 0.5 && d.pctGt2 > 1.0,
                    "scaled chain still changes the picture");
        }
    }

    // ------------------------------------------------------- model selection
    // WHICH network inside nvngx_dlssnr.dll the features are created with. The
    // preset is a feature-CREATION parameter, so this is the only check that can
    // tell "the selector works" from "the switch did nothing" -- the same class
    // of bug the picture/parameter checks above exist for.
    //
    // What is testable comes from the DLL itself, not from the file version:
    //   * 310.9.16 carries TWO networks (WEIGHTS_HT + WEIGHTS_CC_SMALL_FP8), so
    //     preset 1 and preset 2 must give DIFFERENT frames, and preset 2 must
    //     match the default (0) -- 0 means "the DLL's default", and that DLL's
    //     default IS the new lighter network;
    //   * 310.8.0 carries ONE, so every preset must give the SAME frame, which is
    //     the check that a preset the DLL cannot serve does not break the session
    //     (the engine also validates against the scan and falls back, and says so).
    printf("\n--- model preset (which network inside the DLL) ---\n");
    {
        dlssnr::DllWeight wts[dlssnr::kDllWeightMax];
        const int nw = dlssnr::DllWeights(wts, dlssnr::kDllWeightMax);
        wchar_t ver[32] = L"";
        dlssnr::DllVersion(ver, _countof(ver));
        printf("DLL %ls carries %d network(s):", ver[0] ? ver : L"?", nw);
        for (int i = 0; i < nw; ++i)
            printf(" %s(preset %d)", wts[i].name, wts[i].preset);
        printf("\n");

        dlssnr::SetOptions(OptsAllLayers(1, 1));
        std::vector<unsigned char> outModel[3];
        bool ran = true;
        for (int p = 0; p < 3; ++p) {
            if (!dlssnr::Init(W, H, appDir, "engine_selftest.log", 100, p)) {
                printf("FAIL: dlssnr::Init with preset %d (see engine_selftest.log)\n", p);
                ok = false; ran = false; break;
            }
            if (!RunCapture(inY, inUV, W, H, 6, outY, outUV)) {
                printf("FAIL: ProcessNv12 at preset %d\n", p);
                ok = false; ran = false; break;
            }
            outModel[p] = outY;
            printf("requested %d -> applied %d%s\n", p, dlssnr::ModelPreset(),
                   dlssnr::ModelFellBack() ? "   (the DLL refused it -> its default)" : "");
        }
        if (ran) {
            const Diff d01 = compare(outModel[0], outModel[1], (size_t)W * H);
            const Diff d02 = compare(outModel[0], outModel[2], (size_t)W * H);
            printf("preset 0 vs 1 : mean|d| %.3f  max %d  %.2f%% of pixels differ by >2\n",
                   d01.meanAbs, d01.maxAbs, d01.pctGt2);
            printf("preset 0 vs 2 : mean|d| %.3f  max %d  %.2f%% of pixels differ by >2\n",
                   d02.meanAbs, d02.maxAbs, d02.pctGt2);
            const bool same02 = (d02.maxAbs == 0);
            if (nw >= 2) {
                verdict(d01.meanAbs > 0.05 || d01.pctGt2 > 0.1,
                        "preset 1 runs a DIFFERENT network");
                verdict(same02, "preset 2 IS the DLL default (0)");
            } else {
                verdict(same02 && (d01.maxAbs == 0),
                        "one network: every preset gives the same frame");
            }
        }
    }

    printf("\n%s\n", ok ? "ALL CHECKS PASSED" : "SOME CHECKS FAILED");
    return ok ? 0 : 1;
}
