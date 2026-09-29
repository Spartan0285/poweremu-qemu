/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef PPC_MAC_GPU_R300_DATA_H
#define PPC_MAC_GPU_R300_DATA_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "ppc_mac_gpu_r300_vp.h"

typedef struct R300VertexInput {
    float a[R300_VP_MAX_ATTRIBUTES][4];
} R300VertexInput;

/* Decode the normal/zero subset of S16E7 constants. Nonzero subnormals and
 * exponent-all-ones encodings are rejected pending hardware validation. */
bool r300_float24_decode(uint32_t word, float *value);

/* Input words are canonical host uint32_t values after PM4 byte swapping.
 * Stream entries describe packed fields in order; EXT selects vector lanes.
 * Supported: FLOAT1..4 and unsigned BYTE/D3DCOLOR, normalized or integer.
 * Requires a LAST_VEC marker, full written vectors, and exact payload length.
 * Layout and bounds are checked before touching output. */
bool r300_decode_immediate(const uint32_t stream[8], const uint32_t ext[8],
                           const uint32_t *words, size_t word_count,
                           unsigned vertex_count, R300VertexInput *out,
                           size_t output_count, uint32_t *attribute_mask);
/* Expand smooth-shaded triangle lists, fans, strips and independent quads.
 * Preserves winding. Rejects incomplete primitives before writing output. */
bool r300_triangle_indices(unsigned primitive, unsigned vertices,
                            unsigned *indices, size_t capacity, unsigned *count);
/* R300 rasterizes points as screen-space quads.  POINT_SIZE contains half
 * width/height in the subpixel precision selected by GB_TILE_CONFIG.  Point
 * texture coordinates are stuffed into attribute 1 for fragment input 0. */
bool r300_expand_points(const R300VertexInput *points, unsigned point_count,
                        uint32_t point_size, unsigned subpixel_divisor,
                        float s0, float t0, float s1, float t1,
                        R300VertexInput *triangles, size_t capacity,
                        unsigned *triangle_vertices);
/* Backend adapter for TCL-bypass constant-color draws with window XY and
 * depth disabled. Produces clip-space coordinates that preserve window XY. */
bool r300_window_vertex_program(R300VPProgram *program, float constants[256][4],
                                 float xscale, float xoffset, float yscale, float yoffset);
bool r300_window_point_program(R300VPProgram *program, float constants[256][4],
                               float xscale, float xoffset,
                               float yscale, float yoffset);
#endif
