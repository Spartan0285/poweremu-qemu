/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef R300_FP_STANDALONE
#include "qemu/osdep.h"
#endif
#include <string.h>
#include <math.h>
#include "ppc_mac_gpu_r300_data.h"

bool r300_float24_decode(uint32_t word, float *value)
{
    unsigned exp = (word >> 16) & 127, mantissa = word & 65535;
    if ((word & 0xff000000u) || exp == 127 || (!exp && mantissa)) {
        return false;
    }
    uint32_t bits = (word & 0x800000u) << 8;
    if (exp) {
        bits |= ((exp + 64) << 23) | (mantissa << 7);
    }
    memcpy(value, &bits, sizeof(bits));
    return true;
}

bool r300_decode_immediate(const uint32_t stream[8], const uint32_t ext[8],
                           const uint32_t *words, size_t word_count,
                           unsigned vertex_count, R300VertexInput *out,
                           size_t output_count, uint32_t *attribute_mask)
{
    struct Entry {
        unsigned type, dst, count, skip, writes, swizzle, normalized;
    } entries[16];
    unsigned n = 0, stride = 0;
    uint8_t masks[16] = {0};
    bool last = false;
    uint32_t attrs = 0;
    *attribute_mask = 0;
    if (!vertex_count || output_count < vertex_count) {
        return false;
    }
    for (unsigned i = 0; i < 16; i++) {
        unsigned desc = (stream[i / 2] >> (16 * (i % 2))) & 65535;
        unsigned extra = (ext[i / 2] >> (16 * (i % 2))) & 65535;
        struct Entry *e = &entries[n++];
        e->type = desc & 15;
        e->dst = (desc >> 8) & 31;
        e->count = e->type <= 3 ? e->type + 1 : 1;
        e->skip = (desc >> 4) & 15;
        e->writes = extra >> 12;
        e->swizzle = extra & 4095;
        e->normalized = desc >> 15;
        if (e->type > 5 || e->dst >= 16 || (e->type >= 4 && (desc & (1u << 14)))) {
            return false;
        }
        for (unsigned c = 0; c < 4; c++) {
            if (((e->swizzle >> (3 * c)) & 7) > 5) {
                return false;
            }
        }
        masks[e->dst] |= e->writes;
        stride += e->count + e->skip;
        if (desc & (1u << 13)) {
            last = true;
            break;
        }
    }
    if (!last || word_count / stride != vertex_count || word_count % stride) {
        return false;
    }
    for (unsigned i = 0; i < 16; i++) {
        if (masks[i] && masks[i] != 15) {
            return false;
        }
        if (masks[i] == 15) {
            attrs |= 1u << i;
        }
    }
    const uint32_t *cursor = words;
    for (unsigned v = 0; v < vertex_count; v++) {
        memset(&out[v], 0, sizeof(out[v]));
        for (unsigned i = 0; i < n; i++) {
            const struct Entry *e = &entries[i];
            float values[6] = {0, 0, 0, 1, 0, 1};
            if (e->type <= 3) {
                for (unsigned c = 0; c < e->count; c++) {
                    memcpy(&values[c], &cursor[c], sizeof(float));
                }
            } else {
                for (unsigned c = 0; c < 4; c++) {
                    unsigned lane = (e->type == 5 && c != 3) ? 2 - c : c;
                    values[c] = (cursor[0] >> (8 * lane)) & 255;
                    if (e->normalized) {
                        values[c] /= 255.0f;
                    }
                }
            }
            for (unsigned c = 0; c < 4; c++) {
                if (e->writes & (1u << c)) {
                    out[v].a[e->dst][c] = values[(e->swizzle >> (3 * c)) & 7];
                }
            }
            cursor += e->count + e->skip;
        }
    }
    *attribute_mask = attrs;
    return true;
}

bool r300_triangle_indices(unsigned primitive, unsigned vertices,
                            unsigned *indices, size_t capacity, unsigned *count)
{
    if (!indices || !count || !vertices || vertices > 65536) return false;
    unsigned n;
    switch (primitive) {
    case 4:
        if (vertices % 3) return false;
        n = vertices; break;
    case 5: case 6:
        if (vertices < 3) return false;
        n = (vertices - 2) * 3; break;
    case 13:
        if (vertices % 4) return false;
        n = vertices / 4 * 6; break;
    default: return false;
    }
    if (n > capacity) return false;
    for (unsigned i=0;i<n/3;i++) {
        unsigned *t=indices+i*3;
        if (primitive==4) {t[0]=i*3;t[1]=i*3+1;t[2]=i*3+2;}
        else if (primitive==5) {t[0]=0;t[1]=i+1;t[2]=i+2;}
        else if (primitive==6) {t[0]=i+(i&1);t[1]=i+1-(i&1);t[2]=i+2;}
        else {t[0]=i/2*4;t[1]=t[0]+1+(i&1);t[2]=t[1]+1;}
    }
    *count=n;
    return true;
}

bool r300_expand_points(const R300VertexInput *points, unsigned point_count,
                        uint32_t point_size, unsigned subpixel_divisor,
                        float s0, float t0, float s1, float t1,
                        R300VertexInput *triangles, size_t capacity,
                        unsigned *triangle_vertices)
{
    if (!points || !triangles || !triangle_vertices || !point_count ||
        !subpixel_divisor || point_count > 16384 ||
        capacity < (size_t)point_count * 6 ||
        !isfinite(s0) || !isfinite(t0) || !isfinite(s1) || !isfinite(t1)) {
        return false;
    }
    float half_width = (point_size >> 16) / (float)subpixel_divisor;
    float half_height = (point_size & 0xffff) / (float)subpixel_divisor;
    if (!(half_width > 0) || !(half_height > 0) ||
        !isfinite(half_width) || !isfinite(half_height)) {
        return false;
    }
    static const unsigned corner[6] = { 0, 1, 2, 0, 2, 3 };
    for (unsigned p = 0; p < point_count; p++) {
        float x = points[p].a[0][0], y = points[p].a[0][1];
        if (!isfinite(x) || !isfinite(y)) {
            return false;
        }
        const float xy[4][2] = {
            { x - half_width, y + half_height },
            { x + half_width, y + half_height },
            { x + half_width, y - half_height },
            { x - half_width, y - half_height },
        };
        const float st[4][2] = { {s0,t0}, {s1,t0}, {s1,t1}, {s0,t1} };
        for (unsigned i = 0; i < 6; i++) {
            unsigned c = corner[i];
            R300VertexInput *v = &triangles[p * 6 + i];
            *v = points[p];
            v->a[0][0] = xy[c][0]; v->a[0][1] = xy[c][1];
            v->a[1][0] = st[c][0]; v->a[1][1] = st[c][1];
            v->a[1][2] = 0; v->a[1][3] = 1;
        }
    }
    *triangle_vertices = point_count * 6;
    return true;
}

static bool window_program(R300VPProgram *p, float c[256][4],
                           float xs, float xo, float ys, float yo,
                           bool point_varying)
{
    if (!p || !c || !isfinite(xs) || !isfinite(xo) || !isfinite(ys) ||
        !isfinite(yo) || xs<=0 || ys>=0 || !isfinite(1/xs) || !isfinite(1/ys) ||
        !isfinite(xo/xs) || !isfinite(yo/ys)) return false;
    memset(p,0,sizeof(*p));memset(c,0,256*4*sizeof(float));
    p->control[0]=(3u<<10)|((point_varying ? 4u : 3u)<<20);
    p->control[1]=(point_varying ? 4u : 3u)<<16;
    p->control[2]=point_varying ? 4 : 3;
    p->attribute_mask=point_varying ? 3 : 1;
    for(unsigned i=0;i<4;i++) {
        p->code[i][0]=((1u<<i)<<20)|0x201;
        p->code[i][1]=0x00d10002|(i<<5);
        p->code[i][2]=0x00d10001;
        p->code[i][3]=0x00d10005;
        p->code_valid[i]=15;
    }
    c[0][0]=1/xs;c[0][3]=-xo/xs;
    c[1][1]=1/ys;c[1][3]=-yo/ys;
    c[3][3]=1;
    if (point_varying) {
        p->code[4][0]=0x00f02202; /* MUL a[1] by c[4], write o[1]. */
        p->code[4][1]=0x00d10021;
        p->code[4][2]=0x00d10082;
        p->code[4][3]=0x00d10005;
        p->code_valid[4]=15;
        c[4][0]=c[4][1]=c[4][2]=c[4][3]=1;
        p->varying_mask=1;
        p->varying_output[0]=1;
    }
    return true;
}

bool r300_window_vertex_program(R300VPProgram *p, float c[256][4],
                                 float xs, float xo, float ys, float yo)
{
    return window_program(p,c,xs,xo,ys,yo,false);
}

bool r300_window_point_program(R300VPProgram *p, float c[256][4],
                               float xs, float xo, float ys, float yo)
{
    return window_program(p,c,xs,xo,ys,yo,true);
}
