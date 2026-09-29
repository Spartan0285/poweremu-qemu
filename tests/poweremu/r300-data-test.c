/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "ppc_mac_gpu_r300_data.h"

int main(void)
{
    const uint32_t constants[] = {0,0x800000,0x3f0000,0xbf0000,0x3e0000,0x400000,0x3d8000};
    const float expected[] = {0,-0.0f,1,-1,0.5f,2,0.375f};
    for (unsigned i = 0; i < sizeof(constants)/sizeof(constants[0]); i++) {
        float value = 123;
        assert(r300_float24_decode(constants[i], &value));
        assert(value == expected[i]);
        assert(!!signbit(value) == !!signbit(expected[i]));
    }
    for (unsigned e = 1; e < 127; e++) {
        for (unsigned m = 0; m <= 65535; m += 21845) {
            float value;
            assert(r300_float24_decode((e << 16) | m, &value));
            assert(value == ldexpf(1.0f + m / 65536.0f, (int)e - 63));
        }
    }
    float untouched = 123;
    assert(!r300_float24_decode(1, &untouched));
    assert(!r300_float24_decode(0x7f0000, &untouched));
    assert(!r300_float24_decode(0x1000000, &untouched));
    assert(untouched == 123);

    /* Stream descriptors and payload layout from captured Leopard draw 1. */
    uint32_t stream[8] = {0x01030003,0x00002203};
    uint32_t ext[8] = {0xf688f688,0x0000f688};
    uint32_t words[48] = {0};
    const float positions[4][2] = {{0,0},{256,0},{256,256},{0,256}};
    for (unsigned v = 0; v < 4; v++) {
        float values[12] = {positions[v][0],positions[v][1],0,1,1,1,1,1,
                            positions[v][0],positions[v][1],0,1};
        memcpy(words + v * 12, values, sizeof(values));
    }
    R300VertexInput out[4];
    uint32_t mask;
    assert(r300_decode_immediate(stream,ext,words,48,4,out,4,&mask));
    assert(mask == 7);
    for (unsigned v = 0; v < 4; v++) {
        assert(out[v].a[0][0] == positions[v][0]);
        assert(out[v].a[0][1] == positions[v][1]);
        assert(out[v].a[0][2] == 0 && out[v].a[0][3] == 1);
        assert(out[v].a[2][0] == positions[v][0]);
        for (unsigned c = 0; c < 4; c++) assert(out[v].a[1][c] == 1);
    }
    memset(out,0x5a,sizeof(out));
    R300VertexInput sentinel[4]; memcpy(sentinel,out,sizeof(out));
    assert(!r300_decode_immediate(stream,ext,words,47,4,out,4,&mask));
    assert(mask == 0 && !memcmp(out,sentinel,sizeof(out)));
    assert(!r300_decode_immediate(stream,ext,words,48,4,out,3,&mask));
    stream[1] &= ~(1u << 13); /* missing LAST */
    assert(!r300_decode_immediate(stream,ext,words,48,4,out,4,&mask));
    assert(!memcmp(out,sentinel,sizeof(out)));
    memset(stream,0,sizeof(stream)); memset(ext,0,sizeof(ext));
    stream[0] = 0xa505; /* unsigned normalized D3DCOLOR -> attribute 5 */
    ext[0] = 0xf688;
    uint32_t packed = 0x80402010;
    assert(r300_decode_immediate(stream,ext,&packed,1,1,out,4,&mask));
    assert(mask == 1u << 5);
    const float color[4] = {64.0f/255,32.0f/255,16.0f/255,128.0f/255};
    for (unsigned c = 0; c < 4; c++) assert(out[0].a[5][c] == color[c]);
    stream[0] = 0x2504; /* unsigned integer BYTE, full vector */
    ext[0] = 0xf000 | 5 | (4 << 3) | (1 << 6) | (0 << 9);
    assert(r300_decode_immediate(stream,ext,&packed,1,1,out,4,&mask));
    assert(out[0].a[5][0] == 1 && out[0].a[5][1] == 0);
    assert(out[0].a[5][2] == 32 && out[0].a[5][3] == 16);
    stream[0] |= 1u << 14;
    assert(!r300_decode_immediate(stream,ext,&packed,1,1,out,4,&mask));
    stream[0] = 0x2010; /* FLOAT1 then skip one dword, supplies implicit W=1 */
    ext[0] = 0xf688;
    uint32_t skip[2] = {0x40000000,0xffffffff};
    assert(r300_decode_immediate(stream,ext,skip,2,1,out,4,&mask));
    assert(mask == 1 && out[0].a[0][0] == 2 && out[0].a[0][3] == 1);
    assert(out[0].a[0][1] == 0 && out[0].a[0][2] == 0);
    ext[0] |= 7;
    assert(!r300_decode_immediate(stream,ext,skip,2,1,out,4,&mask));
    puts("PASS R300 float24 constants, captured immediate layout, packed colors, swizzles and bounds");
    unsigned indices[12], n=99;
    const unsigned strip[12]={0,1,2,2,1,3,2,3,4,4,3,5};
    const unsigned fan[12]={0,1,2,0,2,3,0,3,4,0,4,5};
    const unsigned quads[12]={0,1,2,0,2,3,4,5,6,4,6,7};
    assert(r300_triangle_indices(6,6,indices,12,&n) && n==12);
    assert(!memcmp(indices,strip,sizeof(strip)));
    assert(r300_triangle_indices(5,6,indices,12,&n) && n==12);
    assert(!memcmp(indices,fan,sizeof(fan)));
    assert(r300_triangle_indices(13,8,indices,12,&n) && n==12);
    assert(!memcmp(indices,quads,sizeof(quads)));
    assert(r300_triangle_indices(4,12,indices,12,&n) && n==12);
    for(unsigned i=0;i<12;i++) assert(indices[i]==i);
    unsigned saved[12];memcpy(saved,indices,sizeof(saved));n=99;
    assert(!r300_triangle_indices(6,6,indices,11,&n));
    assert(!r300_triangle_indices(13,7,indices,12,&n));
    assert(!r300_triangle_indices(4,11,indices,12,&n));
    assert(!r300_triangle_indices(5,2,indices,12,&n));
    assert(!r300_triangle_indices(15,6,indices,12,&n));
    assert(!r300_triangle_indices(6,65537,indices,12,&n));
    assert(n==99 && !memcmp(saved,indices,sizeof(saved)));
    puts("PASS R300 primitive winding, fan/strip/quad assembly and reject preservation");

    R300VertexInput point={0}, expanded[6]; unsigned expanded_count=0;
    point.a[0][0]=512;point.a[0][1]=384;point.a[0][3]=1;
    assert(r300_expand_points(&point,1,0x18001200,12,0,1,1,0,
                              expanded,6,&expanded_count));
    assert(expanded_count==6);
    const float point_xy[6][2]={{0,768},{1024,768},{1024,0},
                                {0,768},{1024,0},{0,0}};
    const float point_st[6][2]={{0,1},{1,1},{1,0},{0,1},{1,0},{0,0}};
    for(unsigned i=0;i<6;i++) {
        assert(expanded[i].a[0][0]==point_xy[i][0]);
        assert(expanded[i].a[0][1]==point_xy[i][1]);
        assert(expanded[i].a[1][0]==point_st[i][0]);
        assert(expanded[i].a[1][1]==point_st[i][1]);
        assert(expanded[i].a[1][2]==0 && expanded[i].a[1][3]==1);
    }
    expanded_count=77;
    assert(!r300_expand_points(&point,1,0x18001200,12,0,1,1,0,
                               expanded,5,&expanded_count));
    assert(!r300_expand_points(&point,1,0,12,0,1,1,0,
                               expanded,6,&expanded_count));
    assert(expanded_count==77);
    R300VPProgram point_program;float point_constants[256][4];
    assert(r300_window_point_program(&point_program,point_constants,
                                     512,512,-384,384));
    assert(point_program.attribute_mask==3 && point_program.varying_mask==1);
    assert(point_program.varying_output[0]==1 && point_program.code_valid[4]==15);
    assert(point_constants[4][0]==1 && point_constants[4][3]==1);
    puts("PASS R300 full-screen point expansion and point-texture vertex adapter");
    return 0;
}
