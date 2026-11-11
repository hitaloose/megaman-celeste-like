/* CRT overlays from ourigen's perfect_overlays (assets/overlays/, CREDITS.txt
 * there), embedded as PNG bytes in mm_overlays_data.h and decoded with stb_image. */
#include "mm_overlay.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#endif
#include "stb_image.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include "mm_overlays_data.h"

#define OVERLAYS ((int)(sizeof(k_overlays) / sizeof(k_overlays[0])))
static uint32_t* s_pixels[OVERLAYS];

int overlay_count(void) { return OVERLAYS; }

const char* overlay_name(int i) { return i >= 0 && i < OVERLAYS ? k_overlays[i].name : ""; }

const uint32_t* overlay_pixels(int i) {
    if (i < 0 || i >= OVERLAYS) return NULL;
    if (s_pixels[i]) return s_pixels[i];
    int w, h, n;
    unsigned char* rgba = stbi_load_from_memory(k_overlays[i].png, (int)k_overlays[i].size, &w, &h, &n, 4);
    if (!rgba || w != OVERLAY_W || h != OVERLAY_H) {
        fprintf(stderr, "[overlay] can't decode %s\n", k_overlays[i].name);
        stbi_image_free(rgba);
        return NULL;
    }
    uint32_t* px = (uint32_t*)malloc((size_t)w * h * sizeof(uint32_t));
    if (px) {
        for (int k = 0; k < w * h; k++) {
            const unsigned char* p = rgba + k * 4;
            px[k] = (uint32_t)p[3] << 24 | (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];
        }
    }
    stbi_image_free(rgba);
    s_pixels[i] = px;
    return px;
}

const uint32_t* overlay_fit(int i, int w) {
    static uint32_t* fit;
    const uint32_t* src = overlay_pixels(i);
    if (!src || w <= 0 || w > OVERLAY_W) return NULL;
    if (w == OVERLAY_W) return src;
    if (!fit && !(fit = (uint32_t*)malloc((size_t)OVERLAY_W * OVERLAY_H * sizeof(uint32_t)))) return NULL;
    int left = w / 2, cut = OVERLAY_W - w;
    for (int y = 0; y < OVERLAY_H; y++) {
        const uint32_t* s = src + y * OVERLAY_W;
        uint32_t* d = fit + y * w;
        memcpy(d, s, (size_t)left * sizeof(uint32_t));
        memcpy(d + left, s + left + cut, (size_t)(w - left) * sizeof(uint32_t));
    }
    return fit;
}
