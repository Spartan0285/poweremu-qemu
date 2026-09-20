/*
 * PowerEmu paravirtual GPU: the guest/host command protocol.
 *
 * The emulated R200 is faithful, and that fidelity is expensive: the guest's
 * ATI driver is emulated instruction by instruction while it builds command
 * streams, and each register it touches leaves translated code through the
 * software MMU and takes the big QEMU lock.  This device gives a cooperating
 * guest driver a way to hand over whole batches of work instead.
 *
 * Everything here is shared between the guest driver and the device model,
 * so it must stay a plain C header with fixed-width, explicitly ordered
 * fields.  The guest is big-endian PowerPC and the host is little-endian
 * aarch64: every multi-byte field below is BIG-ENDIAN, which is what the
 * guest writes naturally and what the host byte-swaps on the way in.
 *
 * Copyright (c) 2026 Spartan0285
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_DISPLAY_POWEREMU_GPU_RING_H
#define HW_DISPLAY_POWEREMU_GPU_RING_H

#define PE_GPU_MAGIC        0x50454750u     /* 'PEGP' */
#define PE_GPU_VERSION      1u

/*
 * BAR layout.  The control page is small and trapping (the guest touches it
 * rarely: once to set up, once per batch to ring the doorbell).  The ring
 * and the data area are ordinary shared memory the guest writes without
 * trapping at all -- that is the entire point of the device.
 */
#define PE_GPU_CTRL_OFFSET  0x0000
#define PE_GPU_CTRL_SIZE    0x1000
#define PE_GPU_RING_OFFSET  0x1000
#define PE_GPU_RING_BYTES   (1u << 20)      /* 1 MB of packets */
#define PE_GPU_DATA_OFFSET  (PE_GPU_RING_OFFSET + PE_GPU_RING_BYTES)
#define PE_GPU_DATA_BYTES   (32u << 20)     /* vertices, textures */

/*
 * A PCI BAR's size must be a power of two -- the decoder is an address
 * mask, so there is no way to express one that is not -- and control page
 * plus ring plus data is 0x2101000, which is not.  The BAR is therefore
 * rounded up to the next power of two and the tail is simply left
 * unmapped: it is a container region with nothing behind it, so it costs
 * address space and no memory.
 *
 * What must not follow the rounding is PE_GPU_SHARED_BYTES below.  That is
 * the extent the host validates every guest offset against, and widening it
 * to the BAR would have the host accept offsets that point past the end of
 * the memory it actually allocated.
 */
#define PE_GPU_BAR_BYTES    (64u << 20)

/*
 * Every offset a packet carries -- vertices, texels, render targets, the
 * scanout -- is relative to the start of the shared mapping, i.e. to the
 * ring base, not to the BAR base and not to the data area.  One base for
 * all of them means the host can validate any offset with a single range
 * check and hand it to the renderer unmodified, because the shared mapping
 * is exactly what the renderer is given as "VRAM".
 */
#define PE_GPU_SHARED_BYTES (PE_GPU_RING_BYTES + PE_GPU_DATA_BYTES)
#define PE_GPU_DATA_BASE    (PE_GPU_DATA_OFFSET - PE_GPU_RING_OFFSET)

/* Control registers, byte offsets within the control page. */
#define PE_GPU_REG_MAGIC    0x00            /* R:  PE_GPU_MAGIC */
#define PE_GPU_REG_VERSION  0x04            /* R:  PE_GPU_VERSION */
#define PE_GPU_REG_FEATURES 0x08            /* R:  PE_GPU_FEAT_* */
#define PE_GPU_REG_ENABLE   0x0c            /* W:  1 to take over scanout */
#define PE_GPU_REG_DOORBELL 0x10            /* W:  new ring head, in bytes */
#define PE_GPU_REG_TAIL     0x14            /* R:  bytes the host consumed */
#define PE_GPU_REG_FENCE    0x18            /* R:  last completed fence */
#define PE_GPU_REG_ERROR    0x1c            /* R:  PE_GPU_ERR_* */

#define PE_GPU_FEAT_DRAW    (1u << 0)       /* triangle batches */
#define PE_GPU_FEAT_BLIT    (1u << 1)       /* 2D copies */
#define PE_GPU_FEAT_TEXTURE (1u << 2)       /* texture upload */

#define PE_GPU_ERR_NONE     0u
#define PE_GPU_ERR_PACKET   1u              /* malformed packet */
#define PE_GPU_ERR_BOUNDS   2u              /* offset/length outside the BAR */
#define PE_GPU_ERR_VERSION  3u

/*
 * Packets.  Every packet starts with this header; `bytes` covers the header
 * and its payload, so the host can skip a packet it does not understand
 * without guessing its size.  Packets are 8-byte aligned.
 */
typedef struct PEGpuPacketHdr {
    uint16_t type;          /* PE_GPU_PKT_* */
    uint16_t flags;
    uint32_t bytes;         /* total packet size, including this header */
} PEGpuPacketHdr;

#define PE_GPU_PKT_NOP      0u
#define PE_GPU_PKT_FENCE    1u              /* payload: PEGpuFence */
#define PE_GPU_PKT_STATE    2u              /* payload: PEGpuState */
#define PE_GPU_PKT_DRAW     3u              /* payload: PEGpuDraw */
#define PE_GPU_PKT_TEXTURE  4u              /* payload: PEGpuTexture */
#define PE_GPU_PKT_BLIT     5u              /* payload: PEGpuBlit */
#define PE_GPU_PKT_PRESENT  6u              /* payload: PEGpuPresent */

typedef struct PEGpuFence {
    PEGpuPacketHdr hdr;
    uint32_t value;         /* published in PE_GPU_REG_FENCE when reached */
    uint32_t reserved;
} PEGpuFence;

/*
 * Render state for the draws that follow, in the guest's own terms rather
 * than as R200 register writes.  Deliberately small: a paravirtual driver
 * knows what it wants and does not need to express it as hardware state.
 */
typedef struct PEGpuState {
    PEGpuPacketHdr hdr;
    uint32_t target_offset;     /* render target, byte offset in VRAM */
    uint32_t target_pitch;
    uint16_t target_width, target_height;
    uint32_t depth_offset;      /* 0: no depth buffer */
    uint32_t depth_pitch;
    uint16_t depth_bits;        /* 16 or 24/32 */
    uint16_t flags;             /* PE_GPU_ST_* */
    uint32_t blend;             /* src | dst << 16, PE_GPU_BLEND_* */
    int16_t  scissor_x, scissor_y, scissor_w, scissor_h;
} PEGpuState;

#define PE_GPU_ST_DEPTH_TEST   (1u << 0)
#define PE_GPU_ST_DEPTH_WRITE  (1u << 1)
#define PE_GPU_ST_BLEND        (1u << 2)
#define PE_GPU_ST_TEXTURE      (1u << 3)

/*
 * Blend factors, packed into PEGpuState.blend as src | dst << 16.
 *
 * The order is not arbitrary: it is the R200's own BLENDCNTL factor order
 * biased to zero, so the host translates with an add instead of a table.
 * Extending this list means extending the R200 side of it too.
 */
#define PE_GPU_BLEND_ZERO          0u
#define PE_GPU_BLEND_ONE           1u
#define PE_GPU_BLEND_SRC_COLOUR    2u
#define PE_GPU_BLEND_INV_SRC_COLOUR 3u
#define PE_GPU_BLEND_DST_COLOUR    4u
#define PE_GPU_BLEND_INV_DST_COLOUR 5u
#define PE_GPU_BLEND_SRC_ALPHA     6u
#define PE_GPU_BLEND_INV_SRC_ALPHA 7u
#define PE_GPU_BLEND_DST_ALPHA     8u
#define PE_GPU_BLEND_INV_DST_ALPHA 9u
#define PE_GPU_BLEND_LAST          PE_GPU_BLEND_INV_DST_ALPHA

/*
 * A batch of triangles.  Vertices live in the data area, so the guest
 * writes them once into shared memory and the host reads them in place --
 * no copy through registers, no per-draw allocation on either side.
 */
typedef struct PEGpuDraw {
    PEGpuPacketHdr hdr;
    uint32_t vertex_offset;     /* into the data area */
    uint32_t vertex_count;
    uint16_t vertex_stride;
    uint16_t prim;              /* PE_GPU_PRIM_* */
    uint32_t index_offset;      /* 0: not indexed */
    uint32_t index_count;
    uint32_t texture_id;        /* 0: untextured */
} PEGpuDraw;

#define PE_GPU_PRIM_TRIANGLES  0u
#define PE_GPU_PRIM_STRIP      1u
#define PE_GPU_PRIM_FAN        2u
#define PE_GPU_PRIM_LINES      3u
#define PE_GPU_PRIM_POINTS     4u

/*
 * The vertex the data area holds, at PEGpuDraw.vertex_offset and every
 * vertex_stride bytes after it.  A stride larger than this struct is fine:
 * the guest may interleave data the host does not look at.
 *
 * The coordinates are already transformed -- window pixels in the render
 * target, z in 0..1, w the clip w to divide by (0 means 1).  A paravirtual
 * driver has the transform in software anyway, and doing it guest-side
 * keeps the host from having to model a TCL unit it would only ever be
 * asked to run in one configuration.
 *
 * The float fields are uint32_t because this header is shared with a guest
 * that may be built without an FP ABI: they are IEEE-754 single-precision
 * bit patterns, big-endian like everything else here.
 */
typedef struct PEGpuVertex {
    uint32_t x, y, z, w;        /* float bits: window x,y; depth; clip w */
    uint32_t colour;            /* ARGB8888, not float */
    uint32_t s, t;              /* float bits: texture coordinates, 0..1 */
    uint32_t reserved;
} PEGpuVertex;

/* Texture id 0 means "untextured", so the table is 1..PE_GPU_MAX_TEXTURES-1. */
#define PE_GPU_MAX_TEXTURES    64u

typedef struct PEGpuTexture {
    PEGpuPacketHdr hdr;
    uint32_t id;
    uint32_t data_offset;       /* into the data area */
    uint16_t width, height;
    uint16_t pitch;
    uint16_t format;            /* PE_GPU_TEX_* */
    uint16_t filter;            /* 0 nearest, 1 linear */
    uint16_t wrap;              /* 0 repeat, 1 clamp */
} PEGpuTexture;

#define PE_GPU_TEX_ARGB8888    0u
#define PE_GPU_TEX_RGB565      1u
#define PE_GPU_TEX_ARGB1555    2u
#define PE_GPU_TEX_ARGB4444    3u

typedef struct PEGpuBlit {
    PEGpuPacketHdr hdr;
    uint32_t src_offset, dst_offset;
    uint32_t src_pitch, dst_pitch;
    uint16_t src_x, src_y, dst_x, dst_y;
    uint16_t width, height;
    uint32_t colour;            /* for solid fills, when src_pitch == 0 */
} PEGpuBlit;

typedef struct PEGpuPresent {
    PEGpuPacketHdr hdr;
    uint32_t offset;            /* scanout base in VRAM */
    uint32_t pitch;
    uint16_t width, height;
} PEGpuPresent;

#endif /* HW_DISPLAY_POWEREMU_GPU_RING_H */
