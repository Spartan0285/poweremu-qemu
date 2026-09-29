/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef PPC_MAC_GPU_R300_FP_H
#define PPC_MAC_GPU_R300_FP_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define R300_FP_MAX_ALU 64
#define R300_FP_MAX_TEX 32
#define R300_FP_MAX_INPUTS 10
#define R300_FP_MSL_CAPACITY 65536

typedef struct R300FPInstruction {
    uint32_t rgb_addr;        /* 0x46c0 + index * 4 */
    uint32_t alpha_addr;      /* 0x47c0 + index * 4 */
    uint32_t rgb_inst;        /* 0x48c0 + index * 4 */
    uint32_t alpha_inst;      /* 0x49c0 + index * 4 */
} R300FPInstruction;

typedef struct R300FPProgram {
    uint32_t control[3];      /* 0x4600, 0x4604, 0x4608 */
    uint32_t node[4];         /* 0x4610..0x461c; last active node is node[3] */
    uint32_t tex[R300_FP_MAX_TEX];
    R300FPInstruction alu[R300_FP_MAX_ALU];
    /* Host linkage, decoded separately from guest RS routing. Each set bit
     * supplies the initial value of that temporary via vN. Never infer it
     * from a register read: undeclared/unwritten inputs are rejected. */
    uint32_t input_mask;
    uint32_t alpha_func;      /* 0x4bd4: comparison only, no alpha-to-coverage */
} R300FPProgram;

/* Current gate: one node, MAD ALU, TEX/TXP 2D sampling, full final output 0.
 * Constants must already be float32. The caller supplies correct interpolator
 * routing, sampler state, texture formats, target state and synchronization.
 * This compiler alone does not establish support for a guest draw.
 * On failure clears output and supplies a reason. Non-NULL buffers and
 * nonzero capacities are required. Generated entry is r300_fragment, with
 * R300FragmentInput varyings and texture/sampler bindings matching image IDs.
 */
bool r300_fp_compile_msl(const R300FPProgram *p, char *out, size_t capacity,
                         char *error, size_t error_capacity);
#endif
