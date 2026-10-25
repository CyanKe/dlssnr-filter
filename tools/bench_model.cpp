// ===========================================================================
//  bench_model.cpp -- the two networks inside one nvngx_dlssnr.dll, measured on
//  REAL frames: what each costs per frame, and what the switch changes.
//
//  WHY THIS EXISTS
//    310.9.16 carries TWO networks (see src\dlssnr_models.h):
//      preset 1 = WEIGHTS_HT           -- the 310.8.0 network, byte-identical
//      preset 2 = WEIGHTS_CC_SMALL_FP8 -- the 2026-10-16 "2x faster" one
//    and the panel lets a user switch between them. So the switch raises exactly
//    two questions: what does it cost, and what does it change? The engine
//    selftest answers "did the switch do anything at all" on a synthetic frame;
//    this answers both on the frames a user actually watches.
//
//  METHOD (docs\ENGINEERING_NOTES.md section 3)
//    * Every model gets a long warm-up FIRST. This machine sits at ~1.3 GHz until
//      it has been under load for tens of seconds and only then ramps to ~2.4
//      GHz; sampling right after start measures the cold clock, up to 1.5x off.
//    * The frames are real, but the sequence is short and LOOPED during the
//      warm-up. That is legitimate because cost is resolution-bound, not
//      content-bound (measured: <=1.3 ms across flat / gradient / noise /
//      detailed content), so a looped sequence warms the same pipeline. The
//      visual comparison uses a real frame, never a loop artifact.
//    * Both models see the IDENTICAL frames, options and geometry; only
//      DLSSNR.Hint.Render.Preset differs.
//    * Time is reported as median with p10/p90, never as a mean: one scheduler
//      hiccup must not move the number.
//
//  USAGE
//    bench_model <in.yuv> <W> <H> <seqFrames> [options]
//      --appdir <dir>     folder holding nvngx_dlssnr.dll  (default "app")
//      --warm <seconds>   warm-up per round                 (default 20)
//      --timed <frames>   timed frames per round           (default 240)
//      --rounds <n>       interleaved new/old rounds       (default 3)
//      --outdir <dir>     where to write in/m1/m2 NV12     (default build)
//      --style <0..3> --intensity <0..100> --tone <0..200>
//      --struct <0..200> --skin <0..200> --automask <0|1> --layers <1..8>
//
//  The two models are measured INTERLEAVED (new, old, new, old, ...) and the
//  summary uses each model's BEST round: this laptop's power state moves the same
//  configuration by more than 2x between sessions, so a single measurement per
//  model cannot tell a real difference from the machine changing its mind.
//
//  Input must be tightly packed NV12 (ffmpeg -pix_fmt nv12 -f rawvideo). The
//  three files written to --outdir go straight back into ffmpeg for a montage.
// ===========================================================================
// Must precede every CRT header (windows.h pulls them in), which is why it sits
// above the engine includes rather than next to <stdio.h>.
#define _CRT_SECURE_NO_WARNINGS      // fopen/fread, like the other tools here

#include "../src/dlssnr_engine.h"
#include "../src/dlssnr_models.h"

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <string>
#include <vector>
#include <algorithm>

static double NowMs() {
    static LARGE_INTEGER f{}; static bool init = false;
    if (!init) { QueryPerformanceFrequency(&f); init = true; }
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

static std::wstring Widen(const char* s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    std::wstring w(n ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s, -1, &w[0], n);
    return w;
}

struct Diff { double meanAbs; int maxAbs; double pctGt2; double psnr; };

// Luma comparison. PSNR uses an 8-bit peak, the way the published DLSS 5 write-ups
// do, so these numbers can be read next to theirs.
static Diff Compare(const std::vector<unsigned char>& a,
                    const std::vector<unsigned char>& b, size_t n) {
    Diff d{ 0.0, 0, 0.0, 0.0 };
    double sq = 0.0; size_t big = 0;
    for (size_t i = 0; i < n; ++i) {
        const int x = (int)a[i] - (int)b[i];
        const int ax = x < 0 ? -x : x;
        d.meanAbs += ax;
        if (ax > d.maxAbs) d.maxAbs = ax;
        if (ax > 2) ++big;
        sq += (double)x * (double)x;
    }
    d.meanAbs /= (double)n;
    d.pctGt2 = 100.0 * (double)big / (double)n;
    const double mse = sq / (double)n;
    d.psnr = (mse <= 0.0) ? 999.0 : 10.0 * log10((255.0 * 255.0) / mse);
    return d;
}

static double Percentile(std::vector<double> v, double p) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    const double idx = p * (double)(v.size() - 1);
    const size_t lo = (size_t)idx;
    const size_t hi = (lo + 1 < v.size()) ? lo + 1 : lo;
    return v[lo] + (v[hi] - v[lo]) * (idx - (double)lo);
}

static bool WriteNv12(const char* path, const std::vector<unsigned char>& y,
                      const std::vector<unsigned char>& uv) {
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    const bool ok = fwrite(y.data(), 1, y.size(), f) == y.size() &&
                    fwrite(uv.data(), 1, uv.size(), f) == uv.size();
    fclose(f);
    return ok;
}

struct Model {
    int    preset      = -1;
    int    applied     = -1;
    bool   fellBack    = false;
    double median = 0, p10 = 0, p90 = 0, mean = 0;
    long   warmed  = 0;
    // One full sequence pass, luma only: the metrics below are then averages over
    // `have` REAL frames rather than one anecdote, and the two models can be
    // compared frame by frame.
    std::vector<std::vector<unsigned char>> outs;
    Diff   vsInput{ 0, 0, 0, 0 };
    std::vector<unsigned char> dumpY, dumpUV;   // the frame written for the montage
    int    dumpIdx = -1;
    // One median per timing round. The two models are measured INTERLEAVED
    // (2,1,2,1,...) because this laptop's power state moves the same configuration
    // by more than 2x between runs: measured back to back, the old model came out
    // at 8.83 ms in one session and 21.26 ms in the next while the new model read
    // 5.52 ms in both. A single measurement per model cannot tell a real
    // difference from the machine changing its mind, so the summary uses the BEST
    // round of each (throttling only ever makes a round slower) and prints them all.
    std::vector<double> roundMedians;
};

// Accumulate one frame's comparison into a running average (mean of means and of
// the >2 percentage, max of max, mean of MSE -> PSNR at the end).
struct Acc {
    double meanSum = 0, pctSum = 0, sqSum = 0;
    int    maxAbs = 0, n = 0;
    void Add(const Diff& d, double mse) {
        meanSum += d.meanAbs; pctSum += d.pctGt2; sqSum += mse;
        if (d.maxAbs > maxAbs) maxAbs = d.maxAbs;
        ++n;
    }
    Diff Finish() const {
        Diff d{ 0, maxAbs, 0, 0 };
        if (!n) return d;
        d.meanAbs = meanSum / n;
        d.pctGt2  = pctSum / n;
        const double mse = sqSum / n;
        d.psnr = (mse <= 0.0) ? 999.0 : 10.0 * log10((255.0 * 255.0) / mse);
        return d;
    }
};

static double Mse(const std::vector<unsigned char>& a, const std::vector<unsigned char>& b,
                  size_t n) {
    double sq = 0.0;
    for (size_t i = 0; i < n; ++i) { const double x = (double)a[i] - (double)b[i]; sq += x * x; }
    return sq / (double)n;
}

// One model, start to finish: re-create the session for this preset, warm the
// clock up on the real sequence, time it, then run ONE full pass of the sequence
// for the metrics and the montage frame. Returns false on any engine error.
static bool RunModel(int preset, int W, int H, const std::wstring& appDir,
                     const char* logPath, const std::vector<unsigned char>& seq,
                     int have, size_t nY, size_t nFrame, const dlssnr::Options& opt,
                     double warmSec, int timed, int dumpIdx, bool metrics, Model* out) {
    const char* res = dlssnr::ModelResource(preset);
    printf("=== preset %d (%s) ===\n", preset, (res && res[0]) ? res : "DLL default");

    if (!dlssnr::Init(W, H, appDir.c_str(), logPath, 100, preset)) {
        fprintf(stderr, "FAIL: Init with preset %d (see %s)\n", preset, logPath);
        return false;
    }
    out->preset   = preset;
    out->applied  = dlssnr::ModelPreset();
    out->fellBack = dlssnr::ModelFellBack();
    printf("engine   : ready at %dx%d (network %dx%d), preset %d requested -> %d applied%s\n",
           dlssnr::Width(), dlssnr::Height(), dlssnr::ProcWidth(), dlssnr::ProcHeight(),
           preset, out->applied, out->fellBack ? "  (DLL REFUSED it -> default)" : "");
    if (out->applied != preset)
        printf("WARNING  : this DLL cannot serve preset %d; what follows measures the "
               "DEFAULT network, not the one asked for.\n", preset);

    dlssnr::SetOptions(opt);

    std::vector<unsigned char> outY(nY), outUV(nY / 2);

    const double warmStart = NowMs();
    long fed = 0;
    while ((NowMs() - warmStart) < warmSec * 1000.0) {
        const unsigned char* p = seq.data() + (size_t)(fed % have) * nFrame;
        if (!dlssnr::ProcessNv12(p, W, p + nY, W, outY.data(), W, outUV.data(), W, fed == 0)) {
            fprintf(stderr, "FAIL: ProcessNv12 during warm-up\n");
            return false;
        }
        ++fed;
    }
    out->warmed = fed;
    printf("warm-up  : %ld frames in %.1f s\n", fed, (NowMs() - warmStart) / 1000.0);

    // Back-to-back. Measured earlier that a 60 fps cadence gives the same result
    // as full duty (the engine raises the clock as soon as it is called), so
    // pacing would only make the run longer.
    std::vector<double> ms;
    ms.reserve(timed);
    for (int i = 0; i < timed; ++i) {
        const unsigned char* p = seq.data() + (size_t)((fed + i) % have) * nFrame;
        const double t0 = NowMs();
        if (!dlssnr::ProcessNv12(p, W, p + nY, W, outY.data(), W, outUV.data(), W, false)) {
            fprintf(stderr, "FAIL: ProcessNv12 while timed\n");
            return false;
        }
        ms.push_back(NowMs() - t0);
    }
    out->median = Percentile(ms, 0.50);
    out->p10    = Percentile(ms, 0.10);
    out->p90    = Percentile(ms, 0.90);
    double acc = 0.0; for (double v : ms) acc += v;
    out->mean = acc / (double)ms.size();
    printf("timing   : median %.2f ms   p10 %.2f   p90 %.2f   mean %.2f   (n=%d)\n",
           out->median, out->p10, out->p90, out->mean, (int)ms.size());
    out->roundMedians.push_back(out->median);

    // A timing-only round stops here: the metrics pass and the montage dump cost
    // another warm-up each and would lengthen every interleaved round for nothing.
    if (!metrics) return true;

    // ---- one full pass over the sequence, from frame 0 -------------------
    // The warm-up length DIFFERS between models (the faster one runs more frames
    // in the same wall time), so the sequence phase does not match on its own.
    // Restarting the pass at frame 0 makes both models process the same frames in
    // the same order, which is what makes a frame-by-frame comparison legitimate
    // -- without this, model 2 was being compared against model 1's frame.
    Acc accIn;
    out->outs.assign(have, std::vector<unsigned char>());
    for (int i = 0; i < have; ++i) {
        const unsigned char* p = seq.data() + (size_t)i * nFrame;
        if (!dlssnr::ProcessNv12(p, W, p + nY, W, outY.data(), W, outUV.data(), W, i == 0)) {
            fprintf(stderr, "FAIL: ProcessNv12 during the metrics pass\n");
            return false;
        }
        const std::vector<unsigned char> srcY(p, p + nY);
        const Diff d = Compare(outY, srcY, nY);
        accIn.Add(d, Mse(outY, srcY, nY));
        out->outs[i] = outY;                      // luma only: that is all the metrics need
        if (i == dumpIdx) { out->dumpY = outY; out->dumpUV = outUV; out->dumpIdx = i; }
    }
    out->vsInput = accIn.Finish();
    printf("edit     : vs ITS INPUT over %d real frames: mean|d| %.2f  max %d  %.1f%% >2  PSNR %.2f dB\n",
           have, out->vsInput.meanAbs, out->vsInput.maxAbs, out->vsInput.pctGt2, out->vsInput.psnr);
    return true;
}

int main(int argc, char** argv) {
    if (argc < 5) {
        fprintf(stderr,
            "usage: bench_model <in.yuv> <W> <H> <seqFrames> [--appdir d] [--warm s]\n"
            "                   [--timed n] [--outdir d] [--style 0..3] [--layers n]\n"
            "                   [--intensity pct] [--tone pct] [--struct pct]\n"
            "                   [--skin pct] [--automask 0|1]\n");
        return 1;
    }
    const char* inPath = argv[1];
    const int   W = atoi(argv[2]), H = atoi(argv[3]), seqFrames = atoi(argv[4]);
    std::string appDir = "app", outDir = "build";
    double warmSec = 20.0;
    int timed = 240, style = 1, layers = 1, intensity = 100, tone = 100,
        strct = 100, skin = 0, automask = 0, rounds = 3;

    for (int i = 5; i < argc; ++i) {
        const char* a = argv[i];
        const char* v = (i + 1 < argc) ? argv[i + 1] : NULL;
        if (!v) { fprintf(stderr, "missing value for %s\n", a); return 1; }
        if      (!strcmp(a, "--appdir"))    { appDir = v;   ++i; }
        else if (!strcmp(a, "--outdir"))    { outDir = v;   ++i; }
        else if (!strcmp(a, "--warm"))      { warmSec = atof(v); ++i; }
        else if (!strcmp(a, "--timed"))     { timed = atoi(v);   ++i; }
        else if (!strcmp(a, "--rounds"))    { rounds = atoi(v);  ++i; }
        else if (!strcmp(a, "--style"))     { style = atoi(v);   ++i; }
        else if (!strcmp(a, "--layers"))    { layers = atoi(v);  ++i; }
        else if (!strcmp(a, "--intensity")) { intensity = atoi(v); ++i; }
        else if (!strcmp(a, "--tone"))      { tone = atoi(v);    ++i; }
        else if (!strcmp(a, "--struct"))    { strct = atoi(v);   ++i; }
        else if (!strcmp(a, "--skin"))      { skin = atoi(v);    ++i; }
        else if (!strcmp(a, "--automask"))  { automask = atoi(v);++i; }
        else { fprintf(stderr, "unknown option %s\n", a); return 1; }
    }
    if (W <= 0 || H <= 0 || seqFrames <= 0 || timed <= 0) {
        fprintf(stderr, "bad args\n"); return 1;
    }

    const size_t nY = (size_t)W * H, nUV = nY / 2, nFrame = nY + nUV;

    FILE* fin = fopen(inPath, "rb");
    if (!fin) { fprintf(stderr, "cannot open %s\n", inPath); return 1; }
    std::vector<unsigned char> seq((size_t)seqFrames * nFrame);
    const size_t got = fread(seq.data(), 1, seq.size(), fin);
    fclose(fin);
    const int have = (int)(got / nFrame);
    if (have < 2) { fprintf(stderr, "need at least 2 frames, got %d\n", have); return 1; }
    seq.resize((size_t)have * nFrame);

    printf("input    : %s\n", inPath);
    printf("geometry : %dx%d, frames loaded %d (%.1f MiB)\n", W, H, have,
           (double)have * (double)nFrame / 1048576.0);

    // Which DLL sits in --appdir and what it carries. Printed first because "the
    // model switch did nothing" has one boring cause -- the folder still holds the
    // single-network 310.8.0 DLL -- and this makes it visible before any number.
    const std::wstring appDirW = Widen(appDir.c_str());
    dlssnr::SetModuleDir(appDirW.c_str());
    {
        wchar_t ver[32] = L"";
        dlssnr::DllVersion(ver, _countof(ver));
        dlssnr::DllWeight w[dlssnr::kDllWeightMax];
        const int nw = dlssnr::DllWeights(w, dlssnr::kDllWeightMax);
        printf("dll      : %ls  (%d network(s):", ver[0] ? ver : L"?", nw);
        for (int i = 0; i < nw; ++i) printf(" %s->preset %d", w[i].name, w[i].preset);
        printf(")\n");
    }

    printf("options  : layers=%d style=%d intensity=%d%% tone=%d%% struct=%d%% "
           "skin=%d%% automask=%d  scale=100%%\n",
           layers, style, intensity, tone, strct, skin, automask);
    printf("method   : %.0f s warm-up per model, then %d timed frames (median, p10/p90)\n\n",
           warmSec, timed);

    dlssnr::Options opt;
    opt.layerCount = layers;
    for (int i = 0; i < dlssnr::kMaxLayers; ++i) {
        dlssnr::LayerOptions L;
        L.style = style; L.intensity = intensity / 100.0f;
        L.localTone = tone / 100.0f; L.localStruct = strct / 100.0f;
        L.skinStruct = skin / 100.0f; L.autoMask = automask; L.uiCorrection = 0;
        opt.layers[i] = L;
    }

    char logPath[MAX_PATH];
    _snprintf_s(logPath, sizeof(logPath), _TRUNCATE, "%s\\bench_model.log", outDir.c_str());

    // ---- phase 1: interleaved timing -------------------------------------
    // 2,1,2,1,... so a machine that changes its mind mid-run moves BOTH models
    // instead of only the one that happened to be measured second.
    Model m2, m1;
    printf("=== timing: %d interleaved round(s) of new/old ===\n", rounds);
    for (int r = 0; r < rounds; ++r) {
        // ALTERNATE which model goes first. The GPU's power cap makes each round a
        // little slower than the one before it (measured at 4K: the old model's
        // rounds drifted 68 -> 74 -> 79 ms), so always running the new model first
        // would hand it the fresher slot in every round and flatter it.
        const int first = (r % 2 == 0) ? 2 : 1;
        for (int k = 0; k < 2; ++k) {
            const int preset = (k == 0) ? first : (3 - first);
            Model tmp;
            if (!RunModel(preset, W, H, appDirW, logPath, seq, have, nY, nFrame, opt,
                          warmSec, timed, 0, false, &tmp))
                return 2;
            (preset == 2 ? m2 : m1).roundMedians.push_back(tmp.median);
            printf("  round %d order %s: preset %d median %.2f ms\n", r + 1,
                   (k == 0) ? "first " : "second", preset, tmp.median);
        }
    }
    const double best2 = *std::min_element(m2.roundMedians.begin(), m2.roundMedians.end());
    const double best1 = *std::min_element(m1.roundMedians.begin(), m1.roundMedians.end());

    // ---- phase 2: metrics + the montage frame ----------------------------
    // The frame used for the montage: the middle of the sequence, so it is neither
    // frame 0 (which resets the temporal history) nor a loop boundary.
    const int dumpIdx = have / 2;
    printf("\n=== metrics: one full pass of %d real frames per model, frame %d dumped ===\n",
           have, dumpIdx);
    if (!RunModel(2, W, H, appDirW, logPath, seq, have, nY, nFrame, opt, warmSec, timed,
                  dumpIdx, true, &m2))
        return 2;
    if (!RunModel(1, W, H, appDirW, logPath, seq, have, nY, nFrame, opt, warmSec, timed,
                  dumpIdx, true, &m1))
        return 2;

    // The three images the montage needs, all the SAME source frame.
    const unsigned char* p = seq.data() + (size_t)dumpIdx * nFrame;
    std::vector<unsigned char> srcY(p, p + nY), srcUV(p + nY, p + nFrame);
    char path[MAX_PATH];
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\model_in.yuv",   outDir.c_str());
    WriteNv12(path, srcY, srcUV);
    printf("montage  : %s (input)\n", path);
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\model2_out.yuv", outDir.c_str());
    WriteNv12(path, m2.dumpY, m2.dumpUV);
    printf("montage  : %s (new, preset %d)\n", path, m2.applied);
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\model1_out.yuv", outDir.c_str());
    WriteNv12(path, m1.dumpY, m1.dumpUV);
    printf("montage  : %s (old, preset %d)\n\n", path, m1.applied);

    // The two models against each other, frame by frame over the whole pass.
    Acc accX;
    for (int i = 0; i < have; ++i) {
        const Diff d = Compare(m2.outs[i], m1.outs[i], nY);
        accX.Add(d, Mse(m2.outs[i], m1.outs[i], nY));
    }
    const Diff dx = accX.Finish();

    printf("--- summary (identical frames, options and geometry) ---\n");
    printf("cost     : new (preset %d) best round %6.2f ms   all rounds:", m2.applied, best2);
    for (double v : m2.roundMedians) printf(" %.2f", v);
    printf("\n           old (preset %d) best round %6.2f ms   all rounds:", m1.applied, best1);
    for (double v : m1.roundMedians) printf(" %.2f", v);
    printf("\n           delta %+.2f ms  (%.0f%% less with the new model, %.2fx)\n",
           best2 - best1,
           100.0 * (best1 - best2) / (best1 > 0 ? best1 : 1.0),
           (best2 > 0 ? best1 / best2 : 0.0));
    printf("edit size: new vs input : mean|d| %6.2f  max %3d  %5.1f%% >2  PSNR %.2f dB\n",
           m2.vsInput.meanAbs, m2.vsInput.maxAbs, m2.vsInput.pctGt2, m2.vsInput.psnr);
    printf("           old vs input : mean|d| %6.2f  max %3d  %5.1f%% >2  PSNR %.2f dB\n",
           m1.vsInput.meanAbs, m1.vsInput.maxAbs, m1.vsInput.pctGt2, m1.vsInput.psnr);
    printf("           new vs old   : mean|d| %6.2f  max %3d  %5.1f%% >2  PSNR %.2f dB\n",
           dx.meanAbs, dx.maxAbs, dx.pctGt2, dx.psnr);
    printf("           (all three averaged over %d real frames; \"edit size\" is how far the\n"
           "            model moved the picture from its input -- bigger means a stronger edit)\n",
           have);

    dlssnr::Shutdown();
    printf("\ndone. model_in.yuv / model2_out.yuv / model1_out.yuv are in %s\n", outDir.c_str());
    return 0;
}
