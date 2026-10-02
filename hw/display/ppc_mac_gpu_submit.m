/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Command buffers, staging, VRAM views and hazard tracking, shared by every
 * generation this device emulates.  See ppc_mac_gpu_submit.h for why the
 * boundary sits here.
 *
 * Lifted out of the R200 Metal backend unchanged in behaviour: the same
 * batching, the same shared-event split, the same arena and the same view
 * cache, with the names generalised.  Manual reference counting matches
 * QEMU's Objective-C build.
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include "ppc_mac_gpu_submit.h"

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

static id<MTLDevice> g_device;
static id<MTLCommandQueue> g_queue;
static id<MTLBuffer> g_vram;
static void (*g_end_pass)(void);

static id<MTLCommandBuffer> g_cb;
static id<MTLCommandBuffer> g_inflight;      /* newest committed, unwaited */
static uint32_t g_seq;

static GPUSubmitStats g_stats;

static int64_t now_us(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000000 + tv.tv_usec;
}

static void submit_warn(const char *what)
{
    static unsigned said;
    if (said < 8) {
        said++;
        fprintf(stderr, "[GPU_SUBMIT] %s\n", what);
    }
}

void gpu_submit_init(id<MTLDevice> device, id<MTLCommandQueue> queue,
                     id<MTLBuffer> vram)
{
    g_device = device;
    g_queue = queue;
    g_vram = vram;
}

bool gpu_submit_ready(void)
{
    return g_device != nil && g_queue != nil;
}

void gpu_submit_set_pass_ender(void (*end_pass)(void))
{
    g_end_pass = end_pass;
}

bool gpu_submit_have_buffer(void)
{
    return g_cb != nil;
}

/* ------------------------------------------------------------------ *
 * Batching
 *
 * Rather than stopping the vCPU until the GPU is done, a hazard can end
 * the batch and start a new one the GPU runs only after the previous one
 * completes -- a shared event signalled at the end of each batch and waited
 * for at the start of the next.  PPCGPU_SPLIT=0 goes back to waiting.
 * ------------------------------------------------------------------ */

static uint32_t g_epoch;
static id<MTLSharedEvent> g_event;
static uint64_t g_event_val;

bool gpu_submit_split_enabled(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("PPCGPU_SPLIT");
        on = !(e && e[0] == '0');
    }
    return on;
}

/* A new command buffer, ordered after everything committed before it. */
static id<MTLCommandBuffer> new_command_buffer(void)
{
    id<MTLCommandBuffer> cb = [[g_queue commandBuffer] retain];
    if (g_event && g_event_val) {
        [cb encodeWaitForEvent:g_event value:g_event_val];
    }
    return cb;
}

id<MTLCommandBuffer> gpu_submit_buffer(void)
{
    if (!g_cb && g_queue) {
        g_cb = new_command_buffer();
    }
    return g_cb;
}

/* ------------------------------------------------------------------ *
 * Staging arena
 * ------------------------------------------------------------------ */

#define ARENA_SIZE (8 * 1024 * 1024)

static NSMutableArray *g_arena_free;         /* idle, ready to reuse */
static NSMutableArray *g_arena_used;         /* referenced by the open batch */
static id<MTLBuffer> g_arena;
static size_t g_arena_off;
static pthread_mutex_t g_arena_lock = PTHREAD_MUTEX_INITIALIZER;

void *gpu_stage_alloc(size_t len, id<MTLBuffer> *buf, size_t *offset)
{
    len = (len + 255) & ~(size_t)255;        /* keep offsets aligned */
    if (len > ARENA_SIZE || !g_device) {
        return NULL;
    }
    if (!g_arena || g_arena_off + len > [g_arena length]) {
        if (g_arena) {
            if (!g_arena_used) {
                g_arena_used = [[NSMutableArray alloc] init];
            }
            [g_arena_used addObject:g_arena];
            [g_arena release];
            g_arena = nil;
        }
        pthread_mutex_lock(&g_arena_lock);
        id<MTLBuffer> reuse = [g_arena_free lastObject];
        if (reuse) {
            g_arena = [reuse retain];
            [g_arena_free removeLastObject];
        }
        pthread_mutex_unlock(&g_arena_lock);
        if (!g_arena) {
            /* newBuffer... already returns a reference we own. */
            g_arena = [g_device newBufferWithLength:ARENA_SIZE
                                            options:MTLResourceStorageModeShared];
            if (!g_arena) {
                return NULL;
            }
        }
        g_arena_off = 0;
    }
    *buf = g_arena;
    *offset = g_arena_off;
    void *p = (char *)[g_arena contents] + g_arena_off;
    g_arena_off += len;
    return p;
}

/* Hand this batch's staging buffers back once the GPU is done with them. */
static void arena_recycle_on(id<MTLCommandBuffer> cb)
{
    NSMutableArray *mine = g_arena_used;
    g_arena_used = nil;
    if (g_arena) {
        if (!mine) {
            mine = [[NSMutableArray alloc] init];
        }
        [mine addObject:g_arena];
        [g_arena release];
        g_arena = nil;
        g_arena_off = 0;
    }
    if (!mine) {
        return;
    }
    [cb addCompletedHandler:^(id<MTLCommandBuffer> finished) {
        (void)finished;
        pthread_mutex_lock(&g_arena_lock);
        if (!g_arena_free) {
            g_arena_free = [[NSMutableArray alloc] init];
        }
        /* Keep a few around; a deep batch's extras can go. */
        for (id<MTLBuffer> b in mine) {
            if ([g_arena_free count] < 8) {
                [g_arena_free addObject:b];
            }
        }
        pthread_mutex_unlock(&g_arena_lock);
        [mine release];
    }];
}

/* ------------------------------------------------------------------ *
 * Hazard tracking
 *
 * Views that alias the same memory are distinct Metal objects, so Metal
 * does not order a write through one against a read through another: a
 * draw that would read (or re-target) written memory through a different
 * view has to be ordered explicitly.
 * ------------------------------------------------------------------ */

#define MAX_WRITTEN 128
static struct {
    uint64_t lo, hi;
    GPUSurfaceKey key;
    uint32_t epoch;
} g_written[MAX_WRITTEN];
static int g_nwritten;

#define MAX_READ 64
static struct { uint64_t lo, hi; } g_read[MAX_READ];
static int g_nread;
static bool g_read_overflow;

void gpu_note_written(uint64_t lo, uint64_t hi, const GPUSurfaceKey *through)
{
    int w = 0;
    while (w < g_nwritten && memcmp(&g_written[w].key, through, sizeof(*through))) {
        w++;
    }
    if (w < g_nwritten) {
        g_written[w].epoch = g_epoch;        /* written again in this batch */
        return;
    }
    if (w == MAX_WRITTEN) {
        return;              /* full: the next draw flushes, see the conflict check */
    }
    g_written[w].lo = lo;
    g_written[w].hi = hi;
    g_written[w].key = *through;
    g_written[w].epoch = g_epoch;
    g_nwritten++;
}

void gpu_note_read(uint64_t lo, uint64_t hi)
{
    for (int i = 0; i < g_nread; i++) {
        if (g_read[i].lo == lo && g_read[i].hi == hi) {
            return;
        }
    }
    if (g_nread == MAX_READ) {
        g_read_overflow = true;              /* be conservative */
        return;
    }
    g_read[g_nread].lo = lo;
    g_read[g_nread++].hi = hi;
}

bool gpu_written_full(void)
{
    return g_nwritten == MAX_WRITTEN;
}

bool gpu_range_busy(uint64_t lo, uint64_t hi, bool write_access)
{
    if (!g_cb && !g_inflight) {
        return false;
    }
    for (int i = 0; i < g_nwritten; i++) {
        if (lo < g_written[i].hi && g_written[i].lo < hi) {
            return true;
        }
    }
    if (write_access) {
        if (g_read_overflow) {
            return true;
        }
        for (int i = 0; i < g_nread; i++) {
            if (lo < g_read[i].hi && g_read[i].lo < hi) {
                return true;
            }
        }
    }
    return false;
}

bool gpu_batch_conflict(uint64_t lo, uint64_t hi, const GPUSurfaceKey *same)
{
    for (int i = 0; i < g_nwritten; i++) {
        if (g_written[i].epoch != g_epoch) {
            continue;        /* an earlier batch: the GPU orders it before this one */
        }
        if (lo < g_written[i].hi && g_written[i].lo < hi &&
            !(same && !memcmp(same, &g_written[i].key, sizeof(*same)))) {
            return true;
        }
    }
    return false;
}

void gpu_submit_note_conflict(void)
{
    g_stats.conflicts++;
}

/* ------------------------------------------------------------------ *
 * VRAM views
 * ------------------------------------------------------------------ */

#define VIEW_CACHE 64
static struct {
    GPUSurfaceKey key;
    id<MTLTexture> tex;
    uint64_t used;
} g_views[VIEW_CACHE];
static uint64_t g_view_clock;

bool gpu_view_available(uint32_t offset, uint32_t pitch, uint32_t height,
                        unsigned bpp)
{
    MTLPixelFormat pf = bpp == 1 ? MTLPixelFormatR8Unorm : MTLPixelFormatBGRA8Unorm;

    if (!g_device || !g_vram || (bpp != 1 && bpp != 4) || !pitch || !height) {
        return false;
    }
    NSUInteger align = [g_device minimumLinearTextureAlignmentForPixelFormat:pf];
    if (align && ((offset % align) || (pitch % align))) {
        return false;
    }
    return (uint64_t)offset + (uint64_t)pitch * height <= [g_vram length];
}

id<MTLTexture> gpu_view(GPUSurfaceKey k, MTLPixelFormat pf, bool render_target)
{
    int lru = 0;

    if (!g_vram) {
        return nil;
    }
    for (int i = 0; i < VIEW_CACHE; i++) {
        if (g_views[i].tex && !memcmp(&g_views[i].key, &k, sizeof(k))) {
            g_views[i].used = ++g_view_clock;
            g_stats.view_hit++;
            return g_views[i].tex;
        }
        if (g_views[i].used < g_views[lru].used) {
            lru = i;
        }
    }
    /*
     * Check before asking: newTextureWithDescriptor:offset:bytesPerRow: raises
     * on a misaligned offset or row length rather than returning nil, and the
     * offset and pitch here come from the guest.
     */
    NSUInteger align = [g_device minimumLinearTextureAlignmentForPixelFormat:pf];
    if (align && ((k.offset % align) || (k.pitch % align))) {
        return nil;
    }
    if (!k.width || !k.height || !k.pitch ||
        (uint64_t)k.offset + (uint64_t)k.pitch * k.height > [g_vram length]) {
        return nil;
    }
    MTLTextureDescriptor *d = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:pf width:k.width height:k.height
                                 mipmapped:NO];
    d.usage = MTLTextureUsageShaderRead |
              (render_target ? MTLTextureUsageRenderTarget : 0);
    d.storageMode = MTLStorageModeShared;
    id<MTLTexture> t = [g_vram newTextureWithDescriptor:d
                                                 offset:k.offset
                                            bytesPerRow:k.pitch];
    if (!t) {
        return nil;
    }
    /* An evicted view may still be referenced by the open command buffer;
     * the command buffer retains what it uses, so releasing is safe. */
    g_stats.view_new++;
    [g_views[lru].tex release];
    g_views[lru].key = k;
    g_views[lru].tex = t;
    g_views[lru].used = ++g_view_clock;
    return t;
}

/* ------------------------------------------------------------------ *
 * Committing
 * ------------------------------------------------------------------ */

uint32_t gpu_submit_commit(void (*done)(void *, uint32_t), void *arg)
{
    if (!g_cb) {
        return 0;
    }
    if (g_end_pass) {
        g_end_pass();
    }
    uint32_t seq = ++g_seq;
    if (seq == 0) {
        seq = ++g_seq;
    }
    if (done) {
        [g_cb addCompletedHandler:^(id<MTLCommandBuffer> finished) {
            (void)finished;
            done(arg, seq);
        }];
    }
    if (g_event) {
        [g_cb encodeSignalEvent:g_event value:++g_event_val];
    }
    arena_recycle_on(g_cb);
    [g_cb commit];
    [g_inflight release];
    g_inflight = g_cb;                        /* keeps the reference */
    g_cb = nil;
    /* Committed work is ordered on the queue, but memory it writes may still
     * be read by later batches through other views: keep the written ranges
     * until everything in flight is known complete. */
    return seq;
}

void gpu_submit_split(void)
{
    if (!g_cb) {
        return;
    }
    if (!g_event) {
        /* newSharedEvent already hands back a reference we own. */
        g_event = [g_device newSharedEvent];
        if (!g_event) {
            /* Without an event there is nothing to order the next batch
             * against, and proceeding would let it run concurrently.  Wait
             * instead -- slower, but not wrong. */
            gpu_submit_flush();
            return;
        }
    }
    gpu_submit_commit(NULL, NULL);
    g_epoch++;
    g_stats.splits++;
    g_cb = new_command_buffer();
}

bool gpu_submit_flush(void)
{
    if (!g_cb && !g_inflight) {
        return false;
    }
    int64_t t0 = now_us();
    gpu_submit_commit(NULL, NULL);
    [g_inflight waitUntilCompleted];
    if (g_inflight.status == MTLCommandBufferStatusError) {
        submit_warn("command buffer failed");
    }
    [g_inflight release];
    g_inflight = nil;
    g_nwritten = 0;
    g_nread = 0;
    g_read_overflow = false;
    g_stats.flushes++;
    g_stats.flush_us += now_us() - t0;
    return true;
}

void gpu_submit_stats(GPUSubmitStats *out)
{
    *out = g_stats;
}
