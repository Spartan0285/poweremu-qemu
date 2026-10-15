#include "qemu/osdep.h"
#include "hw/display/ppc_mac_gpu_target.h"

static void test_target_ranges(void)
{
    const uint64_t fb = 0, fbsz = 0x0fc00000;
    const uint64_t aic_lo = 0x0fc00000, aic_hi = 0x17bfffff;
    const uint64_t agp_lo = 0x18000000, agp_hi = 0x1fffffff;

    g_assert_cmpint(pe_gpu_classify_target(0x0fd2e000, 2048 * 240,
        true, aic_lo, aic_hi, true, agp_lo, agp_hi, fb, fbsz),
        ==, PPC_MAC_GPU_TARGET_AIC);
    g_assert_cmpint(pe_gpu_classify_target(0x18001000, 4096,
        true, aic_lo, aic_hi, true, agp_lo, agp_hi, fb, fbsz),
        ==, PPC_MAC_GPU_TARGET_AGP);
    g_assert_cmpint(pe_gpu_classify_target(0x00100000, 4096,
        true, aic_lo, aic_hi, true, agp_lo, agp_hi, fb, fbsz),
        ==, PPC_MAC_GPU_TARGET_LOCAL);
    /* Crossing an aperture boundary or advertised VRAM end is never an alias. */
    g_assert_cmpint(pe_gpu_classify_target(aic_hi - 7, 16,
        true, aic_lo, aic_hi, true, agp_lo, agp_hi, fb, fbsz),
        ==, PPC_MAC_GPU_TARGET_INVALID);
    g_assert_cmpint(pe_gpu_classify_target(fbsz - 8, 16,
        true, aic_lo, aic_hi, true, agp_lo, agp_hi, fb, fbsz),
        ==, PPC_MAC_GPU_TARGET_INVALID);
    /* AIC wins in an overlap, matching the device read precedence. */
    g_assert_cmpint(pe_gpu_classify_target(0x1000, 32,
        true, 0x1000, 0x3fff, true, 0x2000, 0x4fff, 0, 0x10000),
        ==, PPC_MAC_GPU_TARGET_AIC);
    g_assert_cmpint(pe_gpu_classify_target(UINT32_MAX - 3, 8,
        false, 0, 0, false, 0, 0, 0, UINT32_MAX),
        ==, PPC_MAC_GPU_TARGET_INVALID);
}

static void test_physical_spans(void)
{
    PPCMacGPUPhysicalSpan disjoint[] = {
        { .physical = 0x1000, .offset = 4096, .length = 4096 },
        { .physical = 0x3000, .offset = 0, .length = 4096 },
    };
    PPCMacGPUPhysicalSpan alias[] = {
        { .physical = 0x1000, .offset = 0, .length = 4096 },
        { .physical = 0x1800, .offset = 4096, .length = 4096 },
    };
    g_assert_true(pe_gpu_spans_disjoint(disjoint, G_N_ELEMENTS(disjoint)));
    g_assert_false(pe_gpu_spans_disjoint(alias, G_N_ELEMENTS(alias)));
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/ppc-mac-gpu/target-ranges", test_target_ranges);
    g_test_add_func("/ppc-mac-gpu/physical-spans", test_physical_spans);
    return g_test_run();
}
