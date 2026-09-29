/* SPDX-License-Identifier: GPL-2.0-or-later
 * R300 fragment ISA translation. Register encodings reference Linux
 * drivers/gpu/drm/radeon/r300_reg.h. No R200 interpretation is used.
 */
#ifndef R300_FP_STANDALONE
#include "qemu/osdep.h"
#endif
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "ppc_mac_gpu_r300_fp.h"

typedef struct MSLWriter {
    char *out;
    size_t size, used;
    bool failed;
} MSLWriter;

static bool primary_source(char *out, size_t cap, const R300FPInstruction *p,
                           unsigned slot, bool alpha,
                           const uint8_t defined[32], unsigned highest)
{
    uint32_t addresses = alpha ? p->alpha_addr : p->rgb_addr;
    unsigned index = (addresses >> (slot * 6)) & 31;
    bool constant = addresses & (1u << (slot * 6 + 5));
    unsigned needed = alpha ? 8 : 7;
    if (!constant && (index > highest || (defined[index] & needed) != needed)) {
        return false;
    }
    snprintf(out, cap, "%c[%u].%s", constant ? 'c' : 'r', index,
             alpha ? "w" : "xyz");
    return true;
}

static void emit(MSLWriter *w, const char *fmt, ...)
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
        return;
    }
    w->used += n;
}

static bool operand(char *out, size_t cap, const R300FPInstruction *p,
                    unsigned field, bool alpha, const uint8_t defined[32],
                    unsigned highest)
{
    unsigned sel = field & 31, source = 0, needed = 0;
    uint32_t addresses = p->rgb_addr;
    const char *swizzle = NULL;
    char base[64];
    if ((!alpha && (sel == 20 || sel == 21)) ||
        (alpha && (sel == 16 || sel == 17))) {
        snprintf(base, sizeof(base), "%s(%s)", alpha ? "float" : "float3",
                 (sel == 21 || sel == 17) ? "1.0" : "0.0");
    } else if ((!alpha && sel >= 15 && sel <= 19) ||
               (alpha && sel >= 12 && sel <= 15)) {
        bool use_alpha = (!alpha && sel == 19) || (alpha && sel == 15);
        char s0[64], s1[64], expression[160];
        if (!primary_source(s0,sizeof(s0),p,0,use_alpha,defined,highest) ||
            !primary_source(s1,sizeof(s1),p,1,use_alpha,defined,highest)) {
            return false;
        }
        unsigned mode = ((alpha ? p->alpha_inst : p->rgb_inst) >> 21) & 3;
        snprintf(expression,sizeof(expression),mode==0 ? "(1.0 - 2.0 * (%s))" :
                 mode==1 ? "((%s) - (%s))" : mode==2 ? "((%s) + (%s))" :
                 "(1.0 - (%s))", mode==0 || mode==3 ? s0 : s1,
                 mode==1 || mode==2 ? s0 : "");
        if (!alpha) {
            const char *component = (const char *[]) {"xyz","xxx","yyy","zzz","www"}[sel-15];
            if (sel==19) snprintf(base,sizeof(base),"float3(%s)",expression);
            else snprintf(base,sizeof(base),"(%s).%s",expression,component);
        } else if (sel==15) {
            snprintf(base,sizeof(base),"%s",expression);
        } else {
            snprintf(base,sizeof(base),"(%s).%c",expression,"xyz"[sel-12]);
        }
    } else {
        if (alpha) {
            if (sel <= 8) {
                source = sel / 3;
                swizzle = (const char *[]) {"x", "y", "z"}[sel % 3];
                needed = 1u << (sel % 3);
            } else if (sel <= 11) {
                source = sel - 9;
                addresses = p->alpha_addr;
                swizzle = "w";
                needed = 8;
            }
        } else if (sel <= 11) {
            source = sel / 4;
            swizzle = (const char *[]) {"xyz", "xxx", "yyy", "zzz"}[sel % 4];
            needed = (const unsigned[]) {7, 1, 2, 4}[sel % 4];
        } else if (sel <= 14) {
            source = sel - 12;
            addresses = p->alpha_addr;
            swizzle = "www";
            needed = 8;
        } else if (sel >= 23 && sel <= 28) {
            source = (sel - 23) % 3;
            swizzle = sel < 26 ? "yzx" : "zxy";
            needed = 7;
        }
        if (!swizzle) {
            return false;
        }
        unsigned index = (addresses >> (source * 6)) & 31;
        bool constant = addresses & (1u << (source * 6 + 5));
        if (!constant && (index > highest ||
                          (defined[index] & needed) != needed)) {
            return false;
        }
        snprintf(base, sizeof(base), "%c[%u].%s", constant ? 'c' : 'r',
                 index, swizzle);
    }
    snprintf(out, cap, "%s%s(%s)", (field & 32) ? "-" : "",
             (field & 64) ? "abs" : "", base);
    return true;
}

static const char *emit_alu(MSLWriter *w, const R300FPInstruction *p,
                            uint8_t defined[32], unsigned highest,
                            unsigned *output_mask)
{
    unsigned cdst = (p->rgb_addr >> 18) & 31;
    unsigned adst = (p->alpha_addr >> 18) & 31;
    unsigned cmask = (p->rgb_addr >> 23) & 7;
    unsigned amask = (p->alpha_addr >> 23) & 1;
    unsigned cout = (p->rgb_addr >> 26) & 7;
    unsigned aout = (p->alpha_addr >> 24) & 1;
    if ((p->rgb_addr & ~0x1fffffffu) || (p->alpha_addr & ~0x01ffffffu) ||
        (cmask && cdst > highest) || (amask && adst > highest)) {
        return "unsupported destination, depth output or register range";
    }
    unsigned rgb_op=(p->rgb_inst>>23)&15,alpha_op=(p->alpha_inst>>23)&15;
    unsigned rgb_mod=(p->rgb_inst>>27)&7,alpha_mod=(p->alpha_inst>>27)&7;
    bool alpha_unary = alpha_op >= 7 && alpha_op <= 11;
    bool rgb_supported = rgb_op <= 2 || rgb_op == 4 || rgb_op == 5 ||
                         rgb_op == 7 || rgb_op == 8 || rgb_op == 9;
    if ((p->rgb_inst|p->alpha_inst)&0x80000000u || !rgb_supported ||
        (alpha_op > 1 && !alpha_unary) ||
        rgb_mod==7 || alpha_mod==7) {
        return "unsupported ALU operation or modifier";
    }
    char rgb[3][96], alpha[3][96];
    for (unsigned i = 0; i < 3; i++) {
        if (((cmask || cout || alpha_op == 1) && !operand(rgb[i], sizeof(rgb[i]), p,
             (p->rgb_inst >> (7 * i)) & 127, false, defined, highest)) ||
            ((amask || aout || rgb_op == 2) && !operand(alpha[i], sizeof(alpha[i]), p,
             (p->alpha_inst >> (7 * i)) & 127, true, defined, highest))) {
            return "unsupported operand or read of undefined temporary components";
        }
    }
    emit(w, " {\n");
    if (rgb_op == 1 || rgb_op == 2 || alpha_op == 1) {
        emit(w, " float dp = dot(%s, %s);\n", rgb[0], rgb[1]);
        if (rgb_op == 2) {
            /* OUTC_DP4 consumes XYZ from the RGB side and W from the paired
             * alpha operands; OUTA_DP can copy that same scalar. */
            emit(w, " dp += (%s) * (%s);\n", alpha[0], alpha[1]);
        }
    }
    if (cmask || cout) {
        if (rgb_op == 1 || rgb_op == 2) {
            emit(w, " float3 rgb = float3(dp);\n");
        } else if (rgb_op == 4) {
            emit(w, " float3 rgb = min(%s, %s);\n", rgb[0], rgb[1]);
        } else if (rgb_op == 5) {
            emit(w, " float3 rgb = max(%s, %s);\n", rgb[0], rgb[1]);
        } else if (rgb_op == 7) {
            emit(w, " float3 rgb = select(%s, %s, (%s) > float3(0.5f));\n",
                 rgb[1], rgb[0], rgb[2]);
        } else if (rgb_op == 8) {
            emit(w, " float3 rgb = select(%s, %s, (%s) >= float3(0.0f));\n",
                 rgb[1], rgb[0], rgb[2]);
        } else if (rgb_op == 9) {
            emit(w, " float3 rgb = fract(%s);\n", rgb[0]);
        } else {
            emit(w, " float3 rgb = (%s) * (%s) + (%s);\n",
                 rgb[0], rgb[1], rgb[2]);
        }
        static const char *scale[7]={"1.0f","2.0f","4.0f","8.0f","0.5f","0.25f","0.125f"};
        if (rgb_mod) emit(w," rgb *= %s;\n",scale[rgb_mod]);
        if (p->rgb_inst & (1u << 30)) {
            emit(w, " rgb = clamp(rgb, 0.0f, 1.0f);\n");
        }
    }
    if (amask || aout) {
        if (alpha_op == 1) {
            emit(w, " float a = dp;\n");
        } else if (alpha_op == 7) {
            emit(w, " float a = fract(%s);\n", alpha[0]);
        } else if (alpha_op == 8) {
            emit(w, " float a = exp2(%s);\n", alpha[0]);
        } else if (alpha_op == 9) {
            emit(w, " float a = log2(%s);\n", alpha[0]);
        } else if (alpha_op == 10) {
            emit(w, " float a = 1.0f / (%s);\n", alpha[0]);
        } else if (alpha_op == 11) {
            emit(w, " float a = rsqrt(%s);\n", alpha[0]);
        } else {
            emit(w, " float a = (%s) * (%s) + (%s);\n",
                 alpha[0], alpha[1], alpha[2]);
        }
        static const char *scale[7]={"1.0f","2.0f","4.0f","8.0f","0.5f","0.25f","0.125f"};
        if (alpha_mod) emit(w," a *= %s;\n",scale[alpha_mod]);
        if (p->alpha_inst & (1u << 30)) {
            emit(w, " a = clamp(a, 0.0f, 1.0f);\n");
        }
    }
    /* Both ALUs read the pre-instruction register state. Commit only after
     * computing both results, including cross RGB/alpha source swizzles. */
    for (unsigned c = 0; c < 3; c++) {
        char component = "xyz"[c];
        if (cmask & (1u << c)) {
            emit(w, " r[%u].%c = rgb.%c;\n", cdst, component, component);
        }
        if (cout & (1u << c)) {
            emit(w, " output.%c = rgb.%c;\n", component, component);
        }
    }
    if (amask) {
        emit(w, " r[%u].w = a;\n", adst);
    }
    if (aout) {
        emit(w, " output.w = a;\n");
    }
    defined[cdst] |= cmask;
    defined[adst] |= amask << 3;
    *output_mask |= cout | (aout << 3);
    emit(w, " }\n");
    return NULL;
}

bool r300_fp_compile_msl(const R300FPProgram *p, char *out, size_t capacity,
                         char *error, size_t error_capacity)
{
    MSLWriter w = { .out = out, .size = capacity };
    const char *reason = NULL;
    uint8_t defined[32] = {0};
    unsigned textures = 0, output_mask = 0;
    unsigned highest = p->control[1];
    unsigned alu_base = p->control[2] & 63;
    unsigned alu_end = (p->control[2] >> 6) & 63;
    unsigned tex_base = (p->control[2] >> 13) & 31;
    unsigned tex_end = (p->control[2] >> 18) & 31;
    unsigned first_node = 3 - (p->control[0] & 3);
    bool first_node_has_tex = p->control[0] & 8;
    out[0] = error[0] = 0;
    char detail[192] = {0};
    if ((p->alpha_func & ~0xfffu) || (p->control[0] & ~0xbu) || highest > 31 ||
        (p->control[2] & ~0x007fefffu) ||
        (p->node[3] & ~0x007fffffu) || !(p->node[3] & (1u << 22)) ||
        (p->input_mask & ~((1u << R300_FP_MAX_INPUTS) - 1))) {
        reason = "requires supported color-output nodes and input linkage";
        goto fail;
    }
    if (alu_base + alu_end >= R300_FP_MAX_ALU ||
        tex_base + tex_end >= R300_FP_MAX_TEX) {
        reason = "instruction range outside program bank or declared program";
        goto fail;
    }
    for (unsigned node = first_node; node < 4; node++) {
        uint32_t descriptor = p->node[node];
        unsigned afirst = descriptor & 63;
        unsigned alast = afirst + ((descriptor >> 6) & 63);
        unsigned tfirst = (descriptor >> 12) & 31;
        unsigned tlast = tfirst + ((descriptor >> 17) & 31);
        bool has_tex = node != first_node || first_node_has_tex;
        if ((descriptor & ~0x007fffffu) || afirst > alast || alast > alu_end ||
            (has_tex && (tfirst > tlast || tlast > tex_end))) {
            reason = "node instruction range outside declared program";
            goto fail;
        }
        if (!has_tex) {
            continue;
        }
        for (unsigned i = tfirst; i <= tlast; i++) {
            uint32_t inst = p->tex[tex_base + i];
            unsigned op = (inst >> 15) & 7;
            if ((inst & ~0x3ffdfu) || (op != 1 && op != 3)) {
                reason = "unsupported texture opcode or source addressing";
                goto fail;
            }
            textures |= 1u << ((inst >> 11) & 15);
        }
    }
    emit(&w, "#include <metal_stdlib>\nusing namespace metal;\n"
             "struct R300FragmentInput { float4 position [[position]];\n");
    for (unsigned i = 0; i < R300_FP_MAX_INPUTS; i++) {
        emit(&w, " float4 v%u [[user(locn%u)]];\n", i, i);
    }
    emit(&w, "};\nfragment float4 r300_fragment(R300FragmentInput in [[stage_in]], "
             "constant float4 *c [[buffer(0)]]");
    for (unsigned i = 0; i < 16; i++) {
        if (textures & (1u << i)) {
            emit(&w, ", texture2d<float> t%u [[texture(%u)]], "
                     "sampler s%u [[sampler(%u)]]", i, i, i, i);
        }
    }
    emit(&w, ") {\n float4 r[32];\n float4 output;\n");
    for (unsigned i = 0; i < R300_FP_MAX_INPUTS; i++) {
        if (p->input_mask & (1u << i)) {
            if (i > highest) {
                reason = "input exceeds declared temporary register limit";
                goto fail;
            }
            emit(&w, " r[%u] = in.v%u;\n", i, i);
            defined[i] = 15;
        }
    }
    for (unsigned node = first_node; node < 4; node++) {
        uint32_t descriptor = p->node[node];
        unsigned afirst = descriptor & 63;
        unsigned alast = afirst + ((descriptor >> 6) & 63);
        unsigned tfirst = (descriptor >> 12) & 31;
        unsigned tlast = tfirst + ((descriptor >> 17) & 31);
        bool has_tex = node != first_node || first_node_has_tex;
        if (has_tex) {
            for (unsigned i = tfirst; i <= tlast; i++) {
                uint32_t inst = p->tex[tex_base + i];
                unsigned src = inst & 31, dst = (inst >> 6) & 31;
                unsigned image = (inst >> 11) & 15;
                bool projected = ((inst >> 15) & 7) == 3;
                unsigned needed = projected ? 11 : 3;
                if (src > highest || dst > highest || (defined[src] & needed) != needed) {
                    reason = "texture instruction reads undefined or out-of-range coordinates";
                    goto fail;
                }
                /* Read coordinates before writing the sampled value; src==dst
                 * is used by the stock Leopard driver. */
                emit(&w, " r[%u] = t%u.sample(s%u, r[%u].xy", dst, image, image, src);
                if (projected) {
                    emit(&w, " / r[%u].w", src);
                }
                emit(&w, ");\n");
                defined[dst] = 15;
            }
        }
        for (unsigned i = afirst; i <= alast; i++) {
            const R300FPInstruction *inst = &p->alu[alu_base + i];
            const char *alu_reason = emit_alu(&w, inst, defined, highest,
                                              &output_mask);
            if (alu_reason) {
                snprintf(detail, sizeof(detail),
                         "%s index=%u words=%08x,%08x,%08x,%08x",
                         alu_reason, alu_base + i, inst->rgb_addr,
                         inst->alpha_addr, inst->rgb_inst, inst->alpha_inst);
                reason = detail;
                goto fail;
            }
        }
    }
    if (output_mask != 15) {
        reason = "program does not define all output color components";
        goto fail;
    }
    if (p->alpha_func & 0x800) {
        static const char *ops[8] = {NULL, "<", "==", "<=", ">", "!=", ">=", NULL};
        unsigned op = (p->alpha_func >> 8) & 7;
        if (op == 0) {
            emit(&w, " discard_fragment();\n");
        } else if (op != 7) {
            emit(&w, " if (!(output.a %s (%u.0f / 255.0f))) discard_fragment();\n",
                 ops[op], p->alpha_func & 255);
        }
    }
    emit(&w, " return output;\n}\n");
    if (w.failed) {
        reason = "MSL output buffer too small";
        goto fail;
    }
    return true;
fail:
    out[0] = 0;
    snprintf(error, error_capacity, "%s", reason);
    return false;
}
