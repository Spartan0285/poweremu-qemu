/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef PPC_MAC_GPU_R300_VP_H
#define PPC_MAC_GPU_R300_VP_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "ppc_mac_gpu_r300_fp.h"
#define R300_VP_MAX_INSTRUCTIONS 256
#define R300_VP_MAX_PARAMETERS 256
#define R300_VP_MAX_ATTRIBUTES 16

typedef struct R300VPProgram {
    uint32_t control[3];         /* PVS_CNTL_1/2/3: 0x22d0..0x22d8 */
    uint32_t code[R300_VP_MAX_INSTRUCTIONS][4];
    uint8_t code_valid[R300_VP_MAX_INSTRUCTIONS]; /* one bit per uploaded word */
    uint32_t flow_control;       /* 0x22dc: reject branching until implemented */
    uint32_t attribute_mask;     /* host linkage: populated float4 attributes */
    uint32_t varying_mask;
    uint8_t varying_output[R300_FP_MAX_INPUTS]; /* VP output feeding each FS input */
    uint32_t varying_constant_mask; /* RS-generated constants feeding FS inputs */
    float varying_constant[R300_FP_MAX_INPUTS][4];
    float point_size;            /* full rasterized point width/height in pixels */
    float point_half_clip_x;      /* rectangle expansion from point center */
    float point_half_clip_y;
    bool expand_point_rect;       /* six duplicated inputs become two triangles */
    bool zero_initialize_temporaries; /* captured Apple programs read unwritten lanes */
    bool flip_y;                 /* adapt positive guest viewport Y scale to Metal */
} R300VPProgram;

/* Appends a vertex shader to MSL emitted by r300_fp_compile_msl, which defines
 * R300FragmentInput. Current ISA gate: DOT, MUL, ADD, MAD; no relative access.
 * Input buffer 0 holds float4 a[16] per vertex. Constant buffer 1 holds the
 * entire 256-vector parameter bank. The host must apply the guest viewport,
 * primitive assembly, attribute conversion, and RS routing separately.
 */
bool r300_vp_compile_msl(const R300VPProgram *p, char *out, size_t capacity,
                         char *error, size_t error_capacity);
#endif
