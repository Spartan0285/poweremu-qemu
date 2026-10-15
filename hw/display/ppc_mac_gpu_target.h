/* Pure address-range helpers for the optional R200 external render target. */
#ifndef HW_DISPLAY_PPC_MAC_GPU_TARGET_H
#define HW_DISPLAY_PPC_MAC_GPU_TARGET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum PPCMacGPUTargetKind {
    PPC_MAC_GPU_TARGET_INVALID,
    PPC_MAC_GPU_TARGET_LOCAL,
    PPC_MAC_GPU_TARGET_AIC,
    PPC_MAC_GPU_TARGET_AGP,
} PPCMacGPUTargetKind;

typedef struct PPCMacGPUPhysicalSpan {
    uint64_t physical;
    uint64_t offset;
    uint64_t length;
} PPCMacGPUPhysicalSpan;

/* Input is sorted by physical address. Duplicate/aliased pages are unsafe. */
static inline bool pe_gpu_spans_disjoint(const PPCMacGPUPhysicalSpan *spans,
                                         size_t count)
{
    for (size_t i = 1; i < count; i++) {
        if (spans[i - 1].length > UINT64_MAX - spans[i - 1].physical ||
            spans[i - 1].physical + spans[i - 1].length > spans[i].physical) {
            return false;
        }
    }
    return true;
}

static inline bool pe_gpu_range_contains(uint64_t first, uint64_t last,
                                         uint64_t start, uint64_t end)
{
    return first >= start && last <= end;
}

static inline bool pe_gpu_range_overlaps(uint64_t first, uint64_t last,
                                         uint64_t start, uint64_t end)
{
    return first <= end && last >= start;
}

/* AIC has priority over AGP when apertures overlap, matching R350 reads. */
static inline PPCMacGPUTargetKind
pe_gpu_classify_target(uint64_t first, uint64_t length,
                       bool aic_enabled, uint64_t aic_start,
                       uint64_t aic_end, bool agp_enabled,
                       uint64_t agp_start, uint64_t agp_end,
                       uint64_t fb_start, uint64_t fb_size)
{
    uint64_t last, fb_end;
    bool aic_overlap, agp_overlap;

    if (!length || first > UINT32_MAX || length - 1 > UINT32_MAX - first) {
        return PPC_MAC_GPU_TARGET_INVALID;
    }
    last = first + length - 1;
    if (aic_enabled && pe_gpu_range_contains(first, last, aic_start, aic_end)) {
        return PPC_MAC_GPU_TARGET_AIC;
    }
    aic_overlap = aic_enabled &&
        pe_gpu_range_overlaps(first, last, aic_start, aic_end);
    if (aic_overlap) {
        return PPC_MAC_GPU_TARGET_INVALID;
    }
    if (agp_enabled && pe_gpu_range_contains(first, last, agp_start, agp_end)) {
        return PPC_MAC_GPU_TARGET_AGP;
    }
    agp_overlap = agp_enabled &&
        pe_gpu_range_overlaps(first, last, agp_start, agp_end);
    if (agp_overlap || !fb_size || fb_size - 1 > UINT32_MAX - fb_start) {
        return PPC_MAC_GPU_TARGET_INVALID;
    }
    fb_end = fb_start + fb_size - 1;
    return pe_gpu_range_contains(first, last, fb_start, fb_end)
        ? PPC_MAC_GPU_TARGET_LOCAL : PPC_MAC_GPU_TARGET_INVALID;
}

#endif
