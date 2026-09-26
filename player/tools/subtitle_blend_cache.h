/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef DVD_SUBTITLE_BLEND_CACHE_H
#define DVD_SUBTITLE_BLEND_CACHE_H

#include <stdint.h>
#include <string.h>

/* Only palette-dependent data is cached, never the changing video background.
 * CHG_COLCON subtitles retain the spatially varying compositor. */
typedef struct {
    int valid;
    uint32_t palette[4];
    uint8_t alpha[4];
    uint8_t luma[4][256];
    struct { uint8_t alpha, cb, cr; } chroma[256];
} SubtitleBlendCache;

static inline uint8_t subtitle_blend(uint8_t bg, uint8_t fg, unsigned alpha)
{
    if (alpha == 255) return fg;
    if (!alpha) return bg;
    return (uint8_t)((bg * (255u - alpha) + fg * alpha + 127u) * 257u >> 16);
}

static inline void subtitle_cache_prepare(SubtitleBlendCache *cache,
                                          const uint32_t palette[4],
                                          const uint8_t alpha[4])
{
    if (cache->valid && !memcmp(cache->palette, palette, sizeof(cache->palette)) &&
        !memcmp(cache->alpha, alpha, sizeof(cache->alpha)))
        return;
    memcpy(cache->palette, palette, sizeof(cache->palette));
    memcpy(cache->alpha, alpha, sizeof(cache->alpha));
    for (unsigned code = 0; code < 4; code++)
        for (unsigned bg = 0; bg < 256; bg++)
            cache->luma[code][bg] = subtitle_blend(bg, palette[code] >> 16,
                                                  (alpha[code] & 15) * 17u);
    for (unsigned key = 0; key < 256; key++) {
        unsigned sum = 0, cb = 0, cr = 0;
        for (unsigned pixel = 0; pixel < 4; pixel++) {
            unsigned code = (key >> (pixel * 2)) & 3;
            unsigned a = (alpha[code] & 15) * 17u;
            sum += a;
            cb += a * (palette[code] & 255);
            cr += a * ((palette[code] >> 8) & 255);
        }
        /* Match the original integer rounding and alpha-weighted 2x2 blend. */
        cache->chroma[key].alpha = sum >> 2;
        cache->chroma[key].cb = sum ? cb / sum : 0;
        cache->chroma[key].cr = sum ? cr / sum : 0;
    }
    cache->valid = 1;
}

static inline int subtitle_cache_chroma(const SubtitleBlendCache *cache,
                                        const uint8_t *idx, int w, int h,
                                        int sx, int sy, uint8_t *u, uint8_t *v)
{
    /* Edge/clipped blocks need the original partial-coverage calculation. */
    if (sx < 0 || sy < 0 || sx + 1 >= w || sy + 1 >= h)
        return 0;
    const uint8_t *top = idx + (size_t)sy * w + sx;
    const uint8_t *bottom = top + w;
    unsigned key = (top[0] & 3) | ((top[1] & 3) << 2) |
                   ((bottom[0] & 3) << 4) | ((bottom[1] & 3) << 6);
    *u = subtitle_blend(*u, cache->chroma[key].cb, cache->chroma[key].alpha);
    *v = subtitle_blend(*v, cache->chroma[key].cr, cache->chroma[key].alpha);
    return 1;
}
#endif
