/* Narrow helpers for the opt-in R200 local-VRAM to AIC copy. */
#ifndef HW_DISPLAY_PPC_MAC_GPU_AIC_BLIT_H
#define HW_DISPLAY_PPC_MAC_GPU_AIC_BLIT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "hw/display/ppc_mac_gpu_target.h"

#define PE_R200_AIC_BLIT_GMC       0x52cc36f3u
#define PE_R200_AIC_BLIT_BPP       4u
#define PE_R200_AIC_BLIT_MAX_BYTES (16u * 1024u * 1024u)
#define PE_R200_AIC_BLIT_MAX_DIM   0x3fffu
#define PE_R200_AIC_BLIT_MAX_ROWS  (PE_R200_AIC_BLIT_MAX_BYTES / 4096u + 2u)

/* This exact captured operation declares a tiled source and linear
 * destination. The emulator currently stores its R200 render targets
 * linearly; callers must additionally verify that MC tiling is disabled. */
static inline bool pe_r200_aic_linear_storage_mode(uint32_t src_tile,
                                                    uint32_t dst_tile,
                                                    uint8_t tile_valid,
                                                    bool mc_tiling_enabled,
                                                    bool renderer_tiled)
{
    return src_tile == 1 && dst_tile == 0 &&
           tile_valid == 0x03 &&
           !mc_tiling_enabled && !renderer_tiled;
}

static inline bool pe_r200_aic_ranges_overlap(uint64_t a_start, uint64_t a_len,
                                               uint64_t b_start, uint64_t b_len)
{
    if (!a_len || !b_len) {
        return false;
    }
    if (a_len - 1 > UINT64_MAX - a_start ||
        b_len - 1 > UINT64_MAX - b_start) {
        return true; /* malformed range: fail closed */
    }
    return a_start <= b_start + b_len - 1 && b_start <= a_start + a_len - 1;
}

/* Append already validated per-row physical spans without a partial append. */
static inline bool pe_r200_aic_append_spans(PPCMacGPUPhysicalSpan *dst,
                                             size_t capacity, size_t *count,
                                             const PPCMacGPUPhysicalSpan *src,
                                             size_t src_count,
                                             uint64_t offset_base)
{
    if (!dst || !count || (!src && src_count) || *count > capacity ||
        src_count > capacity - *count) {
        return false;
    }
    for (size_t i = 0; i < src_count; i++) {
        if (src[i].offset > UINT64_MAX - offset_base) {
            return false;
        }
    }
    for (size_t i = 0; i < src_count; i++) {
        dst[*count + i] = src[i];
        dst[*count + i].offset += offset_base;
    }
    *count += src_count;
    return true;
}

/* Match the observed source-copy GMC and byte format, with bounded geometry. */
static inline bool pe_r200_aic_blit_signature(uint32_t gmc, uint32_t width,
                                               uint32_t height,
                                               uint32_t src_pitch,
                                               uint32_t dst_pitch,
                                               uint32_t bpp,
                                               uint32_t src_x,
                                               uint32_t src_y,
                                               uint32_t dst_x,
                                               uint32_t dst_y)
{
    uint64_t row_bytes = (uint64_t)width * bpp;
    uint64_t bytes;

    if (gmc != PE_R200_AIC_BLIT_GMC || !width || !height ||
        width > PE_R200_AIC_BLIT_MAX_DIM ||
        height > PE_R200_AIC_BLIT_MAX_DIM ||
        height > PE_R200_AIC_BLIT_MAX_ROWS ||
        bpp != PE_R200_AIC_BLIT_BPP || !src_pitch || !dst_pitch ||
        src_x > UINT16_MAX || src_y > UINT16_MAX ||
        dst_x > UINT16_MAX || dst_y > UINT16_MAX ||
        row_bytes > src_pitch || row_bytes > dst_pitch ||
        (uint64_t)src_x * bpp + row_bytes > src_pitch ||
        (uint64_t)dst_x * bpp + row_bytes > dst_pitch ||
        row_bytes > PE_R200_AIC_BLIT_MAX_BYTES ||
        height > PE_R200_AIC_BLIT_MAX_BYTES / row_bytes) {
        return false;
    }
    bytes = row_bytes * height;
    return bytes <= SIZE_MAX;
}

/* Pack tightly-copied source rows into the exact byte layout passed to AIC. */
static inline bool pe_r200_aic_blit_pack_rows(const uint8_t *source,
                                               size_t source_length,
                                               size_t source_offset,
                                               size_t source_pitch,
                                               size_t row_bytes,
                                               size_t height,
                                               uint8_t *packed,
                                               size_t packed_length)
{
    size_t total;

    if (!source || !packed || !row_bytes || !source_pitch || !height ||
        row_bytes > source_pitch || row_bytes > SIZE_MAX / height) {
        return false;
    }
    total = row_bytes * height;
    if (total > PE_R200_AIC_BLIT_MAX_BYTES || packed_length != total ||
        source_offset > source_length ||
        (height - 1) > (SIZE_MAX - source_offset) / source_pitch) {
        return false;
    }
    size_t last = source_offset + (height - 1) * source_pitch;
    if (last > source_length || row_bytes > source_length - last) {
        return false;
    }
    for (size_t row = 0; row < height; row++) {
        memcpy(packed + row * row_bytes, source + source_offset + row * source_pitch,
               row_bytes);
    }
    return true;
}

#endif
