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
    /*
     * State that has to be compiled into the shader rather than set on the
     * pipeline, and therefore belongs to the compiled variant rather than to
     * the draw.  Mesa keeps the same distinction in
     * r300_fragment_program_external_state.
     *
     * guest_order: the target is guest VRAM read as BGRA8Unorm, so a true
     *   (r,g,b,a) has to be written as .yxwz to land as the big-endian ARGB
     *   word every other consumer of VRAM expects.  The swizzle is its own
     *   inverse, so a destination read back through framebuffer fetch is
     *   unswizzled the same way.
     * blend_in_shader: blend with blend_src/dst_* , computed in the shader.
     *   Metal's fixed-function blending cannot be used on a guest-order
     *   target, because its factors would operate on the swizzled channels.
     *   The factors are the 0x20-based R200/R300 encodings, and the equation
     *   is ADD with clamping -- the only one the device gate accepts.  This
     *   replaces an earlier bool that hard-coded ONE/ONE_MINUS_SRC_ALPHA.
     */
    bool guest_order;
    bool blend_in_shader;
    uint8_t blend_src_rgb, blend_dst_rgb;
    uint8_t blend_src_alpha, blend_dst_alpha;
    /*
     * How to reorder a sampled texel into (r,g,b,a), per image unit.
     * Each entry indexes r,g,b,a,zero,one of the value the sampler returned.
     *
     * A texture read straight out of guest VRAM arrives in the guest's byte
     * order, so the guest's own channel swizzle and that byte order both have
     * to be resolved into this one reorder.  Doing it here rather than on the
     * CPU is what lets the texture be sampled in place instead of being
     * rewritten texel by texel before every draw.
     *
     * Only consulted when tex_swizzled is set, so a program compiled without
     * it -- an offline replay, or a staged texture that was already put in
     * (r,g,b,a) order by the CPU -- samples unchanged.
     */
    uint8_t tex_swizzle[16][4];
    bool tex_swizzled;
    /*
     * US_OUT_FMT_0's C0..C3 selects, already resolved into the swizzle that
     * has to be applied to the shader's own (r,g,b,a) before the target's
     * byte order is applied.  The identity "rgba" reproduces C0=B, C1=G,
     * C2=R, C3=A -- the arrangement that lands a natural (r,g,b,a) as a
     * big-endian ARGB word, and the only one the compiler used to accept.
     *
     * Apple's 9700 accelerator uses a different arrangement for its blit
     * shader (C0=A, C1=B, C2=G, C3=R), because the texture swizzle that
     * feeds it has already rotated the source bytes; the two cancel and the
     * copy is byte-for-byte.  Honouring one without the other would rotate
     * every colour it draws.
     *
     * An empty string means the identity, so a program that never sets it
     * compiles exactly as before.
     */
    char out_swizzle[5];
    /*
     * Fog, applied after the shader output and before the alpha test, which
     * is where the FG block sits in the pipeline.  The factor does not come
     * through RS: GB_SELECT names its source and GA_FOG_SCALE/OFFSET map it,
     * so the source arrives here as one fragment input's alpha (or straight
     * from the fragment position for the Z and 1/W selections).
     *
     * f = saturate(source * fog_scale + fog_offset), the amount of fog.
     * Alpha is not fogged.
     */
    bool fog_enable;
    float fog_colour[3];
    float fog_scale, fog_offset;
    int8_t fog_input;          /* fragment input carrying the source, or -1 */
    uint8_t fog_source;        /* GB_SELECT.FOG_SELECT, for the -1 cases */
    /* Which fragment input each vertex colour was routed to, 0xff if none,
     * and which vertex-program output carries it.  Only meaningful when
     * colour_map_valid: the other routing paths do not fill these in. */
    uint8_t colour_input[4];
    uint8_t colour_vp_output[4];
    bool colour_map_valid;
    /* Experimental second color attachment mirrors passing depth writes.
     * 0 disabled, 1 guest Z16, 2 guest Z24S8; native depth/stencil stays active. */
    uint8_t depth_mirror;
} R300FPProgram;

/* Current gate: one node, MAD ALU, TEX/TXP 2D sampling, full final output 0.
 * Constants must already be float32. The caller supplies correct interpolator
 * routing, sampler state, texture formats, target state and synchronization.
 * This compiler alone does not establish support for a guest draw.
 * On failure clears output and supplies a reason. Non-NULL buffers and
 * nonzero capacities are required. Generated entry is r300_fragment, with
 * R300FragmentInput varyings and texture/sampler bindings matching image IDs.
 */
/* Fragment buffer 0 contains 32 guest constants followed by two host-only
 * fog vectors. Lighting parameters are uniforms, not shader variants. */
#define R300_FP_HOST_CONSTANT_COUNT 34
void r300_fp_pack_constants(const R300FPProgram *p,
                            const float guest[32][4],
                            float host[R300_FP_HOST_CONSTANT_COUNT][4]);

/* Texture units referenced by every active node of a validated program.
 * First-node texture enable does not suppress later nodes' fetches. */
uint16_t r300_fp_texture_mask(const R300FPProgram *p);

bool r300_fp_compile_msl(const R300FPProgram *p, char *out, size_t capacity,
                         char *error, size_t error_capacity);
#endif
