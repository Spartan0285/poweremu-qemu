/* SPDX-License-Identifier: GPL-2.0-or-later
 * Synchronous R300 correctness backend. Manual reference counting matches
 * QEMU's Objective-C build; the standalone tests compile this file likewise.
 */
#import <Foundation/Foundation.h>
#include <math.h>
#import <Metal/Metal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ppc_mac_gpu_r300_metal.h"

typedef struct R300MetalContext {
    id<MTLDevice> device;
    id<MTLCommandQueue> queue;
    NSMutableDictionary *pipelines;
    NSRecursiveLock *lock;
} R300MetalContext;

void *r300_metal_create(void)
{
    R300MetalContext *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) return NULL;
    ctx->device = MTLCreateSystemDefaultDevice();
    ctx->queue = [ctx->device newCommandQueue];
    ctx->pipelines = [NSMutableDictionary new];
    ctx->lock = [NSRecursiveLock new];
    if (!ctx->device || !ctx->queue || !ctx->pipelines || !ctx->lock) {
        r300_metal_destroy(ctx);
        return NULL;
    }
    return ctx;
}

void r300_metal_destroy(void *renderer)
{
    R300MetalContext *ctx = renderer;
    if (!ctx) return;
    [ctx->queue release];
    [ctx->device release];
    [ctx->pipelines release];
    [ctx->lock release];
    free(ctx);
}

static bool fail(char *error, size_t capacity, const char *message)
{
    snprintf(error, capacity, "%s", message);
    return false;
}

static size_t row_pitch(unsigned width)
{
    return ((size_t)width * 4 + 255) & ~(size_t)255;
}

static id<MTLBuffer> upload_buffer(R300MetalContext *ctx, const uint8_t *rgba,
                                  unsigned width, unsigned height)
{
    size_t pitch = row_pitch(width);
    id<MTLBuffer> buffer = [[ctx->device newBufferWithLength:pitch * height
                              options:MTLResourceStorageModeShared] autorelease];
    if (buffer) {
        for (unsigned y = 0; y < height; y++) {
            memcpy((uint8_t *)buffer.contents + y * pitch,
                   rgba + (size_t)y * width * 4, (size_t)width * 4);
        }
    }
    return buffer;
}

static id<MTLTexture> private_texture(R300MetalContext *ctx, unsigned width,
                                      unsigned height, MTLTextureUsage usage)
{
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:
                                MTLPixelFormatRGBA8Unorm width:width height:height mipmapped:NO];
    td.storageMode = MTLStorageModePrivate;
    td.usage = usage;
    return [[ctx->device newTextureWithDescriptor:td] autorelease];
}

static void upload(id<MTLBlitCommandEncoder> blit, id<MTLBuffer> bytes,
                    id<MTLTexture> texture, unsigned width, unsigned height)
{
    [blit copyFromBuffer:bytes sourceOffset:0 sourceBytesPerRow:row_pitch(width)
     sourceBytesPerImage:row_pitch(width) * height sourceSize:MTLSizeMake(width,height,1)
              toTexture:texture destinationSlice:0 destinationLevel:0
      destinationOrigin:MTLOriginMake(0,0,0)];
}

bool r300_metal_draw(void *renderer, const R300MetalDraw *d,
                     char *error, size_t error_size)
{
    R300MetalContext *ctx = renderer;
    if (!ctx || !d || !d->vp || !d->fp || !d->vertices ||
        !d->vertex_constants || !d->fragment_constants || !d->target ||
        !d->vertex_count || d->vertex_count > 65536 ||
        (!d->primitive_points && d->vertex_count % 3) ||
        !d->width || !d->height || d->width > 4096 || d->height > 4096 ||
        !isfinite(d->viewport_x) || !isfinite(d->viewport_y) ||
        !isfinite(d->viewport_width) || !isfinite(d->viewport_height) ||
        fabs(d->viewport_x)>4096 || fabs(d->viewport_y)>4096 ||
        d->viewport_width<0 || d->viewport_height<0 ||
        d->viewport_width>4096 || d->viewport_height>4096 ||
        d->target_size < (uint64_t)d->width * d->height * 4 ||
        d->scissor[0] > d->width || d->scissor[1] > d->height ||
        d->scissor[2] > d->width - d->scissor[0] ||
        d->scissor[3] > d->height - d->scissor[1] ||
        !d->scissor[2] || !d->scissor[3] || (d->color_mask & ~15u) ||
        d->fp->input_mask != d->vp->varying_mask) {
        return fail(error, error_size, "invalid or unsupported R300 draw resources");
    }
    char fragment[R300_FP_MSL_CAPACITY], vertex[R300_FP_MSL_CAPACITY];
    if (!r300_fp_compile_msl(d->fp, fragment, sizeof(fragment), error, error_size) ||
        !r300_vp_compile_msl(d->vp, vertex, sizeof(vertex), error, error_size)) {
        return false;
    }
    unsigned used_textures = 0;
    if (d->fp->control[0] & 8) {
        unsigned base = (d->fp->control[2] >> 13) & 31;
        unsigned start = (d->fp->node[3] >> 12) & 31;
        unsigned count = ((d->fp->node[3] >> 17) & 31) + 1;
        /* Compiler above has already checked the complete instruction range. */
        for (unsigned i = 0; i < count; i++) {
            used_textures |= 1u << ((d->fp->tex[base + start + i] >> 11) & 15);
        }
    }
    for (unsigned i = 0; i < 16; i++) {
        const R300MetalTexture *t = &d->textures[i];
        if ((used_textures & (1u << i)) &&
            (!t->rgba || !t->width || !t->height || t->width > 2048 || t->height > 2048 || t->max_anisotropy > 16 ||
             t->size < (uint64_t)t->width * t->height * 4)) {
            return fail(error, error_size, "missing or invalid texture resource");
        }
    }
    @autoreleasepool {
        [ctx->lock lock];
        @try {
            NSString *source = [[NSString stringWithUTF8String:fragment]
                               stringByAppendingString:[NSString stringWithUTF8String:vertex]];
            NSString *key = [NSString stringWithFormat:@"%u:%u:%@", d->color_mask, d->premultiplied_over, source];
            id<MTLRenderPipelineState> pipeline = [ctx->pipelines objectForKey:key];
            if (!pipeline) {
                NSError *compile_error = nil;
                MTLCompileOptions *options = [[[MTLCompileOptions alloc] init] autorelease];
                options.fastMathEnabled = NO;
                id<MTLLibrary> lib = [[ctx->device newLibraryWithSource:source options:options error:&compile_error] autorelease];
                if (!lib) return fail(error,error_size,compile_error.description.UTF8String ?: "Metal compilation failed");
                MTLRenderPipelineDescriptor *pd = [[[MTLRenderPipelineDescriptor alloc] init] autorelease];
                pd.vertexFunction = [[lib newFunctionWithName:@"r300_vertex"] autorelease];
                pd.fragmentFunction = [[lib newFunctionWithName:@"r300_fragment"] autorelease];
                pd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
                pd.colorAttachments[0].writeMask =
                    ((d->color_mask & 1) ? MTLColorWriteMaskRed : 0) |
                    ((d->color_mask & 2) ? MTLColorWriteMaskGreen : 0) |
                    ((d->color_mask & 4) ? MTLColorWriteMaskBlue : 0) |
                    ((d->color_mask & 8) ? MTLColorWriteMaskAlpha : 0);
                if (d->premultiplied_over) {
                    pd.colorAttachments[0].blendingEnabled = YES;
                    pd.colorAttachments[0].rgbBlendOperation = MTLBlendOperationAdd;
                    pd.colorAttachments[0].alphaBlendOperation = MTLBlendOperationAdd;
                    pd.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorOne;
                    pd.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
                    pd.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
                    pd.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
                }
                pipeline = [[ctx->device newRenderPipelineStateWithDescriptor:pd error:&compile_error] autorelease];
                if (!pipeline) return fail(error,error_size,compile_error.description.UTF8String ?: "Metal pipeline failed");
                if (ctx->pipelines.count >= 64) [ctx->pipelines removeAllObjects];
                [ctx->pipelines setObject:pipeline forKey:key];
            }
            id<MTLCommandBuffer> command = [ctx->queue commandBuffer];
            id<MTLBuffer> target_bytes = upload_buffer(ctx,d->target,d->width,d->height);
            id<MTLTexture> target = private_texture(ctx,d->width,d->height,MTLTextureUsageRenderTarget);
            id<MTLBuffer> vb = [[ctx->device newBufferWithBytes:d->vertices
                          length:d->vertex_count * sizeof(*d->vertices) options:MTLResourceStorageModeShared] autorelease];
            id<MTLBuffer> vc = [[ctx->device newBufferWithBytes:d->vertex_constants
                          length:256 * 4 * sizeof(float) options:MTLResourceStorageModeShared] autorelease];
            id<MTLBuffer> fc = [[ctx->device newBufferWithBytes:d->fragment_constants
                          length:32 * 4 * sizeof(float) options:MTLResourceStorageModeShared] autorelease];
            if (!command || !target_bytes || !target || !vb || !vc || !fc)
                return fail(error,error_size,"Metal resource allocation failed");
            id<MTLTexture> textures[16] = {nil};
            id<MTLSamplerState> samplers[16] = {nil};
            id<MTLBlitCommandEncoder> initial = [command blitCommandEncoder];
            if (!initial) return fail(error,error_size,"Metal upload encoder failed");
            upload(initial,target_bytes,target,d->width,d->height);
            for (unsigned i = 0; i < 16; i++) {
                if (!(used_textures & (1u << i))) continue;
                const R300MetalTexture *t = &d->textures[i];
                textures[i] = private_texture(ctx,t->width,t->height,MTLTextureUsageShaderRead);
                id<MTLBuffer> data = upload_buffer(ctx,t->rgba,t->width,t->height);
                MTLSamplerDescriptor *sd = [[[MTLSamplerDescriptor alloc] init] autorelease];
                sd.normalizedCoordinates = YES;
                sd.maxAnisotropy = t->max_anisotropy ?: 1;
                sd.sAddressMode = t->clamp_to_zero_s ?
                    MTLSamplerAddressModeClampToZero : MTLSamplerAddressModeClampToEdge;
                sd.tAddressMode = t->clamp_to_zero_t ?
                    MTLSamplerAddressModeClampToZero : MTLSamplerAddressModeClampToEdge;
                sd.minFilter = sd.magFilter = t->linear_filter ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
                samplers[i] = [[ctx->device newSamplerStateWithDescriptor:sd] autorelease];
                if (!textures[i] || !data || !samplers[i]) {
                    [initial endEncoding];
                    return fail(error,error_size,"Metal texture allocation failed");
                }
                upload(initial,data,textures[i],t->width,t->height);
            }
            [initial endEncoding];
            MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
            pass.colorAttachments[0].texture = target;
            pass.colorAttachments[0].loadAction = MTLLoadActionLoad;
            pass.colorAttachments[0].storeAction = MTLStoreActionStore;
            id<MTLRenderCommandEncoder> encoder = [command renderCommandEncoderWithDescriptor:pass];
            if (!encoder) return fail(error,error_size,"Metal render encoder failed");
            [encoder setRenderPipelineState:pipeline];
            [encoder setViewport:(MTLViewport){d->viewport_x,d->viewport_y,d->viewport_width ?: d->width,
                d->viewport_height ?: d->height,0,1}];
            [encoder setScissorRect:(MTLScissorRect){d->scissor[0],d->scissor[1],d->scissor[2],d->scissor[3]}];
            [encoder setCullMode:MTLCullModeNone];
            [encoder setVertexBuffer:vb offset:0 atIndex:0];
            [encoder setVertexBuffer:vc offset:0 atIndex:1];
            [encoder setFragmentBuffer:fc offset:0 atIndex:0];
            for (unsigned i = 0; i < 16; i++) {
                if (used_textures & (1u << i)) {
                    [encoder setFragmentTexture:textures[i] atIndex:i];
                    [encoder setFragmentSamplerState:samplers[i] atIndex:i];
                }
            }
            [encoder drawPrimitives:d->primitive_points ? MTLPrimitiveTypePoint : MTLPrimitiveTypeTriangle
                         vertexStart:0 vertexCount:d->vertex_count];
            [encoder endEncoding];
            id<MTLBlitCommandEncoder> readback = [command blitCommandEncoder];
            if (!readback) return fail(error,error_size,"Metal readback encoder failed");
            [readback copyFromTexture:target sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0)
                          sourceSize:MTLSizeMake(d->width,d->height,1) toBuffer:target_bytes destinationOffset:0
              destinationBytesPerRow:row_pitch(d->width) destinationBytesPerImage:row_pitch(d->width)*d->height];
            [readback endEncoding];
            [command commit];
            [command waitUntilCompleted];
            if (command.status != MTLCommandBufferStatusCompleted)
                return fail(error,error_size,command.error.description.UTF8String ?: "Metal command failed");
            for (unsigned y = 0; y < d->height; y++) {
                memcpy(d->target + (size_t)y * d->width * 4,
                       (uint8_t *)target_bytes.contents + y * row_pitch(d->width), (size_t)d->width * 4);
            }
            error[0] = 0;
            return true;
        } @finally {
            [ctx->lock unlock];
        }
    }
}
