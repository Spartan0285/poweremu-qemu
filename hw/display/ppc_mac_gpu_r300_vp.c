/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef R300_FP_STANDALONE
#include "qemu/osdep.h"
#endif
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "ppc_mac_gpu_r300_vp.h"

typedef struct VPWriter {
    char *out;
    size_t size, used;
    bool failed;
} VPWriter;

static void emit(VPWriter *w, const char *fmt, ...)
{
    if (w->failed) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(w->out + w->used, w->size - w->used, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= w->size - w->used) {
        w->failed = true;
    } else {
        w->used += n;
    }
}

/*
 * `need` says which components of the result this operand actually reaches:
 * the instruction's write mask for a component-wise operation, and all four
 * for a dot product, where every component feeds every output.
 *
 * A component outside it may read a register that was never written.  The
 * hardware has no notion of an undefined register -- it returns whatever the
 * file holds -- and the result is discarded by the write mask either way, so
 * refusing the whole instruction over it rejects a program that is perfectly
 * well defined in its own terms.  Chess does exactly this: it writes t[7].xyz
 * and then reads -t[7].xyzw into an add that keeps only xyz.
 */
static bool source(char *out, size_t cap, uint32_t word,
                   uint32_t attributes, const uint8_t defined[32],
                   unsigned param_base, unsigned param_max, unsigned need)
{
    /*
     * Field widths from the hardware's own description of a source operand
     * (R5xx acceleration guide, "PVS Source Operand Description"):
     *
     *   1:0   register type      3     absolute value
     *   2     spare              4     addressing mode 0
     *   12:5  offset             31    addressing mode 1
     *
     * This used to read the type as five bits, which quietly swallowed the
     * spare bit, the absolute-value flag and an addressing mode -- so a
     * source asking for abs() looked like a register class that does not
     * exist, and was refused for the wrong reason.
     */
    if (word & ((1u << 4) | (1u << 31))) {
        return false;               /* relative addressing: not translated */
    }
    unsigned kind = word & 3, index = (word >> 5) & 255;
    bool absolute = (word >> 3) & 1;
    char components[4][48];
    for (unsigned i = 0; i < 4; i++) {
        unsigned swizzle = (word >> (13 + i * 3)) & 7;
        const char *neg = word & (1u << (25 + i)) ? "-" : "";
        if (swizzle == 4 || swizzle == 5) {
            snprintf(components[i], sizeof(components[i]), "%s%s", neg,
                     swizzle == 4 ? "0.0f" : "1.0f");
        } else if (swizzle < 4) {
            char channel = "xyzw"[swizzle];
            if (kind == 0 && index < 32 && !(defined[index] & (1u << swizzle))
                && !(need & (1u << i))) {
                /* Never written, and this lane is thrown away. */
                snprintf(components[i], sizeof(components[i]), "0.0f");
            } else if (kind == 0 && index < 32 && (defined[index] & (1u << swizzle))) {
                snprintf(components[i], sizeof(components[i]), "%st[%u].%c", neg, index, channel);
            } else if (kind == 1 && index < R300_VP_MAX_ATTRIBUTES &&
                       (attributes & (1u << index))) {
                snprintf(components[i], sizeof(components[i]), "%sa[%u].%c", neg, index, channel);
            } else if (kind == 2 && index <= param_max &&
                       param_base + index < R300_VP_MAX_PARAMETERS) {
                snprintf(components[i], sizeof(components[i]), "%sc[%u].%c", neg, param_base + index, channel);
            } else if (kind == 3 && index < R300_VP_ALT_TEMPORARIES) {
                /* The alternate temporary file, which the math half of a
                 * dual instruction writes and the vector half reads back. */
                snprintf(components[i], sizeof(components[i]), "%sm[%u].%c", neg, index, channel);
            } else {
                return false;
            }
        } else {
            return false;
        }
    }
    int n = absolute ?
        snprintf(out, cap, "abs(float4(%s,%s,%s,%s))",
                 components[0], components[1], components[2], components[3]) :
        snprintf(out, cap, "float4(%s,%s,%s,%s)",
                 components[0], components[1], components[2], components[3]);
    return n >= 0 && (size_t)n < cap;
}

bool r300_vp_compile_msl(const R300VPProgram *p, char *out, size_t capacity,
                         char *error, size_t error_capacity)
{
    VPWriter w = {.out = out, .size = capacity};
    uint8_t temps[32] = {0}, results[32] = {0};
    unsigned start = p->control[0] & 1023;
    unsigned position_end = (p->control[0] >> 10) & 1023;
    unsigned end = (p->control[0] >> 20) & 1023;
    unsigned param_base = p->control[1] & 255;
    unsigned param_max = (p->control[1] >> 16) & 255;
    const char *reason = NULL;
    char detail[160] = {0};
    out[0] = error[0] = 0;
    if (p->zero_initialize_temporaries) {
        memset(temps, 15, sizeof(temps));
    }
    if (p->flow_control || (p->control[0] & 0xc0000000u) || start > position_end || position_end > end ||
        end >= R300_VP_MAX_INSTRUCTIONS || p->control[2] < start ||
        p->control[2] > end || (p->control[1] & ~0x00ff00ffu) ||
        param_base + param_max >= R300_VP_MAX_PARAMETERS ||
        (p->attribute_mask & ~0xffffu) ||
        (p->varying_mask & ~((1u << R300_FP_MAX_INPUTS) - 1)) ||
        (p->varying_constant_mask & ~p->varying_mask) ||
        !isfinite(p->point_size) || p->point_size > 511.0f ||
        !isfinite(p->point_half_clip_x) || !isfinite(p->point_half_clip_y) ||
        fabsf(p->point_half_clip_x) > 4096.0f || fabsf(p->point_half_clip_y) > 4096.0f) {
        reason = "unsupported vertex program range, parameters or linkage";
        goto fail;
    }
    emit(&w, "\nstruct R300VertexInput { float4 a[16]; };\n"
             "struct R300VertexOutput { float4 position [[position]];\n");
    for (unsigned i = 0; i < R300_FP_MAX_INPUTS; i++) {
        emit(&w, " float4 v%u [[user(locn%u)]];\n", i, i);
    }
    emit(&w, " float point_size [[point_size]]; };\n"
             "vertex R300VertexOutput r300_vertex(uint vid [[vertex_id]], "
             "device const R300VertexInput *vertices [[buffer(0)]], "
             "constant float4 *c [[buffer(1)]]) {\n"
             " device const float4 *a = vertices[vid].a;\n"
             " float4 t[32]; float4 o[32]; float4 m[%u];\n", R300_VP_ALT_TEMPORARIES);
    /* ATRM is read before it is written where a program pipelines a dual
     * instruction ahead of its consumer; zero is the only defensible value. */
    emit(&w, " for (uint i = 0; i < %u; i++) m[i] = float4(0.0f);\n",
         R300_VP_ALT_TEMPORARIES);
    if (p->zero_initialize_temporaries) {
        emit(&w, " for (uint i = 0; i < 32; i++) t[i] = float4(0.0f);\n");
    }
    for (unsigned pc = start; pc <= end; pc++) {
        const uint32_t *code = p->code[pc];
        /*
         * Field widths from the hardware's description of the opcode and
         * destination word (R5xx acceleration guide, "PVS Opcode &
         * Destination Operand Description"):
         *
         *   5:0  opcode        11:8  register type    20:23 write enables
         *   6    math inst     12    addressing mode  24/25 vector/math sat
         *   7    macro inst    19:13 offset           28    dual math op
         *
         * The register type is four bits and the offset seven, not five
         * apiece; bits 18 and 19 are part of the offset and were being
         * refused as reserved, so any program addressing a destination above
         * 31 was turned away for a reason that was not true.
         */
        unsigned op = code[0] & 255, kind = (code[0] >> 8) & 15;
        unsigned dst = (code[0] >> 13) & 127, mask = (code[0] >> 20) & 15;
        bool dual = (code[0] >> 28) & 1;
        /* 68 is the math engine's lighting coefficient: three operands,
         * W components only. */
        unsigned count = (op == 4 || op == 128 || op == 68) ? 3 : 2;
        /*
         * Bit 28 stays refused, and the reason is worth keeping.
         *
         * It was let through on the reasoning that the top bits of the
         * destination word are modifiers and that the one at 28 selects
         * between the halves of a *math* instruction, which a dot product is
         * not -- so it should be inert.  The 434 draws that set it then
         * failed at the next step instead, reading a source of kind 3.
         *
         * Kind 3 is the alternate temporary file, and that is what settles
         * it: ATRM exists for pair scheduling and is the only file the math
         * half of a dual instruction can write.  So bit 28 is not inert on
         * these instructions at all -- they are dual, a vector op and a math
         * op issued together, and this translator emits only the vector half.
         * Allowing the bit buys a draw that runs with half its arithmetic
         * missing, which is worse than one that does not run.
         *
         * Supporting them means decoding both halves and giving the
         * translator the second register file.  Until then they are refused,
         * which is what the census says.
         */
        /*
         * Accepted: opcode, destination, write enables, and the dual-math
         * bit.  Still refused, because none of it is translated: relative
         * addressing (12, 29:31), the two saturate flags (24, 25) and
         * predication (26, 27).
         *
         * A dual instruction's vector half may use at most two sources --
         * the third source word is where the math instruction lives -- so a
         * three-source opcode cannot be one.
         */
        if (p->code_valid[pc] != 15 || (code[0] & ~0x10ffefffu) ||
            (kind != 0 && kind != 2) || !mask || dst >= 32 ||
            (dual && count == 3) ||
            (op != 1 && op != 2 && op != 3 && op != 4 && op != 128 &&
             op != 68)) {
            /*
             * Say which of these it was, and for which opcode.  As one name
             * it covered five unrelated conditions -- a program uploaded
             * incompletely, a reserved bit, a destination this translator
             * does not write, an empty write mask, and an opcode it cannot
             * translate -- and the census could not tell them apart.  Kept
             * short so that distinct kinds stay few.
             */
            snprintf(detail, sizeof(detail), "vertex %s op=%u kind=%u bits=%08x",
                     p->code_valid[pc] != 15 ? "program incomplete" :
                     (code[0] & ~0x00f3ffffu) ? "reserved bits" :
                     (kind != 0 && kind != 2) ? "destination kind" :
                     !mask ? "empty write mask" : "opcode",
                     op, kind, code[0] & ~0x00f3ffffu);
            reason = detail;
            goto fail;
        }
        char operands[3][256];
        for (unsigned i = 0; i < count; i++) {
            /* A dot product mixes all four components into every output;
             * everything else here is component-wise. */
            if (!source(operands[i], sizeof(operands[i]), code[i + 1],
                        p->attribute_mask, temps, param_base, param_max,
                        (op == 1 || op == 68) ? 15u : mask)) {
                snprintf(detail, sizeof(detail),
                         "undefined vertex source pc=%u operand=%u word=%08x attributes=%04x",
                         pc, i, code[i + 1], p->attribute_mask);
                reason = detail;
                goto fail;
            }
        }
        emit(&w, " { float4 va = %s; float4 vb = %s;\n", operands[0], operands[1]);
        if (op == 68) {
            /*
             * ME_LIGHT_COEFF_DX, the lighting coefficients:
             *
             *   x = 1
             *   y = max(B.w, 0)
             *   z = B.w > 0 ? max(A.w, 0) ^ clamp(C.w, -128, 128) : 0
             *   w = 1
             *
             * The exponent is clamped by the hardware, and the base is
             * clamped at zero before the power -- a negative base would
             * otherwise give a NaN rather than the zero the hardware
             * produces.
             */
            emit(&w, " float4 vc = %s;\n", operands[2]);
            emit(&w, " float lz = vb.w > 0.0f ? pow(max(va.w, 0.0f),"
                     " clamp(vc.w, -128.0f, 128.0f)) : 0.0f;\n");
            emit(&w, " float4 value = float4(1.0f, max(vb.w, 0.0f), lz, 1.0f);\n");
        } else if (count == 3) {
            emit(&w, " float4 vc = %s; float4 value = va * vb + vc;\n", operands[2]);
        } else {
            emit(&w, " float4 value = %s;\n", op == 1 ? "float4(dot(va, vb))" :
                 op == 2 ? "va * vb" : "va + vb");
        }
        for (unsigned i = 0; i < 4; i++) {
            if (mask & (1u << i)) {
                emit(&w, " %c[%u].%c = value.%c;\n", kind == 0 ? 't' : 'o',
                     dst, "xyzw"[i], "xyzw"[i]);
            }
        }
        (kind == 0 ? temps : results)[dst] |= mask;
        /*
         * The math half of a dual instruction.
         *
         * The third source word is not a source at all: it holds the whole
         * math instruction (R5xx acceleration guide, "Dual Math Instruction
         * (Replaces PVS_SRC_OPERAND_2)"):
         *
         *   1:0   source type      18:16 second operand swizzle
         *   2     opcode msb       20:19 destination, ATRM 0-3
         *   3     absolute value   24:21 opcode
         *   12:5  source offset    25/26 negate first/second operand
         *   15:13 first operand    28:27 which component to write
         *
         * Only opcode 9 is translated.  It is the one Chess uses, on all
         * three of its dual instructions, and the others are a scalar unit's
         * worth of work that nothing has yet asked for.
         */
        if (dual) {
            uint32_t d = code[3];
            unsigned mop = ((d >> 21) & 15) | (((d >> 2) & 1) << 4);
            unsigned msrc_type = d & 3, msrc_off = (d >> 5) & 255;
            unsigned mswz_x = (d >> 13) & 7;
            unsigned mdst = (d >> 19) & 3, mwe = (d >> 27) & 3;
            bool mabs = (d >> 3) & 1, mneg_x = (d >> 25) & 1;
            char operand[256];

            if (d & ((1u << 4) | (1u << 31))) {
                snprintf(detail, sizeof(detail), "dual math relative addressing");
                reason = detail;
                goto fail;
            }
            if (mop != 9) {
                snprintf(detail, sizeof(detail), "dual math opcode %u", mop);
                reason = detail;
                goto fail;
            }
            if (mdst >= R300_VP_ALT_TEMP_WRITABLE) {
                snprintf(detail, sizeof(detail), "dual math destination %u", mdst);
                reason = detail;
                goto fail;
            }
            /* One scalar operand, taken from the component the first swizzle
             * selects.  Build it through the same source decoder so that the
             * register classes and their bounds are checked in one place. */
            {
                uint32_t scalar = msrc_type | (mabs ? (1u << 3) : 0u)
                                | (msrc_off << 5)
                                | (mswz_x << 13) | (mswz_x << 16)
                                | (mswz_x << 19) | (mswz_x << 22)
                                | (mneg_x ? (1u << 25) : 0u);
                if (!source(operand, sizeof(operand), scalar, p->attribute_mask,
                            temps, param_base, param_max, 15u)) {
                    snprintf(detail, sizeof(detail),
                             "dual math source word=%08x", d);
                    reason = detail;
                    goto fail;
                }
            }
            /*
             * ME_RECIP_SQRT_FF: 1.0 / sqrt(abs(x)), and an input of zero
             * gives zero -- not infinity, and not the MAX_FLOAT its _DX
             * sibling returns.  rsqrt(0) in Metal is infinity, so the zero
             * case is written out rather than left to the intrinsic.
             */
            emit(&w, " { float ms = abs((%s).x);\n", operand);
            emit(&w, " m[%u].%c = ms == 0.0f ? 0.0f : rsqrt(ms); }\n",
                 mdst, "xyzw"[mwe]);
        }
        emit(&w, " }\n");
        if (pc == position_end && results[0] != 15) {
            reason = "position is incomplete at declared position-program end";
            goto fail;
        }
    }
    if (results[0] != 15) {
        reason = "vertex program does not define complete position output";
        goto fail;
    }
    /* OpenGL guest clip depth is -w..w, whereas Metal uses 0..w. */
    emit(&w, " R300VertexOutput result = {};\n"
             " result.position = float4(o[0].x, %so[0].y, (o[0].z + o[0].w) * 0.5f, o[0].w);\n"
             " result.point_size = %.9ff;\n", p->flip_y ? "-" : "",
             p->point_size > 0 ? p->point_size : 1.0f);
    if (p->expand_point_rect) {
        emit(&w, " constexpr float2 corners[6] = {float2(-1,-1),float2(1,-1),float2(-1,1),"
                 "float2(-1,1),float2(1,-1),float2(1,1)};\n"
                 " float2 corner = corners[vid %% 6];\n"
                 " result.position.xy += corner * float2(%.9ff, %.9ff) * result.position.w;\n",
                 p->point_half_clip_x, p->point_half_clip_y);
    }
    for (unsigned i = 0; i < R300_FP_MAX_INPUTS; i++) {
        if (p->varying_mask & (1u << i)) {
            if (p->varying_constant_mask & (1u << i)) {
                const float *v = p->varying_constant[i];
                if (!isfinite(v[0]) || !isfinite(v[1]) ||
                    !isfinite(v[2]) || !isfinite(v[3])) {
                    reason = "non-finite rasterizer constant";
                    goto fail;
                }
                emit(&w, " result.v%u = float4(%.9ff, %.9ff, %.9ff, %.9ff);\n",
                     i, v[0], v[1], v[2], v[3]);
                continue;
            }
            unsigned output = p->varying_output[i];
            if (output >= 32 || results[output] != 15) {
                reason = "fragment linkage references incomplete vertex output";
                goto fail;
            }
            emit(&w, " result.v%u = o[%u];\n", i, output);
        }
    }
    emit(&w, " return result;\n}\n");
    if (w.failed) {
        reason = "vertex MSL output buffer too small";
        goto fail;
    }
    return true;
fail:
    /*
     * The program itself, once, whatever turned it away.
     *
     * Four dwords per slot, as the hardware takes them.  A rejection names a
     * register; the program says what the shader is actually doing, which is
     * what decides whether the next step is a few lines or a feature.
     */
    {
        static bool dumped;
        if (!dumped) {
            dumped = true;
            fprintf(stderr, "[R350_VP] %s\n", reason);
            for (unsigned q = start; q <= end && q < R300_VP_MAX_INSTRUCTIONS; q++) {
                uint32_t w = p->code[q][0];
                fprintf(stderr, "[R350_VP] %2u: %08x %08x %08x %08x  valid=%x"
                        "  op=%u math=%u macro=%u dst=%u off=%u we=%x%s\n",
                        q, w, p->code[q][1], p->code[q][2], p->code[q][3],
                        p->code_valid[q], w & 63, (w >> 6) & 1, (w >> 7) & 1,
                        (w >> 8) & 15, (w >> 13) & 127, (w >> 20) & 15,
                        (w & (1u << 28)) ? "  DUAL" : "");
            }
        }
    }
    out[0] = 0;
    snprintf(error, error_capacity, "%s", reason);
    return false;
}
