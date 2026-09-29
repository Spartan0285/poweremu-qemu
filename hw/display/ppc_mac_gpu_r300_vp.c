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

static bool source(char *out, size_t cap, uint32_t word,
                   uint32_t attributes, const uint8_t defined[32],
                   unsigned param_base, unsigned param_max)
{
    if (word & ~0x1fffffffu) {
        return false; /* relative addressing / unsupported extension */
    }
    unsigned kind = word & 31, index = (word >> 5) & 255;
    char components[4][48];
    for (unsigned i = 0; i < 4; i++) {
        unsigned swizzle = (word >> (13 + i * 3)) & 7;
        const char *neg = word & (1u << (25 + i)) ? "-" : "";
        if (swizzle == 4 || swizzle == 5) {
            snprintf(components[i], sizeof(components[i]), "%s%s", neg,
                     swizzle == 4 ? "0.0f" : "1.0f");
        } else if (swizzle < 4) {
            char channel = "xyzw"[swizzle];
            if (kind == 0 && index < 32 && (defined[index] & (1u << swizzle))) {
                snprintf(components[i], sizeof(components[i]), "%st[%u].%c", neg, index, channel);
            } else if (kind == 1 && index < R300_VP_MAX_ATTRIBUTES &&
                       (attributes & (1u << index))) {
                snprintf(components[i], sizeof(components[i]), "%sa[%u].%c", neg, index, channel);
            } else if (kind == 2 && index <= param_max &&
                       param_base + index < R300_VP_MAX_PARAMETERS) {
                snprintf(components[i], sizeof(components[i]), "%sc[%u].%c", neg, param_base + index, channel);
            } else {
                return false;
            }
        } else {
            return false;
        }
    }
    int n = snprintf(out, cap, "float4(%s,%s,%s,%s)",
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
             " float4 t[32]; float4 o[32];\n");
    if (p->zero_initialize_temporaries) {
        emit(&w, " for (uint i = 0; i < 32; i++) t[i] = float4(0.0f);\n");
    }
    for (unsigned pc = start; pc <= end; pc++) {
        const uint32_t *code = p->code[pc];
        unsigned op = code[0] & 255, kind = (code[0] >> 8) & 31;
        unsigned dst = (code[0] >> 13) & 31, mask = (code[0] >> 20) & 15;
        unsigned count = (op == 4 || op == 128) ? 3 : 2;
        if (p->code_valid[pc] != 15 || (code[0] & ~0x00f3ffffu) ||
            (kind != 0 && kind != 2) || !mask ||
            (op != 1 && op != 2 && op != 3 && op != 4 && op != 128)) {
            reason = "incomplete upload or unsupported vertex opcode/destination";
            goto fail;
        }
        char operands[3][256];
        for (unsigned i = 0; i < count; i++) {
            if (!source(operands[i], sizeof(operands[i]), code[i + 1],
                        p->attribute_mask, temps, param_base, param_max)) {
                snprintf(detail, sizeof(detail),
                         "undefined vertex source pc=%u operand=%u word=%08x attributes=%04x",
                         pc, i, code[i + 1], p->attribute_mask);
                reason = detail;
                goto fail;
            }
        }
        emit(&w, " { float4 va = %s; float4 vb = %s;\n", operands[0], operands[1]);
        if (count == 3) {
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
    out[0] = 0;
    snprintf(error, error_capacity, "%s", reason);
    return false;
}
