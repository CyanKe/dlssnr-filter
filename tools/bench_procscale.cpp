// bench_procscale.cpp -- what does running the network at HALF resolution cost,
// and what does it buy?
//
// The filter's drop policy trades frame rate for spatial quality. This measures
// the other trade: the network runs on a 2:1-downscaled frame (a quarter of the
// pixels) and the result is upscaled back, so the frame rate fits but every
// frame is softer than what the source actually had. Both numbers decide whether
// that is worth a switch in the panel.
//
// Method follows docs\ENGINEERING_NOTES.md section 3: each configuration gets a
// long warm-up first, because this machine sits at ~1.3 GHz until it has been
// under load for tens of seconds and only then ramps to ~2.4 GHz -- sampling
// right after start measures the cold clock, up to 1.5x off.
//
// Usage: bench_procscale [appDir] [width] [height] [warmSeconds]
//        (appDir defaults to "app", geometry to 3840x2160, warm-up to 20 s)

#include "dlssnr_engine.h"

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <vector>
#include <algorithm>

static double NowMs() {
    static LARGE_INTEGER f{}; static bool init = false;
    if (!init) { QueryPerformanceFrequency(&f); init = true; }
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

// A frame with real detail (smooth gradients + hard edges + grain): the softness
// difference between the two paths has to show up in the metrics below.
static void makeNv12(std::vector<unsigned char>& y, std::vector<unsigned char>& uv,
                     int w, int h) {
    y.resize((size_t)w * h);
    uv.resize((size_t)w * h / 2);
    unsigned s = 0x12345u;
    for (int j = 0; j < h; ++j) {
        for (int i = 0; i < w; ++i) {
            double v = 110.0 + 40.0 * sin(i / 29.0) * sin(j / 31.0)
                             + 18.0 * sin((i + j) / 7.0);
            if ((((i / 64) + (j / 64)) & 1) != 0) v += 22.0;      // hard edges
            s = s * 1103515245u + 12345u;
            v += ((int)(s >> 16) % 41) - 20;                      // grain
            int c = (int)(v + 0.5);
            y[(size_t)j * w + i] = (unsigned char)(c < 0 ? 0 : (c > 255 ? 255 : c));
        }
    }
    // Big saturated COLOUR PATCHES, not a smooth tint: if the chroma is scaled
    // around the wrong origin (or clamped to an edge), patches land in the wrong
    // place and the picture visibly loses its colour -- the bug this pattern is
    // here to catch. A slow gradient would hide both.
    for (int j = 0; j < h / 2; ++j)
        for (int i = 0; i < w / 2; ++i) {
            const int bx = (i * 4) / (w / 2), by = (j * 4) / (h / 2);   // 4x4 patches
            const int k = (by * 4 + bx) % 8;
            static const int CU[8] = { 128,  90, 160,  90, 160, 128,  80, 176 };
            static const int CV[8] = { 128, 160,  90,  80, 176, 100, 128, 150 };
            uv[((size_t)j * w / 2 + i) * 2 + 0] = (unsigned char)CU[k];
            uv[((size_t)j * w / 2 + i) * 2 + 1] = (unsigned char)CV[k];
        }
}

static double MeanAbsDiff(const std::vector<unsigned char>& a,
                          const std::vector<unsigned char>& b, size_t n) {
    double acc = 0.0;
    for (size_t i = 0; i < n; ++i) acc += abs((int)a[i] - (int)b[i]);
    return acc / (double)n;
}

// Mean |4c - l - r - u - d| over the luma plane: a detail/energy proxy. Denoising
// lowers it a little; upscaling from half resolution lowers it a lot.
static double LaplacianEnergy(const std::vector<unsigned char>& y, int w, int h) {
    double acc = 0.0; size_t n = 0;
    for (int j = 1; j < h - 1; ++j)
        for (int i = 1; i < w - 1; ++i) {
            int c = y[(size_t)j * w + i];
            int l = y[(size_t)j * w + i - 1], r = y[(size_t)j * w + i + 1];
            int u = y[(size_t)(j - 1) * w + i], d = y[(size_t)(j + 1) * w + i];
            acc += abs(4 * c - l - r - u - d);
            ++n;
        }
    return n ? acc / (double)n : 0.0;
}

// Mean |gx| + |gy|: a fairer detail proxy than the Laplacian, which a bilinear
// upscale drives towards zero by construction (a linear ramp has no curvature).
static double GradientEnergy(const std::vector<unsigned char>& y, int w, int h) {
    double acc = 0.0; size_t n = 0;
    for (int j = 1; j < h - 1; ++j)
        for (int i = 1; i < w - 1; ++i) {
            acc += abs((int)y[(size_t)j * w + i + 1] - (int)y[(size_t)j * w + i - 1])
                 + abs((int)y[(size_t)(j + 1) * w + i] - (int)y[(size_t)(j - 1) * w + i]);
            ++n;
        }
    return n ? acc / (double)n : 0.0;
}

static double MeanY(const std::vector<unsigned char>& y, size_t n) {
    double acc = 0.0;
    for (size_t i = 0; i < n; ++i) acc += y[i];
    return acc / (double)n;
}

// CPU mirror of the scaled path's two resampling steps -- area-average down,
// bilinear up, at ANY ratio -- on the luma plane, with no network involved.
// Comparing a scaled OUTPUT against this reference isolates what the network did
// from what the resampling alone did; the difference is the denoising, and for a
// usable mode it has to stay in the ballpark of the native path's.
static std::vector<unsigned char> ResampleCpu(const std::vector<unsigned char>& y,
                                             int w, int h, int sw, int sh) {
    auto overlap = [](int i, double a, double b) {
        const double lo = i > a ? (double)i : a;
        const double hi = (i + 1.0) < b ? (i + 1.0) : b;
        return hi > lo ? hi - lo : 0.0;
    };
    std::vector<float> ds((size_t)sw * sh, 0.0f);
    const double sx = (double)w / sw, sy = (double)h / sh;
    for (int j = 0; j < sh; ++j)
        for (int i = 0; i < sw; ++i) {
            const double xa = i * sx, xb = xa + sx, ya = j * sy, yb = ya + sy;
            double acc = 0.0, wsum = 0.0;
            for (int v = (int)floor(ya); v < (int)ceil(yb); ++v) {
                const double wy = overlap(v, ya, yb);
                if (wy <= 0.0) continue;
                const int cy = v < 0 ? 0 : (v >= h ? h - 1 : v);
                for (int u = (int)floor(xa); u < (int)ceil(xb); ++u) {
                    const double wx = overlap(u, xa, xb);
                    if (wx <= 0.0) continue;
                    const int cx = u < 0 ? 0 : (u >= w ? w - 1 : u);
                    acc  += wx * wy * ((double)y[(size_t)cy * w + cx] / 255.0);
                    wsum += wx * wy;
                }
            }
            ds[(size_t)j * sw + i] = (float)(acc / (wsum > 0.0 ? wsum : 1.0));
        }

    std::vector<unsigned char> up((size_t)w * h);
    const double rx = (double)sw / w, ry = (double)sh / h;
    for (int j = 0; j < h; ++j)
        for (int i = 0; i < w; ++i) {
            const double px = (i + 0.5) * rx - 0.5, py = (j + 0.5) * ry - 0.5;
            const double fx = floor(px), fy = floor(py);
            const double tx = px - fx, ty = py - fy;
            int x0 = (int)fx, y0 = (int)fy, x1 = x0 + 1, y1 = y0 + 1;
            x0 = x0 < 0 ? 0 : (x0 > sw - 1 ? sw - 1 : x0);
            y0 = y0 < 0 ? 0 : (y0 > sh - 1 ? sh - 1 : y0);
            x1 = x1 < 0 ? 0 : (x1 > sw - 1 ? sw - 1 : x1);
            y1 = y1 < 0 ? 0 : (y1 > sh - 1 ? sh - 1 : y1);
            const double a = ds[(size_t)y0 * sw + x0], b = ds[(size_t)y0 * sw + x1];
            const double c = ds[(size_t)y1 * sw + x0], d = ds[(size_t)y1 * sw + x1];
            const double val = ((a * (1 - tx) + b * tx) * (1 - ty)
                              + (c * (1 - tx) + d * tx) * ty) * 255.0;
            up[(size_t)j * w + i] = (unsigned char)(val + 0.5);
        }
    return up;
}

// Dump a crop of the luma plane as a 24-bit BMP, so the two paths can be LOOKED
// at instead of argued about: numbers say how much moved, not whether it looks
// right. 1:1 crop on purpose -- a downscaled full frame hides exactly the
// difference this is about.
static bool WriteBmp(const char* path, const std::vector<unsigned char>& y, int w, int h,
                     int x0, int y0, int cw, int ch) {
    const int stride = (cw * 3 + 3) & ~3;
    const int imgSize = stride * ch;
    const int fileSize = 54 + imgSize;
    std::vector<unsigned char> hdr(54, 0);
    hdr[0] = 'B'; hdr[1] = 'M';
    memcpy(&hdr[2], &fileSize, 4);
    int off = 54; memcpy(&hdr[10], &off, 4);
    int ih = 40; memcpy(&hdr[14], &ih, 4);
    int bw = cw, bh = ch; memcpy(&hdr[18], &bw, 4); memcpy(&hdr[22], &bh, 4);
    short planes = 1, bits = 24;
    memcpy(&hdr[26], &planes, 2); memcpy(&hdr[28], &bits, 2);
    memcpy(&hdr[34], &imgSize, 4);

    FILE* f = nullptr;
    if (fopen_s(&f, path, "wb") != 0 || !f) return false;
    fwrite(hdr.data(), 1, hdr.size(), f);
    std::vector<unsigned char> row((size_t)stride, 0);
    for (int j = ch - 1; j >= 0; --j) {                 // BMP rows are bottom-up
        for (int i = 0; i < cw; ++i) {
            const unsigned char v = y[(size_t)(y0 + j) * w + (x0 + i)];
            row[i * 3 + 0] = v; row[i * 3 + 1] = v; row[i * 3 + 2] = v;
        }
        fwrite(row.data(), 1, row.size(), f);
    }
    fclose(f);
    return true;
}

// Colour dump. The first version of this tool wrote the LUMA plane only, which
// is exactly why a chroma bug could pass every metric here and still look broken
// in the player: colour is only visible if the dump has colour.
static bool WriteBmpColor(const char* path, const std::vector<unsigned char>& y,
                          const std::vector<unsigned char>& uv, int w, int h,
                          int x0, int y0, int cw, int ch) {
    const int stride = (cw * 3 + 3) & ~3;
    const int imgSize = stride * ch;
    const int fileSize = 54 + imgSize;
    std::vector<unsigned char> hdr(54, 0);
    hdr[0] = 'B'; hdr[1] = 'M';
    memcpy(&hdr[2], &fileSize, 4);
    int off = 54; memcpy(&hdr[10], &off, 4);
    int ih = 40; memcpy(&hdr[14], &ih, 4);
    int bw = cw, bh = ch; memcpy(&hdr[18], &bw, 4); memcpy(&hdr[22], &bh, 4);
    short planes = 1, bits = 24;
    memcpy(&hdr[26], &planes, 2); memcpy(&hdr[28], &bits, 2);
    memcpy(&hdr[34], &imgSize, 4);

    FILE* f = nullptr;
    if (fopen_s(&f, path, "wb") != 0 || !f) return false;
    fwrite(hdr.data(), 1, hdr.size(), f);
    const int vw = (w + 1) / 2;
    std::vector<unsigned char> row((size_t)stride, 0);
    for (int j = ch - 1; j >= 0; --j) {
        for (int i = 0; i < cw; ++i) {
            const int X = x0 + i, Y = y0 + j;
            const int C = (int)y[(size_t)Y * w + X] - 16;
            const int D = (int)uv[((size_t)(Y / 2) * vw + X / 2) * 2 + 0] - 128;
            const int E = (int)uv[((size_t)(Y / 2) * vw + X / 2) * 2 + 1] - 128;
            auto cl = [](int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); };
            row[i * 3 + 0] = (unsigned char)cl((298 * C + 541 * D + 128) >> 8);          // B
            row[i * 3 + 1] = (unsigned char)cl((298 * C - 55 * D - 136 * E + 128) >> 8); // G
            row[i * 3 + 2] = (unsigned char)cl((298 * C + 459 * E + 128) >> 8);          // R
        }
        fwrite(row.data(), 1, row.size(), f);
    }
    fclose(f);
    return true;
}

// How far apart two UV planes are, overall and per QUADRANT. A path that scales
// the chroma around the wrong origin puts the colour in one part of the picture
// and clamps the rest, which shows up as wildly different quadrants -- a number
// the luma-only metrics of the first version could never have caught.
static void UvDiff(const std::vector<unsigned char>& a, const std::vector<unsigned char>& b,
                   int w, int h, double* overall, double q[4]) {
    const int cw = (w + 1) / 2, ch = (h + 1) / 2;
    double acc = 0.0, qa[4] = { 0, 0, 0, 0 };
    long   qn[4] = { 0, 0, 0, 0 };
    for (int j = 0; j < ch; ++j)
        for (int i = 0; i < cw; ++i) {
            const int k = (j < ch / 2 ? 0 : 2) + (i < cw / 2 ? 0 : 1);
            const double d = (abs((int)a[((size_t)j * cw + i) * 2 + 0]
                                - (int)b[((size_t)j * cw + i) * 2 + 0])
                            + abs((int)a[((size_t)j * cw + i) * 2 + 1]
                                - (int)b[((size_t)j * cw + i) * 2 + 1])) * 0.5;
            acc += d; qa[k] += d; qn[k]++;
        }
    *overall = acc / (double)(cw * ch);
    for (int k = 0; k < 4; ++k) q[k] = qn[k] ? qa[k] / (double)qn[k] : 0.0;
}

struct Crop { int x, y, w, h; };

// Geometric sanity, not a quality score: mean|d| between two frames as one is
// shifted around. If the half-resolution path has a resampling offset (a half
// texel is a WHOLE pixel at video scale), the minimum lands off (0,0). An image
// that is merely softer or differently denoised still minimises at (0,0).
static void BestShift(const std::vector<unsigned char>& a, const std::vector<unsigned char>& b,
                      int w, int h, int* bestDx, int* bestDy, double* bestVal) {
    *bestDx = *bestDy = 0; *bestVal = 1e30;
    for (int dy = -3; dy <= 3; ++dy)
        for (int dx = -3; dx <= 3; ++dx) {
            double acc = 0.0; size_t n = 0;
            for (int j = 8; j < h - 8; j += 3)
                for (int i = 8; i < w - 8; i += 3) {
                    acc += abs((int)a[(size_t)j * w + i]
                             - (int)b[(size_t)(j + dy) * w + (i + dx)]);
                    ++n;
                }
            const double v = acc / (double)n;
            if (v < *bestVal) { *bestVal = v; *bestDx = dx; *bestDy = dy; }
        }
}

struct Result {
    int scale = 0, netW = 0, netH = 0;
    double initMs = 0, median = 0, p10 = 0, p90 = 0;
    double fps = 0;
    double diffVsInput = 0, diffVsNative = 0, lap = 0, grad = 0, meanY = 0;
    int    shiftX = 0, shiftY = 0;
    double shiftVal = 0;
};

int main(int argc, char** argv) {
    const char*   appDirA = (argc > 1) ? argv[1] : "app";
    const int     W       = (argc > 2) ? atoi(argv[2]) : 3840;
    const int     H       = (argc > 3) ? atoi(argv[3]) : 2160;
    const double  warmSec = (argc > 4) ? atof(argv[4]) : 20.0;
    bool dump = false, sweep = false;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--dump") == 0) dump = true;
        if (strcmp(argv[i], "--sweep") == 0) sweep = true;
    }

    wchar_t appDir[MAX_PATH] = {0};
    MultiByteToWideChar(CP_ACP, 0, appDirA, -1, appDir, MAX_PATH);

    std::vector<unsigned char> inY, inUV, outY, outUV, nativeY, halfY;
    makeNv12(inY, inUV, W, H);
    outY.assign((size_t)W * H, 0);
    outUV.assign((size_t)W * H / 2, 0);

    // One layer on purpose: this tool measures the resolution curve, and the
    // chain is the other axis (see tools\engine_selftest.cpp for that one).
    dlssnr::Options o;
    o.layerCount = 1;
    o.layers[0].style = 1;
    o.layers[0].intensity = 1.0f; o.layers[0].localTone = 1.0f;
    o.layers[0].localStruct = 1.0f;
    o.layers[0].skinStruct = 0.0f; o.layers[0].autoMask = 0;
    dlssnr::SetOptions(o);

    printf("video %dx%d (%.1f MPix), warm-up %.0f s per configuration, appDir=%s\n\n",
           W, H, W * (double)H / 1e6, warmSec, appDirA);

    // Configuration 3 exists to answer a specific question: the local tone /
    // local structure strengths are neighbourhood operators, so at half
    // resolution the same 1.0 acts over an area twice as wide in image terms.
    // If the half-resolution picture is blotchy WITH them and clean WITHOUT
    // them, the switch is usable but needs different defaults -- and that is a
    // finding, not a bug.
    struct Cfg { int scale; float tone, strct; char label[32]; };
    std::vector<Cfg> cfgs;
    auto addCfg = [&](int scale, float tone, float strct, const char* label) {
        Cfg c; c.scale = scale; c.tone = tone; c.strct = strct;
        _snprintf_s(c.label, sizeof(c.label), _TRUNCATE, "%s", label);
        cfgs.push_back(c);
    };
    if (sweep) {
        // One row per slider position: this is what says whether every stop is
        // safe, instead of assuming the picture degrades smoothly.
        addCfg(100, 1.0f, 1.0f, "native 100%");
        char lab[32];
        for (int s = 95; s >= 25; s -= 5) {
            _snprintf_s(lab, sizeof(lab), _TRUNCATE, "scaled %d%%", s);
            addCfg(s, 1.0f, 1.0f, lab);
        }
    } else {
        // The two 50% rows differ only in the local tone/structure strengths: they
        // answer "is the network still denoising at a reduced size, or is the
        // difference from the plain resample just those neighbourhood operators?"
        addCfg(100, 1.0f, 1.0f, "native 100%");
        addCfg(50,  1.0f, 1.0f, "scaled 50%, tone/struct 1.0");
        addCfg(50,  0.0f, 0.0f, "scaled 50%, tone/struct 0");
        addCfg(25,  0.0f, 0.0f, "scaled 25%, tone/struct 0");
    }
    const int kCfgCount = (int)cfgs.size();

    std::vector<Result> res(kCfgCount);
    std::vector<std::vector<unsigned char>> outs(kCfgCount);
    std::vector<std::vector<unsigned char>> outsUV(kCfgCount);   // keep the colour too
    int nres = 0;
    for (int ci = 0; ci < kCfgCount; ++ci) {
        const Cfg& c = cfgs[ci];
        o.layers[0].localTone = c.tone; o.layers[0].localStruct = c.strct;
        dlssnr::SetOptions(o);

        Result r;
        const double t0 = NowMs();
        if (!dlssnr::Init(W, H, appDir, "bench_procscale.log", c.scale)) {
            printf("--- %s: dlssnr::Init FAILED (see bench_procscale.log)\n", c.label);
            return 2;
        }
        r.initMs = NowMs() - t0;
        r.scale  = dlssnr::ProcScalePercent();
        r.netW   = dlssnr::ProcWidth();
        r.netH   = dlssnr::ProcHeight();
        printf("--- %s ---\n", c.label);
        printf("    asked %d%% -> applied %d%%, network %dx%d (%dx%d video), init %.0f ms\n",
               c.scale, r.scale, r.netW, r.netH, W, H, r.initMs);

        // ---- warm-up: GPU clock ramp, not part of the measurement ----
        const double tWarm = NowMs();
        int warmFrames = 0;
        while (NowMs() - tWarm < warmSec * 1000.0) {
            if (!dlssnr::ProcessNv12(inY.data(), W, inUV.data(), W,
                                     outY.data(), W, outUV.data(), W, false)) {
                printf("    FAIL: ProcessNv12 returned false (warm-up)\n");
                return 3;
            }
            ++warmFrames;
        }

        // ---- measure: 3 rounds x 30 frames, report the median ----
        std::vector<double> ms;
        ms.reserve(90);
        for (int round = 0; round < 3; ++round) {
            for (int k = 0; k < 30; ++k) {
                const double t = NowMs();
                if (!dlssnr::ProcessNv12(inY.data(), W, inUV.data(), W,
                                         outY.data(), W, outUV.data(), W, false)) {
                    printf("    FAIL: ProcessNv12 returned false (measure)\n");
                    return 3;
                }
                ms.push_back(NowMs() - t);
            }
        }
        std::sort(ms.begin(), ms.end());
        r.median = ms[ms.size() / 2];
        r.p10    = ms[ms.size() / 10];
        r.p90    = ms[ms.size() * 9 / 10];
        r.fps    = 1000.0 / r.median;

        r.diffVsInput = MeanAbsDiff(inY, outY, (size_t)W * H);
        r.lap         = LaplacianEnergy(outY, W, H);
        r.grad        = GradientEnergy(outY, W, H);
        r.meanY       = MeanY(outY, (size_t)W * H);
        outs[ci]      = outY;
        outsUV[ci]    = outUV;
        if (ci == 0) nativeY = outY;
        else         r.diffVsNative = MeanAbsDiff(nativeY, outY, (size_t)W * H);

        printf("    %d warm-up frames, %d measured: median %7.2f ms "
               "(p10 %.2f / p90 %.2f) -> %5.1f fps\n",
               warmFrames, (int)ms.size(), r.median, r.p10, r.p90, r.fps);
        printf("    Y: vs input mean|d| %.2f, mean %.1f, laplacian %.2f, gradient %.2f\n",
               r.diffVsInput, r.meanY, r.lap, r.grad);
        if (ci > 0) {
            const Result& base = res[0];
            BestShift(nativeY, outY, W, H, &r.shiftX, &r.shiftY, &r.shiftVal);
            printf("    vs native: mean|d| %.2f (best shift %+d,%+d -> %.2f)   "
                   "speed-up %.2fx   detail %.0f%%   mean shift %+.1f\n",
                   r.diffVsNative, r.shiftX, r.shiftY, r.shiftVal,
                   base.median / r.median, 100.0 * r.grad / base.grad,
                   r.meanY - base.meanY);
        }
        res[ci] = r;
        ++nres;
        printf("\n");
    }

    // What did the network do, versus what did the resampling alone do? The
    // reference is the same 2:1 resample WITHOUT any network, so
    //   diff(reference, output) == what the network contributed
    // and for a usable half-resolution mode that has to stay in the same
    // ballpark as the native path's denoising (diff(input, native output))
    // instead of turning into a low-frequency re-shading of the picture.
    // Each configuration gets ITS OWN reference at its own ratio, so "how far
    // the output is from a plain resample" means the same thing on every row.
    std::vector<std::vector<unsigned char>> refs(kCfgCount);
    const Result& base = res[0];
    printf("=== summary at %dx%d, budget 16.67 ms (60 fps) ===\n", W, H);
    printf("%-18s %8s %7s %7s %7s %9s %9s\n",
           "configuration", "ms/frame", "fps", "grad", "ref", "|out-ref|", "|out-native|");
    for (int ci = 0; ci < nres; ++ci) {
        const Result& r = res[ci];
        refs[ci] = ResampleCpu(inY, W, H, r.netW, r.netH);
        char name[40];
        _snprintf_s(name, sizeof(name), _TRUNCATE, "%d%% (%dx%d)",
                    r.scale == 100 ? 100 : r.scale, r.netW, r.netH);
        printf("%-18s %8.2f %7.1f %7.2f %7.2f %9.2f %9.2f   %s\n",
               name, r.median, r.fps, r.grad,
               GradientEnergy(refs[ci], W, H),
               MeanAbsDiff(refs[ci], outs[ci], (size_t)W * H),
               ci == 0 ? 0.0 : r.diffVsNative,
               r.median <= 16.67 ? "fits 60 fps" : "");
    }
    printf("(|out-ref| = what the NETWORK contributed on that row; the native path's\n"
           " own denoising strength is input vs native = %.2f)\n\n", base.diffVsInput);

    // ---- chroma: is the colour still where it belongs? ----
    // Compared against the INPUT's chroma plane: a correct path resamples it
    // (so the difference is small and even), while a chroma box built in the
    // wrong coordinate system leaves one quadrant carrying all the colour and
    // clamps the rest, which shows up as a huge quadrant spread.
    printf("%-18s %9s %9s   quadrant means (TL, TR, BL, BR)\n", "chroma", "|out-in|", "spread");
    {
        auto in = [&](const std::vector<unsigned char>& uv, const char* what) {
            double overall = 0.0, q[4] = { 0, 0, 0, 0 };
            UvDiff(uv, inUV, W, H, &overall, q);
            // no std::min/max here: windows.h defines them as macros
            double mn = q[0], mx = q[0];
            for (int k = 1; k < 4; ++k) {
                if (q[k] < mn) mn = q[k];
                if (q[k] > mx) mx = q[k];
            }
            printf("%-18s %9.2f %9.2f   %5.1f %5.1f %5.1f %5.1f   %s\n",
                   what, overall, mx - mn, q[0], q[1], q[2], q[3],
                   (overall > 12.0 || (mx - mn) > 8.0) ? "<-- LOOKS BROKEN" : "");
        };
        in(inUV, "reference (input)");
        for (int ci = 0; ci < nres; ++ci) {
            char name[40];
            _snprintf_s(name, sizeof(name), _TRUNCATE, "%d%% (%dx%d)",
                        res[ci].scale, res[ci].netW, res[ci].netH);
            in(outsUV[ci], name);
        }
    }

    if (dump) {
        Crop c{ W / 4, H / 4, 384, 384 };            // 1:1, an area with edges + grain
        if (c.x + c.w > W) c.x = W - c.w;
        if (c.y + c.h > H) c.y = H - c.h;
        if (WriteBmp("bench_in.bmp", inY, W, H, c.x, c.y, c.w, c.h))
            printf("wrote bench_in.bmp (%dx%d crop at %d,%d)\n", c.w, c.h, c.x, c.y);
        if (WriteBmpColor("bench_c_in.bmp", inY, inUV, W, H, c.x, c.y, c.w, c.h))
            printf("wrote bench_c_in.bmp (colour)\n");
        for (int ci = 0; ci < nres; ++ci) {
            char p1[64], p2[64], p3[64];
            _snprintf_s(p1, sizeof(p1), _TRUNCATE, "bench_out_%d_%d.bmp", res[ci].scale, ci);
            _snprintf_s(p2, sizeof(p2), _TRUNCATE, "bench_ref_%d_%d.bmp", res[ci].scale, ci);
            _snprintf_s(p3, sizeof(p3), _TRUNCATE, "bench_c_%d_%d.bmp", res[ci].scale, ci);
            const bool a = WriteBmp(p1, outs[ci], W, H, c.x, c.y, c.w, c.h);
            const bool b = WriteBmp(p2, refs[ci], W, H, c.x, c.y, c.w, c.h);
            const bool d = WriteBmpColor(p3, outs[ci], outsUV[ci], W, H, c.x, c.y, c.w, c.h);
            if (a && b && d) printf("wrote %s / %s / %s (colour)\n", p1, p2, p3);
        }
    }
    return 0;
}