/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Native differential test; no DVD, FFmpeg or FPGA required.
 * cc -O2 -Wall -Wextra -Werror -fsanitize=address,undefined \
 *   player/tools/test_subtitle_blend_cache.c -o /tmp/test-subtitle-cache
 */
#include "subtitle_blend_cache.h"
#include <assert.h>
#include <stdio.h>

/* Frozen scalar arithmetic from the existing movie subtitle compositor.
 * Preserve its integer rounding, including the reciprocal multiply. */
static uint8_t reference_blend(uint8_t bg, uint8_t fg, unsigned a)
{
    if (!a) return bg;
    if (a >= 255) return fg;
    unsigned v = bg * (255 - a) + fg * a + 127;
    return (uint8_t)((v * 257) >> 16);
}

static void reference_chroma(const uint32_t palette[4], const uint8_t alpha[4],
                             const uint8_t pixels[4], uint8_t *u, uint8_t *v)
{
    unsigned sum = 0, cb = 0, cr = 0;
    for (int i = 0; i < 4; i++) {
        unsigned code = pixels[i] & 3;
        unsigned a = (alpha[code] & 15) * 17;
        sum += a;
        cb += a * (palette[code] & 255);
        cr += a * ((palette[code] >> 8) & 255);
    }
    if (sum) {
        *u = reference_blend(*u, cb / sum, sum >> 2);
        *v = reference_blend(*v, cr / sum, sum >> 2);
    }
}

static void check_palette(SubtitleBlendCache *cache, const uint32_t palette[4],
                          const uint8_t alpha[4])
{
    subtitle_cache_prepare(cache, palette, alpha);
    SubtitleBlendCache saved = *cache;
    subtitle_cache_prepare(cache, palette, alpha);
    assert(!memcmp(cache, &saved, sizeof(saved)));
    for (unsigned code = 0; code < 4; code++)
        for (unsigned bg = 0; bg < 256; bg++)
            assert(cache->luma[code][bg] == reference_blend(
                bg, (palette[code] >> 16) & 255, (alpha[code] & 15) * 17));

    /* Every 2x2 code combination, every background value in both planes.
     * Repeat on the same cache across palette/alpha changes to check invalidation. */
    for (unsigned key = 0; key < 256; key++) {
        uint8_t pixels[4];
        for (unsigned i = 0; i < 4; i++) pixels[i] = (key >> (2 * i)) & 3;
        for (unsigned bg = 0; bg < 256; bg++) {
            uint8_t u = bg, v = 255 - bg, ru = u, rv = v;
            reference_chroma(palette, alpha, pixels, &ru, &rv);
            assert(subtitle_cache_chroma(cache, pixels, 2, 2, 0, 0, &u, &v));
            assert(u == ru && v == rv);
        }
    }
    /* Odd width and nonzero origin: the second row must use the bitmap stride. */
    uint8_t bitmap[15] = {3, 0, 1, 2, 0, 2, 1, 3, 0, 2, 1, 2, 0, 3, 1};
    uint8_t pixels[4] = {bitmap[6], bitmap[7], bitmap[11], bitmap[12]};
    uint8_t u = 53, v = 217, ru = u, rv = v;
    reference_chroma(palette, alpha, pixels, &ru, &rv);
    assert(subtitle_cache_chroma(cache, bitmap, 5, 3, 1, 1, &u, &v));
    assert(u == ru && v == rv);

    /* Partial blocks must fall back without reading bitmap or altering output. */
    const int edges[][4] = {{2, 2, -1, 0}, {2, 2, 0, -1}, {2, 2, 1, 0},
        {2, 2, 0, 1}, {1, 2, 0, 0}, {2, 1, 0, 0}, {0, 0, 0, 0}};
    for (unsigned i = 0; i < sizeof(edges) / sizeof(edges[0]); i++) {
        u = 53; v = 217;
        assert(!subtitle_cache_chroma(cache, NULL, edges[i][0], edges[i][1],
                                      edges[i][2], edges[i][3], &u, &v));
        assert(u == 53 && v == 217);
    }
}

int main(void)
{
    SubtitleBlendCache cache = {0};
    uint32_t palette[4] = {0x008080, 0xff8080, 0x4c54ff, 0x952b15};
    uint8_t alpha[4] = {0};
    check_palette(&cache, palette, alpha); /* transparent */
    memset(alpha, 15, sizeof(alpha));
    check_palette(&cache, palette, alpha); /* opaque */
    for (unsigned a = 0; a < 16; a++) {
        alpha[0] = 0; alpha[1] = 15; alpha[2] = a; alpha[3] = 15 - a;
        check_palette(&cache, palette, alpha);
    }
    /* Deterministic palette and alpha transitions, including high unused bits. */
    uint32_t rng = 0x5ab713;
    for (int trial = 0; trial < 64; trial++) {
        for (int code = 0; code < 4; code++) {
            rng = rng * 1664525u + 1013904223u;
            palette[code] = rng;
            alpha[code] = rng >> 24;
        }
        check_palette(&cache, palette, alpha);
        palette[2] ^= 0xffffff; /* palette-only change */
        check_palette(&cache, palette, alpha);
        alpha[1] ^= 15; /* alpha-only change */
        check_palette(&cache, palette, alpha);
    }
    puts("PASS: 210 palette/alpha states; all 256 blocks x 256 backgrounds; stride and edge fallback");
    return 0;
}
