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
#include "ppc_mac_gpu_3d_regs.h"

/*
 * One blend factor as an MSL expression over (output, dst), where `output`
 * is the source colour the shader just computed.  `rgb` picks the
 * three-component form; the alpha channel takes the scalar one.  GL's
 * SRC_COLOR/DST_COLOR degenerate to the alpha channel for alpha, which is
 * why those cases differ between the two forms.
 *
 * NULL means the factor has no form here, which the device gate is supposed
 * to have refused already -- see r300_blend_factor_supported().
 */
static const char *r300_fp_blend_factor(unsigned f, bool rgb)
{
    switch (f) {
    case R200_BLEND_GL_ZERO:
        return rgb ? "float3(0.0)" : "0.0";
    case R200_BLEND_GL_ONE:
        return rgb ? "float3(1.0)" : "1.0";
    case R200_BLEND_GL_SRC_COLOR:
        return rgb ? "output.rgb" : "output.a";
    case R200_BLEND_GL_ONE_MINUS_SRC_COLOR:
        return rgb ? "(1.0 - output.rgb)" : "(1.0 - output.a)";
    case R200_BLEND_GL_DST_COLOR:
        return rgb ? "dst.rgb" : "dst.a";
    case R200_BLEND_GL_ONE_MINUS_DST_COLOR:
        return rgb ? "(1.0 - dst.rgb)" : "(1.0 - dst.a)";
    case R200_BLEND_GL_SRC_ALPHA:
        return rgb ? "float3(output.a)" : "output.a";
    case R200_BLEND_GL_ONE_MINUS_SRC_ALPHA:
        return rgb ? "float3(1.0 - output.a)" : "(1.0 - output.a)";
    case R200_BLEND_GL_DST_ALPHA:
        return rgb ? "float3(dst.a)" : "dst.a";
    case R200_BLEND_GL_ONE_MINUS_DST_ALPHA:
        return rgb ? "float3(1.0 - dst.a)" : "(1.0 - dst.a)";
    default:
        return NULL;
    }
}

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
                    unsigned highest, unsigned rgb_components)
{
    unsigned sel = field & 31, source = 0, needed = 0;
    uint32_t addresses = p->rgb_addr;
    const char *swizzle = NULL;
    char base[64];
    if ((!alpha && sel >= 20 && sel <= 22) ||
        (alpha && sel >= 16 && sel <= 18)) {
        /* R300 ARGC/ARGA literal selectors are zero, one, and half.
         * Final Cut's preview shaders use HALF without a constant register. */
        unsigned literal = sel - (alpha ? 16 : 20);
        snprintf(base, sizeof(base), "%s(%s)", alpha ? "float" : "float3",
                 (const char *[]) {"0.0", "1.0", "0.5"}[literal]);
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
            needed = (const unsigned[]) {rgb_components, 1, 2, 4}[sel % 4];
        } else if (sel <= 14) {
            source = sel - 12;
            addresses = p->alpha_addr;
            swizzle = "www";
            needed = 8;
        } else if (sel >= 23 && sel <= 28) {
            source = (sel - 23) % 3;
            swizzle = sel < 26 ? "yzx" : "zxy";
            needed = 0;
            for (unsigned i = 0; i < 3; i++) {
                if (rgb_components & (1u << i)) {
                    needed |= 1u << ((i + (sel < 26 ? 1 : 2)) % 3);
                }
            }
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
        (alpha_op > 1 && alpha_op != 6 && !alpha_unary) ||
        rgb_mod==7 || alpha_mod==7) {
        return "unsupported ALU operation or modifier";
    }
    char rgb[3][96], alpha[3][96];
    /* Componentwise ALUs only consume lanes that they write. A YZ copy
     * from a temporary with an undefined X is valid (Final Cut does this).
     * Dot products still require all RGB lanes. */
    unsigned rgb_components = (rgb_op == 1 || rgb_op == 2 || alpha_op == 1)
                              ? 7 : cmask | cout;
    for (unsigned i = 0; i < 3; i++) {
        if (((cmask || cout || alpha_op == 1) && !operand(rgb[i], sizeof(rgb[i]), p,
             (p->rgb_inst >> (7 * i)) & 127, false, defined, highest,
             rgb_components)) ||
            ((amask || aout || rgb_op == 2) && !operand(alpha[i], sizeof(alpha[i]), p,
             (p->alpha_inst >> (7 * i)) & 127, true, defined, highest, 7))) {
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
        } else if (alpha_op == 6) {
            /* OUTA_CMP uses the third operand as the sign test, matching
             * OUTC_CMP. Final Cut uses this to bound preview alpha. */
            emit(w, " float a = (%s) >= 0.0f ? (%s) : (%s);\n",
                 alpha[2], alpha[0], alpha[1]);
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

uint16_t r300_fp_texture_mask(const R300FPProgram *p)
{
    unsigned first = 3 - (p->control[0] & 3);
    unsigned base = (p->control[2] >> 13) & 31;
    uint16_t mask = 0;
    for (unsigned node = first; node < 4; node++) {
        if (node == first && !(p->control[0] & 8)) continue;
        unsigned start = (p->node[node] >> 12) & 31;
        unsigned count = ((p->node[node] >> 17) & 31) + 1;
        for (unsigned i = 0; i < count; i++) {
            mask |= 1u << ((p->tex[base + start + i] >> 11) & 15);
        }
    }
    return mask;
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

        }
    }
    textures = r300_fp_texture_mask(p);
    emit(&w, "#include <metal_stdlib>\nusing namespace metal;\n"
             "struct R300FragmentInput { float4 position [[position]];\n"
             " float4 vfog [[user(locnfog)]];\n");
    for (unsigned i = 0; i < R300_FP_MAX_INPUTS; i++) {
        emit(&w, " float4 v%u [[user(locn%u)]];\n", i, i);
    }
    emit(&w, "};\n");
    if (p->depth_mirror > 2) {
        reason = "unsupported depth mirror format";
        goto fail;
    }
    if (p->depth_mirror) {
        emit(&w, "struct R300FragmentOutput { float4 color [[color(0)]]; uint z [[color(1)]]; };\n");
    }
    emit(&w, "fragment %s r300_fragment(R300FragmentInput in [[stage_in]], "
             "constant float4 *c [[buffer(0)]]",
             p->depth_mirror ? "R300FragmentOutput" : "float4");
    if (p->depth_mirror) {
        emit(&w, ", uint oldz [[color(1)]], constant uint &zwrite [[buffer(1)]]");
    }
    if (p->blend_in_shader) {
        /* Framebuffer fetch: the destination has to be read in the shader
         * because it cannot be blended by the pipeline in guest order. */
        emit(&w, ", float4 fbin [[color(0)]]");
    }
    for (unsigned i = 0; i < 16; i++) {
        if (textures & (1u << i)) {
            emit(&w, ", texture2d<float> t%u [[texture(%u)]], "
                     "sampler s%u [[sampler(%u)]]", i, i, i, i);
        }
    }
    /* Inactive vector lanes may appear in generated expressions; give them
     * deterministic values without admitting undefined active-lane reads. */
    emit(&w, ") {\n float4 r[32] = {};\n float4 output;\n");
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
                if (p->tex_swizzled) {
                    static const char *const lane[6] = {
                        "r", "g", "b", "a", NULL, NULL
                    };
                    char part[4][24];
                    for (unsigned c = 0; c < 4; c++) {
                        unsigned sel = p->tex_swizzle[image][c];
                        if (sel >= 6) {
                            reason = "texture channel swizzle";
                            goto fail;
                        }
                        if (lane[sel]) {
                            snprintf(part[c], sizeof(part[c]), "r[%u].%s",
                                     dst, lane[sel]);
                        } else {
                            snprintf(part[c], sizeof(part[c]), "%s",
                                     sel == 4 ? "0.0" : "1.0");
                        }
                    }
                    emit(&w, " r[%u] = float4(%s, %s, %s, %s);\n",
                         dst, part[0], part[1], part[2], part[3]);
                }
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
    if (p->fog_enable) {
        /*
         * The fog unit sits between the shader and the alpha test, so this
         * goes here rather than with the blend further down.
         *
         * GA produces fog amount, not OpenGL's remaining visibility.
         * Warcraft's unfogged UI emits C1A=1 with GA scale=-1, offset=1;
         * amount zero must preserve that UI. Disabling FG fog bypasses the
         * blend and does not establish either endpoint's meaning.
         * Alpha is left alone.
         */
        const char *src;
        switch (p->fog_source) {
        case 4:     /* 1/(1/W): Metal's fragment position carries 1/w. */
            src = "(1.0f / in.position.w)";
            break;
        case 5:     /* Z straight off the scan converter. */
            src = "in.position.z";
            break;
        default:    /* the alpha of one of the vertex colours, carried in
                     * the dedicated fog varying */
            src = "in.vfog.a";
            break;
        }
        emit(&w, " {\n  float fogf = saturate(%s * c[33].x + c[33].y);\n"
                 "  output.rgb = output.rgb * (1.0f - fogf)"
                 " + c[32].rgb * fogf;\n }\n", src);
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
    const char *out_swizzle = p->out_swizzle[0] ? p->out_swizzle : "rgba";
    for (const char *c = out_swizzle; *c; c++) {
        if (!strchr("rgba", *c) || c - out_swizzle >= 4) {
            reason = "render-target component select";
            goto fail;
        }
    }
    if (strlen(out_swizzle) != 4) {
        reason = "render-target component select";
        goto fail;
    }
    if (p->blend_in_shader) {
        /*
         * The blend reads the destination back as a true (r,g,b,a) and uses
         * output.a as the source alpha.  A non-identity component select
         * means the shader's own channels are not the target's, so neither
         * holds; refuse rather than guess at the composition.
         */
        if (strcmp(out_swizzle, "rgba") != 0) {
            reason = "blending with a non-identity component select";
            goto fail;
        }
        const char *fs_rgb = r300_fp_blend_factor(p->blend_src_rgb, true);
        const char *fd_rgb = r300_fp_blend_factor(p->blend_dst_rgb, true);
        const char *fs_a = r300_fp_blend_factor(p->blend_src_alpha, false);
        const char *fd_a = r300_fp_blend_factor(p->blend_dst_alpha, false);
        if (!fs_rgb || !fd_rgb || !fs_a || !fd_a) {
            reason = "unsupported blend factor";
            goto fail;
        }
        /*
         * saturate() is COMB_FCN_ADD_CLAMP, which is the only equation the
         * device gate accepts.  The store to a unorm target would clamp
         * anyway, so this changes nothing for the premultiplied case that
         * used to be hard-coded here, and is correct for the rest.
         */
        emit(&w, " float4 dst = %s;\n"
                 " output = float4(saturate(output.rgb * %s + dst.rgb * %s),\n"
                 "                 saturate(output.a * %s + dst.a * %s));\n",
             p->guest_order ? "fbin.yxwz" : "fbin",
             fs_rgb, fd_rgb, fs_a, fd_a);
    }
    if (strcmp(out_swizzle, "rgba") != 0) {
        emit(&w, " output = output.%s;\n", out_swizzle);
    }
    if (p->depth_mirror) {
        emit(&w, " R300FragmentOutput result; result.color = %s; result.z = oldz;\n",
             p->guest_order ? "output.yxwz" : "output");
        /* Native depth/stencil decides whether this output is committed.
         * R350 Z24S8 has depth in the upper 24 bits, stencil in the low byte.
         * Integer attachments view little-endian words; the guest stores BE.
         * Stencil is preserved here; its authoritative state remains native. */
        emit(&w, " if (zwrite != 0u) { uint z = min(uint(rint(saturate(in.position.z) * %s)), %s);\n",
             p->depth_mirror == 1 ? "65535.0f" : "16777215.0f",
             p->depth_mirror == 1 ? "65535u" : "16777215u");
        if (p->depth_mirror == 1) {
            emit(&w, " result.z = ((z & 255u) << 8) | (z >> 8); }\n");
        } else {
            emit(&w, " result.z = (oldz & 0xff000000u) | ((z & 255u) << 16) | (z & 0xff00u) | (z >> 16); }\n");
        }
        emit(&w, " return result;\n}\n");
    } else {
        emit(&w, " return %s;\n}\n", p->guest_order ? "output.yxwz" : "output");
    }
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

void r300_fp_pack_constants(const R300FPProgram *p,
                            const float guest[32][4],
                            float host[R300_FP_HOST_CONSTANT_COUNT][4])
{
    memcpy(host, guest, 32 * 4 * sizeof(float));
    memcpy(host[32], p->fog_colour, sizeof(p->fog_colour));
    host[32][3] = 0;
    host[33][0] = p->fog_scale;
    host[33][1] = p->fog_offset;
    host[33][2] = host[33][3] = 0;
}
