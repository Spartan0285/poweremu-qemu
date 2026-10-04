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
    /*
     * Sample straight out of guest VRAM rather than from a staged copy.
     * `rgba` is then unused, and the fragment program must have been compiled
     * with the matching tex_swizzle: the texels arrive in the guest's byte
     * order and nothing has reordered them.
     */
    bool guest_source;
    uint32_t guest_offset;
    uint32_t guest_pitch;    /* bytes per row */
    unsigned guest_bpp;      /* 1 or 4 */
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
    /*
     * Blending as the guest programmed it.  This replaces an earlier
     * `premultiplied_over` flag that hard-coded ONE / ONE_MINUS_SRC_ALPHA:
     * Chess asks for ordinary non-premultiplied alpha (SRC_ALPHA /
     * ONE_MINUS_SRC_ALPHA) and so every one of its blended draws was
     * refused by the device's blend gate.  Factors are the 0x20-based
     * R200/R300 encodings; the equation is always ADD with clamping, which
     * is the only one the gate accepts.
     */
    bool blend_enable;
    uint8_t blend_src_rgb, blend_dst_rgb;
    uint8_t blend_src_alpha, blend_dst_alpha;
    unsigned color_mask; /* RGBA bits 0..3 */
    /*
     * Render straight into guest VRAM instead of staging.
     *
     * With this set, `target` is unused: the colour attachment is a linear
     * view over VRAM at guest_offset with guest_pitch bytes per row, written
     * in guest byte order.  The fragment program must have been compiled
     * with guest_order, and with blend_in_shader when blending --
     * Metal's own blending cannot be used on a guest-order target because
     * its factors would operate on the swizzled channels.
     *
     * Nothing is uploaded, nothing is read back, and the draw does not wait
     * for the GPU.  Ordering against the guest CPU and the 2D engine comes
     * from the shared submission layer's hazard tracking.
     */
    bool guest_target;
    uint32_t guest_offset;
    uint32_t guest_pitch;    /* bytes per row */
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
