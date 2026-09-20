/*
 * PowerEmu paravirtual GPU: the device side of the command ring.
 *
 * A cooperating guest driver writes packets into shared memory and rings a
 * doorbell once per batch, instead of building R200 command streams and
 * poking registers that each trap out of translated code.  This file owns
 * the transport: the BAR, the ring, validation and fences -- and the
 * translation from packets into the host renderer's own draw structures.
 *
 * Nothing in the ring is trusted.  Offsets and lengths come from the guest
 * and are checked against the data area on every use; a malformed packet
 * stops the batch and latches an error the guest can read back, rather than
 * being clamped and quietly half-executed.
 *
 * Why it renders through the R200 renderer
 * ----------------------------------------
 * The Metal backend in ppc_mac_gpu_metal.m is the only accelerated path
 * that exists, and it is already tuned: a texture-view cache, a vertex
 * staging arena, command-buffer batching with hazard tracking and fences.
 * A second backend would duplicate all of that to reach the same triangles.
 * So this device does no rendering of its own; it translates PEGpu*
 * packets into an R200DrawPacket -- the renderer's actual input, not R200
 * register writes -- and calls draw_r200().  Constraint: nothing here may
 * read the emulated R200's device state, and nothing does; the translation
 * is from the guest's packet to the renderer's struct and stops there.
 *
 * Why the ring's data area is also the render target
 * --------------------------------------------------
 * The renderer takes a base pointer and treats offsets within it as VRAM,
 * rendering in place so that 2D work and scanout observe the result with no
 * copies.  Giving it the shared BAR mapping as that base makes the guest's
 * shared memory the framebuffer: the guest writes vertices into it, the GPU
 * renders into it, and the scanout reads it, all at the same addresses.
 * On Darwin the mapping therefore has to be a Metal shared MTLBuffer (see
 * pe_gpu_realize), because metal_draw_r200() builds linear MTLTexture views
 * of that buffer and drops every draw without one.
 *
 * Known limitation of sharing the backend: the Metal R200 path keeps its
 * command buffer, its open encoder and its texture-view cache in file-scope
 * globals, and the view cache is keyed on (offset, size, pitch, format)
 * without the MTLBuffer the view belongs to.  Two devices rendering in the
 * same process can therefore collide on a key and get each other's memory.
 * Nothing does that today -- a guest drives either the emulated R200 or
 * this device, not both -- but the two must not be used together until the
 * cache key carries its buffer.
 *
 * Byte order
 * ----------
 * Everything the guest writes is big-endian.  Conversion happens exactly
 * once, at the top of each pe_gpu_exec_* handler, into the host-native
 * PEGpuHostState / PEGpuHostTexture / R200DrawPacket structures.  Below
 * that boundary all values are host-native; above it, none are.  The one
 * exception is pixel data, which stays in the guest's big-endian ARGB byte
 * order all the way to the renderer -- that is the convention VRAM already
 * uses (see qemu-ppc-gpu-endianness-and-tiling-are-correct) and the Metal
 * shaders unswizzle it themselves.
 *
 * Copyright (c) 2026 Spartan0285
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/bswap.h"
#include "qapi/error.h"
#include "hw/pci/pci_device.h"
#include "hw/qdev-properties.h"
#include "migration/vmstate.h"
#include "qom/object.h"
#include "ui/console.h"
#include "hw/display/poweremu_gpu_ring.h"
#include "ppc_mac_gpu_renderer.h"

#define TYPE_POWEREMU_GPU "poweremu-gpu"
OBJECT_DECLARE_SIMPLE_TYPE(PowerEmuGPUState, POWEREMU_GPU)

/*
 * Caps on what one packet may ask us to allocate.  The data area is 32 MB,
 * so an unbounded vertex_count would let the guest ask for gigabytes of
 * host R200Vertex (144 bytes each) from a 32-byte packet.  These are not
 * hardware limits, they are the point past which a batch is a bug.
 */
#define PE_GPU_MAX_VERTS    65536u
#define PE_GPU_MAX_INDICES  (PE_GPU_MAX_VERTS * 3u)

/*
 * Metal will only build a linear texture view whose offset and bytes-per-row
 * are multiples of minimumLinearTextureAlignmentForPixelFormat.  That is 256
 * for every colour format we use on Apple GPUs, and it is not queryable from
 * C, so textures that do not meet it are copied instead of viewed in place.
 */
#define PE_GPU_LINEAR_ALIGN 256u

/* R200 encodings we synthesise.  See ppc_mac_gpu_metal.m's shader source. */
#define R200_CFMT_ARGB8888      6u      /* RB3D_CNTL bits 13:10 */
#define R200_RB3D_BLEND_ENABLE  (1u << 0)
#define R200_BLEND_FACTOR_BASE  32u     /* bfactor(): 32 == ZERO */
#define R200_ZFUNC_SHIFT        4u      /* RB3D_ZSTENCILCNTL depth compare */
#define R200_ZFUNC_LESS         1u
#define R200_ZFUNC_ALWAYS       7u
#define R200_ZWRITE_ENABLE      (1u << 30)
#define R200_PP_CNTL_STAGE0     (1u << 12)

/* Host-native render state, rebuilt from each PE_GPU_PKT_STATE. */
typedef struct PEGpuHostState {
    bool valid;
    uint32_t target_offset;         /* bytes, from the shared mapping base */
    uint32_t target_pitch;          /* bytes */
    uint32_t target_width, target_height;
    uint32_t depth_offset;
    uint32_t depth_pitch;           /* bytes */
    uint32_t depth_bpp;             /* 2 or 4 */
    uint32_t flags;                 /* PE_GPU_ST_* */
    uint32_t blend_src, blend_dst;  /* already R200 bfactor() codes */
    uint32_t scissor[4];            /* x0, y0, x1, y1 exclusive */
} PEGpuHostState;

/* Host-native texture table entry, rebuilt from each PE_GPU_PKT_TEXTURE. */
typedef struct PEGpuHostTexture {
    bool valid;
    uint32_t offset;
    uint32_t width, height;
    uint32_t pitch;                 /* bytes */
    uint32_t bpp;                   /* bytes per texel */
    uint32_t format;                /* R200 TXFORMAT code */
    uint32_t filter;                /* raw PP_TXFILTER bits r200_sampler reads */
} PEGpuHostTexture;

struct PowerEmuGPUState {
    PCIDevice parent_obj;

    MemoryRegion bar;           /* what the guest sees */
    MemoryRegion ctrl;          /* trapping control page */
    MemoryRegion shared;        /* ring + data, plain RAM: no traps */

    uint8_t *shared_ptr;        /* host view of `shared` */
    void *metal_vram_opaque;    /* ppc_mac_gpu_metal_alloc_vram() handle */

    PPCMacGPURenderer *renderer;    /* NULL: consume packets, draw nothing */
    void *renderer_opaque;

    QemuConsole *con;
    uint8_t *shadow;            /* scanout, byte-swapped for the UI */
    size_t shadow_size;
    uint32_t surface_width, surface_height;
    uint8_t *surface_data;

    /* Published by PE_GPU_PKT_PRESENT; read by the display update. */
    bool scan_valid;
    uint32_t scan_offset, scan_pitch, scan_width, scan_height;

    PEGpuHostState state;
    PEGpuHostTexture tex[PE_GPU_MAX_TEXTURES];

    uint32_t enable;
    uint32_t head;              /* guest's ring head, bytes */
    uint32_t tail;              /* what we have consumed */
    uint32_t fence;             /* last completed fence value */
    uint32_t error;

    /* Statistics, readable with `qom-get ... stats`. */
    uint64_t batches, packets, draws, vertices, textures, blits, presents;
    uint64_t rejected, dropped;
};

static uint8_t *pe_gpu_ring(PowerEmuGPUState *s)
{
    return s->shared_ptr + (PE_GPU_RING_OFFSET - PE_GPU_RING_OFFSET);
}

/* The data area, where vertices and texels live. */
static bool pe_gpu_data_ok(PowerEmuGPUState *s, uint32_t off, uint32_t len)
{
    uint64_t end = (uint64_t)off + len;

    return off >= PE_GPU_DATA_BASE &&
           end <= (uint64_t)PE_GPU_DATA_BASE + PE_GPU_DATA_BYTES;
}

/*
 * A width x height rectangle at (x, y) in a surface of `pitch` bytes per
 * row: every byte it can touch must be inside the data area, and the rows
 * must not overlap each other.
 */
static bool pe_gpu_rect_ok(PowerEmuGPUState *s, uint32_t off, uint32_t pitch,
                           uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                           uint32_t bpp)
{
    uint64_t row_end = ((uint64_t)x + w) * bpp;
    uint64_t last_row, end;

    if (!w || !h || !pitch || row_end > pitch) {
        return false;
    }
    last_row = (uint64_t)off + ((uint64_t)y + h - 1) * pitch;
    end = last_row + row_end;
    if (end > UINT32_MAX) {
        return false;
    }
    return pe_gpu_data_ok(s, off, (uint32_t)(end - off));
}

/*
 * A whole surface: width x height at `off` with `pitch` bytes per row.
 *
 * Stricter than pe_gpu_rect_ok() on purpose -- it demands that the padding
 * after the last row be in range too.  Both the renderer and our own
 * memory_region_set_dirty() calls describe such a surface as pitch * height
 * bytes, so a surface whose final row is short would either be refused
 * silently by the backend or dirty memory past the end of the region.
 */
static bool pe_gpu_surface_ok(PowerEmuGPUState *s, uint32_t off, uint32_t pitch,
                              uint32_t w, uint32_t h, uint32_t bpp)
{
    uint64_t span = (uint64_t)pitch * h;

    if (!w || !h || !pitch || (uint64_t)w * bpp > pitch || span > UINT32_MAX) {
        return false;
    }
    return pe_gpu_data_ok(s, off, (uint32_t)span);
}

static void pe_gpu_fail(PowerEmuGPUState *s, uint32_t err, const char *why)
{
    if (!s->error) {              /* keep the first failure, not the last */
        s->error = err;
        qemu_log_mask(LOG_GUEST_ERROR, "poweremu-gpu: %s\n", why);
    }
    s->rejected++;
}

/*
 * A packet that is well-formed but that the host cannot execute right now
 * (no renderer, a render target Metal will not take a view of).  The guest
 * is not at fault, so nothing is latched and the batch continues -- the
 * device is allowed to be a no-op, it is not allowed to stall.
 */
static void pe_gpu_drop(PowerEmuGPUState *s)
{
    s->dropped++;
}

/* IEEE-754 bits, already host-native, to a float. */
static float pe_gpu_f32(uint32_t bits)
{
    float f;

    QEMU_BUILD_BUG_ON(sizeof(f) != sizeof(bits));
    memcpy(&f, &bits, sizeof(f));
    return f;
}

/*
 * Mark guest-visible memory as changed.
 *
 * Guarded because the self-test drives this same code with a plain malloc'd
 * buffer and no MemoryRegion behind it; on the real device the region is
 * always RAM and the check is one predictable branch.
 */
static void pe_gpu_dirty(PowerEmuGPUState *s, uint64_t off, uint64_t len)
{
    if (memory_region_is_ram(&s->shared)) {
        memory_region_set_dirty(&s->shared, off, len);
    }
}

static void pe_gpu_flush(PowerEmuGPUState *s)
{
    if (s->renderer && s->renderer->flush_r200) {
        s->renderer->flush_r200(s->renderer_opaque);
    }
}

/*
 * Wait for batched GPU work that touches [lo, hi) before the CPU reads or
 * writes it.  range_busy_r200() lets the renderer say "nothing pending
 * there", which is the common case for a blit into a surface the 3D pipe
 * never rendered to.
 */
static void pe_gpu_sync_range(PowerEmuGPUState *s, uint64_t lo, uint64_t hi,
                              bool write)
{
    if (!s->renderer) {
        return;
    }
    if (s->renderer->range_busy_r200 &&
        !s->renderer->range_busy_r200(s->renderer_opaque, lo, hi, write)) {
        return;
    }
    pe_gpu_flush(s);
}

/* ====================================================================
 * PE_GPU_PKT_STATE
 * ==================================================================== */

static uint32_t pe_gpu_blend_factor(uint32_t code)
{
    /*
     * PE_GPU_BLEND_* is the R200 factor order biased to zero, so anything
     * in range translates by addition.  Out of range becomes ONE, which is
     * the renderer's own default for an unknown factor.
     */
    if (code > PE_GPU_BLEND_LAST) {
        return R200_BLEND_FACTOR_BASE + PE_GPU_BLEND_ONE;
    }
    return R200_BLEND_FACTOR_BASE + code;
}

static bool pe_gpu_exec_state(PowerEmuGPUState *s, const PEGpuState *p)
{
    PEGpuHostState st = { 0 };
    uint32_t blend = be32_to_cpu(p->blend);
    int32_t sx, sy, sw, sh;

    /* Big-endian in, host-native from here down. */
    st.target_offset = be32_to_cpu(p->target_offset);
    st.target_pitch = be32_to_cpu(p->target_pitch);
    st.target_width = be16_to_cpu(p->target_width);
    st.target_height = be16_to_cpu(p->target_height);
    st.depth_offset = be32_to_cpu(p->depth_offset);
    st.depth_pitch = be32_to_cpu(p->depth_pitch);
    st.flags = be16_to_cpu(p->flags);
    st.blend_src = pe_gpu_blend_factor(blend & 0xFFFF);
    st.blend_dst = pe_gpu_blend_factor(blend >> 16);

    if (!pe_gpu_surface_ok(s, st.target_offset, st.target_pitch,
                           st.target_width, st.target_height, 4)) {
        pe_gpu_fail(s, PE_GPU_ERR_BOUNDS, "render target outside the BAR");
        return false;
    }
    /*
     * The renderer states pitches in pixels, so a byte pitch that is not a
     * whole number of pixels cannot be expressed to it -- the division would
     * quietly describe a narrower surface than the guest wrote.
     */
    if (st.target_pitch % 4) {
        pe_gpu_fail(s, PE_GPU_ERR_PACKET, "render target pitch not 4-aligned");
        return false;
    }

    if (st.flags & (PE_GPU_ST_DEPTH_TEST | PE_GPU_ST_DEPTH_WRITE)) {
        st.depth_bpp = be16_to_cpu(p->depth_bits) == 16 ? 2 : 4;
        if (!st.depth_offset ||
            !pe_gpu_surface_ok(s, st.depth_offset, st.depth_pitch,
                               st.target_width, st.target_height,
                               st.depth_bpp)) {
            pe_gpu_fail(s, PE_GPU_ERR_BOUNDS, "depth buffer outside the BAR");
            return false;
        }
        if (st.depth_pitch % st.depth_bpp) {
            pe_gpu_fail(s, PE_GPU_ERR_PACKET, "depth pitch not a whole pixel");
            return false;
        }
    }

    /*
     * The scissor arrives signed because a guest clipping against a window
     * origin naturally produces negatives; clamp it into the target here so
     * the renderer only ever sees an ordered, in-range rectangle.
     */
    sx = (int16_t)be16_to_cpu(p->scissor_x);
    sy = (int16_t)be16_to_cpu(p->scissor_y);
    sw = (int16_t)be16_to_cpu(p->scissor_w);
    sh = (int16_t)be16_to_cpu(p->scissor_h);
    if (sw <= 0 || sh <= 0) {            /* no scissor: the whole target */
        st.scissor[0] = 0;
        st.scissor[1] = 0;
        st.scissor[2] = st.target_width;
        st.scissor[3] = st.target_height;
    } else {
        int64_t x1 = (int64_t)sx + sw, y1 = (int64_t)sy + sh;
        st.scissor[0] = MIN(MAX(sx, 0), (int32_t)st.target_width);
        st.scissor[1] = MIN(MAX(sy, 0), (int32_t)st.target_height);
        st.scissor[2] = MIN(MAX(x1, 0), (int64_t)st.target_width);
        st.scissor[3] = MIN(MAX(y1, 0), (int64_t)st.target_height);
    }

    st.valid = true;
    s->state = st;
    return true;
}

/* ====================================================================
 * PE_GPU_PKT_TEXTURE
 * ==================================================================== */

static bool pe_gpu_exec_texture(PowerEmuGPUState *s, const PEGpuTexture *p)
{
    PEGpuHostTexture t = { 0 };
    uint32_t id = be32_to_cpu(p->id);
    uint32_t format = be16_to_cpu(p->format);
    uint32_t filter = be16_to_cpu(p->filter);
    uint32_t wrap = be16_to_cpu(p->wrap);

    if (id == 0 || id >= PE_GPU_MAX_TEXTURES) {
        pe_gpu_fail(s, PE_GPU_ERR_PACKET, "texture id out of range");
        return false;
    }

    t.offset = be32_to_cpu(p->data_offset);
    t.width = be16_to_cpu(p->width);
    t.height = be16_to_cpu(p->height);
    t.pitch = be16_to_cpu(p->pitch);

    /*
     * PE_GPU_TEX_* was chosen to be the R200 TXFORMAT code already:
     * 3 = ARGB1555, 4 = RGB565, 5 = ARGB4444, 6 = ARGB8888.  The protocol
     * numbers them from zero, so shift the two that differ rather than
     * carrying a table the shader would disagree with.
     */
    switch (format) {
    case PE_GPU_TEX_ARGB8888: t.format = 6; break;
    case PE_GPU_TEX_RGB565:   t.format = 4; break;
    case PE_GPU_TEX_ARGB1555: t.format = 3; break;
    case PE_GPU_TEX_ARGB4444: t.format = 5; break;
    default:
        pe_gpu_fail(s, PE_GPU_ERR_PACKET, "unknown texture format");
        return false;
    }

    t.bpp = t.format == 6 ? 4 : 2;
    if (!pe_gpu_surface_ok(s, t.offset, t.pitch, t.width, t.height, t.bpp)) {
        pe_gpu_fail(s, PE_GPU_ERR_BOUNDS, "texture outside the BAR");
        return false;
    }

    /*
     * PP_TXFILTER as r200_sampler() decodes it: bit 0 mag linear, bits 1-4
     * min linear, bits 23-25 and 27-29 the S and T address modes, where 0
     * repeats and 2 clamps to edge.
     */
    t.filter = filter ? 0x3u : 0u;
    if (wrap) {
        t.filter |= (2u << 23) | (2u << 27);
    }

    t.valid = true;
    s->tex[id] = t;
    return true;
}

/*
 * Bind a texture table entry into a draw packet's unit 0.
 *
 * Metal can view the texels in place -- zero copy, straight out of the
 * guest's shared memory -- only when the offset and pitch satisfy the
 * linear-view alignment.  When they do not, the renderer would warn and
 * skip the unit, so copy the texels into host_data instead, which has no
 * alignment rule.  The copy is freed by the caller right after the draw:
 * the backend uploads it synchronously in replaceRegion:.
 */
static void pe_gpu_bind_texture(PowerEmuGPUState *s, R200TexUnit *tu,
                                const PEGpuHostTexture *t)
{
    memset(tu, 0, sizeof(*tu));
    tu->enabled = 1;
    tu->offset = t->offset;
    tu->pitch = t->pitch;
    tu->width = t->width;
    tu->height = t->height;
    tu->format = t->format;
    tu->alpha_in_map = 1;       /* the protocol's formats all carry alpha */
    tu->filter = t->filter;
    tu->denorm = 0;             /* s,t are 0..1 */
    tu->swap = 0;               /* texels are already in the VRAM byte order */

    if ((t->offset % PE_GPU_LINEAR_ALIGN) || (t->pitch % PE_GPU_LINEAR_ALIGN)) {
        size_t bytes = (size_t)t->pitch * t->height;
        uint8_t *copy = g_malloc(bytes);

        memcpy(copy, s->shared_ptr + t->offset, bytes);
        tu->host_data = copy;
    }
}

/* ====================================================================
 * PE_GPU_PKT_DRAW
 * ==================================================================== */

/*
 * Expand one primitive type into the flat index list draw_r200() wants.
 * `src` is the guest's index list, or NULL for a sequential draw.  Returns
 * the number of indices written, or 0 if `count` cannot form a primitive.
 */
static uint32_t pe_gpu_expand(uint32_t prim, const uint32_t *src,
                              uint32_t count, uint32_t *out)
{
#define PE_IDX(i)   (src ? src[i] : (i))
    uint32_t n = 0, i;

    switch (prim) {
    case PE_GPU_PRIM_TRIANGLES:
        for (i = 0; i + 2 < count; i += 3) {
            out[n++] = PE_IDX(i);
            out[n++] = PE_IDX(i + 1);
            out[n++] = PE_IDX(i + 2);
        }
        break;
    case PE_GPU_PRIM_STRIP:
        for (i = 0; i + 2 < count; i++) {
            /* Flip odd triangles so the strip keeps one winding order. */
            out[n++] = PE_IDX(i & 1 ? i + 1 : i);
            out[n++] = PE_IDX(i & 1 ? i : i + 1);
            out[n++] = PE_IDX(i + 2);
        }
        break;
    case PE_GPU_PRIM_FAN:
        for (i = 0; i + 2 < count; i++) {
            out[n++] = PE_IDX(0);
            out[n++] = PE_IDX(i + 1);
            out[n++] = PE_IDX(i + 2);
        }
        break;
    case PE_GPU_PRIM_LINES:
        for (i = 0; i + 1 < count; i += 2) {
            out[n++] = PE_IDX(i);
            out[n++] = PE_IDX(i + 1);
        }
        break;
    case PE_GPU_PRIM_POINTS:
        for (i = 0; i < count; i++) {
            out[n++] = PE_IDX(i);
        }
        break;
    default:
        break;
    }
    return n;
#undef PE_IDX
}

static uint32_t pe_gpu_prim_class(uint32_t prim)
{
    switch (prim) {
    case PE_GPU_PRIM_LINES:  return 1;
    case PE_GPU_PRIM_POINTS: return 2;
    default:                 return 0;
    }
}

/* How many indices pe_gpu_expand() will emit for `count` inputs. */
static uint32_t pe_gpu_expanded_count(uint32_t prim, uint32_t count)
{
    switch (prim) {
    case PE_GPU_PRIM_TRIANGLES: return (count / 3) * 3;
    case PE_GPU_PRIM_STRIP:
    case PE_GPU_PRIM_FAN:       return count < 3 ? 0 : (count - 2) * 3;
    case PE_GPU_PRIM_LINES:     return (count / 2) * 2;
    case PE_GPU_PRIM_POINTS:    return count;
    default:                    return 0;
    }
}

static bool pe_gpu_exec_draw(PowerEmuGPUState *s, const PEGpuDraw *p)
{
    const PEGpuHostState *st = &s->state;
    uint32_t vertex_offset = be32_to_cpu(p->vertex_offset);
    uint32_t vertex_count = be32_to_cpu(p->vertex_count);
    uint32_t stride = be16_to_cpu(p->vertex_stride);
    uint32_t prim = be16_to_cpu(p->prim);
    uint32_t index_offset = be32_to_cpu(p->index_offset);
    uint32_t index_count = be32_to_cpu(p->index_count);
    uint32_t texture_id = be32_to_cpu(p->texture_id);
    uint32_t count = index_offset ? index_count : vertex_count;
    uint32_t nidx, i;
    uint32_t *src = NULL, *idx = NULL;
    R200Vertex *verts = NULL;
    R200DrawPacket pkt = { 0 };     /* the `out:` path frees pkt.tex[0] */
    const uint8_t *vbase;
    bool ok = false;

    if (!st->valid) {
        pe_gpu_fail(s, PE_GPU_ERR_PACKET, "draw before any state packet");
        return false;
    }
    if (!vertex_count || vertex_count > PE_GPU_MAX_VERTS ||
        stride < sizeof(PEGpuVertex)) {
        pe_gpu_fail(s, PE_GPU_ERR_PACKET, "draw vertex count or stride bad");
        return false;
    }
    if (!pe_gpu_data_ok(s, vertex_offset,
                        (vertex_count - 1) * stride + sizeof(PEGpuVertex))) {
        pe_gpu_fail(s, PE_GPU_ERR_BOUNDS, "draw vertices outside the BAR");
        return false;
    }
    if (index_offset) {
        if (!index_count || index_count > PE_GPU_MAX_INDICES ||
            !pe_gpu_data_ok(s, index_offset, index_count * 2)) {
            pe_gpu_fail(s, PE_GPU_ERR_BOUNDS, "draw indices outside the BAR");
            return false;
        }
    }
    nidx = pe_gpu_expanded_count(prim, count);
    if (!nidx || nidx > PE_GPU_MAX_INDICES) {
        pe_gpu_fail(s, PE_GPU_ERR_PACKET, "draw primitive count bad");
        return false;
    }
    if ((st->flags & PE_GPU_ST_TEXTURE) && texture_id &&
        (texture_id >= PE_GPU_MAX_TEXTURES || !s->tex[texture_id].valid)) {
        pe_gpu_fail(s, PE_GPU_ERR_PACKET, "draw names an unknown texture");
        return false;
    }

    if (index_offset) {
        const uint8_t *ip = s->shared_ptr + index_offset;

        src = g_new(uint32_t, index_count);
        for (i = 0; i < index_count; i++) {
            src[i] = lduw_be_p(ip + i * 2);
            if (src[i] >= vertex_count) {
                pe_gpu_fail(s, PE_GPU_ERR_BOUNDS, "draw index out of range");
                goto out;
            }
        }
    }

    /*
     * Counted once the packet is known to be well-formed, and before the
     * renderer is consulted: the statistics describe what the guest validly
     * asked for, so they stay meaningful on a host with no Metal and do not
     * count draws that were rejected.
     */
    s->draws++;
    s->vertices += vertex_count;

    /*
     * Big-endian guest vertices to host-native R200Vertex.  ld*_be_p() is
     * used rather than a struct cast because vertex_stride may put a vertex
     * on any 1-byte boundary the guest likes.
     */
    vbase = s->shared_ptr + vertex_offset;
    verts = g_new0(R200Vertex, vertex_count);
    for (i = 0; i < vertex_count; i++) {
        const uint8_t *v = vbase + (size_t)i * stride;
        uint32_t colour = ldl_be_p(v + offsetof(PEGpuVertex, colour));

        verts[i].pos[0] = pe_gpu_f32(ldl_be_p(v + offsetof(PEGpuVertex, x)));
        verts[i].pos[1] = pe_gpu_f32(ldl_be_p(v + offsetof(PEGpuVertex, y)));
        verts[i].pos[2] = pe_gpu_f32(ldl_be_p(v + offsetof(PEGpuVertex, z)));
        verts[i].pos[3] = pe_gpu_f32(ldl_be_p(v + offsetof(PEGpuVertex, w)));
        verts[i].color[0] = ((colour >> 16) & 0xFF) / 255.0f;
        verts[i].color[1] = ((colour >> 8) & 0xFF) / 255.0f;
        verts[i].color[2] = (colour & 0xFF) / 255.0f;
        verts[i].color[3] = (colour >> 24) / 255.0f;
        verts[i].tex[0][0] = pe_gpu_f32(ldl_be_p(v + offsetof(PEGpuVertex, s)));
        verts[i].tex[0][1] = pe_gpu_f32(ldl_be_p(v + offsetof(PEGpuVertex, t)));
        verts[i].tex[0][3] = 1.0f;      /* q: the shader divides by it */
    }

    idx = g_new(uint32_t, nidx);
    nidx = pe_gpu_expand(prim, src, count, idx);

    pkt.rt_offset = st->target_offset;
    pkt.rt_pitch = st->target_pitch / 4;         /* the renderer wants pixels */
    pkt.rt_format = R200_CFMT_ARGB8888;
    pkt.rt_width = st->target_width;
    pkt.rt_height = st->target_height;
    pkt.rb3d_cntl = R200_CFMT_ARGB8888 << 10;
    pkt.plane_mask = 0xFFFFFFFFu;                /* all ones: no masking */
    pkt.prim_class = pe_gpu_prim_class(prim);
    for (i = 0; i < 4; i++) {
        pkt.scissor[i] = st->scissor[i];
    }

    if (st->flags & PE_GPU_ST_BLEND) {
        pkt.rb3d_cntl |= R200_RB3D_BLEND_ENABLE;
        /* bcombine() 0 is saturating add; factors live at bits 21:16, 29:24. */
        pkt.cblend = (st->blend_src << 16) | (st->blend_dst << 24);
        pkt.ablend = pkt.cblend;                 /* separate alpha stays off */
    }

    if (st->flags & (PE_GPU_ST_DEPTH_TEST | PE_GPU_ST_DEPTH_WRITE)) {
        uint32_t func = (st->flags & PE_GPU_ST_DEPTH_TEST) ? R200_ZFUNC_LESS
                                                           : R200_ZFUNC_ALWAYS;
        pkt.depth_enable = 1;                    /* gates writes too */
        pkt.depth_offset = st->depth_offset;
        pkt.depth_bpp = st->depth_bpp;
        pkt.depth_pitch = st->depth_pitch / st->depth_bpp;
        pkt.zstencil = func << R200_ZFUNC_SHIFT;
        if (st->flags & PE_GPU_ST_DEPTH_WRITE) {
            pkt.zstencil |= R200_ZWRITE_ENABLE;
        }
    }

    if ((st->flags & PE_GPU_ST_TEXTURE) && texture_id) {
        pe_gpu_bind_texture(s, &pkt.tex[0], &s->tex[texture_id]);
        /*
         * Stage 0 modulates the texel by the interpolated colour:
         * A = tex0 (carg 10), B = diffuse (carg 4), C = 0, op MADD, result
         * saturated into R[0], which is what the shader emits.  With no
         * stage enabled it emits the diffuse colour instead, which is
         * exactly what an untextured draw wants -- so pp_cntl stays 0.
         */
        pkt.pp_cntl = R200_PP_CNTL_STAGE0;
        pkt.txcblend[0] = 10u | (4u << 5);
        pkt.txcblend2[0] = (1u << 12) | (1u << 16);
        pkt.txablend[0] = 10u | (4u << 5);
        pkt.txablend2[0] = (1u << 12) | (1u << 16);
    }

    pkt.verts = verts;
    pkt.num_verts = vertex_count;
    pkt.indices = idx;
    pkt.num_indices = nidx;

    if (!s->renderer || !s->renderer->draw_r200) {
        pe_gpu_drop(s);                          /* no-op host: constraint 5 */
    } else if (s->renderer->draw_r200(s->renderer_opaque, s->shared_ptr,
                                      PE_GPU_SHARED_BYTES, &pkt) == 0) {
        pe_gpu_dirty(s, st->target_offset,
                     (uint64_t)st->target_height * st->target_pitch);
    } else {
        pe_gpu_drop(s);                          /* backend refused the packet */
    }
    ok = true;

out:
    g_free((void *)pkt.tex[0].host_data);
    g_free(verts);
    g_free(idx);
    g_free(src);
    return ok;
}

/* ====================================================================
 * PE_GPU_PKT_BLIT
 *
 * Done on the CPU rather than through renderer->blit_2d().  That entry
 * point only succeeds for surfaces the Metal backend already holds a
 * shadow render target for, which is an artefact of how the emulated R200
 * composites; a paravirtual blit has no such shadow and would always take
 * the -1 fallback.  Going straight to memory is both correct and shorter,
 * as long as pending GPU work touching either surface is flushed first.
 * ==================================================================== */

static bool pe_gpu_exec_blit(PowerEmuGPUState *s, const PEGpuBlit *p)
{
    uint32_t src_offset = be32_to_cpu(p->src_offset);
    uint32_t dst_offset = be32_to_cpu(p->dst_offset);
    uint32_t src_pitch = be32_to_cpu(p->src_pitch);
    uint32_t dst_pitch = be32_to_cpu(p->dst_pitch);
    uint32_t src_x = be16_to_cpu(p->src_x), src_y = be16_to_cpu(p->src_y);
    uint32_t dst_x = be16_to_cpu(p->dst_x), dst_y = be16_to_cpu(p->dst_y);
    uint32_t w = be16_to_cpu(p->width), h = be16_to_cpu(p->height);
    uint32_t colour = be32_to_cpu(p->colour);
    uint64_t dlo, dhi;
    uint32_t y;

    /* The protocol carries no bpp: blits are 32bpp, like the render target. */
    if (!pe_gpu_rect_ok(s, dst_offset, dst_pitch, dst_x, dst_y, w, h, 4)) {
        pe_gpu_fail(s, PE_GPU_ERR_BOUNDS, "blit destination outside the BAR");
        return false;
    }
    dlo = (uint64_t)dst_offset + (uint64_t)dst_y * dst_pitch;
    dhi = (uint64_t)dst_offset + ((uint64_t)dst_y + h) * dst_pitch;

    if (src_pitch == 0) {                        /* solid fill */
        pe_gpu_sync_range(s, dlo, dhi, true);
        for (y = 0; y < h; y++) {
            uint8_t *row = s->shared_ptr + dst_offset +
                           (size_t)(dst_y + y) * dst_pitch + (size_t)dst_x * 4;
            uint32_t x;
            /* stl_be_p, not a uint32_t store: the guest picks the pitch and
             * nothing here guarantees a 4-byte aligned row. */
            for (x = 0; x < w; x++) {
                stl_be_p(row + x * 4, colour);
            }
        }
    } else {
        uint64_t slo, shi;

        if (!pe_gpu_rect_ok(s, src_offset, src_pitch, src_x, src_y, w, h, 4)) {
            pe_gpu_fail(s, PE_GPU_ERR_BOUNDS, "blit source outside the BAR");
            return false;
        }
        slo = (uint64_t)src_offset + (uint64_t)src_y * src_pitch;
        shi = (uint64_t)src_offset + ((uint64_t)src_y + h) * src_pitch;
        pe_gpu_sync_range(s, MIN(slo, dlo), MAX(shi, dhi), true);

        /*
         * Copy bottom-up when the destination is below an overlapping
         * source, so a downward scroll does not smear its own rows.
         */
        for (y = 0; y < h; y++) {
            uint32_t ry = (dst_offset == src_offset && dst_y > src_y)
                          ? h - 1 - y : y;
            const uint8_t *sp = s->shared_ptr + src_offset +
                                (size_t)(src_y + ry) * src_pitch + src_x * 4;
            uint8_t *dp = s->shared_ptr + dst_offset +
                          (size_t)(dst_y + ry) * dst_pitch + dst_x * 4;
            memmove(dp, sp, (size_t)w * 4);
        }
    }

    pe_gpu_dirty(s, dlo, dhi - dlo);
    return true;
}

/* ====================================================================
 * PE_GPU_PKT_PRESENT
 * ==================================================================== */

static bool pe_gpu_exec_present(PowerEmuGPUState *s, const PEGpuPresent *p)
{
    uint32_t offset = be32_to_cpu(p->offset);
    uint32_t pitch = be32_to_cpu(p->pitch);
    uint32_t width = be16_to_cpu(p->width);
    uint32_t height = be16_to_cpu(p->height);

    if (!pe_gpu_surface_ok(s, offset, pitch, width, height, 4)) {
        pe_gpu_fail(s, PE_GPU_ERR_BOUNDS, "scanout outside the BAR");
        return false;
    }

    s->scan_offset = offset;
    s->scan_pitch = pitch;
    s->scan_width = width;
    s->scan_height = height;
    s->scan_valid = true;

    /*
     * The frame is only finished once the renderer's batched work has
     * landed in memory; the display update reads those same bytes.
     */
    pe_gpu_flush(s);
    pe_gpu_dirty(s, offset, (uint64_t)pitch * height);
    return true;
}

/* ====================================================================
 * Display
 * ==================================================================== */

static void pe_gpu_gfx_update(void *opaque)
{
    PowerEmuGPUState *s = opaque;
    uint32_t w = s->scan_width, h = s->scan_height, pitch = s->scan_pitch;
    size_t need;
    uint32_t y;

    if (!s->enable || !s->scan_valid) {
        return;                     /* the guest has not taken over scanout */
    }
    pe_gpu_flush(s);

    need = (size_t)w * h * 4;
    if (s->shadow_size < need) {
        g_free(s->shadow);
        s->shadow = g_malloc(need);
        s->shadow_size = need;
    }

    /*
     * Replace the surface only when the geometry actually changed: each
     * replacement runs every UI listener's gfx_switch, which copies the
     * whole frame.  Contents reach the UI through dpy_gfx_update_full.
     */
    if (w != s->surface_width || h != s->surface_height ||
        s->shadow != s->surface_data) {
        DisplaySurface *ds = qemu_create_displaysurface_from(
            w, h, PIXMAN_x8r8g8b8, w * 4, s->shadow);

        s->surface_width = w;
        s->surface_height = h;
        s->surface_data = s->shadow;
        dpy_gfx_replace_surface(s->con, ds);
    }

    /*
     * The shared area holds big-endian ARGB, the byte order the renderer
     * and the guest agree on; the Cocoa front end only takes 32bpp
     * little-endian.  One bswap per pixel is the whole conversion.
     */
    for (y = 0; y < h; y++) {
        const uint8_t *sp = s->shared_ptr + s->scan_offset + (size_t)y * pitch;
        uint32_t *dp = (uint32_t *)s->shadow + (size_t)y * w;
        uint32_t x;

        for (x = 0; x < w; x++) {
            dp[x] = ldl_be_p(sp + x * 4);
        }
    }

    dpy_gfx_update_full(s->con);
}

static const GraphicHwOps pe_gpu_gfx_ops = {
    .gfx_update = pe_gpu_gfx_update,
};

/* ====================================================================
 * Ring
 * ==================================================================== */

/*
 * Execute one packet.  Returns false when the batch must stop, which only
 * happens for a packet the guest got wrong.
 */
static bool pe_gpu_packet(PowerEmuGPUState *s, const PEGpuPacketHdr *hdr,
                          uint32_t avail)
{
    uint32_t type = be16_to_cpu(hdr->type);
    uint32_t bytes = be32_to_cpu(hdr->bytes);

    if (bytes < sizeof(*hdr) || bytes > avail || (bytes & 7)) {
        pe_gpu_fail(s, PE_GPU_ERR_PACKET, "packet size out of range");
        return false;
    }
    s->packets++;

    switch (type) {
    case PE_GPU_PKT_NOP:
        break;

    case PE_GPU_PKT_FENCE: {
        const PEGpuFence *f = (const PEGpuFence *)hdr;
        if (bytes < sizeof(*f)) {
            pe_gpu_fail(s, PE_GPU_ERR_PACKET, "short fence");
            return false;
        }
        /*
         * A fence must not be published before the work it follows is
         * actually done, or the guest will reuse vertex memory the GPU is
         * still reading.  Waiting here is the conservative choice; an
         * asynchronous fence via submit_r200()'s completion callback would
         * cost less, and needs the callback to be made safe against the
         * BQL first.
         */
        pe_gpu_flush(s);
        s->fence = be32_to_cpu(f->value);
        break;
    }

    case PE_GPU_PKT_STATE:
        if (bytes < sizeof(PEGpuState)) {
            pe_gpu_fail(s, PE_GPU_ERR_PACKET, "short state");
            return false;
        }
        return pe_gpu_exec_state(s, (const PEGpuState *)hdr);

    case PE_GPU_PKT_DRAW:
        if (bytes < sizeof(PEGpuDraw)) {
            pe_gpu_fail(s, PE_GPU_ERR_PACKET, "short draw");
            return false;
        }
        return pe_gpu_exec_draw(s, (const PEGpuDraw *)hdr);

    case PE_GPU_PKT_TEXTURE:
        if (bytes < sizeof(PEGpuTexture)) {
            pe_gpu_fail(s, PE_GPU_ERR_PACKET, "short texture");
            return false;
        }
        if (!pe_gpu_exec_texture(s, (const PEGpuTexture *)hdr)) {
            return false;
        }
        s->textures++;
        break;

    case PE_GPU_PKT_BLIT:
        if (bytes < sizeof(PEGpuBlit)) {
            pe_gpu_fail(s, PE_GPU_ERR_PACKET, "short blit");
            return false;
        }
        if (!pe_gpu_exec_blit(s, (const PEGpuBlit *)hdr)) {
            return false;
        }
        s->blits++;
        break;

    case PE_GPU_PKT_PRESENT:
        if (bytes < sizeof(PEGpuPresent)) {
            pe_gpu_fail(s, PE_GPU_ERR_PACKET, "short present");
            return false;
        }
        if (!pe_gpu_exec_present(s, (const PEGpuPresent *)hdr)) {
            return false;
        }
        s->presents++;
        break;

    default:
        /* Unknown but well-formed: skip it, so the protocol can grow. */
        break;
    }
    return true;
}

/* Consume everything the guest has published up to `head`. */
static void pe_gpu_run(PowerEmuGPUState *s, uint32_t head)
{
    uint8_t *ring = pe_gpu_ring(s);

    if (head > PE_GPU_RING_BYTES || (head & 7)) {
        pe_gpu_fail(s, PE_GPU_ERR_PACKET, "doorbell outside the ring");
        return;
    }
    s->batches++;
    s->head = head;

    while (s->tail < head) {
        const PEGpuPacketHdr *hdr = (const PEGpuPacketHdr *)(ring + s->tail);
        uint32_t avail = head - s->tail;
        uint32_t bytes;

        if (avail < sizeof(*hdr)) {
            pe_gpu_fail(s, PE_GPU_ERR_PACKET, "truncated header");
            break;
        }
        if (!pe_gpu_packet(s, hdr, avail)) {
            break;
        }
        bytes = be32_to_cpu(hdr->bytes);
        s->tail += bytes;
    }
    /*
     * The batch is over either way.  Abandoning the rest of a failed batch
     * rather than leaving the tail parked mid-packet matters: the error is
     * latched until the guest acknowledges it, and a tail that never moves
     * would make every later doorbell re-parse the same bad packet.
     */
    s->tail = 0;
    s->head = 0;
}

static uint64_t pe_gpu_ctrl_read(void *opaque, hwaddr addr, unsigned size)
{
    PowerEmuGPUState *s = opaque;

    switch (addr) {
    case PE_GPU_REG_MAGIC:    return PE_GPU_MAGIC;
    case PE_GPU_REG_VERSION:  return PE_GPU_VERSION;
    case PE_GPU_REG_FEATURES: return PE_GPU_FEAT_DRAW | PE_GPU_FEAT_BLIT |
                                     PE_GPU_FEAT_TEXTURE;
    case PE_GPU_REG_ENABLE:   return s->enable;
    case PE_GPU_REG_TAIL:     return s->tail;
    case PE_GPU_REG_FENCE:    return s->fence;
    case PE_GPU_REG_ERROR:    return s->error;
    default:                  return 0;
    }
}

static void pe_gpu_ctrl_write(void *opaque, hwaddr addr, uint64_t val,
                              unsigned size)
{
    PowerEmuGPUState *s = opaque;

    switch (addr) {
    case PE_GPU_REG_ENABLE:
        s->enable = val & 1;
        break;
    case PE_GPU_REG_DOORBELL:
        if (s->enable) {
            pe_gpu_run(s, val);
        }
        break;
    case PE_GPU_REG_ERROR:
        s->error = PE_GPU_ERR_NONE;         /* write to acknowledge */
        break;
    default:
        break;
    }
}

static const MemoryRegionOps pe_gpu_ctrl_ops = {
    .read = pe_gpu_ctrl_read,
    .write = pe_gpu_ctrl_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

static char *pe_gpu_get_stats(Object *obj, Error **errp)
{
    PowerEmuGPUState *s = POWEREMU_GPU(obj);

    return g_strdup_printf("renderer=%s batches=%" PRIu64 " packets=%" PRIu64
                           " draws=%" PRIu64 " vertices=%" PRIu64
                           " textures=%" PRIu64 " blits=%" PRIu64
                           " presents=%" PRIu64 " rejected=%" PRIu64
                           " dropped=%" PRIu64 " fence=%u error=%u",
                           s->renderer ? s->renderer->name : "none",
                           s->batches, s->packets, s->draws, s->vertices,
                           s->textures, s->blits, s->presents, s->rejected,
                           s->dropped, s->fence, s->error);
}

/* ====================================================================
 * Self-test
 *
 * The transport is plain memory and pure C: no PCI bus, no guest and no
 * Metal device are needed to prove that well-formed packets advance the
 * counters and malformed ones are rejected.  It runs with no renderer
 * attached, which is also the no-op host path from constraint 5, so a
 * failure here is a transport or validation bug and never a backend one.
 * ==================================================================== */

typedef struct PEGpuTestRing {
    uint8_t *base;              /* the ring, inside the shared mapping */
    uint32_t used;
} PEGpuTestRing;

static void *pe_gpu_test_emit(PEGpuTestRing *r, uint32_t type, size_t bytes)
{
    PEGpuPacketHdr *h = (PEGpuPacketHdr *)(r->base + r->used);
    uint32_t padded = ROUND_UP(bytes, 8);

    memset(h, 0, padded);
    h->type = cpu_to_be16(type);
    h->bytes = cpu_to_be32(padded);
    r->used += padded;
    return h;
}

static void pe_gpu_test_vertex(uint8_t *p, float x, float y, uint32_t colour)
{
    uint32_t xb, yb, one = 0x3F800000u;      /* 1.0f */

    memcpy(&xb, &x, 4);
    memcpy(&yb, &y, 4);
    memset(p, 0, sizeof(PEGpuVertex));
    stl_be_p(p + offsetof(PEGpuVertex, x), xb);
    stl_be_p(p + offsetof(PEGpuVertex, y), yb);
    stl_be_p(p + offsetof(PEGpuVertex, w), one);
    stl_be_p(p + offsetof(PEGpuVertex, colour), colour);
}

static int pe_gpu_test_failures;

static void pe_gpu_check(bool cond, const char *what)
{
    printf("%-46s %s\n", what, cond ? "ok" : "FAIL");
    if (!cond) {
        pe_gpu_test_failures++;
    }
}

/* Reset everything a previous case may have latched. */
static void pe_gpu_test_reset(PowerEmuGPUState *s, PEGpuTestRing *r)
{
    s->error = PE_GPU_ERR_NONE;
    s->tail = 0;
    s->head = 0;
    r->used = 0;
}

static int pe_gpu_selftest(void)
{
    PowerEmuGPUState *s = g_new0(PowerEmuGPUState, 1);
    PEGpuTestRing r;
    uint32_t vbase = PE_GPU_DATA_BASE;
    uint32_t tbase = PE_GPU_DATA_BASE + 0x10000;
    uint64_t before;
    int i;

    s->shared_ptr = g_malloc0(PE_GPU_SHARED_BYTES);
    s->enable = 1;
    r.base = s->shared_ptr;
    r.used = 0;

    printf("poweremu-gpu self-test (no renderer attached)\n");

    /* Three vertices and a 4x4 ARGB8888 texture for the draws below. */
    for (i = 0; i < 3; i++) {
        pe_gpu_test_vertex(s->shared_ptr + vbase + i * sizeof(PEGpuVertex),
                           10.0f * i, 20.0f * i, 0xFF204080u);
    }

    /* 1. NOP and FENCE: the transport alone. */
    {
        PEGpuFence *f;

        pe_gpu_test_emit(&r, PE_GPU_PKT_NOP, sizeof(PEGpuPacketHdr));
        f = pe_gpu_test_emit(&r, PE_GPU_PKT_FENCE, sizeof(PEGpuFence));
        f->value = cpu_to_be32(0x1234);
        pe_gpu_run(s, r.used);
        pe_gpu_check(s->packets == 2, "nop + fence counted");
        pe_gpu_check(s->fence == 0x1234, "fence published");
        pe_gpu_check(s->error == PE_GPU_ERR_NONE, "fence batch clean");
        pe_gpu_check(s->tail == 0, "ring drained");
    }

    /* 2. STATE then DRAW. */
    pe_gpu_test_reset(s, &r);
    {
        PEGpuState *st = pe_gpu_test_emit(&r, PE_GPU_PKT_STATE,
                                          sizeof(PEGpuState));
        PEGpuDraw *d;

        st->target_offset = cpu_to_be32(PE_GPU_DATA_BASE + 0x100000);
        st->target_pitch = cpu_to_be32(640 * 4);
        st->target_width = cpu_to_be16(640);
        st->target_height = cpu_to_be16(480);

        d = pe_gpu_test_emit(&r, PE_GPU_PKT_DRAW, sizeof(PEGpuDraw));
        d->vertex_offset = cpu_to_be32(vbase);
        d->vertex_count = cpu_to_be32(3);
        d->vertex_stride = cpu_to_be16(sizeof(PEGpuVertex));
        d->prim = cpu_to_be16(PE_GPU_PRIM_TRIANGLES);

        pe_gpu_run(s, r.used);
        pe_gpu_check(s->error == PE_GPU_ERR_NONE, "state + draw accepted");
        pe_gpu_check(s->draws == 1, "draw counted");
        pe_gpu_check(s->vertices == 3, "vertices counted");
        pe_gpu_check(s->dropped == 1, "draw dropped, no renderer");
    }

    /* 3. TEXTURE, BLIT and PRESENT. */
    pe_gpu_test_reset(s, &r);
    {
        PEGpuTexture *t = pe_gpu_test_emit(&r, PE_GPU_PKT_TEXTURE,
                                           sizeof(PEGpuTexture));
        PEGpuBlit *b;
        PEGpuPresent *pr;

        t->id = cpu_to_be32(1);
        t->data_offset = cpu_to_be32(tbase);
        t->width = cpu_to_be16(4);
        t->height = cpu_to_be16(4);
        t->pitch = cpu_to_be16(16);
        t->format = cpu_to_be16(PE_GPU_TEX_ARGB8888);

        b = pe_gpu_test_emit(&r, PE_GPU_PKT_BLIT, sizeof(PEGpuBlit));
        b->dst_offset = cpu_to_be32(PE_GPU_DATA_BASE + 0x100000);
        b->dst_pitch = cpu_to_be32(640 * 4);
        b->width = cpu_to_be16(16);
        b->height = cpu_to_be16(16);
        b->colour = cpu_to_be32(0xFF00FF00u);   /* src_pitch 0: solid fill */

        pr = pe_gpu_test_emit(&r, PE_GPU_PKT_PRESENT, sizeof(PEGpuPresent));
        pr->offset = cpu_to_be32(PE_GPU_DATA_BASE + 0x100000);
        pr->pitch = cpu_to_be32(640 * 4);
        pr->width = cpu_to_be16(640);
        pr->height = cpu_to_be16(480);

        pe_gpu_run(s, r.used);
        pe_gpu_check(s->error == PE_GPU_ERR_NONE, "texture/blit/present ok");
        pe_gpu_check(s->textures == 1 && s->blits == 1 && s->presents == 1,
                     "texture, blit, present counted");
        pe_gpu_check(ldl_be_p(s->shared_ptr + PE_GPU_DATA_BASE + 0x100000) ==
                     0xFF00FF00u, "fill reached memory big-endian");
        pe_gpu_check(s->scan_valid && s->scan_width == 640,
                     "scanout published");
    }

    /* 4. A packet whose size is not 8-aligned. */
    pe_gpu_test_reset(s, &r);
    {
        PEGpuPacketHdr *h = pe_gpu_test_emit(&r, PE_GPU_PKT_NOP,
                                             sizeof(PEGpuPacketHdr));
        before = s->rejected;
        h->bytes = cpu_to_be32(12);
        pe_gpu_run(s, r.used);
        pe_gpu_check(s->error == PE_GPU_ERR_PACKET, "misaligned size rejected");
        pe_gpu_check(s->rejected == before + 1, "rejection counted");
    }

    /* 5. A packet that claims to be shorter than its payload. */
    pe_gpu_test_reset(s, &r);
    {
        PEGpuPacketHdr *h = pe_gpu_test_emit(&r, PE_GPU_PKT_DRAW,
                                             sizeof(PEGpuDraw));
        h->bytes = cpu_to_be32(16);
        pe_gpu_run(s, r.used);
        pe_gpu_check(s->error == PE_GPU_ERR_PACKET, "short draw rejected");
    }

    /* 6. Vertices outside the data area. */
    pe_gpu_test_reset(s, &r);
    {
        PEGpuDraw *d = pe_gpu_test_emit(&r, PE_GPU_PKT_DRAW, sizeof(PEGpuDraw));

        before = s->draws;
        d->vertex_offset = cpu_to_be32(PE_GPU_DATA_BASE + PE_GPU_DATA_BYTES - 8);
        d->vertex_count = cpu_to_be32(3);
        d->vertex_stride = cpu_to_be16(sizeof(PEGpuVertex));
        d->prim = cpu_to_be16(PE_GPU_PRIM_TRIANGLES);
        pe_gpu_run(s, r.used);
        pe_gpu_check(s->error == PE_GPU_ERR_BOUNDS, "vertices past the end");
        pe_gpu_check(s->draws == before, "rejected draw not counted");
    }

    /* 7. An offset that points into the ring rather than the data area. */
    pe_gpu_test_reset(s, &r);
    {
        PEGpuDraw *d = pe_gpu_test_emit(&r, PE_GPU_PKT_DRAW, sizeof(PEGpuDraw));

        d->vertex_offset = cpu_to_be32(0);
        d->vertex_count = cpu_to_be32(3);
        d->vertex_stride = cpu_to_be16(sizeof(PEGpuVertex));
        pe_gpu_run(s, r.used);
        pe_gpu_check(s->error == PE_GPU_ERR_BOUNDS, "ring offset refused");
    }

    /* 8. An index list that names a vertex the draw does not have. */
    pe_gpu_test_reset(s, &r);
    {
        PEGpuDraw *d;
        uint32_t ibase = PE_GPU_DATA_BASE + 0x20000;

        stw_be_p(s->shared_ptr + ibase, 0);
        stw_be_p(s->shared_ptr + ibase + 2, 1);
        stw_be_p(s->shared_ptr + ibase + 4, 99);
        d = pe_gpu_test_emit(&r, PE_GPU_PKT_DRAW, sizeof(PEGpuDraw));
        d->vertex_offset = cpu_to_be32(vbase);
        d->vertex_count = cpu_to_be32(3);
        d->vertex_stride = cpu_to_be16(sizeof(PEGpuVertex));
        d->prim = cpu_to_be16(PE_GPU_PRIM_TRIANGLES);
        d->index_offset = cpu_to_be32(ibase);
        d->index_count = cpu_to_be32(3);
        pe_gpu_run(s, r.used);
        pe_gpu_check(s->error == PE_GPU_ERR_BOUNDS, "bad index refused");
    }

    /* 9. A texture whose rows overflow the data area. */
    pe_gpu_test_reset(s, &r);
    {
        PEGpuTexture *t = pe_gpu_test_emit(&r, PE_GPU_PKT_TEXTURE,
                                           sizeof(PEGpuTexture));
        t->id = cpu_to_be32(2);
        t->data_offset = cpu_to_be32(PE_GPU_DATA_BASE + PE_GPU_DATA_BYTES - 64);
        t->width = cpu_to_be16(1024);
        t->height = cpu_to_be16(1024);
        t->pitch = cpu_to_be16(4096);
        t->format = cpu_to_be16(PE_GPU_TEX_ARGB8888);
        pe_gpu_run(s, r.used);
        pe_gpu_check(s->error == PE_GPU_ERR_BOUNDS, "oversized texture refused");
    }

    /* 10. A blit whose destination rectangle leaves its own surface. */
    pe_gpu_test_reset(s, &r);
    {
        PEGpuBlit *b = pe_gpu_test_emit(&r, PE_GPU_PKT_BLIT, sizeof(PEGpuBlit));

        b->dst_offset = cpu_to_be32(PE_GPU_DATA_BASE + 0x100000);
        b->dst_pitch = cpu_to_be32(64);
        b->dst_x = cpu_to_be16(60);
        b->width = cpu_to_be16(32);          /* (60 + 32) * 4 > 64 */
        b->height = cpu_to_be16(4);
        pe_gpu_run(s, r.used);
        pe_gpu_check(s->error == PE_GPU_ERR_BOUNDS, "blit past its pitch");
    }

    /* 11. A doorbell outside the ring. */
    pe_gpu_test_reset(s, &r);
    pe_gpu_run(s, PE_GPU_RING_BYTES + 8);
    pe_gpu_check(s->error == PE_GPU_ERR_PACKET, "doorbell past the ring");

    /* 12. The ring keeps working after an error is acknowledged. */
    pe_gpu_test_reset(s, &r);
    {
        PEGpuFence *f = pe_gpu_test_emit(&r, PE_GPU_PKT_FENCE,
                                         sizeof(PEGpuFence));
        f->value = cpu_to_be32(0x5678);
        pe_gpu_run(s, r.used);
        pe_gpu_check(s->fence == 0x5678 && s->error == PE_GPU_ERR_NONE,
                     "ring recovers after a bad batch");
    }

    printf("%s (%d failure%s)\n", pe_gpu_test_failures ? "FAILED" : "PASSED",
           pe_gpu_test_failures, pe_gpu_test_failures == 1 ? "" : "s");

    g_free(s->shared_ptr);
    g_free(s);
    return pe_gpu_test_failures ? 1 : 0;
}

/* ====================================================================
 * Device
 * ==================================================================== */

static void pe_gpu_renderer_init(PowerEmuGPUState *s)
{
#ifdef CONFIG_DARWIN
    s->renderer = ppc_mac_gpu_renderer_metal();
    if (s->renderer) {
        s->renderer_opaque = s->renderer->init(s->shared_ptr,
                                               PE_GPU_SHARED_BYTES);
        if (!s->renderer_opaque) {
            s->renderer = NULL;
        }
    }
#endif
    /*
     * Everything below draw_r200() is optional: a backend without it (the
     * software renderer) cannot execute a paravirtual draw at all, so treat
     * it as no renderer.  That keeps the no-op host on a single code path
     * instead of a second, rarely exercised one -- packets are still
     * consumed, counted and fenced, the guest just sees nothing drawn.
     */
    if (s->renderer && !s->renderer->draw_r200) {
        s->renderer = NULL;
    }
    qemu_log("poweremu-gpu: renderer %s\n",
             s->renderer ? s->renderer->name : "none (packets consumed, "
                                               "nothing drawn)");
}

static void pe_gpu_realize(PCIDevice *dev, Error **errp)
{
    PowerEmuGPUState *s = POWEREMU_GPU(dev);
    Error *local_err = NULL;

    memory_region_init(&s->bar, OBJECT(s), "poweremu-gpu", PE_GPU_BAR_BYTES);
    memory_region_init_io(&s->ctrl, OBJECT(s), &pe_gpu_ctrl_ops, s,
                          "poweremu-gpu-ctrl", PE_GPU_CTRL_SIZE);

#ifdef CONFIG_DARWIN
    /*
     * The Metal backend renders through linear MTLTexture views of the
     * buffer it was handed as VRAM, so the shared mapping has to *be* that
     * buffer: allocate it from Metal and let QEMU map it, the same way
     * ppc_mac_gpu.c does for the emulated R200's VRAM.  With ordinary QEMU
     * RAM, metal_init() leaves vramBuffer nil and every draw is refused.
     */
    s->shared_ptr = ppc_mac_gpu_metal_alloc_vram(PE_GPU_SHARED_BYTES,
                                                 &s->metal_vram_opaque);
#endif
    if (s->shared_ptr) {
        memory_region_init_ram_ptr(&s->shared, OBJECT(s), "poweremu-gpu-shared",
                                   PE_GPU_SHARED_BYTES, s->shared_ptr);
    } else {
        memory_region_init_ram(&s->shared, OBJECT(s), "poweremu-gpu-shared",
                               PE_GPU_SHARED_BYTES, &local_err);
        if (local_err) {
            error_propagate(errp, local_err);
            return;
        }
        s->shared_ptr = memory_region_get_ram_ptr(&s->shared);
    }

    memory_region_add_subregion(&s->bar, PE_GPU_CTRL_OFFSET, &s->ctrl);
    memory_region_add_subregion(&s->bar, PE_GPU_RING_OFFSET, &s->shared);
    pci_register_bar(dev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->bar);

    pe_gpu_renderer_init(s);
    s->con = graphic_console_init(DEVICE(dev), 0, &pe_gpu_gfx_ops, s);
}

static void pe_gpu_exit(PCIDevice *dev)
{
    PowerEmuGPUState *s = POWEREMU_GPU(dev);

    if (s->renderer && s->renderer->fini) {
        s->renderer->fini(s->renderer_opaque);
    }
    s->renderer = NULL;
    /*
     * The MTLBuffer behind `shared` is deliberately not released here: the
     * memory region that maps it outlives this callback, and nothing else
     * in the process can hand those pages back.
     */
    g_free(s->shadow);
    s->shadow = NULL;
    s->surface_data = NULL;
}

static void pe_gpu_reset(DeviceState *dev)
{
    PowerEmuGPUState *s = POWEREMU_GPU(dev);

    s->enable = s->head = s->tail = s->fence = s->error = 0;
    s->scan_valid = false;
    s->state.valid = false;
    memset(s->tex, 0, sizeof(s->tex));
}

static const VMStateDescription vmstate_pe_gpu = {
    .name = "poweremu-gpu",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_PCI_DEVICE(parent_obj, PowerEmuGPUState),
        VMSTATE_UINT32(enable, PowerEmuGPUState),
        VMSTATE_UINT32(head, PowerEmuGPUState),
        VMSTATE_UINT32(tail, PowerEmuGPUState),
        VMSTATE_UINT32(fence, PowerEmuGPUState),
        VMSTATE_UINT32(error, PowerEmuGPUState),
        VMSTATE_END_OF_LIST()
    }
};

static void pe_gpu_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->realize = pe_gpu_realize;
    k->exit = pe_gpu_exit;
    k->vendor_id = PCI_VENDOR_ID_REDHAT;        /* placeholder */
    k->device_id = 0x1050;
    k->class_id = PCI_CLASS_DISPLAY_OTHER;
    dc->desc = "PowerEmu paravirtual GPU";
    dc->vmsd = &vmstate_pe_gpu;
    device_class_set_legacy_reset(dc, pe_gpu_reset);
    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);

    object_class_property_add_str(klass, "stats", pe_gpu_get_stats, NULL);
}

static const TypeInfo pe_gpu_info = {
    .name = TYPE_POWEREMU_GPU,
    .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(PowerEmuGPUState),
    .class_init = pe_gpu_class_init,
    .interfaces = (InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static void pe_gpu_register(void)
{
    type_register_static(&pe_gpu_info);

    /*
     * Run the self-test here, from MODULE_INIT_QOM, because it is the last
     * point that is guaranteed to be reached before anything machine- or
     * accelerator-specific exists: the test needs neither, and exiting from
     * it cannot leave a half-built machine behind.
     */
    if (getenv("POWEREMU_GPU_SELFTEST")) {
        exit(pe_gpu_selftest());
    }
}
type_init(pe_gpu_register);
