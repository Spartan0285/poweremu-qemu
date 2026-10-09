/* SPDX-License-Identifier: GPL-2.0-or-later */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <math.h>
#include "ppc_mac_gpu_r300_fp.h"
#include "ppc_mac_gpu_r300_vp.h"
#include "ati_r350_pvs.h"
#include "ppc_mac_gpu_r300_metal.h"
#include "ppc_mac_gpu_submit.h"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

/* The production translator consults the independent PVS interpreter only
 * to enrich a rejection diagnostic.  This standalone Metal test deliberately
 * links just the translator, so provide the diagnostic-only hook here. */
void r300_pvs_analyse(R300PvsProgram *p, const uint32_t *code,
                      const uint32_t *slot_valid, unsigned code_slots,
                      const uint32_t *cnst, unsigned const_slots,
                      uint32_t code_cntl, uint32_t const_cntl,
                      unsigned first_texcoord)
{
    (void)p; (void)code; (void)slot_valid; (void)code_slots;
    (void)cnst; (void)const_slots; (void)code_cntl; (void)const_cntl;
    (void)first_texcoord;
}
static R300FPProgram base_program(void)
{
    R300FPProgram p = {0};
    p.node[3] = 1u << 22;
    p.alu[0].rgb_addr = (7u << 26) | 32;
    p.alu[0].alpha_addr = (1u << 24) | 32;
    p.alu[0].rgb_inst = 0 | (21 << 7) | (20 << 14);
    p.alu[0].alpha_inst = 9 | (17 << 7) | (16 << 14);
    return p;
}

static void reject(R300FPProgram p)
{
    char out[4096], error[256];
    memset(out, 'x', sizeof(out));
    CHECK(!r300_fp_compile_msl(&p, out, sizeof(out), error, sizeof(error)));
    CHECK(out[0] == 0 && error[0] != 0);
}

static void render_pipeline(id<MTLDevice> device, R300FPProgram p,
                   float constants[32][4], const float expected[4], const char *name, unsigned mode, const R300VPProgram *vp)
{
    char source[4096], reason[256];
    CHECK(r300_fp_compile_msl(&p, source, sizeof(source), reason, sizeof(reason)));
    NSString *msl = [[NSString stringWithUTF8String:source] stringByAppendingString:
        @"\nvertex R300FragmentInput test_vertex(uint n [[vertex_id]]) {\n"
         " const float2 v[3] = {float2(-0.75,-0.75), float2(0.75,-0.75), float2(0,0.75)};\n"
         " R300FragmentInput o = {}; o.position = float4(v[n], 0, 1); return o; }\n"];
    unsigned tex_index = (p.control[2] >> 13) & 31;
    tex_index += (p.node[3] >> 12) & 31;
    unsigned image_id = (p.tex[tex_index] >> 11) & 15;
    if (mode) {
        unsigned op = (p.tex[tex_index] >> 15) & 7;
        float q = op == 3 ? 2.0f : 1.0f;
        NSString *vertex = [NSString stringWithFormat:
            @"\nvertex R300FragmentInput test_vertex(uint n [[vertex_id]]) {\n"
             " const float2 v[3] = {float2(-0.75,-0.75),float2(0.75,-0.75),float2(0,0.75)};\n"
             " R300FragmentInput o = {}; o.position = float4(v[n],0,1);\n"
             " o.v0 = float4(0.5,0.75,0.25,0.875);\n"
             " o.v1 = float4((v[n].x+1)*0.5*%f, (1-v[n].y)*0.5*%f, 0, %f);\n"
             " return o; }\n", q, q, q];
        msl = [[NSString stringWithUTF8String:source] stringByAppendingString:vertex];
    }
    if (vp) {
        char vertex[R300_FP_MSL_CAPACITY];
        CHECK(r300_vp_compile_msl(vp, vertex, sizeof(vertex), reason, sizeof(reason)));
        msl = [[NSString stringWithUTF8String:source] stringByAppendingString:[NSString stringWithUTF8String:vertex]];
    }
    if (p.depth_mirror && !vp) {
        msl = [msl stringByReplacingOccurrencesOfString:@"float4(v[n], 0, 1)"
                                           withString:@"float4(v[n], 0.75, 1)"];
    }
    NSError *error = nil;
    MTLCompileOptions *options = [MTLCompileOptions new];
    options.fastMathEnabled = NO;
    id<MTLLibrary> library = [device newLibraryWithSource:msl options:options error:&error];
    if (!library) fprintf(stderr, "%s\n", error.description.UTF8String);
    CHECK(library);
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = [library newFunctionWithName:vp ? @"r300_vertex" : @"test_vertex"];
    pd.fragmentFunction = [library newFunctionWithName:@"r300_fragment"];
    pd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA32Float;
    if (p.depth_mirror) pd.colorAttachments[1].pixelFormat = p.depth_mirror == 1 ?
        MTLPixelFormatR16Uint : MTLPixelFormatR32Uint;
    id<MTLRenderPipelineState> pipeline = [device newRenderPipelineStateWithDescriptor:pd error:&error];
    CHECK(pipeline);
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float width:64 height:64 mipmapped:NO];
    td.storageMode = MTLStorageModePrivate;
    td.usage = MTLTextureUsageRenderTarget;
    id<MTLTexture> target = [device newTextureWithDescriptor:td];
    CHECK(target);
    id<MTLBuffer> readback = [device newBufferWithLength:64*64*16 options:MTLResourceStorageModeShared];
    CHECK(readback);
    id<MTLCommandQueue> queue = [device newCommandQueue];
    id<MTLCommandBuffer> command = [queue commandBuffer];
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor new];
    pass.colorAttachments[0].texture = target;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0.0625, 0.125, 0.1875, 0.25);
    id<MTLTexture> zmirror = nil;
    id<MTLBuffer> zread = nil;
    if (p.depth_mirror) {
        MTLTextureDescriptor *zd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:
            p.depth_mirror == 1 ? MTLPixelFormatR16Uint : MTLPixelFormatR32Uint
            width:64 height:64 mipmapped:NO];
        zd.usage = MTLTextureUsageRenderTarget;
        zmirror = [device newTextureWithDescriptor:zd];
        CHECK(zmirror);
        zread = [device newBufferWithLength:256*64 options:MTLResourceStorageModeShared];
        pass.colorAttachments[1].texture = zmirror;
        pass.colorAttachments[1].loadAction = MTLLoadActionClear;
        pass.colorAttachments[1].storeAction = MTLStoreActionStore;
        pass.colorAttachments[1].clearColor = MTLClearColorMake(p.depth_mirror == 1 ? 0 : 0x07000000,0,0,0);
    }
    id<MTLRenderCommandEncoder> encoder = [command renderCommandEncoderWithDescriptor:pass];
    [encoder setRenderPipelineState:pipeline];
    if (p.depth_mirror) {
        uint32_t zwrite = 1;
        [encoder setFragmentBytes:&zwrite length:sizeof(zwrite) atIndex:1];
    }
    if (vp) {
        float attrs[3][16][4] = {0};
        const float pos[3][2] = {{-0.75f,-0.75f},{0.75f,-0.75f},{0,0.75f}};
        for (unsigned i = 0; i < 3; i++) {
            attrs[i][0][0] = (pos[i][0] + 1) * 512;
            attrs[i][0][1] = (1 - pos[i][1]) * 384;
            attrs[i][0][3] = 1;
            const float color[4] = {0.5f,0.75f,0.25f,0.875f};
            memcpy(attrs[i][1], color, sizeof(color));
            attrs[i][2][0] = (pos[i][0] + 1) * 128;
            attrs[i][2][1] = (1 - pos[i][1]) * 128;
            attrs[i][2][3] = 1;
        }
        float params[256][4] = {0};
        const uint32_t raw[8][4] = {
            {0x3b000000,0,0,0xbf800000}, {0,0xbb2aaaab,0,0x3f800000},
            {0,0,0xbf800000,0}, {0,0,0,0x3f800000},
            {0x3b800000,0,0,0}, {0,0x3b800000,0,0},
            {0,0,0x3f800000,0}, {0,0,0,0x3f800000}};
        memcpy(&params[128], raw, sizeof(raw));
        id<MTLBuffer> vb = [device newBufferWithBytes:attrs length:sizeof(attrs) options:MTLResourceStorageModeShared];
        id<MTLBuffer> cb = [device newBufferWithBytes:params length:sizeof(params) options:MTLResourceStorageModeShared];
        CHECK(vb && cb);
        [encoder setVertexBuffer:vb offset:0 atIndex:0];
        [encoder setVertexBuffer:cb offset:0 atIndex:1];
    }
    float host_constants[R300_FP_HOST_CONSTANT_COUNT][4];
    r300_fp_pack_constants(&p, constants, host_constants);
    [encoder setFragmentBytes:host_constants length:sizeof(host_constants) atIndex:0];
    if (mode) {
        MTLTextureDescriptor *sampleDesc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float width:4 height:4 mipmapped:NO];
        sampleDesc.storageMode = MTLStorageModeShared;
        sampleDesc.usage = MTLTextureUsageShaderRead;
        id<MTLTexture> sample = [device newTextureWithDescriptor:sampleDesc];
        CHECK(sample);
        float texels[4][4][4];
        for (unsigned y = 0; y < 4; y++) {
            for (unsigned x = 0; x < 4; x++) {
                texels[y][x][0] = 0.125f + x * 0.125f;
                texels[y][x][1] = 0.0625f + y * 0.125f;
                texels[y][x][2] = 0.75f;
                texels[y][x][3] = 0.5f;
            }
        }
        [sample replaceRegion:MTLRegionMake2D(0,0,4,4) mipmapLevel:0 withBytes:texels bytesPerRow:4*16];
        MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
        sd.minFilter = MTLSamplerMinMagFilterNearest;
        sd.magFilter = MTLSamplerMinMagFilterNearest;
        sd.sAddressMode = MTLSamplerAddressModeClampToEdge;
        sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
        sd.normalizedCoordinates = YES;
        id<MTLSamplerState> sampler = [device newSamplerStateWithDescriptor:sd];
        [encoder setFragmentTexture:sample atIndex:image_id];
        [encoder setFragmentSamplerState:sampler atIndex:image_id];
    }
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [encoder endEncoding];
    id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
    [blit copyFromTexture:target sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(64,64,1) toBuffer:readback destinationOffset:0 destinationBytesPerRow:64*16 destinationBytesPerImage:64*64*16];
    if (zmirror) {
        [blit copyFromTexture:zmirror sourceSlice:0 sourceLevel:0
            sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(64,64,1)
            toBuffer:zread destinationOffset:0 destinationBytesPerRow:256 destinationBytesPerImage:256*64];
    }
    [blit endEncoding];
    [command commit];
    [command waitUntilCompleted];
    if (zmirror) {
        const uint8_t *row = (const uint8_t *)zread.contents + 32 * 256;
        if (p.depth_mirror == 1) CHECK(((const uint16_t *)row)[32] == 0xffbf);
        else CHECK(((const uint32_t *)row)[32] == 0x07ffffbf);
    }
    CHECK(command.status == MTLCommandBufferStatusCompleted);
    const float *pixels = readback.contents;
    const float clear[4] = {0.0625, 0.125, 0.1875, 0.25};
    unsigned checked = 0;
    for (unsigned y = 0; y < 64; y++) {
        for (unsigned x = 0; x < 64; x++) {
            float px = (x + 0.5f) / 32 - 1;
            float py = 1 - (y + 0.5f) / 32;
            float side = (0.75f - py) * 0.5f - fabsf(px);
            /* Exclude raster edge ownership: test interiors and clear exterior. */
            if (fabsf(py + 0.75f) < 0.04f || fabsf(side) < 0.04f) continue;
            bool inside = py > -0.75f && side > 0;
            float color[4];
            memcpy(color, expected, sizeof(color));
            if (mode) {
                color[0] *= 0.125f + (x / 16) * 0.125f;
                color[1] *= 0.0625f + (y / 16) * 0.125f;
                color[2] *= 0.75f;
                if (mode == 1) color[3] *= 0.5f;
            }
            for (unsigned c = 0; c < 4; c++) {
                float actual = pixels[(y * 64 + x) * 4 + c];
                CHECK(isfinite(actual));
                CHECK(fabsf(actual - (inside ? color[c] : clear[c])) < 0.00001f);
            }
            checked++;
        }
    }
    printf("PASS %s: %u pixels checked on %s\n", name, checked, device.name.UTF8String);
}

static void render_mode(id<MTLDevice> device, R300FPProgram p,
                        float constants[32][4], const float expected[4],
                        const char *name, unsigned mode)
{
    render_pipeline(device, p, constants, expected, name, mode, NULL);
}

static R300VPProgram leopard_vertex_program(void)
{
    R300VPProgram p = {0};
    p.control[0] = 0x08820c80;
    p.control[1] = 0x00080080;
    p.control[2] = 0x00000088;
    p.attribute_mask = 7; p.varying_mask = 3;
    p.varying_output[0] = 1; p.varying_output[1] = 2;
    const uint32_t code[9][4] = {
        {0x00100201,0x00d10002,0x00d10001,0x00d10005},
        {0x00200201,0x00d10022,0x00d10001,0x00d10005},
        {0x00400201,0x00d10042,0x00d10001,0x00d10005},
        {0x00800201,0x00d10062,0x00d10001,0x00d10005},
        {0x00104201,0x00d10082,0x00d10041,0x00d10045},
        {0x00204201,0x00d100a2,0x00d10041,0x00d10045},
        {0x00404201,0x00d100c2,0x00d10041,0x00d10045},
        {0x00804201,0x00d100e2,0x00d10041,0x00d10045},
        {0x00f02202,0x00d10021,0x016da021,0x016da025}};
    memcpy(&p.code[128], code, sizeof(code));
    memset(&p.code_valid[128], 15, 9);
    return p;
}

static void reject_vertex(R300VPProgram p)
{
    char source[R300_FP_MSL_CAPACITY], reason[256];
    CHECK(!r300_vp_compile_msl(&p, source, sizeof(source), reason, sizeof(reason)));
    CHECK(source[0] == 0 && reason[0] != 0);
}

static void accept_vertex(R300VPProgram p)
{
    char source[R300_FP_MSL_CAPACITY], reason[256];
    bool accepted = r300_vp_compile_msl(&p, source, sizeof(source), reason,
                                        sizeof(reason));
    if (!accepted) {
        fprintf(stderr, "unexpected vertex rejection: %s\n", reason);
    }
    CHECK(accepted);
    CHECK(source[0] != 0 && reason[0] == 0);
}

static void render(id<MTLDevice> device, R300FPProgram p,
                   float constants[32][4], const float expected[4], const char *name)
{
    render_mode(device, p, constants, expected, name, 0);
}

/*
 * Guest byte order, checked as bytes.
 *
 * The live path renders straight into guest VRAM, which every other consumer
 * -- the CPU through the aperture, the 2D engine, the CRTC -- reads as
 * big-endian ARGB.  Viewed as BGRA8Unorm that means a true (r,g,b,a) has to
 * leave the shader as .yxwz.  Checking the floats would not catch a wrong
 * swizzle, so this case checks where each byte lands.
 */
static void render_guest_order(id<MTLDevice> device, R300FPProgram p,
                               float constants[32][4],
                               const uint8_t initial[4], const uint8_t expected[4],
                               bool blend, const uint8_t *texel_bytes, const char *name)
{
    char source[R300_FP_MSL_CAPACITY], reason[256];
    p.guest_order = true;
    p.blend_in_shader = blend;
    p.blend_src_rgb = p.blend_src_alpha = 0x21; /* GL_ONE */
    p.blend_dst_rgb = p.blend_dst_alpha = 0x27; /* ONE_MINUS_SRC_ALPHA */
    CHECK(r300_fp_compile_msl(&p, source, sizeof(source), reason, sizeof(reason)));
    NSString *msl = [[NSString stringWithUTF8String:source] stringByAppendingString:
        @"\nvertex R300FragmentInput test_vertex(uint n [[vertex_id]]) {\n"
         " const float2 v[3] = {float2(-3,-1), float2(3,-1), float2(0,3)};\n"
         " R300FragmentInput o = {}; o.position = float4(v[n], 0, 1); return o; }\n"];
    NSError *error = nil;
    MTLCompileOptions *options = [MTLCompileOptions new];
    options.fastMathEnabled = NO;
    id<MTLLibrary> library = [device newLibraryWithSource:msl options:options error:&error];
    if (!library) fprintf(stderr, "%s\n", error.description.UTF8String);
    CHECK(library);
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = [library newFunctionWithName:@"test_vertex"];
    pd.fragmentFunction = [library newFunctionWithName:@"r300_fragment"];
    pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    id<MTLRenderPipelineState> pipeline = [device newRenderPipelineStateWithDescriptor:pd error:&error];
    if (!pipeline) fprintf(stderr, "%s\n", error.description.UTF8String);
    CHECK(pipeline);

    /* A texture over ordinary memory, the way a VRAM view is made. */
    const unsigned w = 16, h = 4, pitch = 256;
    id<MTLBuffer> vram = [device newBufferWithLength:pitch * h
                                             options:MTLResourceStorageModeShared];
    CHECK(vram);
    uint8_t *bytes = vram.contents;
    for (unsigned y = 0; y < h; y++) {
        for (unsigned x = 0; x < w; x++) {
            memcpy(bytes + y * pitch + x * 4, initial, 4);
        }
    }
    MTLTextureDescriptor *td = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                     width:w height:h mipmapped:NO];
    td.storageMode = MTLStorageModeShared;
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    id<MTLTexture> target = [vram newTextureWithDescriptor:td offset:0 bytesPerRow:pitch];
    CHECK(target);

    id<MTLCommandQueue> queue = [device newCommandQueue];
    id<MTLCommandBuffer> command = [queue commandBuffer];
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor new];
    pass.colorAttachments[0].texture = target;
    pass.colorAttachments[0].loadAction = MTLLoadActionLoad;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> encoder = [command renderCommandEncoderWithDescriptor:pass];
    [encoder setRenderPipelineState:pipeline];
    float host_constants[R300_FP_HOST_CONSTANT_COUNT][4];
    r300_fp_pack_constants(&p, constants, host_constants);
    [encoder setFragmentBytes:host_constants length:sizeof(host_constants) atIndex:0];
    if (texel_bytes) {
        /* A texture in guest byte order, sampled in place the way a VRAM view
         * is: BGRA8Unorm over bytes that are really big-endian ARGB. */
        MTLTextureDescriptor *sd = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                         width:1 height:1 mipmapped:NO];
        sd.storageMode = MTLStorageModeShared;
        sd.usage = MTLTextureUsageShaderRead;
        id<MTLTexture> tex = [device newTextureWithDescriptor:sd];
        CHECK(tex);
        [tex replaceRegion:MTLRegionMake2D(0,0,1,1) mipmapLevel:0
                 withBytes:texel_bytes bytesPerRow:4];
        MTLSamplerDescriptor *smp = [MTLSamplerDescriptor new];
        smp.normalizedCoordinates = YES;
        id<MTLSamplerState> sampler = [device newSamplerStateWithDescriptor:smp];
        CHECK(sampler);
        [encoder setFragmentTexture:tex atIndex:0];
        [encoder setFragmentSamplerState:sampler atIndex:0];
    }
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [encoder endEncoding];
    [command commit];
    [command waitUntilCompleted];
    CHECK(command.status == MTLCommandBufferStatusCompleted);

    unsigned checked = 0;
    for (unsigned y = 0; y < h; y++) {
        for (unsigned x = 0; x < w; x++) {
            const uint8_t *px = bytes + y * pitch + x * 4;
            for (unsigned i = 0; i < 4; i++) {
                if (px[i] != expected[i]) {
                    fprintf(stderr, "%s: (%u,%u) byte %u is %02x, expected %02x\n",
                            name, x, y, i, px[i], expected[i]);
                }
                CHECK(px[i] == expected[i]);
            }
            checked++;
        }
    }
    printf("PASS %s: %u pixels checked byte-for-byte on %s\n",
           name, checked, device.name.UTF8String);
}

static void test_depth_fast_clear(void)
{
    void *renderer = r300_metal_create();
    CHECK(renderer);
    R300VPProgram vp = {0};
    vp.control[0] = 128 | (128u << 10) | (128u << 20);
    vp.control[2] = 128;
    vp.attribute_mask = 1;
    vp.code_valid[128] = 15;
    vp.code[128][0] = 0x00f00204; /* MAD to full position output */
    vp.code[128][1] = 0x00d10001; /* attribute 0, identity */
    for (unsigned i = 0; i < 4; i++) {
        vp.code[128][2] |= 5u << (13 + 3 * i); /* one */
        vp.code[128][3] |= 4u << (13 + 3 * i); /* zero */
    }
    R300FPProgram fp = base_program();
    R300VertexInput vertices[3] = {0};
    const float xy[3][2] = {{-1,-1},{3,-1},{-1,3}};
    for (unsigned i = 0; i < 3; i++) {
        vertices[i].a[0][0] = xy[i][0];
        vertices[i].a[0][1] = xy[i][1];
        vertices[i].a[0][2] = 0.5f;
        vertices[i].a[0][3] = 1;
    }
    float vc[256][4] = {{0}}, fc[32][4] = {{1,0,0,1}};
    uint8_t target[8 * 8 * 4] = {0};
    R300MetalDraw d = {.vp=&vp, .fp=&fp, .vertices=vertices,
        .vertex_count=3, .vertex_constants=vc, .fragment_constants=fc,
        .target=target, .target_size=sizeof(target), .width=8, .height=8,
        .viewport_width=8, .viewport_height=8, .scissor={0,0,8,8},
        .color_mask=15, .depth_enable=true, .depth_write=true,
        .depth_func=1, .depth_offset=0x123000, .depth_pitch=8};
    char error[256];
    const double clears[] = {1, 0, 1};
    for (unsigned pass = 0; pass < 3; pass++) {
        memset(target, 0, sizeof(target));
        CHECK(r300_metal_clear_depth(renderer, d.depth_offset, d.depth_pitch,
                                     clears[pass], 0, true, error, sizeof(error)));
        CHECK(r300_metal_draw(renderer, &d, error, sizeof(error)));
        for (unsigned i = 0; i < 64; i++) {
            if (target[i*4] != (pass == 1 ? 0 : 255)) {
                fprintf(stderr, "depth clear pass=%u pixel=%u rgba=%u,%u,%u,%u\n",
                        pass, i, target[i*4], target[i*4+1], target[i*4+2], target[i*4+3]);
            }
            CHECK(target[i*4] == (pass == 1 ? 0 : 255));
            CHECK(target[i*4+1] == 0 && target[i*4+2] == 0);
            CHECK(target[i*4+3] == (pass == 1 ? 0 : 255));
        }
    }
    CHECK(!r300_metal_clear_depth(renderer, d.depth_offset, 0, 1, 0, true,
                                  error, sizeof(error)));
    d.stencil_enable = true;
    d.stencil_func[0] = d.stencil_func[1] = 3; /* EQUAL */
    d.stencil_ref = 7;
    d.stencil_read_mask = 255;
    for (unsigned pass = 0; pass < 4; pass++) {
        memset(target, 0, sizeof(target));
        unsigned stencil = pass == 1 ? 8 : 7;
        CHECK(r300_metal_clear_depth(renderer, d.depth_offset, d.depth_pitch,
                                     1, stencil, pass != 2, error, sizeof(error)));
        CHECK(r300_metal_draw(renderer, &d, error, sizeof(error)));
        for (unsigned i = 0; i < 64; i++) {
            CHECK(target[i*4] == ((pass == 0 || pass == 3) ? 255 : 0));
        }
    }
    uint8_t *vram = malloc(0x124000);
    CHECK(vram);
    memset(vram, 0xcd, 0x124000);
    CHECK(r300_metal_read_depth(renderer, 0, vram, 0x124000,
                                d.depth_offset + 3*16, d.depth_offset + 3*16 + 2,
                                error, sizeof(error)));
    for (unsigned x = 0; x < 8; x++) {
        CHECK(vram[d.depth_offset + 3*16 + x*2] == 0xbf);
        CHECK(vram[d.depth_offset + 3*16 + x*2 + 1] == 0xff);
    }
    CHECK(vram[d.depth_offset + 3*16 - 1] == 0xcd);
    CHECK(vram[d.depth_offset + 4*16] == 0xcd);
    d.depth_format = 2;
    CHECK(r300_metal_clear_depth(renderer, d.depth_offset, d.depth_pitch,
                                 1, 7, true, error, sizeof(error)));
    CHECK(r300_metal_draw(renderer, &d, error, sizeof(error)));
    CHECK(r300_metal_read_depth(renderer, 0, vram, 0x124000,
                                d.depth_offset + 3*32, d.depth_offset + 3*32 + 4,
                                error, sizeof(error)));
    const uint8_t z24s8[4] = {0xbf, 0xff, 0xff, 7};
    for (unsigned x = 0; x < 8; x++) {
        CHECK(!memcmp(vram + d.depth_offset + 3*32 + x*4, z24s8, 4));
    }
    free(vram);
    /* Real guest clear form: a programmable point in window coordinates,
     * expanded to the full target, with color writes disabled. */
    CHECK(r300_metal_clear_depth(renderer, d.depth_offset, d.depth_pitch,
                                 0, 7, true, error, sizeof(error)));
    R300VertexInput clear_point[6] = {0};
    for (unsigned i = 0; i < 6; i++) {
        clear_point[i].a[0][0] = clear_point[i].a[0][1] = 4;
        clear_point[i].a[0][2] = clear_point[i].a[0][3] = 1;
    }
    vp.window_position_width = vp.window_position_height = 8;
    vp.expand_point_rect = true;
    vp.point_half_clip_x = vp.point_half_clip_y = 1;
    d.vertices = clear_point; d.vertex_count = 6;
    d.stencil_enable = false; d.depth_func = 7; d.color_mask = 0;
    CHECK(r300_metal_draw(renderer, &d, error, sizeof(error)));
    vp.window_position_width = vp.window_position_height = 0;
    vp.expand_point_rect = false;
    d.vertices = vertices; d.vertex_count = 3; d.depth_func = 1; d.color_mask = 15;
    memset(target,0,sizeof(target));
    CHECK(r300_metal_draw(renderer, &d, error, sizeof(error)));
    for (unsigned i = 0; i < 64; i++) CHECK(target[i*4] == 255);
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    /* The submit layer borrows its owner's resources, as in the card. Keep
     * this standalone owner alive for the other tests in this process. */
    static id<MTLBuffer> shared;
    static id<MTLCommandQueue> submit_queue;
    shared = [device newBufferWithLength:4096 options:MTLResourceStorageModeShared];
    submit_queue = [device newCommandQueue];
    CHECK(shared);
    CHECK(submit_queue);
    gpu_submit_init(device, submit_queue, shared);
    memset(shared.contents, 0xa5, shared.length);
    d.stencil_enable = false;
    d.depth_tiled_export = true;
    d.depth_guest_vram = shared.contents;
    d.depth_guest_offset = 256;
    d.depth_guest_size = shared.length;
    CHECK(r300_metal_clear_depth(renderer, d.depth_offset, d.depth_pitch,
                                 1, 7, true, error, sizeof(error)));
    if (!r300_metal_draw(renderer, &d, error, sizeof(error))) {
        fprintf(stderr,"tiled depth export: %s\n",error); CHECK(false);
    }
    /* Two 4x8 blocks across, sample1 occupies the omitted byte bit4. */
    for (unsigned y = 0; y < 8; y++) {
        for (unsigned x = 0; x < 8; x++) {
            unsigned addr = 256 + (x/4)*256 + ((y/4)&1)*128 +
                ((y/2)&1)*64 + ((x/2)&1)*32 + (y&1)*8 + (x&1)*4;
            const uint8_t expected[] = {0xbf,0xff,0xff,0xa5};
            CHECK(!memcmp((uint8_t *)shared.contents + addr,expected,4));
            CHECK(!memcmp((uint8_t *)shared.contents + addr + 16,expected,4));
        }
    }
    CHECK(((uint8_t *)shared.contents)[255] == 0xa5);
    CHECK(((uint8_t *)shared.contents)[768] == 0xa5);
    /* Direct color rendering must also track the compute depth write, so
     * CPU depth reads wait for it rather than racing the pending batch. */
    d.guest_target = true;
    d.guest_offset = 1024;
    d.guest_pitch = 64;
    CHECK(r300_metal_draw(renderer, &d, error, sizeof(error)));
    CHECK(gpu_range_busy(256, 768, false));
    CHECK(gpu_batch_conflict(256, 768, NULL));
    CHECK(!gpu_range_busy(768, 1024, false));
    CHECK(gpu_submit_flush());
    CHECK(!gpu_range_busy(256, 768, false));
    for (unsigned y = 0; y < 8; y++) {
        for (unsigned x = 0; x < 8; x++) {
            unsigned addr = 256 + (x/4)*256 + ((y/4)&1)*128 +
                ((y/2)&1)*64 + ((x/2)&1)*32 + (y&1)*8 + (x&1)*4;
            const uint8_t expected[] = {0xbf,0xff,0xff,0xa5};
            CHECK(!memcmp((uint8_t *)shared.contents + addr,expected,4));
            CHECK(!memcmp((uint8_t *)shared.contents + addr + 16,expected,4));
        }
    }
    /* Multiple queued draws must snapshot constants rather than aliasing
     * the caller's arrays or reusing an in-flight arena slice. The second
     * draw also hits the exact shader cache with different constant data. */
    d.depth_tiled_export = false;
    d.depth_enable = d.depth_write = false;
    d.scissor[0] = 0; d.scissor[2] = 4;
    fc[0][0] = 1; fc[0][1] = 0;
    CHECK(r300_metal_draw(renderer, &d, error, sizeof(error)));
    d.scissor[0] = 4;
    fc[0][0] = 0; fc[0][1] = 1;
    CHECK(r300_metal_draw(renderer, &d, error, sizeof(error)));
    /* A changed pipeline in the retained pass must see the first draw's
     * destination color through framebuffer fetch, without a pass break. */
    d.scissor[0] = 0;
    fc[0][0] = fc[0][1] = 0; fc[0][2] = fc[0][3] = 0.5f;
    fp.blend_in_shader = true;
    fp.blend_src_rgb = fp.blend_src_alpha = 0x21; /* ONE */
    fp.blend_dst_rgb = fp.blend_dst_alpha = 0x27; /* ONE_MINUS_SRC_ALPHA */
    CHECK(r300_metal_draw(renderer, &d, error, sizeof(error)));
    memset(fc, 0, sizeof(fc));
    /* Another encoder owner must close the retained R350 render pass before
     * opening a blit encoder on this same command buffer. */
    id<MTLBlitCommandEncoder> transition = [gpu_submit_buffer_for(NULL) blitCommandEncoder];
    CHECK(transition);
    [transition fillBuffer:shared range:NSMakeRange(2048, 16) value:0x6b];
    [transition endEncoding];
    CHECK(gpu_submit_flush());
    CHECK(((uint8_t *)shared.contents)[2048] == 0x6b);
    for (unsigned y = 0; y < 8; y++) {
        for (unsigned x = 0; x < 8; x++) {
            const uint8_t *px = (uint8_t *)shared.contents + 1024 + y*64 + x*4;
            CHECK(px[3] == 255); /* native BGRA view */
            CHECK(px[1] == (x >= 4 ? 255 : 0));
            if (x < 4) {
                CHECK(px[0] >= 127 && px[0] <= 128);
                CHECK(px[2] >= 127 && px[2] <= 128);
            } else {
                CHECK(px[0] == 0 && px[2] == 0);
            }
        }
    }
    puts("PASS queued R350 constants, shader-cache reuse, framebuffer-fetch blending and encoder handoff");
    d.scissor[0] = 0; d.scissor[2] = 8;
    /* Render to local VRAM and sample it through a different view before
     * any CPU flush. The shared queue must order the producer/consumer. */
    fp = base_program();
    fc[0][0] = 1; fc[0][1] = fc[0][2] = 0; fc[0][3] = 1;
    CHECK(r300_metal_draw(renderer, &d, error, sizeof(error)));
    CHECK(gpu_range_busy(1024, 1536, false));
    fp.control[0] = 8; fp.control[1] = 1; fp.input_mask = 1;
    fp.tex[0] = (1 << 6) | (1 << 15);
    fp.alu[0].rgb_addr = (7u << 26) | 1;
    fp.alu[0].alpha_addr = (1u << 24) | 1;
    /* Exercise the real backend resource walk, not only shader compilation:
     * no first-node fetch, followed by a second-node texture read. */
    fp.control[0] = 1;
    fp.control[2] = 1 << 6;
    fp.node[2] = 0;
    fp.node[3] = (1u << 22) | 1;
    fp.alu[1] = fp.alu[0];
    fp.alu[0].rgb_addr = 7u << 23;
    fp.alu[0].alpha_addr = 1u << 23;
    vp.varying_mask = 1; vp.varying_output[0] = 0;
    d.textures[0] = (R300MetalTexture){.width=8, .height=8,
        .address_s=2, .address_t=2, .guest_source=true,
        .guest_offset=1024, .guest_pitch=64, .guest_bpp=4};
    d.guest_offset = 2560;
    CHECK(r300_metal_draw(renderer, &d, error, sizeof(error)));
    CHECK(gpu_submit_flush());
    for (unsigned y = 0; y < 8; y++) {
        for (unsigned x = 0; x < 8; x++) {
            const uint8_t *px = (uint8_t *)shared.contents + 2560 + y*64 + x*4;
            CHECK(px[0] == 0 && px[1] == 0 && px[2] == 255 && px[3] == 255);
        }
    }
    puts("PASS pending R350 render-to-texture alias orders without a CPU wait");
    /* Now the source has no pending GPU writer. Queue a reader, then
     * overwrite its source through a target alias: this is a read/write
     * hazard, not the write/read hazard checked above. */
    CHECK(r300_metal_draw(renderer, &d, error, sizeof(error)));
    GPUSurfaceKey overwrite = {1024, 8, 8, 64, MTLPixelFormatBGRA8Unorm};
    CHECK(gpu_batch_conflict(1024, 1536, &overwrite));
    fp = base_program(); vp.varying_mask = 0;
    d.guest_offset = 1024;
    fc[0][0] = 0; fc[0][1] = 1;
    CHECK(r300_metal_draw(renderer, &d, error, sizeof(error)));
    CHECK(gpu_submit_flush());
    for (unsigned y = 0; y < 8; y++) {
        for (unsigned x = 0; x < 8; x++) {
            const uint8_t *old = (uint8_t *)shared.contents + 2560 + y*64 + x*4;
            const uint8_t *fresh = (uint8_t *)shared.contents + 1024 + y*64 + x*4;
            CHECK(old[0] == 0 && old[1] == 0 && old[2] == 255 && old[3] == 255);
            CHECK(fresh[0] == 0 && fresh[1] == 255 && fresh[2] == 0 && fresh[3] == 255);
        }
    }
    puts("PASS pending GPU texture read orders before an aliased target overwrite");
    /* Same shader, different fog uniforms, queued before completion. */
    fp.fog_enable = true; fp.fog_source = 4;
    fp.fog_scale = 0; fp.fog_offset = 1;
    fp.fog_colour[0] = 1;
    d.scissor[0] = 0; d.scissor[2] = 4;
    CHECK(r300_metal_draw(renderer, &d, error, sizeof(error)));
    fp.fog_colour[0] = 0; fp.fog_colour[2] = 1;
    d.scissor[0] = 4;
    CHECK(r300_metal_draw(renderer, &d, error, sizeof(error)));
    memset(fp.fog_colour, 0, sizeof(fp.fog_colour));
    CHECK(gpu_submit_flush());
    for (unsigned y = 0; y < 8; y++) {
        for (unsigned x = 0; x < 8; x++) {
            const uint8_t *px = (uint8_t *)shared.contents + 1024 + y*64 + x*4;
            CHECK(px[0] == (x < 4 ? 0 : 255));
            CHECK(px[1] == 0 && px[2] == (x < 4 ? 255 : 0) && px[3] == 255);
        }
    }
    fp.fog_offset = NAN;
    CHECK(!r300_metal_draw(renderer, &d, error, sizeof(error)));
    puts("PASS queued fog uniforms preserve lighting across shader-cache reuse");
    d.scissor[0] = 0; d.scissor[2] = 8;
    fp = base_program();
    vp.varying_mask = 0;
    d.depth_tiled_export = true;
    d.depth_write = true;
    d.guest_target = false;
    d.depth_guest_size = 767;
    CHECK(!r300_metal_draw(renderer, &d, error, sizeof(error)));
    CHECK(strstr(error,"bounds"));
    r300_metal_destroy(renderer);
    puts("PASS R350 fast depth/stencil clears restore visibility and preserve unrequested stencil");
    puts("PASS R350 selected-row depth readback packs Z16 and Z24S8 in guest byte order");
    puts("PASS R350 GPU tiled two-sample depth export, endian, stencil preservation and bounds");
    puts("PASS R350 direct tiled depth tracks pending writes and flushes before CPU access");
    puts("PASS R350 window-coordinate full-point depth clear restores next frame");
}

int main(void)
{
    @autoreleasepool {
        test_depth_fast_clear();
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        CHECK(device);
        float c[32][4] = {{0.25f, 0.5f, 0.75f, 0.375f}, {0.125f, 0.625f, 0.875f, 0.9375f}};
        R300FPProgram p = base_program();
        const float first[4] = {0.25f, 0.5f, 0.75f, 0.375f};
        render(device, p, c, first, "constant MAD");
        {
            R300FPProgram fog = p;
            fog.fog_enable = true;
            fog.fog_source = 4; /* test vertex has W=1 */
            fog.fog_scale = -1;
            fog.fog_offset = 1;
            fog.fog_colour[0] = 0.125f;
            fog.fog_colour[1] = 0.25f;
            fog.fog_colour[2] = 0.5f;
            render(device, fog, c, first, "fog amount zero preserves unfogged UI");
            const float haze[4] = {0.1875f, 0.375f, 0.625f, 0.375f};
            fog.fog_offset = 1.5f;
            render(device, fog, c, haze, "half fog blends RGB and preserves alpha");
            const float full[4] = {0.125f, 0.25f, 0.5f, 0.375f};
            fog.fog_offset = 2;
            render(device, fog, c, full, "fog amount one reaches fog color");
            R300VPProgram fog_vp = leopard_vertex_program();
            fog_vp.varying_mask = 0;
            fog_vp.fog_varying = true;
            fog_vp.fog_output = 1;
            fog_vp.code[136][0] = 0x00802202; /* only o[1].a is written */
            fog.fog_source = 1;
            fog.fog_offset = 1;
            const float partial[4] = {0.234375f,0.46875f,0.71875f,0.375f};
            render_pipeline(device, fog, c, partial,
                            "fog links alpha-only vertex output", 0, &fog_vp);
        }
        p.depth_mirror = 1;
        render(device,p,c,first,"fragment depth mirror Z16 guest bytes");
        p.depth_mirror = 2;
        render(device,p,c,first,"fragment depth mirror Z24 preserves guest stencil byte");
        p.depth_mirror = 0;
        {
            /* Exact 1/255 steps, so nothing here depends on unorm rounding. */
            float g[32][4] = {{0x40/255.0f, 0x80/255.0f, 0xbf/255.0f, 0x60/255.0f}};
            R300FPProgram q = base_program();
            const uint8_t was[4] = {0x11, 0x22, 0x33, 0x44};
            /* true (r,g,b,a) = 40 80 bf 60 lands as big-endian ARGB. */
            const uint8_t argb[4] = {0x60, 0x40, 0x80, 0xbf};
            render_guest_order(device, q, g, was, argb, false, NULL,
                               "guest-order target writes big-endian ARGB");
            /* Zero source alpha, so premultiplied-over is a plain sum and the
             * expected bytes stay exact. */
            float b[32][4] = {{0x40/255.0f, 0x80/255.0f, 0x3f/255.0f, 0.0f}};
            const uint8_t dst[4] = {0x40, 0x10, 0x20, 0x30};   /* ARGB in memory */
            const uint8_t sum[4] = {0x40, 0x50, 0xa0, 0x6f};
            render_guest_order(device, q, b, dst, sum, true, NULL,
                               "in-shader premultiplied-over on a guest-order target");
            /*
             * Sampling a guest-order texture and writing a guest-order target
             * has to be the identity: the same four bytes come back out.  That
             * is the whole claim behind reading textures in place, and a float
             * comparison could not check it.
             */
            R300FPProgram r = base_program();
            r.control[0] = 8; r.control[1] = 1; r.input_mask = 2;
            r.tex[0] = 1 | (1 << 6) | (1 << 15);
            r.alu[0].rgb_addr = (7u << 26) | 1;
            r.alu[0].alpha_addr = (1u << 24) | 1;
            r.tex_swizzled = true;
            /* BGRA8Unorm over [A][R][G][B]: red is byte 2, green byte 1,
             * blue byte 3 via alpha, alpha byte 0 via blue. */
            const uint8_t lanes[4] = {1, 0, 3, 2};
            memcpy(r.tex_swizzle[0], lanes, 4);
            const uint8_t texel[4] = {0x60, 0x40, 0x80, 0xbf};  /* A R G B */
            const uint8_t cleared[4] = {0x01, 0x02, 0x03, 0x04};
            float none[32][4] = {{0}};
            render_guest_order(device, r, none, cleared, texel, false, texel,
                               "guest-order texture sampled in place round-trips");
            /* Chess's identity RGBA upload must not inherit the ARGB
             * aperture reversal used by WindowServer and Warcraft. */
            const uint8_t rgba_lanes[4] = {2,1,0,3};
            memcpy(r.tex_swizzle[0], rgba_lanes, 4);
            const uint8_t chess_texel[4] = {0x40,0x80,0xbf,0x60};
            render_guest_order(device, r, none, cleared, texel, false,
                               chess_texel,
                               "identity RGBA texture view preserves Chess colors");
        }
        p.alu[0].alpha_addr |= 1;
        p.alu[0].rgb_inst = 12 | (21 << 7) | (20 << 14);
        p.alu[0].alpha_inst = 2 | (17 << 7) | (16 << 14);
        const float cross[4] = {0.9375f, 0.9375f, 0.9375f, 0.75f};
        render(device, p, c, cross, "independent RGB/alpha source banks");
        p = base_program();
        p.alu[0].rgb_addr = (7u << 26) | 32 | (33u << 6);
        p.alu[0].alpha_addr = (1u << 24) | 32 | (33u << 6);
        p.alu[0].rgb_inst = (2u << 23) | 0 | (4u << 7) | (20u << 14);
        p.alu[0].alpha_inst = (1u << 23) | 9 | (10u << 7) | (16u << 14);
        const float dot4[4] = {1.3515625f,1.3515625f,1.3515625f,1.3515625f};
        render(device, p, c, dot4, "paired four-component dot product");
        p = base_program();
        p.alu[0].rgb_inst = 22 | (21 << 7) | (20 << 14);
        p.alu[0].alpha_inst = 18 | (17 << 7) | (16 << 14);
        const float half_literal[4] = {0.5f,0.5f,0.5f,0.5f};
        render(device, p, c, half_literal, "RGB and alpha half literals");
        p.alu[0].rgb_inst = 0 | (21 << 7) | (22 << 14);
        p.alu[0].alpha_inst = 9 | (17 << 7) | (18 << 14);
        const float half_add[4] = {0.75f,1.0f,1.25f,0.875f};
        render(device, p, c, half_add, "half literal in multiply-add third operand");
        p.alu[0].rgb_inst = (22 | 32 | 64) | (21 << 7) | (20 << 14);
        p.alu[0].alpha_inst = (18 | 32 | 64) | (17 << 7) | (16 << 14);
        const float negative_half[4] = {-0.5f,-0.5f,-0.5f,-0.5f};
        render(device, p, c, negative_half, "half literal abs and negate modifiers");
        p = base_program();
        p.alu[0].alpha_inst = (9u << 23) | 9;
        const float logarithm[4] = {0.25f,0.5f,0.75f,-1.4150375f};
        render(device, p, c, logarithm, "alpha base-two logarithm");
        p.alu[0].alpha_inst = (10u << 23) | 9;
        const float reciprocal[4] = {0.25f,0.5f,0.75f,2.6666667f};
        render(device, p, c, reciprocal, "alpha reciprocal");
        p = base_program();
        p.alu[0].rgb_addr = (7u << 26) | 32 | (33u << 6) | (32u << 12);
        p.alu[0].rgb_inst = (8u << 23) | 0 | (4u << 7) | (32u << 14);
        const float compare[4] = {0.125f,0.625f,0.875f,0.375f};
        render(device, p, c, compare, "component compare selection");
        p = base_program();
        p.alu[0].alpha_addr |= 33u << 6;
        p.alu[0].alpha_inst = (6u << 23) | 9 | (10u << 7) | (16u << 14);
        const float alpha_cmp_nonnegative[4] = {0.25f,0.5f,0.75f,0.375f};
        render(device, p, c, alpha_cmp_nonnegative, "alpha compare selects first at zero");
        p.alu[0].alpha_inst = (6u << 23) | 9 | (10u << 7) | ((18u | 32u) << 14);
        const float alpha_cmp_negative[4] = {0.25f,0.5f,0.75f,0.9375f};
        render(device, p, c, alpha_cmp_negative, "alpha compare selects second for negative half");
        p.alu[0].alpha_inst = (6u << 23) | 17 | (17u << 7) | (10u << 14);
        const float alpha_cmp_literal[4] = {0.25f,0.5f,0.75f,1.0f};
        render(device, p, c, alpha_cmp_literal, "Final Cut alpha compare literal operands");
        p = base_program();
        p.control[1] = 1; p.control[2] = 3 << 6;
        p.node[3] |= 3 << 6;
        p.alu[0].rgb_addr = (7u << 23) | 32;
        p.alu[1] = p.alu[0];
        p.alu[1].rgb_addr = (1u << 18) | (6u << 23) | 33;
        p.alu[2] = p.alu[0];
        p.alu[2].rgb_addr = (6u << 23) | 1;
        p.alu[3] = p.alu[0];
        p.alu[3].rgb_addr = 7u << 26;
        const float masked_copy[4] = {0.25f,0.625f,0.875f,0.375f};
        render(device, p, c, masked_copy, "partial YZ temporary copy preserves defined X");
        p.alu[2].rgb_addr = (7u << 23) | 1;
        reject(p); /* Copying the undefined X remains invalid. */
        p = base_program();
        p.alu[0].rgb_addr |= 33u << 6;
        p.alu[0].alpha_addr |= 33u << 6;
        p.alu[0].rgb_inst = 0 | (4 << 7) | (21 << 14);
        p.alu[0].alpha_inst = 9 | (10 << 7) | (16 << 14);
        const float mad[4] = {1.03125f, 1.3125f, 1.65625f, 0.3515625f};
        render(device, p, c, mad, "multiply-add without implicit saturation");
        p.alu[0].rgb_inst |= 1u << 30;
        const float sat[4] = {1,1,1,0.3515625f};
        render(device, p, c, sat, "explicit RGB saturation");
        p = base_program();
        c[31][0] = -0.25f; c[31][1] = -0.5f; c[31][2] = -0.75f; c[31][3] = -0.375f;
        p.alu[0].rgb_addr |= 31; p.alu[0].alpha_addr |= 31;
        p.alu[0].rgb_inst = (23 | 64 | 32) | (21 << 7) | (20 << 14);
        p.alu[0].alpha_inst = (9 | 64) | (17 << 7) | (16 << 14);
        const float modifiers[4] = {-0.5f,-0.75f,-0.25f,0.375f};
        render(device, p, c, modifiers, "constant 31, swizzle, abs then negate");
        p = base_program();
        p.control[0] = 8; p.control[1] = 1; p.input_mask = 2;
        p.tex[0] = 1 | (1 << 6) | (1 << 15);
        p.alu[0].rgb_addr = (7u << 26) | 1;
        p.alu[0].alpha_addr = (1u << 24) | 1;
        const float white[4] = {1,1,1,1};
        render_mode(device, p, c, white, "TEX nearest 2D gradient", 1);
        p.tex[0] = 1 | (1 << 6) | (3 << 15) | (5 << 11);
        render_mode(device, p, c, white, "TXP divide by W, image binding 5", 1);
        p.tex[4] = p.tex[0]; p.tex[0] = 0;
        p.control[2] = (3 << 13) | (1 << 18) | (1 << 6);
        p.node[3] |= (1 << 12) | 1;
        p.alu[1] = p.alu[0]; memset(&p.alu[0], 0, sizeof(p.alu[0]));
        render_mode(device, p, c, white, "relocated texture bank and nonzero node starts", 1);
        /* FCP scales coordinates in an ALU-only first node, then fetches
         * packed video in the second. Bit 3 is clear but unit 5 is required. */
        p = base_program();
        p.control[0] = 1; p.control[1] = 1; p.control[2] = 1 << 6;
        p.input_mask = 2;
        p.node[2] = 0;
        p.node[3] = (1u << 22) | 1;
        p.tex[0] = 1 | (1 << 6) | (1 << 15) | (5 << 11);
        p.alu[1] = p.alu[0];
        p.alu[1].rgb_addr = (7u << 26) | 1;
        p.alu[1].alpha_addr = (1u << 24) | 1;
        p.alu[0].rgb_addr = (7u << 23) | (1u << 18) | 1;
        p.alu[0].alpha_addr = (1u << 23) | (1u << 18) | 1;
        CHECK(r300_fp_texture_mask(&p) == (1u << 5));
        render_mode(device, p, c, white, "ALU first node then TEX image 5", 1);
        /* Distinct image IDs and relocated banks must be included too. */
        p.control[0] |= 8;
        p.control[2] |= (2 << 13) | (1 << 18);
        p.node[3] |= 1 << 12;
        p.tex[2] = (1 << 15) | (3 << 11);
        p.tex[3] = (1 << 15) | (5 << 11);
        CHECK(r300_fp_texture_mask(&p) == ((1u << 3) | (1u << 5)));
        /* Exact instruction words from Leopard's first captured draw.
         * Interpolator linkage and host textures are explicit test inputs. */
        p = base_program();
        p.control[0] = 8; p.control[1] = 1; p.control[2] = 2;
        p.input_mask = 3; p.tex[0] = 0x00018041;
        p.alu[2] = (R300FPInstruction) {0x1c020040,0x01020800,0x40050200,0x40040509};
        const float leopard[4] = {0.5f,0.75f,0.25f,0.875f * 0.375f};
        render_mode(device, p, c, leopard, "Leopard captured TXP/MAD at ALU offset 2", 2);
        R300VPProgram vp = leopard_vertex_program();
        render_pipeline(device, p, c, leopard, "captured Leopard vertex + fragment programs", 2, &vp);
        vp.code[136][0] = (vp.code[136][0] & ~255u) | R300_VE_ADD;
        vp.code[136][2] = vp.code[136][1]; /* doubled vertex colour */
        vp.varying_colour_mask = 1;
        const float clipped_colour[4] = {1,1,0.5f,0.375f};
        render_pipeline(device, p, c, clipped_colour,
                        "GA vertex colour clamps RGB and alpha before interpolation", 2, &vp);
        vp.colour_clamp_mode = 1;
        const float extended_rgb[4] = {1,1.5f,0.5f,0.375f};
        render_pipeline(device, p, c, extended_rgb,
                        "GA extended RGB preserves values above one independently of alpha", 2, &vp);
        vp.colour_clamp_mode = 2;
        const float extended_alpha[4] = {1,1,0.5f,1.75f*0.375f};
        render_pipeline(device, p, c, extended_alpha,
                        "GA extended alpha preserves values above one independently of RGB", 2, &vp);
        vp = leopard_vertex_program();
        vp.code[136][0] = (vp.code[136][0] & ~255u) | R300_VE_SET_GREATER_THAN_EQUAL;
        vp.code[136][2] = 0x000a6021; /* input1.WZYX */
        const float compare_mixed[4] = {0,1,0,0.375f};
        render_pipeline(device, p, c, compare_mixed, "vector SGE mixed lanes", 2, &vp);
        vp.code[136][2] = vp.code[136][1];
        const float compare_equal[4] = {1,1,1,0.375f};
        render_pipeline(device, p, c, compare_equal, "vector SGE equality", 2, &vp);
        vp = leopard_vertex_program();
        /* Output-replicate-X must use value.x, not each output lane. */
        vp.code[136][0] = 0x00f02302;
        const float repl[4] = {0.5f,0.5f,0.5f,0.5f * 0.375f};
        render_pipeline(device, p, c, repl, "vector output replicate X", 2, &vp);
        vp = leopard_vertex_program();
        vp.code[136][0] = 0x00f02345;
        vp.code[136][1] = 0x00d10021;
        vp.code[136][2] = 0xffffffff; /* unused B must not be validated */
        vp.code[136][3] = 0x00000021; /* exponent from input1.X = 0.5 */
        const float power_ff[4] = {0.93541435f,0.93541435f,0.93541435f,0.93541435f*0.375f};
        render_pipeline(device, p, c, power_ff, "FF power exponent from C", 2, &vp);
        vp.code[136][1] |= 1u << 28;
        /* Negate the result before the fragment shader's explicit saturation;
         * otherwise a wrong zero result and a correct negative result alias. */
        vp.code[136][0] = 0x00f00045;
        vp.control[0] += 1u << 20; vp.control[2] = 137;
        vp.code_valid[137] = 15;
        vp.code[137][0] = 0x00f02302;
        vp.code[137][1] = 0x00d10000;
        vp.code[137][2] = 0x1f6da025; /* literal negative one */
        render_pipeline(device, p, c, power_ff, "FF power signed negative base", 2, &vp);
        vp.control[0] -= 1u << 20; vp.control[2] = 136;
        vp.code[136][0] = 0x00f02345;
        vp.code[136][3] = 0x01248021; /* exponent literal zero */
        render_pipeline(device, p, c, compare_equal, "FF power zero exponent", 2, &vp);
        /* Full scalar RSQ reads W even when its output mask is XYZ. */
        vp = leopard_vertex_program();
        vp.code[136][0] = 0x00f02349;
        vp.code[136][1] = 0x00d10021;
        const float reciprocal_sqrt[4] = {1.069045f,1.069045f,1.069045f,1.069045f * 0.375f};
        render_pipeline(device, p, c, reciprocal_sqrt, "full FF reciprocal square root", 2, &vp);
        vp.code[136][1] |= 1u << 28; /* negative W uses absolute value */
        render_pipeline(device, p, c, reciprocal_sqrt, "negative FF reciprocal square root", 2, &vp);
        vp.code[136][1] = 0x01248021; /* literal zero */
        const float zero_scalar[4] = {0,0,0,0};
        render_pipeline(device, p, c, zero_scalar, "zero FF reciprocal square root", 2, &vp);
        vp = leopard_vertex_program();
        vp.code[136][0] = 0x00800449; /* full RSQ -> alternate temp0.W */
        vp.code[136][1] = 0x00d10021;
        vp.control[0] += 1u << 20; vp.control[2] = 137;
        vp.code[137][0] = 0x00f02302;
        vp.code[137][1] = 0x00db6003; /* alternate temp0.W replicated */
        vp.code[137][2] = vp.code[137][3] = 0x016da021;
        vp.code_valid[137] = 15;
        render_pipeline(device, p, c, reciprocal_sqrt, "full RSQ alternate temporary roundtrip", 2, &vp);
        vp = leopard_vertex_program();
        vp.flow_control = 1; reject_vertex(vp);
        vp = leopard_vertex_program(); vp.code_valid[128] = 7; reject_vertex(vp);
        vp = leopard_vertex_program(); vp.attribute_mask = 3; reject_vertex(vp);
        vp = leopard_vertex_program(); vp.control[1] = 0x00060080; reject_vertex(vp);
        vp = leopard_vertex_program(); vp.code[128][1] |= 0x80000000u; reject_vertex(vp);
        vp = leopard_vertex_program(); vp.code[128][0] = (vp.code[128][0] & ~255u) | 70; reject_vertex(vp);
        vp = leopard_vertex_program(); vp.varying_output[1] = 31; reject_vertex(vp);
        vp = leopard_vertex_program(); vp.control[0] = 0xffffffffu; reject_vertex(vp);
        /* Warcraft III issues ME_RECIP_FF as the math half of a dual
         * instruction.  It has one scalar source and writes one ATRM lane;
         * accepting the program verifies that this no longer falls through
         * the dual-opcode gate. */
        vp = leopard_vertex_program();
        vp.code[136][0] |= 1u << 28;
        vp.code[136][3] = 1u | (R300_ME_RECIP_FF << 21);
        accept_vertex(vp);
        /* Warcraft III clamps transformed values with VE_MAXIMUM before
         * sending them to the rasterizer. */
        vp = leopard_vertex_program();
        vp.code[136][0] = (vp.code[136][0] & ~255u) | R300_VE_MAXIMUM;
        accept_vertex(vp);
        /* Exact math opcode and operand shape from Warcraft III's 53-slot
         * lighting program. */
        vp = leopard_vertex_program();
        vp.code[135][0] = 0x00f02001;
        vp.code[136][0] = 0x0080004f;
        vp.code[136][1] = 0x00db6020;
        vp.code[136][2] = 0x016da005;
        vp.code[136][3] = 0x016da000;
        vp.varying_mask = 0;
        accept_vertex(vp);
        /* Warcraft's menu program writes a full ME_RECIP_FF instruction to
        * alternate temporary 0, then consumes it from the vector engine. */
        vp = leopard_vertex_program();
        vp.code[134][0] = 0x00f08001;
        vp.code[135][0] = 0x00800447;
        vp.code[135][1] = vp.code[135][2] = vp.code[135][3] = 0x00db6080;
        vp.varying_mask = 0;
        accept_vertex(vp);
        /* Apple's Leopard compositor reads unwritten lanes of a temporary
         * whose live XY lanes were produced earlier.  The captured route
         * requires those omitted lanes to enter RS as zero. */
        vp = leopard_vertex_program();
        vp.control[0] += 1u << 20; vp.control[2] = 137;
        vp.code[137][0] = 0x00f06203;
        vp.code[137][1] = 0x00d10080;
        vp.code[137][2] = vp.code[137][3] = 0x01248040;
        vp.code_valid[137] = 15;
        vp.varying_mask |= 4; vp.varying_output[2] = 3;
        reject_vertex(vp);
        vp.zero_initialize_temporaries = true;
        accept_vertex(vp);
        p.input_mask = 1; reject(p); /* missing texture coordinates */
        p.input_mask = 3; p.tex[0] |= 32; reject(p); /* unverified constant addressing */
        p.tex[0] = 1 | (1 << 6) | (2 << 15); reject(p); /* KIL not implemented */
        p = base_program(); p.control[0] = 1; reject(p); /* multiple nodes */
        p = base_program(); p.alu[0].rgb_addr &= ~(1u << 28); reject(p); /* undefined blue output */
        p = base_program(); p.control[1] = 32; reject(p);
        p = base_program(); p.input_mask = 1u << 10; reject(p);
        /* Paired ALUs must both read the old register value. */
        p = base_program(); p.control[1] = 1; p.control[2] = 64;
        p.node[3] |= 64;
        p.alu[0].rgb_addr = 32 | (1 << 18) | (7 << 23);
        p.alu[0].alpha_addr = 32 | (1 << 18) | (1 << 23);
        p.alu[1] = (R300FPInstruction) {
            (7u << 26) | (7u << 23) | (1u << 18) | 1u,
            (1u << 24) | 1u,
            21 | (21 << 7) | (20 << 14),
            0 | (17 << 7) | (16 << 14)};
        /* Output index must stay zero; write only a temporary in RGB half,
         * while alpha reads its old X. A third instruction exports RGB. */
        p.alu[1].rgb_addr &= ~(7u << 26);
        p.control[2] = 128; p.node[3] = (1u << 22) | 128;
        p.alu[2] = base_program().alu[0];
        p.alu[2].rgb_addr = (7u << 26) | 1;
        p.alu[2].alpha_addr = 0; /* preserve alpha output from instruction 1 */
        const float paired[4] = {1,1,1,0.25f};
        render(device, p, c, paired, "paired ALU old-value reads and partial output preservation");
        p = base_program();
        c[0][3] = 0.5f;
        const bool alpha_passes[8] = {false,true,false,true,false,true,false,true};
        const float alpha_color[4] = {0.25f,0.5f,0.75f,0.5f};
        const float background[4] = {0.0625f,0.125f,0.1875f,0.25f};
        for (unsigned op=0;op<8;op++) {
            p.alpha_func = 0x800 | (op << 8) | 128;
            char label[80]; snprintf(label,sizeof(label),"alpha comparison opcode %u",op);
            render(device,p,c,alpha_passes[op] ? alpha_color:background,label);
        }
        p.alpha_func |= 1u << 16; reject(p);
        p = base_program(); p.control[0] = 8; reject(p);
        p = base_program(); p.control[2] = 63 | 64; reject(p);
        p = base_program(); p.node[3] |= 1u << 23; reject(p);
        p = base_program(); p.alu[0].rgb_inst |= 3u << 23; reject(p);
        p = base_program(); p.alu[0].rgb_addr &= ~32u; reject(p);
        p = base_program(); p.alu[0].rgb_addr |= 1u << 29; reject(p);
        p = base_program(); p.alu[0].alpha_addr |= 1u << 25; reject(p);
        p = base_program(); p.alu[0].alpha_inst = 19 | (17 << 7) | (16 << 14); reject(p); /* reserved alpha selector */
        p = base_program();
        char tiny[2], error[128];
        CHECK(!r300_fp_compile_msl(&p, tiny, sizeof(tiny), error, sizeof(error)));
        CHECK(tiny[0] == 0 && error[0] != 0);
        puts("PASS unsupported-state rejection and bounded output");
    }
    return 0;
}
