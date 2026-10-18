// -----------------------------------------------------------------------------
//  premul_bench.cpp  --  measure the CPU premultiply pass on its own.
//
//  Why this exists separately from the live [present] breakdown: the breakdown
//  needs a match, a game, and UAC, and it measures four things at once.  If the
//  premultiply is the slow phase, that can be established here in ten seconds,
//  on any machine, with no game running -- and if it is not the slow phase, this
//  says so just as clearly and saves looking in the wrong place.
//
//  Two destinations are measured, and the comparison is the point:
//
//    heap    an ordinary std::vector<BYTE>.  This is the speed of the
//            arithmetic itself.
//
//    dib     a CreateDIBSection buffer, which is what the overlay actually
//            writes into.  A DIB section is not necessarily ordinary memory:
//            the kernel may back it with video aperture space, and writes to
//            that are dramatically slower than writes to RAM -- on some drivers
//            by more than an order of magnitude.
//
//  If heap is fast and dib is slow, the answer is not to make the arithmetic
//  cheaper; it is to stop writing into a DIB section, which is a change to the
//  present path rather than to the premultiply.  The two numbers pointing in
//  different directions is exactly why they are measured together.
// -----------------------------------------------------------------------------
#include <Windows.h>
#include <emmintrin.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

// The production premultiply, copied verbatim from overlay.cpp.
void premultiply_row(const BYTE* src, BYTE* dst, int width) {
    int x = 0;

    const __m128i zero   = _mm_setzero_si128();
    const __m128i rnd    = _mm_set1_epi16(127);
    const __m128i one    = _mm_set1_epi16(1);
    const __m128i alpha_mask = _mm_set1_epi32((int)0xFF000000u);

    for (; x + 4 <= width; x += 4) {
        const __m128i px = _mm_loadu_si128((const __m128i*)(src + (size_t)x * 4));

        const __m128i a32 = _mm_set1_epi32(255);
        const __m128i alpha_lane = _mm_srli_epi32(px, 24);
        if (_mm_movemask_epi8(_mm_cmpeq_epi32(alpha_lane, a32)) == 0xFFFF) {
            _mm_storeu_si128((__m128i*)(dst + (size_t)x * 4), px);
            continue;
        }

        __m128i lo = _mm_unpacklo_epi8(px, zero);
        __m128i hi = _mm_unpackhi_epi8(px, zero);

        __m128i alo = _mm_shufflelo_epi16(lo, _MM_SHUFFLE(3, 3, 3, 3));
        alo = _mm_shufflehi_epi16(alo, _MM_SHUFFLE(3, 3, 3, 3));
        __m128i ahi = _mm_shufflelo_epi16(hi, _MM_SHUFFLE(3, 3, 3, 3));
        ahi = _mm_shufflehi_epi16(ahi, _MM_SHUFFLE(3, 3, 3, 3));

        lo = _mm_mullo_epi16(lo, alo);
        hi = _mm_mullo_epi16(hi, ahi);
        lo = _mm_add_epi16(lo, rnd);
        hi = _mm_add_epi16(hi, rnd);
        lo = _mm_srli_epi16(_mm_add_epi16(_mm_add_epi16(lo, _mm_srli_epi16(lo, 8)), one), 8);
        hi = _mm_srli_epi16(_mm_add_epi16(_mm_add_epi16(hi, _mm_srli_epi16(hi, 8)), one), 8);

        __m128i out = _mm_packus_epi16(lo, hi);
        out = _mm_or_si128(_mm_andnot_si128(alpha_mask, out),
                           _mm_and_si128(alpha_mask, px));

        const __m128i zero32 = _mm_setzero_si128();
        __m128i mask = _mm_cmpeq_epi32(_mm_srli_epi32(px, 24), zero32);
        out = _mm_andnot_si128(mask, out);

        _mm_storeu_si128((__m128i*)(dst + (size_t)x * 4), out);
    }

    for (; x < width; ++x) {
        const BYTE* s = src + (size_t)x * 4;
        BYTE*       d = dst + (size_t)x * 4;
        const BYTE a = s[3];
        if (a == 0) {
            d[0] = d[1] = d[2] = d[3] = 0;
        } else if (a == 255) {
            d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = 255;
        } else {
            d[0] = (BYTE)((s[0] * a + 127) / 255);
            d[1] = (BYTE)((s[1] * a + 127) / 255);
            d[2] = (BYTE)((s[2] * a + 127) / 255);
            d[3] = a;
        }
    }
}

void fill_overlay_like(std::vector<BYTE>& v, int w, int h) {
    // The real overlay is mostly fully transparent with a few drawn elements, so
    // the data is built the same way: that distribution is what the fast paths
    // are for and it is what determines the measured average.
    v.assign((size_t)w * h * 4, 0);
    for (int i = 0; i < 400; ++i) {
        // Scatter some opaque and some semi-transparent runs.
        const size_t at = (size_t)(rand() % (w * h)) * 4;
        if (at + 64 >= v.size()) continue;
        const BYTE a = (i & 1) ? 255 : (BYTE)(60 + (i % 120));
        for (int k = 0; k < 16; ++k) {
            v[at + (size_t)k * 4 + 0] = 0xC2;
            v[at + (size_t)k * 4 + 1] = 0xC2;
            v[at + (size_t)k * 4 + 2] = 0xC2;
            v[at + (size_t)k * 4 + 3] = a;
        }
    }
}

double time_once(const std::vector<BYTE>& src, BYTE* dst, int w, int h) {
    auto t0 = std::chrono::high_resolution_clock::now();
    for (int y = 0; y < h; ++y) {
        premultiply_row(src.data() + (size_t)y * w * 4, dst + (size_t)y * w * 4, w);
    }
    auto t1 = std::chrono::high_resolution_clock::now();
    return std::chrono::duration<double, std::milli>(t1 - t0).count();
}

} // namespace

int main() {
    const int w = 1920, h = 1080;

    std::vector<BYTE> src;
    fill_overlay_like(src, w, h);

    printf("premul_bench: %dx%d, %zu bytes per frame\n", w, h, src.size());

    // -------------------------------------------------------------------------
    //  Heap destination.
    // -------------------------------------------------------------------------
    std::vector<BYTE> heap_dst(src.size());
    double heap_best = 1e9;
    for (int i = 0; i < 20; ++i) heap_best = std::min(heap_best, time_once(src, heap_dst.data(), w, h));
    printf("  heap destination : %6.3f ms/frame  (best of 20)\n", heap_best);

    // -------------------------------------------------------------------------
    //  DIB section destination -- what the overlay uses.
    // -------------------------------------------------------------------------
    HDC screen = GetDC(nullptr);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = w;
    bi.bmiHeader.biHeight      = -h;
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);

    if (!dib || !bits) {
        printf("  DIB creation failed; comparing against heap only\n");
    } else {
        double dib_best = 1e9;
        for (int i = 0; i < 20; ++i) dib_best = std::min(dib_best, time_once(src, (BYTE*)bits, w, h));
        printf("  dib destination  : %6.3f ms/frame  (best of 20)\n", dib_best);

        printf("\n  ratio dib/heap    : %.2fx\n", dib_best / (heap_best > 0 ? heap_best : 1e-9));
        if (dib_best > heap_best * 3.0) {
            printf("  VERDICT: the DIB section is the slow part, not the arithmetic.\n"
                   "           The fix is to stop writing frames into a DIB section\n"
                   "           (present from the staging buffer directly), not to make\n"
                   "           the premultiply cheaper.\n");
        } else {
            printf("  VERDICT: a DIB section costs about what heap memory costs here,\n"
                   "           so the premultiply is not the bottleneck.  Look at the\n"
                   "           Map and UpdateLayeredWindow phases instead.\n");
        }

        DeleteObject(dib);
    }

    return 0;
}
