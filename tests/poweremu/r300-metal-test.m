/* SPDX-License-Identifier: GPL-2.0-or-later */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <math.h>
#include "ppc_mac_gpu_r300_fp.h"
#include "ppc_mac_gpu_r300_vp.h"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
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
    id<MTLRenderCommandEncoder> encoder = [command renderCommandEncoderWithDescriptor:pass];
    [encoder setRenderPipelineState:pipeline];
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
    [encoder setFragmentBytes:constants length:32*4*sizeof(float) atIndex:0];
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
    [blit endEncoding];
    [command commit];
    [command waitUntilCompleted];
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
    CHECK(r300_vp_compile_msl(&p, source, sizeof(source), reason, sizeof(reason)));
    CHECK(source[0] != 0 && reason[0] == 0);
}

static void render(id<MTLDevice> device, R300FPProgram p,
                   float constants[32][4], const float expected[4], const char *name)
{
    render_mode(device, p, constants, expected, name, 0);
}

int main(void)
{
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        CHECK(device);
        float c[32][4] = {{0.25f, 0.5f, 0.75f, 0.375f}, {0.125f, 0.625f, 0.875f, 0.9375f}};
        R300FPProgram p = base_program();
        const float first[4] = {0.25f, 0.5f, 0.75f, 0.375f};
        render(device, p, c, first, "constant MAD");
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
        vp.flow_control = 1; reject_vertex(vp);
        vp = leopard_vertex_program(); vp.code_valid[128] = 7; reject_vertex(vp);
        vp = leopard_vertex_program(); vp.attribute_mask = 3; reject_vertex(vp);
        vp = leopard_vertex_program(); vp.control[1] = 0x00060080; reject_vertex(vp);
        vp = leopard_vertex_program(); vp.code[128][1] |= 0x80000000u; reject_vertex(vp);
        vp = leopard_vertex_program(); vp.code[128][0] = (vp.code[128][0] & ~255u) | 70; reject_vertex(vp);
        vp = leopard_vertex_program(); vp.varying_output[1] = 31; reject_vertex(vp);
        vp = leopard_vertex_program(); vp.control[0] = 0xffffffffu; reject_vertex(vp);
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
        p = base_program(); p.alu[0].rgb_inst = 22 | (21 << 7) | (20 << 14); reject(p);
        p = base_program();
        char tiny[2], error[128];
        CHECK(!r300_fp_compile_msl(&p, tiny, sizeof(tiny), error, sizeof(error)));
        CHECK(tiny[0] == 0 && error[0] != 0);
        puts("PASS unsupported-state rejection and bounded output");
    }
    return 0;
}
