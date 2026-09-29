/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef PPC_MAC_GPU_R300_METAL_H
#define PPC_MAC_GPU_R300_METAL_H
#include "ppc_mac_gpu_r300_data.h"

typedef struct R300MetalTexture {
    const uint8_t *rgba;
    size_t size;
    unsigned width, height;
    bool linear_filter;
    bool clamp_to_zero_s;
    bool clamp_to_zero_t;
    unsigned max_anisotropy; /* zero/one means isotropic, at most 16 */
} R300MetalTexture;

typedef struct R300MetalDraw {
    const R300VPProgram *vp;
    const R300FPProgram *fp;
    const R300VertexInput *vertices; /* triangle list, or points when selected */
    unsigned vertex_count;
    bool primitive_points;
    const float (*vertex_constants)[4];   /* 256 vectors */
    const float (*fragment_constants)[4]; /* 32 vectors */
    R300MetalTexture textures[16];
    /* Linear RGBA8 target, initialized with existing destination contents.
     * Updated only after successful GPU completion/readback. */
    uint8_t *target;
    size_t target_size;
    unsigned width, height;
    unsigned scissor[4]; /* x,y,width,height */
    double viewport_x, viewport_y;
    double viewport_width, viewport_height; /* zero means full target */
    bool premultiplied_over; /* src + dst * (1 - src alpha), RGB and alpha */
    unsigned color_mask; /* RGBA bits 0..3 */
} R300MetalDraw;

void *r300_metal_create(void);
void r300_metal_destroy(void *renderer);
/* Synchronous correctness path: only premultiplied-over blending; no depth/stencil/tiling.
 * Device-side decoding must reject such state until an implementation exists.
 * Viewport covers the target; vertex shader supplies GL clip coordinates.
 * May be used by offline replay and, later, the isolated R350 device. */
bool r300_metal_draw(void *renderer, const R300MetalDraw *draw,
                     char *error, size_t error_size);
#endif
