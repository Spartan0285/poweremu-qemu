/* SPDX-License-Identifier: GPL-2.0-or-later
 * Synchronous R300 correctness backend. Manual reference counting matches
 * QEMU's Objective-C build; the standalone tests compile this file likewise.
 */
#import <Foundation/Foundation.h>
#include <math.h>
#import <Metal/Metal.h>
#import <dispatch/dispatch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
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

/* Keep the exact shader and attachment/blend state in the cache key without
 * formatting/copying the entire MSL program on every draw under the BQL. */
@interface R300PipelineKey : NSObject <NSCopying> {
    NSString *shader;
    uint32_t state[8];
    NSUInteger cachedHash;
}
- (instancetype)initWithShader:(NSString *)source state:(const uint32_t *)values;
@end

@implementation R300PipelineKey
- (instancetype)initWithShader:(NSString *)source state:(const uint32_t *)values
{
    if ((self = [super init])) {
        shader = [source retain];
        memcpy(state, values, sizeof(state));
        cachedHash = [source hash];
        for (unsigned i = 0; i < 8; i++) {
            cachedHash = (cachedHash * 16777619u) ^ state[i];
        }
    }
    return self;
}
- (NSUInteger)hash { return cachedHash; }
- (BOOL)isEqual:(id)other
{
    if (self == other) return YES;
    if (![other isKindOfClass:[R300PipelineKey class]]) return NO;
    R300PipelineKey *key = other;
    return !memcmp(state, key->state, sizeof(state)) &&
           (shader == key->shader || [shader isEqualToString:key->shader]);
}
- (id)copyWithZone:(NSZone *)zone { return [self retain]; }
- (void)dealloc
{
    [shader release];
    [super dealloc];
}
@end

/* Cache entries are owned by the dictionary. Links are non-owning and only
 * used while the context lock serializes lookup, promotion, and eviction. */
@interface R300LRUEntry : NSObject {
@public
    id key;
    id object;
    R300LRUEntry *previous, *next;
}
- (instancetype)initWithObject:(id)value key:(id<NSCopying>)cacheKey;
@end

@implementation R300LRUEntry
- (instancetype)initWithObject:(id)value key:(id<NSCopying>)cacheKey
{
    if ((self = [super init])) {
        key = [(id)cacheKey copy];
        object = [value retain];
    }
    return self;
}
- (void)dealloc
{
    [key release];
    [object release];
    [super dealloc];
}
@end

/* Bounded LRU with constant-time promotion. An ordered set shifts/hashes
 * keys again on each hit, which is costly for exact shader-byte keys under
 * the BQL. The dictionary still checks exact key equality; only bookkeeping
 * changes. All access remains serialized by the context lock. */
@interface R300LRUCache : NSObject {
    NSMutableDictionary *objects;
    R300LRUEntry *oldest, *newest;
    NSUInteger capacity;
}
- (instancetype)initWithCapacity:(NSUInteger)limit;
- (id)objectForKey:(id)key;
- (void)setObject:(id)object forKey:(id<NSCopying>)key;
- (NSUInteger)count;
@end

@implementation R300LRUCache
- (instancetype)initWithCapacity:(NSUInteger)limit
{
    if ((self = [super init])) {
        objects = [NSMutableDictionary new];
        capacity = MAX(limit, 1u);
    }
    return self;
}
- (void)detach:(R300LRUEntry *)entry
{
    if (entry->previous) entry->previous->next = entry->next;
    else oldest = entry->next;
    if (entry->next) entry->next->previous = entry->previous;
    else newest = entry->previous;
    entry->previous = entry->next = nil;
}
- (void)appendNewest:(R300LRUEntry *)entry
{
    entry->previous = newest;
    entry->next = nil;
    if (newest) newest->next = entry;
    else oldest = entry;
    newest = entry;
}
- (id)objectForKey:(id)key
{
    R300LRUEntry *entry = [objects objectForKey:key];
    if (!entry) return nil;
    if (entry != newest) {
        [self detach:entry];
        [self appendNewest:entry];
    }
    return entry->object;
}
- (void)setObject:(id)object forKey:(id<NSCopying>)key
{
    R300LRUEntry *entry = [objects objectForKey:(id)key];
    if (entry) {
        [object retain];
        [entry->object release];
        entry->object = object;
        if (entry != newest) {
            [self detach:entry];
            [self appendNewest:entry];
        }
        return;
    }
    if (objects.count >= capacity) {
        R300LRUEntry *evicted = oldest;
        [self detach:evicted];
        [objects removeObjectForKey:evicted->key];
    }
    entry = [[R300LRUEntry alloc] initWithObject:object key:key];
    [objects setObject:entry forKey:entry->key];
    [self appendNewest:entry];
    [entry release];
}
- (NSUInteger)count { return objects.count; }
- (void)dealloc
{
    [objects release];
    [super dealloc];
}
@end

typedef struct R300MetalContext {
    id<MTLDevice> device;
    id<MTLCommandQueue> queue;
    R300LRUCache *pipelines;
    NSMutableDictionary *samplers;
    R300LRUCache *shader_sources;
    R300LRUCache *libraries;
    NSRecursiveLock *lock;
    NSString *warm_path;
    NSMutableOrderedSet *warm_sources;
    dispatch_queue_t warm_writer;
} R300MetalContext;

/* Experimental source-only warm cache. Recompile on this host before the
 * guest runs; exact source equality still selects every live library.
 * No serialized GPU object, guest state, or changed draw ordering. */
static void r300_warm_load(R300MetalContext *ctx)
{
    const char *path = getenv("POWEREMU_R350_SHADER_CACHE");
    if (!path || !*path) return;
    ctx->warm_path = [[NSString stringWithUTF8String:path] copy];
    if (!ctx->warm_path) return;
    ctx->warm_sources = [NSMutableOrderedSet new];
    ctx->warm_writer = dispatch_queue_create("poweremu.r350.source-cache", NULL);
    NSDictionary *attrs = [[NSFileManager defaultManager]
        attributesOfItemAtPath:ctx->warm_path error:nil];
    if ([attrs fileSize] > 32 * 1024 * 1024) return;
    NSData *data = [NSData dataWithContentsOfFile:ctx->warm_path];
    id sources = data ? [NSPropertyListSerialization propertyListWithData:data
        options:NSPropertyListImmutable format:NULL error:nil] : nil;
    if (![sources isKindOfClass:[NSArray class]] || [sources count] > 128) return;
    MTLCompileOptions *options = [[[MTLCompileOptions alloc] init] autorelease];
    options.fastMathEnabled = NO;
    unsigned loaded = 0;
    for (id source in sources) {
        if (![source isKindOfClass:[NSString class]] ||
            [source lengthOfBytesUsingEncoding:NSUTF8StringEncoding] > 131072) continue;
        id<MTLLibrary> lib = [ctx->device newLibraryWithSource:source
            options:options error:nil];
        if (lib) {
            [ctx->libraries setObject:lib forKey:source];
            [ctx->warm_sources addObject:source];
            loaded++;
        }
        [lib release];
    }
    fprintf(stderr, "R350_SHADER_WARM loaded=%u before-guest-boot\n", loaded);
}

static void r300_warm_record(R300MetalContext *ctx, NSString *source)
{
    if (!ctx->warm_writer || [ctx->warm_sources containsObject:source]) return;
    if (ctx->warm_sources.count >= 128) [ctx->warm_sources removeObjectAtIndex:0];
    [ctx->warm_sources addObject:source];
    NSArray *snapshot = [ctx->warm_sources.array copy];
    NSString *path = ctx->warm_path;
    /* Serialization and disk writes run outside MMIO/BQL. Dispatch copies
     * the block and retains these Objective-C values until it completes. */
    dispatch_async(ctx->warm_writer, ^{
        @autoreleasepool {
            NSData *data = [NSPropertyListSerialization dataWithPropertyList:snapshot
                format:NSPropertyListBinaryFormat_v1_0 options:0 error:nil];
            [data writeToFile:path options:NSDataWritingAtomic error:nil];
        }
    });
    [snapshot release];
}

/* One shared submission owner at a time, as enforced by buffer_for().
 * Consecutive compatible draws keep tile color/depth in one Metal pass. */
static id<MTLRenderCommandEncoder> g_r300_encoder;
static id<MTLCommandBuffer> g_r300_command;
static id<MTLTexture> g_r300_color, g_r300_depth_attachment, g_r300_mirror;
static R300MetalContext *g_r300_owner;

static void r300_end_pass(void)
{
    if (g_r300_encoder) {
        [g_r300_encoder endEncoding];
        [g_r300_encoder release];
        g_r300_encoder = nil;
    }
    g_r300_command = nil;
    g_r300_color = g_r300_depth_attachment = g_r300_mirror = nil;
    g_r300_owner = NULL;
}

void *r300_metal_create(void)
{
    R300MetalContext *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) return NULL;
    ctx->device = MTLCreateSystemDefaultDevice();
    ctx->queue = [ctx->device newCommandQueue];
    ctx->pipelines = [[R300LRUCache alloc] initWithCapacity:256];
    ctx->samplers = [NSMutableDictionary new];
    ctx->shader_sources = [[R300LRUCache alloc] initWithCapacity:128];
    ctx->libraries = [[R300LRUCache alloc] initWithCapacity:128];
    ctx->lock = [NSRecursiveLock new];
    if (!ctx->device || !ctx->queue || !ctx->pipelines || !ctx->samplers || !ctx->shader_sources || !ctx->libraries || !ctx->lock) {
        r300_metal_destroy(ctx);
        return NULL;
    }
    r300_warm_load(ctx);
    return ctx;
}

void r300_metal_destroy(void *renderer)
{
    R300MetalContext *ctx = renderer;
    if (!ctx) return;
    if (g_r300_owner == ctx) gpu_submit_flush();
    if (ctx->warm_writer) {
        dispatch_sync(ctx->warm_writer, ^{});
        dispatch_release(ctx->warm_writer);
    }
    [ctx->warm_sources release];
    [ctx->warm_path release];
    [ctx->queue release];
    [ctx->device release];
    [ctx->pipelines release];
    [ctx->samplers release];
    [ctx->shader_sources release];
    [ctx->libraries release];
    [ctx->lock release];
    free(ctx);
}

static bool fail(char *error, size_t capacity, const char *message)
{
    snprintf(error, capacity, "%s", message);
    return false;
}

/* Shared submissions retain arena pages until the command completes. Each
 * draw gets its own immutable slice; later constants cannot overwrite it. */
static id<MTLBuffer> stage_draw_data(R300MetalContext *ctx, const void *bytes,
                                    size_t length, bool shared, size_t *offset)
{
    *offset = 0;
    if (shared) {
        id<MTLBuffer> buffer;
        void *dst = gpu_stage_alloc(length, &buffer, offset);
        if (dst) {
            memcpy(dst, bytes, length);
            return buffer;
        }
    }
    *offset = 0;
    return [[ctx->device newBufferWithBytes:bytes length:length
                             options:MTLResourceStorageModeShared] autorelease];
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
 * Metal cannot view a VRAM buffer as a depth attachment. Native testing uses
 * a private texture; guest CPU depth reads need a separate export with the
 * guest's byte order and tile/sample layout. `fresh` says the texture has
 * never been rendered to, and its first pass clears it instead of loading.
 */
/* One format for both native attachments: a depth
 * buffer and a stencil buffer are the same guest allocation. */
#define R300_DEPTH_FORMAT MTLPixelFormatDepth32Float_Stencil8
#define R300_MAX_DEPTH 4
static struct {
    uint32_t offset, pitch;
    id<MTLTexture> tex;
    uint64_t used;
    bool fresh;
    unsigned height, format;
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
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
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
    g_r300_depth[lru].height = 0;
    g_r300_depth[lru].format = 0;
    *fresh = true;                      /* this pass clears it */
    fprintf(stderr, "ppc-mac-gpu r350: depth buffer at 0x%x, pitch %u\n", offset, pitch);
    return t;
}

/* R350 two-sample Z uses 4x8 pixel blocks, with sample one at byte bit4.
 * Export the current attachment only, in the command that rendered it. This
 * cannot overwrite a cached allocation merely because a later fence fires.
 * Native rendering is still single-sample; both guest samples get its depth.
 * Guest stencil bytes are preserved; native stencil readback is separate. */
static bool export_tiled_depth(R300MetalContext *ctx, id<MTLCommandBuffer> command,
                               id<MTLTexture> depth, const R300MetalDraw *d,
                               char *error, size_t error_size)
{
    uint64_t size = (uint64_t)d->depth_pitch * ((d->height + 7u) & ~7u) * 8;
    if (!depth || !d->depth_guest_vram ||
        gpu_submit_vram().contents != d->depth_guest_vram ||
        d->depth_pitch < d->width || d->depth_pitch % 4 ||
        d->depth_guest_offset % 4 || d->depth_format != 2 ||
        (uint64_t)d->depth_guest_offset + size > d->depth_guest_size) {
        return fail(error, error_size, "tiled depth export bounds or owner");
    }
    NSString *key = @"r350-z24-two-sample-export";
    id<MTLComputePipelineState> pipeline = [ctx->pipelines objectForKey:key];
    if (!pipeline) {
        NSString *source = @"#include <metal_stdlib>\nusing namespace metal;\n"
          "kernel void zexport(texture2d<float, access::read> z [[texture(0)]], "
          "device uint *v [[buffer(0)]], constant uint4 &p [[buffer(1)]], "
          "uint2 xy [[thread_position_in_grid]]) {\n"
          " if (xy.x >= p.z || xy.y >= p.w) return;\n"
          " uint x=xy.x, y=xy.y;\n"
          " uint block=((x>>2)*2+((y>>2)&1))*4+(p.y>>2)*8*(y>>3);\n"
          " uint fine=(((y>>1)&1)<<4)|(((x>>1)&1)<<3)|((y&1)<<1)|(x&1);\n"
          " uint addr=p.x+block*32+(fine<<2);\n"
          " uint depth=uint(rint(clamp(z.read(xy).x,0.0f,1.0f)*16777215.0f));\n"
          " uint packed=((depth&255)<<16)|(depth&65280)|(depth>>16);\n"
          " v[addr>>2]=(v[addr>>2]&0xff000000u)|packed;\n"
          " v[(addr+16)>>2]=(v[(addr+16)>>2]&0xff000000u)|packed;\n}\n";
        NSError *e = nil;
        id<MTLLibrary> lib = [ctx->device newLibraryWithSource:source options:nil error:&e];
        id<MTLFunction> fn = [lib newFunctionWithName:@"zexport"];
        pipeline = [ctx->device newComputePipelineStateWithFunction:fn error:&e];
        [fn release]; [lib release];
        if (!pipeline) return fail(error, error_size,
                                  e.description.UTF8String ?: "depth export shader failed");
        [ctx->pipelines setObject:pipeline forKey:key];
        [pipeline release];
    }
    if (!d->guest_target) gpu_submit_flush();
    uint32_t args[4] = {d->depth_guest_offset, d->depth_pitch, d->width, d->height};
    id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
    if (!encoder) return fail(error,error_size,"depth export encoder failed");
    [encoder setComputePipelineState:pipeline];
    [encoder setTexture:depth atIndex:0];
    [encoder setBuffer:gpu_submit_vram() offset:0 atIndex:0];
    [encoder setBytes:args length:sizeof(args) atIndex:1];
    [encoder dispatchThreads:MTLSizeMake(d->width,d->height,1)
        threadsPerThreadgroup:MTLSizeMake(8,8,1)];
    [encoder endEncoding];
    if (d->guest_target) {
        /* This compute write is a buffer range, not a linear texture view.
         * The submission tracker requires a non-null identity; Invalid keeps
         * this tile layout distinct from every actual texture attachment. */
        GPUSurfaceKey key = {d->depth_guest_offset, d->depth_pitch,
                              (d->height + 7u) & ~7u, d->depth_pitch * 8u,
                              MTLPixelFormatInvalid};
        gpu_note_written(d->depth_guest_offset, d->depth_guest_offset + size,
                         &key);
    }
    return true;
}

bool r300_metal_clear_depth(void *renderer, uint32_t offset, uint32_t pitch,
                            double depth, unsigned stencil, bool clear_stencil,
                            char *error, size_t error_size)
{
    R300MetalContext *ctx = renderer;
    if (!ctx || !pitch || pitch > 4096 || !isfinite(depth) ||
        depth < 0.0 || depth > 1.0 || stencil > 255) {
        return fail(error, error_size, "invalid depth fast clear");
    }
    @autoreleasepool {
        [ctx->lock lock];
        @try {
            /* Direct VRAM draws can still be queued on the shared submit
             * queue. Finish them before clearing the same private depth
             * attachment through this context's synchronous queue. */
            gpu_submit_flush();
            bool fresh;
            id<MTLTexture> texture = r300_depth_tex(ctx->device, offset,
                                                    pitch, &fresh);
            if (!texture) {
                return fail(error, error_size, "depth clear texture unavailable");
            }
            id<MTLCommandBuffer> command = [ctx->queue commandBuffer];
            MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
            pass.depthAttachment.texture = texture;
            pass.depthAttachment.loadAction = MTLLoadActionClear;
            pass.depthAttachment.clearDepth = depth;
            pass.depthAttachment.storeAction = MTLStoreActionStore;
            pass.stencilAttachment.texture = texture;
            pass.stencilAttachment.loadAction = (clear_stencil || fresh) ?
                MTLLoadActionClear : MTLLoadActionLoad;
            pass.stencilAttachment.clearStencil = stencil;
            pass.stencilAttachment.storeAction = MTLStoreActionStore;
            id<MTLRenderCommandEncoder> encoder = [command renderCommandEncoderWithDescriptor:pass];
            if (!encoder) {
                return fail(error, error_size, "depth clear encoder unavailable");
            }
            [encoder endEncoding];
            [command commit];
            [command waitUntilCompleted];
            if (command.status != MTLCommandBufferStatusCompleted) {
                return fail(error, error_size, "depth clear GPU failure");
            }
            error[0] = 0;
            return true;
        } @finally {
            [ctx->lock unlock];
        }
    }
}

bool r300_metal_read_depth(void *renderer, uint32_t fb_base, uint8_t *vram,
                           uint64_t vram_size, uint64_t lo, uint64_t hi,
                           char *error, size_t error_size)
{
    R300MetalContext *ctx = renderer;
    if (!ctx || !vram || hi <= lo || hi > vram_size) {
        return fail(error, error_size, "invalid depth readback range");
    }
    @autoreleasepool {
        [ctx->lock lock];
        @try {
            int found = -1;
            for (unsigned i = 0; i < R300_MAX_DEPTH; i++) {
                if (!g_r300_depth[i].tex || !g_r300_depth[i].height ||
                    g_r300_depth[i].offset < fb_base) {
                    continue;
                }
                uint64_t off = g_r300_depth[i].offset - fb_base;
                unsigned bpp = g_r300_depth[i].format == 0 ? 2 : 4;
                uint64_t end = off + (uint64_t)g_r300_depth[i].pitch *
                                     g_r300_depth[i].height * bpp;
                if (lo < end && hi > off && end <= vram_size &&
                    (found < 0 || g_r300_depth[i].used > g_r300_depth[found].used)) {
                    found = i;
                }
            }
            if (found < 0) { error[0] = 0; return true; }
            gpu_submit_flush();
            unsigned width = g_r300_depth[found].pitch;
            unsigned bpp = g_r300_depth[found].format == 0 ? 2 : 4;
            uint64_t off = g_r300_depth[found].offset - fb_base;
            uint64_t stride = (uint64_t)width * bpp;
            unsigned y0 = (MAX(lo, off) - off) / stride;
            unsigned y1 = MIN((hi - off + stride - 1) / stride,
                              g_r300_depth[found].height);
            unsigned rows = y1 - y0;
            if (getenv("POWEREMU_R350_DEPTH_READBACK")) {
                fprintf(stderr, "R350_ZREAD off=%08x pitch=%u rows=%u+%u format=%u\n",
                        g_r300_depth[found].offset, width, y0, rows,
                        g_r300_depth[found].format);
            }
            NSUInteger dp = (width * 4 + 255) & ~255u;
            NSUInteger sp = (width + 255) & ~255u;
            id<MTLBuffer> depth = [ctx->device newBufferWithLength:dp * rows
                                  options:MTLResourceStorageModeShared];
            id<MTLBuffer> stencil = bpp == 4 ?
                [ctx->device newBufferWithLength:sp * rows options:MTLResourceStorageModeShared] : nil;
            if (!depth || (bpp == 4 && !stencil)) {
                [depth release]; [stencil release];
                return fail(error, error_size, "depth readback allocation failed");
            }
            id<MTLCommandBuffer> command = [ctx->queue commandBuffer];
            id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
            [blit copyFromTexture:g_r300_depth[found].tex sourceSlice:0 sourceLevel:0
                sourceOrigin:MTLOriginMake(0,y0,0) sourceSize:MTLSizeMake(width,rows,1)
                toBuffer:depth destinationOffset:0 destinationBytesPerRow:dp
                destinationBytesPerImage:dp * rows options:MTLBlitOptionDepthFromDepthStencil];
            if (stencil) {
                [blit copyFromTexture:g_r300_depth[found].tex sourceSlice:0 sourceLevel:0
                    sourceOrigin:MTLOriginMake(0,y0,0) sourceSize:MTLSizeMake(width,rows,1)
                    toBuffer:stencil destinationOffset:0 destinationBytesPerRow:sp
                    destinationBytesPerImage:sp * rows options:MTLBlitOptionStencilFromDepthStencil];
            }
            [blit endEncoding]; [command commit]; [command waitUntilCompleted];
            if (command.status != MTLCommandBufferStatusCompleted) {
                [depth release]; [stencil release];
                return fail(error, error_size, "depth readback GPU failure");
            }
            for (unsigned y = 0; y < rows; y++) {
                const float *z = (const float *)((uint8_t *)depth.contents + y * dp);
                const uint8_t *st = stencil ? (uint8_t *)stencil.contents + y * sp : NULL;
                uint8_t *dst = vram + off + (uint64_t)(y0 + y) * stride;
                for (unsigned x = 0; x < width; x++) {
                    double f = fmin(1.0, fmax(0.0, z[x]));
                    uint32_t packed = (uint32_t)llround(f * (bpp == 2 ? 65535 : 16777215));
                    if (bpp == 4) { packed = (packed << 8) | st[x]; }
                    for (unsigned b = 0; b < bpp; b++) {
                        dst[x * bpp + b] = packed >> (8 * (bpp - b - 1));
                    }
                }
            }
            [depth release]; [stencil release];
            error[0] = 0; return true;
        } @finally { [ctx->lock unlock]; }
    }
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

/* Stencil operations, in the hardware's order. */
static MTLStencilOperation r300_sop(unsigned f)
{
    static const MTLStencilOperation map[8] = {
        MTLStencilOperationKeep, MTLStencilOperationZero,
        MTLStencilOperationReplace, MTLStencilOperationIncrementClamp,
        MTLStencilOperationDecrementClamp, MTLStencilOperationInvert,
        MTLStencilOperationIncrementWrap, MTLStencilOperationDecrementWrap,
    };
    return map[f & 7];
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
static struct { uint64_t key; id<MTLDepthStencilState> st; } g_r300_dss[R300_DSS_CACHE];
static int g_r300_ndss;

static id<MTLDepthStencilState> r300_dss(id<MTLDevice> dev, const R300MetalDraw *d)
{
    /* Writing without testing is a real arrangement -- 80 of Chess's draws
     * ask for it -- and in Metal that is the ALWAYS compare. */
    unsigned func = d->depth_enable ? (d->depth_func & 7) : 7;
    uint64_t key = func | (d->depth_write ? 8u : 0u);

    if (d->stencil_enable && !getenv("POWEREMU_R350_NOSTENCIL")) {
        key |= 1ull << 4;
        for (int f = 0; f < 2; f++) {
            key |= (uint64_t)(d->stencil_func[f] & 7)  << (5 + f * 12);
            key |= (uint64_t)(d->stencil_fail[f] & 7)  << (8 + f * 12);
            key |= (uint64_t)(d->stencil_zpass[f] & 7) << (11 + f * 12);
            key |= (uint64_t)(d->stencil_zfail[f] & 7) << (14 + f * 12);
        }
        key |= (uint64_t)(d->stencil_read_mask & 0xff) << 32;
        key |= (uint64_t)(d->stencil_write_mask & 0xff) << 40;
    }
    for (int i = 0; i < g_r300_ndss; i++) {
        if (g_r300_dss[i].key == key) {
            return g_r300_dss[i].st;
        }
    }
    MTLDepthStencilDescriptor *dd = [[MTLDepthStencilDescriptor alloc] init];
    dd.depthCompareFunction = r300_cmp(func);
    dd.depthWriteEnabled = d->depth_write;
    if (d->stencil_enable) {
        MTLStencilDescriptor *front = [[MTLStencilDescriptor alloc] init];
        MTLStencilDescriptor *back = [[MTLStencilDescriptor alloc] init];
        MTLStencilDescriptor *side[2] = { front, back };
        for (int f = 0; f < 2; f++) {
            side[f].stencilCompareFunction = r300_cmp(d->stencil_func[f]);
            side[f].stencilFailureOperation = r300_sop(d->stencil_fail[f]);
            side[f].depthStencilPassOperation = r300_sop(d->stencil_zpass[f]);
            side[f].depthFailureOperation = r300_sop(d->stencil_zfail[f]);
            side[f].readMask = d->stencil_read_mask;
            side[f].writeMask = d->stencil_write_mask;
        }
        dd.frontFaceStencil = front;
        dd.backFaceStencil = back;
        [front release];
        [back release];
    }
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
        (d->fp->fog_enable &&
         (!isfinite(d->fp->fog_scale) || !isfinite(d->fp->fog_offset) ||
          !isfinite(d->fp->fog_colour[0]) || !isfinite(d->fp->fog_colour[1]) ||
          !isfinite(d->fp->fog_colour[2]))) ||
        d->fp->input_mask != d->vp->varying_mask) {
        return fail(error, error_size, "invalid or unsupported R300 draw resources");
    }
    /* Validate every active node before the shared texture walk, including
     * cache hits that do not compile the shader again. */
    unsigned first_node = 3 - (d->fp->control[0] & 3);
    unsigned tex_base = (d->fp->control[2] >> 13) & 31;
    for (unsigned node = first_node; node < 4; node++) {
        if (node == first_node && !(d->fp->control[0] & 8)) continue;
        unsigned start = (d->fp->node[node] >> 12) & 31;
        unsigned count = ((d->fp->node[node] >> 17) & 31) + 1;
        if (tex_base + start + count > R300_FP_MAX_TEX)
            return fail(error, error_size, "texture instruction range");
    }
    unsigned used_textures = r300_fp_texture_mask(d->fp);
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
            /* Fog values are per-draw uniforms; routing/enables remain part
             * of the exact shader key. Changing lighting must not compile
             * another Metal pipeline or evict unrelated programs. */
            R300FPProgram shader_fp = *d->fp;
            memset(shader_fp.fog_colour, 0, sizeof(shader_fp.fog_colour));
            shader_fp.fog_scale = shader_fp.fog_offset = 0;
            NSMutableData *shader_key = [NSMutableData dataWithBytes:&shader_fp
                                                              length:sizeof(shader_fp)];
            [shader_key appendBytes:d->vp length:sizeof(*d->vp)];
            NSString *source = [ctx->shader_sources objectForKey:shader_key];
            if (!source) {
                char fragment[R300_FP_MSL_CAPACITY], vertex[R300_FP_MSL_CAPACITY];
                if (!r300_fp_compile_msl(d->fp, fragment, sizeof(fragment), error, error_size) ||
                    !r300_vp_compile_msl(d->vp, vertex, sizeof(vertex), error, error_size)) {
                    return false;
                }
                source = [[NSString stringWithUTF8String:fragment]
                          stringByAppendingString:[NSString stringWithUTF8String:vertex]];
                [ctx->shader_sources setObject:source forKey:shader_key];
            }
            /*
             * The attachment set belongs in the key.  A pipeline is built
             * against the formats of the pass it will be used in, so one
             * compiled without a depth attachment cannot encode in a pass
             * that has one -- and with the same shader and blend state it
             * would otherwise be handed straight back from this cache.
             */
            unsigned ds = (d->depth_enable || d->depth_write ||
                           d->stencil_enable) ? 1u : 0u;
            const uint32_t pipeline_state[8] = {
                d->color_mask, (unsigned)d->blend_enable,
                d->blend_src_rgb, d->blend_dst_rgb,
                d->blend_src_alpha, d->blend_dst_alpha,
                (unsigned)d->guest_target, ds,
            };
            R300PipelineKey *key = [[[R300PipelineKey alloc]
                initWithShader:source state:pipeline_state] autorelease];
            id<MTLRenderPipelineState> pipeline = [ctx->pipelines objectForKey:key];
            if (!pipeline) {
                struct timespec build_start, build_end;
                static int timing = -1;
                if (timing < 0) timing = getenv("POWEREMU_AUDIO_TIMING") != NULL;
                if (timing) clock_gettime(CLOCK_MONOTONIC, &build_start);
                NSError *compile_error = nil;
                MTLCompileOptions *options = [[[MTLCompileOptions alloc] init] autorelease];
                options.fastMathEnabled = NO;
                id<MTLLibrary> lib = [ctx->libraries objectForKey:source];
                bool library_miss = lib == nil;
                if (!lib) {
                    lib = [[ctx->device newLibraryWithSource:source options:options
                                                      error:&compile_error] autorelease];
                    if (lib) {
                        [ctx->libraries setObject:lib forKey:source];
                        r300_warm_record(ctx, source);
                    }
                }
                if (!lib) {
                    /* The message gives a line and column in a source nobody
                     * has seen; print it once so the generator can be fixed
                     * rather than guessed at. */
                    if (getenv("POWEREMU_R350_SHADER_ON_ERROR")) {
                        static bool dumped;
                        if (!dumped) {
                            dumped = true;
                            fprintf(stderr, "R350_SHADER_ERROR %s\n--- source ---\n%s\n--- end ---\n",
                                    compile_error.description.UTF8String ?: "?",
                                    source.UTF8String ?: "");
                        }
                    }
                    return fail(error,error_size,compile_error.description.UTF8String ?: "Metal compilation failed");
                }
                MTLRenderPipelineDescriptor *pd = [[[MTLRenderPipelineDescriptor alloc] init] autorelease];
                pd.vertexFunction = [[lib newFunctionWithName:@"r300_vertex"] autorelease];
                pd.fragmentFunction = [[lib newFunctionWithName:@"r300_fragment"] autorelease];
                pd.colorAttachments[0].pixelFormat = d->guest_target ?
                    MTLPixelFormatBGRA8Unorm : MTLPixelFormatRGBA8Unorm;
                /* Declared here and keyed below: a pipeline built without the
                 * attachment cannot be used in a pass that has one. */
                if (d->fp->depth_mirror) {
                    pd.colorAttachments[1].pixelFormat = d->fp->depth_mirror == 1 ?
                        MTLPixelFormatR16Uint : MTLPixelFormatR32Uint;
                }
                if (ds) {
                    pd.depthAttachmentPixelFormat = R300_DEPTH_FORMAT;
                    pd.stencilAttachmentPixelFormat = R300_DEPTH_FORMAT;
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
                [ctx->pipelines setObject:pipeline forKey:key];
                if (timing) {
                    clock_gettime(CLOCK_MONOTONIC, &build_end);
                    int64_t elapsed = (build_end.tv_sec - build_start.tv_sec) *
                        INT64_C(1000000000) + build_end.tv_nsec - build_start.tv_nsec;
                    if (elapsed >= 5 * INT64_C(1000000))
                        fprintf(stderr, "R350_PIPELINE_BUILD us=%lld cached=%lu shader=%lu key=%lu library-miss=%u begin-ns=%lld end-ns=%lld\n",
                                (long long)(elapsed / 1000),
                                (unsigned long)ctx->pipelines.count,
                                (unsigned long)[source hash],
                                (unsigned long)[key hash], library_miss,
                                (long long)build_start.tv_sec * INT64_C(1000000000) + build_start.tv_nsec,
                                (long long)build_end.tv_sec * INT64_C(1000000000) + build_end.tv_nsec);
                }
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
                command = gpu_submit_buffer_for(r300_end_pass);
                target = gpu_view(gkey, MTLPixelFormatBGRA8Unorm, true);
                if (!target) {
                    return fail(error,error_size,
                                "render target is not a usable VRAM view");
                }
            } else {
                /* Native/staged depth uses the same private attachments;
                 * finish any shared pass before a separate queue uses them. */
                gpu_submit_flush();
                command = [ctx->queue commandBuffer];
                target_bytes = upload_buffer(ctx,d->target,d->width,d->height);
                target = private_texture(ctx,d->width,d->height,MTLTextureUsageRenderTarget);
            }
            size_t vb_offset, vc_offset, fc_offset;
            id<MTLBuffer> vb = stage_draw_data(ctx, d->vertices,
                d->vertex_count * sizeof(*d->vertices), d->guest_target, &vb_offset);
            id<MTLBuffer> vc = stage_draw_data(ctx, d->vertex_constants,
                256 * 4 * sizeof(float), d->guest_target, &vc_offset);
            float host_fc[R300_FP_HOST_CONSTANT_COUNT][4];
            r300_fp_pack_constants(d->fp, d->fragment_constants, host_fc);
            id<MTLBuffer> fc = stage_draw_data(ctx, host_fc,
                sizeof(host_fc), d->guest_target, &fc_offset);
            if (!command || !target || !vb || !vc || !fc ||
                (!d->guest_target && !target_bytes))
                return fail(error,error_size,"Metal resource allocation failed");
            id<MTLTexture> textures[16] = {nil};
            id<MTLSamplerState> samplers[16] = {nil};
            bool needs_upload = !d->guest_target;
            for (unsigned i = 0; i < 16; i++) {
                needs_upload |= (used_textures & (1u << i)) &&
                                !d->textures[i].guest_source;
            }
            if (needs_upload) r300_end_pass();
            id<MTLBlitCommandEncoder> initial = needs_upload ?
                [command blitCommandEncoder] : nil;
            if (needs_upload && !initial)
                return fail(error,error_size,"Metal upload encoder failed");
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
                unsigned anisotropy = t->max_anisotropy ?: 1;
                MTLSamplerAddressMode address_s = r300_address_mode(t->address_s);
                MTLSamplerAddressMode address_t = r300_address_mode(t->address_t);
                NSNumber *sampler_key = @(address_s | (address_t << 8) |
                                          (anisotropy << 16) |
                                          ((unsigned)t->linear_filter << 24));
                samplers[i] = [ctx->samplers objectForKey:sampler_key];
                if (!samplers[i]) {
                    MTLSamplerDescriptor *sd = [[[MTLSamplerDescriptor alloc] init] autorelease];
                    sd.normalizedCoordinates = YES;
                    sd.maxAnisotropy = anisotropy;
                    sd.sAddressMode = address_s;
                    sd.tAddressMode = address_t;
                    sd.minFilter = sd.magFilter = t->linear_filter ?
                        MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
                    samplers[i] = [[ctx->device newSamplerStateWithDescriptor:sd] autorelease];
                    if (samplers[i]) {
                        [ctx->samplers setObject:samplers[i] forKey:sampler_key];
                    }
                }
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
             * each guest depth buffer gets a private one. A buffer seen for
             * the first time is cleared
             * rather than loaded -- there is nothing in our copy yet, and the
             * guest's own clear is a draw with the compare set to ALWAYS,
             * which lands on the cleared texture and agrees with it.
             */
            /*
             * Bisect switches, each isolating one thing introduced today:
             *   POWEREMU_R350_NOCULL   -- cull nothing (the winding convention
             *                             has never run on a path that
             *                             produced correct pixels)
             *   POWEREMU_R350_ZCLEAR   -- clear depth and stencil every pass,
             *                             rather than once per buffer
             *   POWEREMU_R350_NOSTENCIL-- ignore the stencil test
             */
            static int no_cull = -1, always_clear = -1, no_stencil = -1;
            if (no_cull < 0) {
                no_cull = getenv("POWEREMU_R350_NOCULL") != NULL;
                always_clear = getenv("POWEREMU_R350_ZCLEAR") != NULL;
                no_stencil = getenv("POWEREMU_R350_NOSTENCIL") != NULL;
            }
            id<MTLTexture> depth_tex = nil;
            bool depth_fresh = false;
            if (d->depth_enable || d->depth_write || d->stencil_enable) {
                depth_tex = r300_depth_tex(ctx->device, d->depth_offset,
                                           d->depth_pitch, &depth_fresh);
                if (!depth_tex) {
                    return fail(error,error_size,"depth buffer unavailable");
                }
                for (unsigned i = 0; i < R300_MAX_DEPTH; i++) {
                    if (g_r300_depth[i].tex == depth_tex) {
                        g_r300_depth[i].height = MIN(d->height, 2048);
                        g_r300_depth[i].format = d->depth_format;
                    }
                }
                if (always_clear) { depth_fresh = true; }
            }
            MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
            if (depth_tex) {
                pass.depthAttachment.texture = depth_tex;
                pass.depthAttachment.loadAction = depth_fresh ?
                    MTLLoadActionClear : MTLLoadActionLoad;
                pass.depthAttachment.clearDepth = 1.0;
                pass.depthAttachment.storeAction = MTLStoreActionStore;
                /* The same allocation carries the stencil planes. */
                pass.stencilAttachment.texture = depth_tex;
                pass.stencilAttachment.loadAction = depth_fresh ?
                    MTLLoadActionClear : MTLLoadActionLoad;
                pass.stencilAttachment.clearStencil = 0;
                pass.stencilAttachment.storeAction = MTLStoreActionStore;
            }
            pass.colorAttachments[0].texture = target;
            pass.colorAttachments[0].loadAction = MTLLoadActionLoad;
            pass.colorAttachments[0].storeAction = MTLStoreActionStore;
            id<MTLTexture> mirror = nil;
            if (d->fp->depth_mirror) {
                if (!d->depth_guest_vram || gpu_submit_vram().contents != d->depth_guest_vram) {
                    return fail(error,error_size,"shared depth VRAM owner mismatch");
                }
                unsigned bpp = d->fp->depth_mirror == 1 ? 2 : 4;
                MTLPixelFormat zpf = bpp == 2 ? MTLPixelFormatR16Uint : MTLPixelFormatR32Uint;
                GPUSurfaceKey zk = {d->depth_guest_offset, d->width, d->height,
                                    d->depth_pitch * bpp, (uint32_t)zpf};
                NSUInteger alignment = [ctx->device minimumLinearTextureAlignmentForPixelFormat:zpf];
                if (!depth_tex || d->depth_pitch < d->width ||
                    zk.offset % alignment || zk.pitch % alignment) {
                    return fail(error,error_size,"shared depth attachment alignment or state");
                }
                if (!d->guest_target) { gpu_submit_flush(); }
                mirror = gpu_view(zk, zpf, true);
                if (!mirror) return fail(error,error_size,"shared depth attachment unavailable");
                pass.colorAttachments[1].texture = mirror;
                pass.colorAttachments[1].loadAction = MTLLoadActionLoad;
                pass.colorAttachments[1].storeAction = MTLStoreActionStore;
                if (d->guest_target) {
                    gpu_note_written(zk.offset, (uint64_t)zk.offset + (uint64_t)zk.pitch * zk.height, &zk);
                }
            }
            bool reuse_pass = d->guest_target && !depth_fresh &&
                g_r300_encoder && g_r300_owner == ctx &&
                g_r300_command == command && g_r300_color == target &&
                g_r300_depth_attachment == depth_tex && g_r300_mirror == mirror;
            if (!reuse_pass) r300_end_pass();
            id<MTLRenderCommandEncoder> encoder = reuse_pass ? g_r300_encoder :
                [command renderCommandEncoderWithDescriptor:pass];
            if (!encoder) return fail(error,error_size,"Metal render encoder failed");
            if (d->guest_target && !reuse_pass) {
                g_r300_encoder = [encoder retain];
                g_r300_owner = ctx; g_r300_command = command;
                g_r300_color = target; g_r300_depth_attachment = depth_tex;
                g_r300_mirror = mirror;
            }
            [encoder setRenderPipelineState:pipeline];
            if (depth_tex) {
                [encoder setDepthStencilState:r300_dss(ctx->device, d)];
                if (d->stencil_enable && !no_stencil) {
                    [encoder setStencilReferenceValue:d->stencil_ref];
                }
            }
            [encoder setViewport:(MTLViewport){d->viewport_x,d->viewport_y,d->viewport_width ?: d->width,
                d->viewport_height ?: d->height,0,1}];
            [encoder setScissorRect:(MTLScissorRect){d->scissor[0],d->scissor[1],d->scissor[2],d->scissor[3]}];
            [encoder setCullMode:no_cull ? MTLCullModeNone :
                                 d->cull_front ? MTLCullModeFront :
                                 d->cull_back ? MTLCullModeBack : MTLCullModeNone];
            [encoder setFrontFacingWinding:d->front_is_cw ?
                MTLWindingClockwise : MTLWindingCounterClockwise];
            [encoder setVertexBuffer:vb offset:vb_offset atIndex:0];
            [encoder setVertexBuffer:vc offset:vc_offset atIndex:1];
            [encoder setFragmentBuffer:fc offset:fc_offset atIndex:0];
            if (d->fp->depth_mirror) {
                uint32_t write = d->depth_write;
                [encoder setFragmentBytes:&write length:sizeof(write) atIndex:1];
            }
            for (unsigned i = 0; i < 16; i++) {
                if (used_textures & (1u << i)) {
                    [encoder setFragmentTexture:textures[i] atIndex:i];
                    [encoder setFragmentSamplerState:samplers[i] atIndex:i];
                }
            }
            [encoder drawPrimitives:d->primitive_points ? MTLPrimitiveTypePoint : MTLPrimitiveTypeTriangle
                         vertexStart:0 vertexCount:d->vertex_count];
            if (!d->guest_target) [encoder endEncoding];
            if (d->depth_tiled_export) r300_end_pass();
            if (d->depth_tiled_export &&
                !export_tiled_depth(ctx, command, depth_tex, d, error, error_size)) {
                return false;
            }
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
