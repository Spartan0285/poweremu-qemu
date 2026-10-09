/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Getting work to the GPU: command buffers, staging, VRAM views and the
 * hazard tracking that decides when a batch has to end.
 *
 * Nothing here names a register or a generation.  That boundary is
 * deliberate and worth defending: Mesa shared exactly this much between its
 * Radeon drivers -- memory, tiling, command submission -- and shared no
 * state translation at all, across R300 through GCN.  The moment this
 * interface needs to know which card it is serving, it has stopped being
 * this interface.
 *
 * It grew out of the R200 path, which needed it to stop stalling the vCPU
 * on every draw.  The R350 experiment needs the same thing and had none of
 * it, so it is lifted out here rather than written twice.
 */
#ifndef PPC_MAC_GPU_SUBMIT_H
#define PPC_MAC_GPU_SUBMIT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Whether a linear view over VRAM at this offset and row pitch is possible:
 * the alignment Metal demands for the format, and the bounds of the buffer.
 * Callable from plain C, so a device can choose between sampling in place and
 * staging before it has committed to either.  bpp is 1 or 4.
 */
bool gpu_view_available(uint32_t offset, uint32_t pitch, uint32_t height,
                        unsigned bpp);

#ifdef __OBJC__
#import <Metal/Metal.h>

/*
 * Identifies one way of looking at a region of VRAM.  Two views of the same
 * bytes with different geometry are different Metal objects, and Metal will
 * not order a write through one against a read through the other -- which
 * is the whole reason the hazard tracking below exists.
 */
typedef struct GPUSurfaceKey {
    uint32_t offset, width, height, pitch, format;
} GPUSurfaceKey;

/*
 * Hand over the device, the queue, and the buffer that backs guest VRAM.
 * The VRAM buffer must be shared storage: that is what makes a zero-copy
 * texture view over guest memory legal.
 */
void gpu_submit_init(id<MTLDevice> device, id<MTLCommandQueue> queue,
                     id<MTLBuffer> vram);
bool gpu_submit_ready(void);

/*
 * The VRAM buffer this layer is bound to, or nil before anything attached.
 *
 * The layer is a process-wide singleton: it is bound to the first card that
 * attaches, and every texture it builds is a linear view over *that* buffer.
 * With two cards that is a trap -- the second card's draws, and the hazard
 * tables that decide when a scan-out must wait, would all land on the first
 * card's video memory.  A caller that is not the owner has to stay off this
 * path, so it needs to be able to ask.
 */
id<MTLBuffer> gpu_submit_vram(void);

/*
 * A batch cannot be committed while the client still has a render pass open,
 * and this layer does not know what the client's passes are for.  The client
 * registers how to close one.
 */
void gpu_submit_set_pass_ender(void (*end_pass)(void));

/* The open command buffer, opening one if there is none. */
id<MTLCommandBuffer> gpu_submit_buffer(void);
/* Claim encoding ownership, closing another backend's open pass first. */
id<MTLCommandBuffer> gpu_submit_buffer_for(void (*end_pass)(void));
bool gpu_submit_have_buffer(void);

/*
 * End the open batch and begin one the GPU will not start until it has
 * finished, ordered by a shared event rather than by stopping the vCPU.
 */
void gpu_submit_split(void);
bool gpu_submit_split_enabled(void);

/* Commit the open batch; returns its sequence number, or 0 if none was open.
 * @done, if given, runs on completion with that sequence number. */
uint32_t gpu_submit_commit(void (*done)(void *arg, uint32_t seq), void *arg);

/* Commit and wait.  True if there was anything to wait for. */
bool gpu_submit_flush(void);

/*
 * Staging for data a draw passes by reference -- vertices, constants.  Bump
 * allocated out of a large buffer and returned to the pool once the batch
 * that referenced it completes; a game submits hundreds of draws a frame and
 * a buffer per draw is millions of allocations a session.  Returns NULL for
 * a request too large to stage, in which case the caller needs its own
 * buffer.
 */
void *gpu_stage_alloc(size_t len, id<MTLBuffer> *buf, size_t *offset);

/* A linear texture over guest VRAM, cached.  Cached views stay alive. */
id<MTLTexture> gpu_view(GPUSurfaceKey key, MTLPixelFormat format,
                        bool render_target);

/* Record what the open batch touches, so a later access can be ordered. */
void gpu_note_written(uint64_t lo, uint64_t hi, const GPUSurfaceKey *through);
void gpu_note_read(uint64_t lo, uint64_t hi);

/*
 * True when the written-range table is full.  Nothing more can be tracked,
 * so the only safe thing left is to flush and start over.
 */
bool gpu_written_full(void);

/* Does anything in flight touch [lo, hi)?  Reads only conflict with writes. */
bool gpu_range_busy(uint64_t lo, uint64_t hi, bool write_access);

/*
 * Would an access to [lo, hi) in *this* batch read or retarget memory the
 * same batch has already written through a different view?  @same names the
 * view the caller will use; writing again through that same view is not a
 * conflict, because Metal orders those against each other. A target also
 * conflicts with earlier sampled reads overlapping the bytes it overwrites.
 */
bool gpu_batch_conflict(uint64_t lo, uint64_t hi, const GPUSurfaceKey *same);

/* Counters for the performance overlay and for tests. */
typedef struct GPUSubmitStats {
    uint64_t flushes, splits, conflicts, view_hit, view_new;
    int64_t flush_us;
} GPUSubmitStats;
void gpu_submit_stats(GPUSubmitStats *out);
void gpu_submit_note_conflict(void);

#endif /* __OBJC__ */
#endif
