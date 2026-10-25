// ===========================================================================
//  frame_dump.cpp -- run the real DLSSNR engine over a raw NV12 sequence.
//
//  WHY THIS EXISTS
//    The promo video needs "original vs engine output" pairs that are actually
//    produced by the engine. Nothing in the repo could produce them: the
//    control panel's compare window keeps a frame pair in memory but never
//    writes it to disk, and tools\engine_selftest.cpp only asserts on the
//    output (it never dumps it).
//
//  WHAT IT DOES
//    Reads a tightly packed NV12 sequence, feeds it through dlssnr::ProcessNv12
//    in order -- temporal history included, frame 0 resets it, exactly like
//    RunCapture() in engine_selftest.cpp -- and writes the processed sequence in
//    the same layout. Point ffmpeg at either file to get PNG/JPEG frames.
//
//  USAGE
//    frame_dump <in.yuv> <out.yuv> <W> <H> <frames> [options]
//
//      --appdir <dir>        folder holding nvngx_dlssnr.dll
//                            (default D:\VSCODE\dlssnr-filter\app)
//      --log <file>          engine log path      (default frame_dump.log)
//      --skip <n>            feed n frames without writing them, so the
//                            temporal filter has settled (default 8)
//      --layers <1..8>       default 1
//      --style <0..3>        default 1   (1 = 自然)
//      --intensity <0..100>  default 100
//      --tone <0..200>       default 100
//      --struct <0..200>     default 100
//      --skin <0..200>       default 0
//      --automask <0|1>      default 0
//      --scale <25..100>     network resolution percent;
//                            100 = native (处理分辨率 only applies when
//                            「降分辨率处理」 is ticked)
//
//  Build: tools\build_frame_dump.bat   ->  build\frame_dump.exe
// ===========================================================================
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

#include "../src/dlssnr_engine.h"

static std::wstring Widen(const char* s) {
    if (!s) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    std::wstring w(n ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s, -1, &w[0], n);
    return w;
}

static bool ReadExact(FILE* f, void* dst, size_t bytes) {
    return bytes == 0 || fread(dst, 1, bytes, f) == bytes;
}

int main(int argc, char** argv) {
    if (argc < 6) {
        fprintf(stderr,
            "usage: frame_dump <in.yuv> <out.yuv> <W> <H> <frames> [options]\n"
            "  --appdir <dir>  --log <file>  --skip <n>  --layers <1..8>\n"
            "  --style <0..3>  --intensity <0..100>  --tone <0..200>\n"
            "  --struct <0..200>  --skin <0..200>  --automask <0|1>  --scale <25..100>\n");
        return 1;
    }

    const char* inPath  = argv[1];
    const char* outPath = argv[2];
    const int   W       = atoi(argv[3]);
    const int   H       = atoi(argv[4]);
    const long  frames  = atol(argv[5]);

    std::string appDir = "D:\\VSCODE\\dlssnr-filter\\app";
    std::string logPath = "frame_dump.log";
    int  skip = 8, layers = 1, style = 1, intensity = 100, tone = 100,
         strct = 100, skin = 0, automask = 0, scale = 100, extrap = 100;

    for (int i = 6; i < argc; ++i) {
        const char* a = argv[i];
        const char* v = (i + 1 < argc) ? argv[i + 1] : NULL;
        if (!v) { fprintf(stderr, "missing value for %s\n", a); return 1; }
        if      (!strcmp(a, "--appdir"))    { appDir = v;   ++i; }
        else if (!strcmp(a, "--log"))       { logPath = v;  ++i; }
        else if (!strcmp(a, "--skip"))      { skip = atoi(v);      ++i; }
        else if (!strcmp(a, "--layers"))    { layers = atoi(v);    ++i; }
        else if (!strcmp(a, "--style"))     { style = atoi(v);     ++i; }
        else if (!strcmp(a, "--intensity")) { intensity = atoi(v); ++i; }
        else if (!strcmp(a, "--tone"))      { tone = atoi(v);      ++i; }
        else if (!strcmp(a, "--struct"))    { strct = atoi(v);     ++i; }
        else if (!strcmp(a, "--skin"))      { skin = atoi(v);      ++i; }
        else if (!strcmp(a, "--automask"))  { automask = atoi(v);  ++i; }
        else if (!strcmp(a, "--scale"))     { scale = atoi(v);     ++i; }
        else if (!strcmp(a, "--extrap"))    { extrap = atoi(v);    ++i; }
        else { fprintf(stderr, "unknown option %s\n", a); return 1; }
    }
    if (W <= 0 || H <= 0 || frames <= 0) { fprintf(stderr, "bad geometry/frames\n"); return 1; }

    FILE* fin = fopen(inPath, "rb");
    if (!fin) { fprintf(stderr, "cannot open input %s\n", inPath); return 1; }
    FILE* fout = fopen(outPath, "wb");
    if (!fout) { fprintf(stderr, "cannot open output %s\n", outPath); fclose(fin); return 1; }

    const size_t nY = (size_t)W * H, nUV = (size_t)W * H / 2;

    printf("frame_dump: %dx%d, %ld frames (+%d warm-up), layers=%d style=%d "
           "intensity=%d%% tone=%d%% struct=%d%% skin=%d%% automask=%d scale=%d%% "
           "extrap=%d%%\n",
           W, H, frames, skip, layers, style, intensity, tone, strct, skin, automask,
           scale, extrap);

    std::wstring appDirW = Widen(appDir.c_str());
    if (!dlssnr::Init(W, H, appDirW.c_str(), logPath.c_str(), scale)) {
        fprintf(stderr, "FAIL: dlssnr::Init returned false (see %s)\n", logPath.c_str());
        fclose(fin); fclose(fout);
        return 2;
    }
    printf("engine ready at %dx%d (network %dx%d, scale %d%% applied)\n",
           dlssnr::Width(), dlssnr::Height(),
           dlssnr::ProcWidth(), dlssnr::ProcHeight(), dlssnr::ProcScalePercent());

    dlssnr::Options opt;
    opt.layerCount = layers;
    opt.extrapPct  = extrap;   // 100 = off
    for (int i = 0; i < dlssnr::kMaxLayers; ++i) {
        dlssnr::LayerOptions L;
        L.style       = style;
        L.intensity   = intensity / 100.0f;
        L.localTone   = tone      / 100.0f;
        L.localStruct = strct     / 100.0f;
        L.skinStruct  = skin      / 100.0f;
        L.autoMask    = automask;
        L.uiCorrection = 0;
        opt.layers[i] = L;
    }
    dlssnr::SetOptions(opt);

    std::vector<unsigned char> inY(nY), inUV(nUV), outY(nY), outUV(nUV);
    long frameIndex = 0, written = 0;
    double lastMean = 0.0;

    while (written < frames) {
        if (!ReadExact(fin, inY.data(), nY) || !ReadExact(fin, inUV.data(), nUV)) break;
        const bool reset = (frameIndex == 0);
        if (!dlssnr::ProcessNv12(inY.data(), W, inUV.data(), W,
                                 outY.data(), W, outUV.data(), W, reset)) {
            fprintf(stderr, "FAIL: ProcessNv12 at frame %ld\n", frameIndex);
            dlssnr::Shutdown(); fclose(fin); fclose(fout);
            return 3;
        }
        ++frameIndex;
        if (frameIndex <= skip) continue;          // temporal warm-up, not written

        if (fwrite(outY.data(), 1, nY, fout) != nY ||
            fwrite(outUV.data(), 1, nUV, fout) != nUV) {
            fprintf(stderr, "FAIL: short write at frame %ld\n", frameIndex);
            dlssnr::Shutdown(); fclose(fin); fclose(fout);
            return 4;
        }
        ++written;

        unsigned long long acc = 0;
        for (size_t i = 0; i < nY; i += 7) {
            int d = (int)outY[i] - (int)inY[i];
            acc += (unsigned long long)(d < 0 ? -d : d);
        }
        lastMean = (double)acc / (double)(nY / 7);

        if ((written % 60) == 0 || written == frames)
            printf("  %ld/%ld frames  (mean |out-in| luma = %.2f)\n", written, frames, lastMean);
    }

    printf("done: wrote %ld frames to %s\n", written, outPath);
    if (written > 0)
        printf("final mean |out-in| luma = %.2f  (0 would mean the engine did nothing)\n", lastMean);

    dlssnr::Shutdown();
    fclose(fin); fclose(fout);
    return (written == frames) ? 0 : 5;
}
