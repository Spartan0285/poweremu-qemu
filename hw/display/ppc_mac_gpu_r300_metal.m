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
#include "ppc_mac_gpu_submit.h"
#include "ppc_mac_gpu_3d_regs.h"

/*
 * One R200/R300 blend factor as an MTLBlendFactor.  Kept in step with
 * r300_blend_factor_supported(): the device gate refuses anything that has
 * no mapping here, so a false return means the gate and this table have
 * drifted apart rather than that a guest asked for something exotic.
 */
static bool r300_blend_factor_metal(unsigned f, MTLBlendFactor *out)
{
    switch (f) {
    case R200_BLEND_GL_ZERO: *out = MTLBlendFactorZero; return true;
    case R200_BLEND_GL_ONE: *out = MTLBlendFactorOne; return true;
    case R200_BLEND_GL_SRC_COLOR: *out = MTLBlendFactorSourceColor; return true;
    case R200_BLEND_GL_ONE_MINUS_SRC_COLOR:
        *out = MTLBlendFactorOneMinusSourceColor; return true;
    case R200_BLEND_GL_DST_COLOR:
        *out = MTLBlendFactorDestinationColor; return true;
    case R200_BLEND_GL_ONE_MINUS_DST_COLOR:
        *out = MTLBlendFactorOneMinusDestinationColor; return true;
    case R200_BLEND_GL_SRC_ALPHA: *out = MTLBlendFactorSourceAlpha; return true;
    case R200_BLEND_GL_ONE_MINUS_SRC_ALPHA:
        *out = MTLBlendFactorOneMinusSourceAlpha; return true;
    case R200_BLEND_GL_DST_ALPHA:
        *out = MTLBlendFactorDestinationAlpha; return true;
    case R200_BLEND_GL_ONE_MINUS_DST_ALPHA:
        *out = MTLBlendFactorOneMinusDestinationAlpha; return true;
    default: return false;
    }
}

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

/*
 * Private depth textures, one per guest depth buffer.
 *
 * Ported from the R200 backend's g_r200_depth: the guest's buffer lives in
 * VRAM, Metal will not make a depth texture out of a buffer, and a depth
 * buffer is read back only by the hardware itself, so a private texture per
 * (offset, pitch) is enough.  `fresh` says the texture has never been
 * rendered to, and its first pass clears it instead of loading.
 */
#define R300_DEPTH_FORMAT MTLPixelFormatDepth32Float
#define R300_MAX_DEPTH 4
static struct {
    uint32_t offset, pitch;
    id<MTLTexture> tex;
    uint64_t used;
    bool fresh;
} g_r300_depth[R300_MAX_DEPTH];
static uint64_t g_r300_depth_clock;

static id<MTLTexture> r300_depth_tex(id<MTLDevice> dev, uint32_t offset,
                                     uint32_t pitch, bool *fresh)
{
    int lru = 0;

    if (!pitch) {
        return nil;
    }
    for (int i = 0; i < R300_MAX_DEPTH; i++) {
        if (g_r300_depth[i].tex && g_r300_depth[i].offset == offset &&
            g_r300_depth[i].pitch == pitch) {
            g_r300_depth[i].used = ++g_r300_depth_clock;
            *fresh = g_r300_depth[i].fresh;
            g_r300_depth[i].fresh = false;
            return g_r300_depth[i].tex;
        }
        if (g_r300_depth[i].used < g_r300_depth[lru].used) {
            lru = i;
        }
    }
    MTLTextureDescriptor *td = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:R300_DEPTH_FORMAT width:pitch
                                    height:2048 mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget;
    td.storageMode = MTLStorageModePrivate;
    id<MTLTexture> t = [dev newTextureWithDescriptor:td];
    if (!t) {
        return nil;
    }
    [g_r300_depth[lru].tex release];
    g_r300_depth[lru].offset = offset;
    g_r300_depth[lru].pitch = pitch;
    g_r300_depth[lru].tex = t;
    g_r300_depth[lru].used = ++g_r300_depth_clock;
    g_r300_depth[lru].fresh = false;
    *fresh = true;                      /* this pass clears it */
    fprintf(stderr, "ppc-mac-gpu r350: depth buffer at 0x%x, pitch %u\n", offset, pitch);
    return t;
}

/* The hardware's texture addressing modes.  Only the three the validation
 * lets through reach here; anything else is refused where it can still say
 * which mode it was. */
static MTLSamplerAddressMode r300_address_mode(unsigned mode)
{
    switch (mode) {
    case 0:  return MTLSamplerAddressModeRepeat;
    case 6:  return MTLSamplerAddressModeClampToZero;
    default: return MTLSamplerAddressModeClampToEdge;   /* 2 */
    }
}

/* The hardware's compare encoding, the same eight values the R200 path uses. */
static MTLCompareFunction r300_cmp(unsigned f)
{
    static const MTLCompareFunction map[8] = {
        MTLCompareFunctionNever, MTLCompareFunctionLess,
        MTLCompareFunctionLessEqual, MTLCompareFunctionEqual,
        MTLCompareFunctionGreaterEqual, MTLCompareFunctionGreater,
        MTLCompareFunctionNotEqual, MTLCompareFunctionAlways,
    };
    return map[f & 7];
}

#define R300_DSS_CACHE 16
static struct { uint32_t key; id<MTLDepthStencilState> st; } g_r300_dss[R300_DSS_CACHE];
static int g_r300_ndss;

static id<MTLDepthStencilState> r300_dss(id<MTLDevice> dev, const R300MetalDraw *d)
{
    /* Writing without testing is a real arrangement -- 80 of Chess's draws
     * ask for it -- and in Metal that is the ALWAYS compare. */
    unsigned func = d->depth_enable ? (d->depth_func & 7) : 7;
    uint32_t key = func | (d->depth_write ? 8u : 0u);

    for (int i = 0; i < g_r300_ndss; i++) {
        if (g_r300_dss[i].key == key) {
            return g_r300_dss[i].st;
        }
    }
    MTLDepthStencilDescriptor *dd = [[MTLDepthStencilDescriptor alloc] init];
    dd.depthCompareFunction = r300_cmp(func);
    dd.depthWriteEnabled = d->depth_write;
    id<MTLDepthStencilState> st = [dev newDepthStencilStateWithDescriptor:dd];
    [dd release];
    if (g_r300_ndss < R300_DSS_CACHE) {
        g_r300_dss[g_r300_ndss].key = key;
        g_r300_dss[g_r300_ndss++].st = st;
    }
    return st;
}

bool r300_metal_draw(void *renderer, const R300MetalDraw *d,
                     char *error, size_t error_size)
{
    R300MetalContext *ctx = renderer;
    if (!ctx || !d || !d->vp || !d->fp || !d->vertices ||
        !d->vertex_constants || !d->fragment_constants ||
        (!d->target && !d->guest_target) ||
        !d->vertex_count || d->vertex_count > 65536 ||
        (!d->primitive_points && d->vertex_count % 3) ||
        !d->width || !d->height || d->width > 4096 || d->height > 4096 ||
        !isfinite(d->viewport_x) || !isfinite(d->viewport_y) ||
        !isfinite(d->viewport_width) || !isfinite(d->viewport_height) ||
        fabs(d->viewport_x)>4096 || fabs(d->viewport_y)>4096 ||
        d->viewport_width<0 || d->viewport_height<0 ||
        d->viewport_width>4096 || d->viewport_height>4096 ||
        (!d->guest_target &&
         d->target_size < (uint64_t)d->width * d->height * 4) ||
        (d->guest_target &&
         (uint64_t)d->guest_pitch < (uint64_t)d->width * 4) ||
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
            (!t->width || !t->height || t->width > 2048 || t->height > 2048 ||
             t->max_anisotropy > 16 ||
             (t->guest_source ? (t->guest_bpp != 1 && t->guest_bpp != 4)
                              : (!t->rgba ||
                                 t->size < (uint64_t)t->width * t->height * 4)))) {
            return fail(error, error_size, "missing or invalid texture resource");
        }
    }
    @autoreleasepool {
        [ctx->lock lock];
        @try {
            NSString *source = [[NSString stringWithUTF8String:fragment]
                               stringByAppendingString:[NSString stringWithUTF8String:vertex]];
            NSString *key = [NSString stringWithFormat:@"%u:%u:%u:%u:%u:%u:%u:%@",
                             d->color_mask, (unsigned)d->blend_enable,
                             d->blend_src_rgb, d->blend_dst_rgb,
                             d->blend_src_alpha, d->blend_dst_alpha,
                             (unsigned)d->guest_target, source];
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
                pd.colorAttachments[0].pixelFormat = d->guest_target ?
                    MTLPixelFormatBGRA8Unorm : MTLPixelFormatRGBA8Unorm;
                /* Declared here and keyed below: a pipeline built without the
                 * attachment cannot be used in a pass that has one. */
                if (d->depth_enable || d->depth_write) {
                    pd.depthAttachmentPixelFormat = R300_DEPTH_FORMAT;
                }
                /*
                 * The mask names the guest's channels, and on a guest-order
                 * target the shader has already swizzled them: red leaves in
                 * Metal's green, green in red, blue in alpha, alpha in blue.
                 * Masking the unswizzled channels would preserve the wrong
                 * ones.
                 */
                pd.colorAttachments[0].writeMask = d->guest_target ?
                    (((d->color_mask & 1) ? MTLColorWriteMaskGreen : 0) |
                     ((d->color_mask & 2) ? MTLColorWriteMaskRed : 0) |
                     ((d->color_mask & 4) ? MTLColorWriteMaskAlpha : 0) |
                     ((d->color_mask & 8) ? MTLColorWriteMaskBlue : 0)) :
                    (((d->color_mask & 1) ? MTLColorWriteMaskRed : 0) |
                     ((d->color_mask & 2) ? MTLColorWriteMaskGreen : 0) |
                     ((d->color_mask & 4) ? MTLColorWriteMaskBlue : 0) |
                     ((d->color_mask & 8) ? MTLColorWriteMaskAlpha : 0));
                /*
                 * A guest-order target blends in the fragment shader
                 * instead: Metal's factors would operate on the swizzled
                 * channels.  See R300FPProgram::blend_in_shader.
                 */
                if (d->blend_enable && !d->guest_target) {
                    MTLBlendFactor srgb, drgb, salpha, dalpha;
                    if (!r300_blend_factor_metal(d->blend_src_rgb, &srgb) ||
                        !r300_blend_factor_metal(d->blend_dst_rgb, &drgb) ||
                        !r300_blend_factor_metal(d->blend_src_alpha, &salpha) ||
                        !r300_blend_factor_metal(d->blend_dst_alpha, &dalpha)) {
                        return fail(error, error_size, "unsupported blend factor");
                    }
                    pd.colorAttachments[0].blendingEnabled = YES;
                    pd.colorAttachments[0].rgbBlendOperation = MTLBlendOperationAdd;
                    pd.colorAttachments[0].alphaBlendOperation = MTLBlendOperationAdd;
                    pd.colorAttachments[0].sourceRGBBlendFactor = srgb;
                    pd.colorAttachments[0].destinationRGBBlendFactor = drgb;
                    pd.colorAttachments[0].sourceAlphaBlendFactor = salpha;
                    pd.colorAttachments[0].destinationAlphaBlendFactor = dalpha;
                }
                pipeline = [[ctx->device newRenderPipelineStateWithDescriptor:pd error:&compile_error] autorelease];
                if (!pipeline) return fail(error,error_size,compile_error.description.UTF8String ?: "Metal pipeline failed");
                if (ctx->pipelines.count >= 64) [ctx->pipelines removeAllObjects];
                [ctx->pipelines setObject:pipeline forKey:key];
            }
            /*
             * A guest target renders in place: the shared submission layer
             * supplies the command buffer, and the attachment is a view over
             * the guest's own VRAM.  Nothing is copied in either direction.
             */
            GPUSurfaceKey gkey = { d->guest_offset, d->width, d->height,
                                   d->guest_pitch, MTLPixelFormatBGRA8Unorm };
            uint64_t glo = d->guest_offset;
            uint64_t ghi = glo + (uint64_t)d->guest_pitch * d->height;
            id<MTLCommandBuffer> command;
            id<MTLBuffer> target_bytes = nil;
            id<MTLTexture> target;
            if (d->guest_target) {
                /* Reading or retargeting memory this batch already wrote
                 * through another view needs the GPU told about the order.
                 * A full written-range table can track no more, so the only
                 * safe thing left is to flush and start over -- the same
                 * contract the R200 backend honours. */
                /*
                 * The textures this draw samples have to be checked too, not
                 * just where it draws.  The compositor renders a surface and
                 * then samples it, so a texture written earlier in this same
                 * batch is read through a different view than it was written
                 * through, and Metal does not order those against each other.
                 * Checking only the target left the Dock composited from
                 * half-written texels -- every icon drawn twice, and
                 * differently on each run.  The R200 backend checks its
                 * textures for exactly this reason.
                 */
                bool tex_conflict = false;
                for (unsigned t = 0; t < 16 && !tex_conflict; t++) {
                    const R300MetalTexture *tx = &d->textures[t];
                    if (!(used_textures & (1u << t)) || !tx->guest_source) {
                        continue;
                    }
                    tex_conflict = gpu_batch_conflict(tx->guest_offset,
                        tx->guest_offset +
                        (uint64_t)tx->guest_pitch * tx->height, NULL);
                }
                if (gpu_written_full()) {
                    gpu_submit_flush();
                } else if (tex_conflict || gpu_batch_conflict(glo, ghi, &gkey)) {
                    gpu_submit_note_conflict();
                    if (gpu_submit_split_enabled()) {
                        gpu_submit_split();
                    } else {
                        gpu_submit_flush();
                    }
                }
                command = gpu_submit_buffer();
                target = gpu_view(gkey, MTLPixelFormatBGRA8Unorm, true);
                if (!target) {
                    return fail(error,error_size,
                                "render target is not a usable VRAM view");
                }
            } else {
                command = [ctx->queue commandBuffer];
                target_bytes = upload_buffer(ctx,d->target,d->width,d->height);
                target = private_texture(ctx,d->width,d->height,MTLTextureUsageRenderTarget);
            }
            id<MTLBuffer> vb = [[ctx->device newBufferWithBytes:d->vertices
                          length:d->vertex_count * sizeof(*d->vertices) options:MTLResourceStorageModeShared] autorelease];
            id<MTLBuffer> vc = [[ctx->device newBufferWithBytes:d->vertex_constants
                          length:256 * 4 * sizeof(float) options:MTLResourceStorageModeShared] autorelease];
            id<MTLBuffer> fc = [[ctx->device newBufferWithBytes:d->fragment_constants
                          length:32 * 4 * sizeof(float) options:MTLResourceStorageModeShared] autorelease];
            if (!command || !target || !vb || !vc || !fc ||
                (!d->guest_target && !target_bytes))
                return fail(error,error_size,"Metal resource allocation failed");
            id<MTLTexture> textures[16] = {nil};
            id<MTLSamplerState> samplers[16] = {nil};
            id<MTLBlitCommandEncoder> initial = [command blitCommandEncoder];
            if (!initial) return fail(error,error_size,"Metal upload encoder failed");
            if (!d->guest_target) {
                upload(initial,target_bytes,target,d->width,d->height);
            }
            for (unsigned i = 0; i < 16; i++) {
                if (!(used_textures & (1u << i))) continue;
                const R300MetalTexture *t = &d->textures[i];
                id<MTLBuffer> data = nil;
                if (t->guest_source) {
                    /* In place: the texels stay where the guest put them. */
                    MTLPixelFormat tf = t->guest_bpp == 1 ?
                        MTLPixelFormatR8Unorm : MTLPixelFormatBGRA8Unorm;
                    GPUSurfaceKey tk = { t->guest_offset, t->width, t->height,
                                         t->guest_pitch, (uint32_t)tf };
                    textures[i] = gpu_view(tk, tf, false);
                    gpu_note_read(t->guest_offset,
                                  t->guest_offset +
                                  (uint64_t)t->guest_pitch * t->height);
                } else {
                    textures[i] = private_texture(ctx,t->width,t->height,MTLTextureUsageShaderRead);
                    data = upload_buffer(ctx,t->rgba,t->width,t->height);
                }
                MTLSamplerDescriptor *sd = [[[MTLSamplerDescriptor alloc] init] autorelease];
                sd.normalizedCoordinates = YES;
                sd.maxAnisotropy = t->max_anisotropy ?: 1;
                sd.sAddressMode = r300_address_mode(t->address_s);
                sd.tAddressMode = r300_address_mode(t->address_t);
                sd.minFilter = sd.magFilter = t->linear_filter ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
                samplers[i] = [[ctx->device newSamplerStateWithDescriptor:sd] autorelease];
                if (!textures[i] || !samplers[i] ||
                    (!t->guest_source && !data)) {
                    [initial endEncoding];
                    return fail(error,error_size,"Metal texture allocation failed");
                }
                if (!t->guest_source) {
                    upload(initial,data,textures[i],t->width,t->height);
                }
            }
            [initial endEncoding];
            /*
             * The depth buffer.
             *
             * Metal cannot alias a depth texture onto the VRAM buffer, so
             * each guest depth buffer gets a private one, exactly as the R200
             * backend does.  A buffer seen for the first time is cleared
             * rather than loaded -- there is nothing in our copy yet, and the
             * guest's own clear is a draw with the compare set to ALWAYS,
             * which lands on the cleared texture and agrees with it.
             */
            id<MTLTexture> depth_tex = nil;
            bool depth_fresh = false;
            if (d->depth_enable || d->depth_write) {
                depth_tex = r300_depth_tex(ctx->device, d->depth_offset,
                                           d->depth_pitch, &depth_fresh);
                if (!depth_tex) {
                    return fail(error,error_size,"depth buffer unavailable");
                }
            }
            MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
            if (depth_tex) {
                pass.depthAttachment.texture = depth_tex;
                pass.depthAttachment.loadAction = depth_fresh ?
                    MTLLoadActionClear : MTLLoadActionLoad;
                pass.depthAttachment.clearDepth = 1.0;
                pass.depthAttachment.storeAction = MTLStoreActionStore;
            }
            pass.colorAttachments[0].texture = target;
            pass.colorAttachments[0].loadAction = MTLLoadActionLoad;
            pass.colorAttachments[0].storeAction = MTLStoreActionStore;
            id<MTLRenderCommandEncoder> encoder = [command renderCommandEncoderWithDescriptor:pass];
            if (!encoder) return fail(error,error_size,"Metal render encoder failed");
            [encoder setRenderPipelineState:pipeline];
            if (depth_tex) {
                [encoder setDepthStencilState:r300_dss(ctx->device, d)];
            }
            [encoder setViewport:(MTLViewport){d->viewport_x,d->viewport_y,d->viewport_width ?: d->width,
                d->viewport_height ?: d->height,0,1}];
            [encoder setScissorRect:(MTLScissorRect){d->scissor[0],d->scissor[1],d->scissor[2],d->scissor[3]}];
            [encoder setCullMode:d->cull_front ? MTLCullModeFront :
                                 d->cull_back ? MTLCullModeBack : MTLCullModeNone];
            [encoder setFrontFacingWinding:d->front_is_cw ?
                MTLWindingClockwise : MTLWindingCounterClockwise];
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
            if (d->guest_target) {
                /*
                 * The pixels are already where the guest keeps them, so there
                 * is nothing to read back and nothing to wait for.  Record the
                 * range so a later access through a different view, or by the
                 * guest CPU, is ordered against this work.
                 */
                gpu_note_written(glo, ghi, &gkey);
                /*
                 * Deliberately not committed here.  Committing every draw set
                 * the open command buffer to nil, so the conflict check at the
                 * top of the *next* draw found nothing open and
                 * gpu_submit_split() returned immediately -- which meant the
                 * shared event was never created, never signalled and never
                 * waited on, and consecutive draws to the same surface had no
                 * ordering primitive between them at all.  The counter said
                 * conflicts were being handled while nothing was done about
                 * them.  The R200 path is deterministic precisely because it
                 * keeps a batch open and lets the split do its job; this now
                 * does the same.  The batch is committed by a guest fence, by
                 * a flush when the CPU or the 2D engine touches a busy range,
                 * or by the split itself.
                 */
                error[0] = 0;
                return true;
            }
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
