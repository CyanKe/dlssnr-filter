// simd_conv_nv12_to_bgr.cpp
//   SIMD NV12 -> BGR24 for src/dlssnr_dshow.cpp, plus a bit-exactness harness
//   against the scalar original it must replace.
//
// WHY: the scalar loop costs 29.0 ms/frame at 3840x2076 on this machine (0.82
// GB/s of output) - far below memory bandwidth, i.e. instruction-bound, not
// bandwidth-bound. The filter is built with /O2 and no /arch: flag, so MSVC did
// not vectorise it. The host DLL in dlssnr-toolkit already vectorises its own
// BGR<->RGBA helpers (SSSE3 pshufb, 4 px/step); the filter was simply left behind.
//
// STRATEGY: 8 pixels per iteration.
//   * Y and the interleaved UV pair are widened to int16 lanes;
//   * each chroma sample is duplicated across the 2 pixels it serves;
//   * the three channel formulas become _mm_madd_epi16 dot products (int32);
//   * >>8 is an ARITHMETIC shift (the scalar code shifts a signed int);
//   * packus clips to 0..255 exactly like Clip8;
//   * the packed BGR24 stream (3 bytes/px) is assembled with pshufb + OR.

#include <windows.h>
#include <immintrin.h>
#include <tmmintrin.h>
#include <intrin.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

typedef unsigned char BYTE;
static inline BYTE Clip8(int v) { return (BYTE)(v < 0 ? 0 : (v > 255 ? 255 : v)); }

// ===================== scalar reference (verbatim from the filter) ==========
static void Nv12ToBgr24_ref(const BYTE* src, int w, int h, int strideY, int strideUV,
                            BYTE* dst) {
    for (int y = 0; y < h; ++y) {
        const BYTE* yrow = src + (size_t)y * strideY;
        const BYTE* uv = src + (size_t)strideY * h + (size_t)(y >> 1) * strideUV;
        BYTE* drow = dst + (size_t)y * w * 3;
        for (int x = 0; x < w; ++x) {
            const int Y = yrow[x];
            const int U = uv[(x >> 1) * 2];
            const int V = uv[(x >> 1) * 2 + 1];
            const int C = Y - 16;
            const int D = U - 128;
            const int E = V - 128;
            drow[x * 3 + 0] = Clip8((298 * C + 541 * D + 128) >> 8);
            drow[x * 3 + 1] = Clip8((298 * C - 55 * D - 136 * E + 128) >> 8);
            drow[x * 3 + 2] = Clip8((298 * C + 459 * E + 128) >> 8);
        }
    }
}

// ============================== SIMD version ================================
#if defined(_M_X64) || defined(_M_IX86)

static bool g_ssse3 = false;

static void cpu_init() {
    int r[4] = {0,0,0,0};
    __cpuid(r, 1);
    g_ssse3 = (r[2] & (1 << 9)) != 0;      // ECX bit 9 = SSSE3
}

// 8 pixels -> 8 result bytes (in the low half of the register).
// idx selects 0/1/2 = B/G/R.
static inline __m128i bgr8(__m128i C, __m128i D, __m128i E, int idx) {
    const __m128i zero = _mm_setzero_si128();
    const __m128i k128 = _mm_set1_epi32(128);
    __m128i lo, hi, p;
    if (idx == 0) {                                   // B = (298C + 541D + 128)>>8
        const __m128i k = _mm_setr_epi16(298, 541, 298, 541, 298, 541, 298, 541);
        lo = _mm_madd_epi16(_mm_unpacklo_epi16(C, D), k);
        hi = _mm_madd_epi16(_mm_unpackhi_epi16(C, D), k);
    } else if (idx == 2) {                            // R = (298C + 459E + 128)>>8
        const __m128i k = _mm_setr_epi16(298, 459, 298, 459, 298, 459, 298, 459);
        lo = _mm_madd_epi16(_mm_unpacklo_epi16(C, E), k);
        hi = _mm_madd_epi16(_mm_unpackhi_epi16(C, E), k);
    } else {                                          // G = (298C - 55D - 136E + 128)>>8
        const __m128i kcd = _mm_setr_epi16(298, -55, 298, -55, 298, -55, 298, -55);
        const __m128i ke  = _mm_setr_epi16(-136, 0, -136, 0, -136, 0, -136, 0);
        lo = _mm_add_epi32(_mm_madd_epi16(_mm_unpacklo_epi16(C, D), kcd),
                           _mm_madd_epi16(_mm_unpacklo_epi16(E, zero), ke));
        hi = _mm_add_epi32(_mm_madd_epi16(_mm_unpackhi_epi16(C, D), kcd),
                           _mm_madd_epi16(_mm_unpackhi_epi16(E, zero), ke));
    }
    lo = _mm_srai_epi32(_mm_add_epi32(lo, k128), 8);
    hi = _mm_srai_epi32(_mm_add_epi32(hi, k128), 8);
    p  = _mm_packs_epi32(lo, hi);                     // 8 x int16, saturated
    return _mm_packus_epi16(p, p);                    // 8 bytes, clipped 0..255
}

static void Nv12ToBgr24_simd(const BYTE* src, int w, int h, int strideY, int strideUV,
                             BYTE* dst) {
    const BYTE* uvbase = src + (size_t)strideY * h;
    const __m128i zero = _mm_setzero_si128();
    const __m128i c16  = _mm_set1_epi16(16);
    const __m128i c128 = _mm_set1_epi16(128);
    // interleave B+G (bg = [B0 G0 B1 G1 ...]) and splice R into every 3rd byte
    const __m128i m_bg0 = _mm_setr_epi8(0,1,-1,2,3,-1,4,5,-1,6,7,-1,8,9,-1,10);
    const __m128i m_r0  = _mm_setr_epi8(-1,-1,0,-1,-1,1,-1,-1,2,-1,-1,3,-1,-1,4,-1);
    const __m128i m_bg1 = _mm_setr_epi8(11,-1,12,13,-1,14,15,-1,-1,-1,-1,-1,-1,-1,-1,-1);
    const __m128i m_r1  = _mm_setr_epi8(-1,5,-1,-1,6,-1,-1,7,-1,-1,-1,-1,-1,-1,-1,-1);

    for (int y = 0; y < h; ++y) {
        const BYTE* yrow  = src  + (size_t)y * strideY;
        const BYTE* uvrow = uvbase + (size_t)(y >> 1) * strideUV;
        BYTE* drow = dst + (size_t)y * w * 3;
        int x = 0;
        for (; x + 8 <= w; x += 8) {
            __m128i Y = _mm_unpacklo_epi8(_mm_loadl_epi64((const __m128i*)(yrow + x)), zero);
            __m128i C = _mm_sub_epi16(Y, c16);

            // 8 px need 4 chroma samples; in a packed NV12 row the byte offset of
            // the chroma for pixel x is exactly x.
            __m128i UV = _mm_unpacklo_epi8(_mm_loadl_epi64((const __m128i*)(uvrow + x)), zero);
            __m128i D = _mm_shufflehi_epi16(_mm_shufflelo_epi16(UV, _MM_SHUFFLE(2,2,0,0)),
                                            _MM_SHUFFLE(2,2,0,0));
            __m128i E = _mm_shufflehi_epi16(_mm_shufflelo_epi16(UV, _MM_SHUFFLE(3,3,1,1)),
                                            _MM_SHUFFLE(3,3,1,1));
            D = _mm_sub_epi16(D, c128);
            E = _mm_sub_epi16(E, c128);

            __m128i B = bgr8(C, D, E, 0);
            __m128i G = bgr8(C, D, E, 1);
            __m128i R = bgr8(C, D, E, 2);

            __m128i bg = _mm_unpacklo_epi8(B, G);     // B0 G0 B1 G1 ...
            __m128i c0 = _mm_or_si128(_mm_shuffle_epi8(bg, m_bg0),
                                      _mm_shuffle_epi8(R,  m_r0));
            __m128i c1 = _mm_or_si128(_mm_shuffle_epi8(bg, m_bg1),
                                      _mm_shuffle_epi8(R,  m_r1));
            BYTE* d = drow + (size_t)x * 3;
            _mm_storeu_si128((__m128i*)d, c0);        // bytes 0..15
            _mm_storel_epi64((__m128i*)(d + 16), c1); // bytes 16..23
        }
        for (; x < w; ++x) {                          // tail
            const int Y = yrow[x];
            const int U = uvrow[(x >> 1) * 2];
            const int V = uvrow[(x >> 1) * 2 + 1];
            const int C = Y - 16, D = U - 128, E = V - 128;
            drow[x * 3 + 0] = Clip8((298 * C + 541 * D + 128) >> 8);
            drow[x * 3 + 1] = Clip8((298 * C - 55 * D - 136 * E + 128) >> 8);
            drow[x * 3 + 2] = Clip8((298 * C + 459 * E + 128) >> 8);
        }
    }
}

static void Nv12ToBgr24_dispatch(const BYTE* s, int w, int h, int sy, int suv, BYTE* d) {
    if (g_ssse3) Nv12ToBgr24_simd(s, w, h, sy, suv, d);
    else         Nv12ToBgr24_ref(s, w, h, sy, suv, d);
}
#else
static void cpu_init() {}
static void Nv12ToBgr24_dispatch(const BYTE* s, int w, int h, int sy, int suv, BYTE* d) {
    Nv12ToBgr24_ref(s, w, h, sy, suv, d);
}
#endif

// ================================ test rig ==================================
static size_t nv12_size(int w, int h) { return (size_t)w * h + (size_t)w * ((h + 1) / 2); }
static size_t bgr_size(int w, int h)  { return (size_t)w * h * 3; }

static int check(int w, int h, const BYTE* nv12, const char* label) {
    BYTE* a = (BYTE*)malloc(bgr_size(w, h));
    BYTE* b = (BYTE*)malloc(bgr_size(w, h));
    memset(a, 0xCC, bgr_size(w, h));
    memset(b, 0xCC, bgr_size(w, h));
    Nv12ToBgr24_ref(nv12, w, h, w, w, a);
    Nv12ToBgr24_dispatch(nv12, w, h, w, w, b);
    size_t n = bgr_size(w, h);
    for (size_t i = 0; i < n; ++i) {
        if (a[i] != b[i]) {
            printf("  [FAIL] %-16s %dx%d  first diff at byte %zu: ref=%d simd=%d (px %zu ch %zu)\n",
                   label, w, h, i, a[i], b[i], i / 3, i % 3);
            free(a); free(b); return 0;
        }
    }
    printf("  [ ok ] %-16s %dx%d  (%zu bytes identical)\n", label, w, h, n);
    free(a); free(b); return 1;
}

int main(int argc, char** argv) {
    cpu_init();
    printf("SSSE3 available: %s\n\n", g_ssse3 ? "yes" : "NO (scalar fallback)");

    // ---- 1. awkward geometries, random data ----
    printf("bit-exactness, random data & odd geometries:\n");
    int fails = 0, total = 0;
    const int sizes[][2] = {{1,1},{1,8},{8,1},{2,2},{3,3},{4,2},{5,7},{7,5},{8,8},{9,9},
                            {15,4},{16,16},{17,17},{31,3},{32,2},{33,33},{100,60},
                            {1921,1081},{1920,1080},{3840,2076},{2076,3840}};
    unsigned seed = 20260921u;
    for (auto& s : sizes) {
        int w = s[0], h = s[1];
        size_t n = nv12_size(w, h);
        BYTE* buf = (BYTE*)malloc(n);
        for (size_t i = 0; i < n; ++i) { seed = seed * 1103515245u + 12345u; buf[i] = (BYTE)(seed >> 16); }
        total++; fails += !check(w, h, buf, "random");
        free(buf);
    }
    printf("\n%d/%d geometries bit-exact\n\n", total - fails, total);

    // ---- 2. real 4K frames ----
    const char* raws[] = {
        (argc > 1) ? argv[1] : "runlola_3840x2076.raw",   // real frames, optional
    };
    int rw = 3840, rh = 2076;
    for (const char* path : raws) {
        FILE* f = fopen(path, "rb");
        if (!f) { printf("real frames: %s not readable, skipped\n\n", path); continue; }
        size_t need = bgr_size(rw, rh);
        BYTE* bgr = (BYTE*)malloc(need);
        BYTE* nv12 = (BYTE*)malloc(nv12_size(rw, rh));
        for (int k = 0; k < 4; ++k) {
            if (fread(bgr, 1, need, f) != need) break;
            // feed it as if it were an NV12 plane pair (content only matters for
            // exercising the maths; the geometry is the real one)
            memcpy(nv12, bgr, nv12_size(rw, rh) < need ? nv12_size(rw, rh) : need);
            total++; fails += !check(rw, rh, nv12, "real 4K frame");
        }
        fclose(f); free(bgr); free(nv12);
    }

    // ---- 3. benchmark ----
    printf("benchmark (median of 30, w==stride):\n");
    const int bs[][2] = {{1280,720},{1920,1080},{3840,2076}};
    for (auto& s : bs) {
        int w = s[0], h = s[1];
        size_t n12 = nv12_size(w, h), nb = bgr_size(w, h);
        BYTE* nv12 = (BYTE*)_aligned_malloc(n12, 64);
        BYTE* out  = (BYTE*)_aligned_malloc(nb, 64);
        unsigned sd = 7u;
        for (size_t i = 0; i < n12; ++i) { sd = sd * 1103515245u + 12345u; nv12[i] = (BYTE)(sd >> 16); }
        LARGE_INTEGER fq, t0, t1; QueryPerformanceFrequency(&fq);
        double t_ref, t_simd;
        for (int i = 0; i < 3; ++i) Nv12ToBgr24_ref(nv12, w, h, w, w, out);
        QueryPerformanceCounter(&t0);
        for (int i = 0; i < 30; ++i) Nv12ToBgr24_ref(nv12, w, h, w, w, out);
        QueryPerformanceCounter(&t1);
        t_ref = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / fq.QuadPart / 30.0;
        for (int i = 0; i < 3; ++i) Nv12ToBgr24_dispatch(nv12, w, h, w, w, out);
        QueryPerformanceCounter(&t0);
        for (int i = 0; i < 30; ++i) Nv12ToBgr24_dispatch(nv12, w, h, w, w, out);
        QueryPerformanceCounter(&t1);
        t_simd = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / fq.QuadPart / 30.0;
        printf("  %4dx%-5d  scalar %7.2f ms   simd %7.2f ms   %5.2fx   (%.2f -> %.2f GB/s out)\n",
               w, h, t_ref, t_simd, t_ref / t_simd,
               (double)nb / (t_ref / 1000.0) / 1e9, (double)nb / (t_simd / 1000.0) / 1e9);
        _aligned_free(nv12); _aligned_free(out);
    }
    printf("\n%s\n", (fails == 0) ? "ALL BIT-EXACT" : "FAILURES PRESENT");
    return fails ? 1 : 0;
}
