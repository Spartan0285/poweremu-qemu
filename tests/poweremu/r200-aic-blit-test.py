#!/usr/bin/env python3
"""Sanitizer fixture for the production R200 AIC blit shape/row packer."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
test = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "hw/display/ppc_mac_gpu_aic_blit.h"

int main(void) {
    uint8_t source[64], packed[24];
    for (unsigned i = 0; i < sizeof(source); i++) source[i] = (uint8_t)i;

    assert(pe_r200_aic_linear_storage_mode(1, 0, 3, false, false));
    assert(!pe_r200_aic_linear_storage_mode(1, 0, 0, false, false));
    assert(!pe_r200_aic_linear_storage_mode(0, 0, 3, false, false));
    assert(!pe_r200_aic_linear_storage_mode(1, 1, 3, false, false));
    assert(!pe_r200_aic_linear_storage_mode(1, 0, 3, true, false));
    assert(!pe_r200_aic_linear_storage_mode(1, 0, 3, false, true));

    assert(pe_r200_aic_blit_signature(PE_R200_AIC_BLIT_GMC,
        480, 220, 0x800, 0x800, 4, 0, 0, 0, 0));
    assert(pe_r200_aic_blit_signature(PE_R200_AIC_BLIT_GMC,
        127, 3, 640, 768, 4, 4, 6, 8, 9));
    assert(!pe_r200_aic_blit_signature(PE_R200_AIC_BLIT_GMC ^ 1,
        127, 3, 512, 768, 4, 4, 6, 8, 9));
    assert(!pe_r200_aic_blit_signature(PE_R200_AIC_BLIT_GMC,
        127, 3, 512, 768, 2, 4, 6, 8, 9));
    assert(!pe_r200_aic_blit_signature(PE_R200_AIC_BLIT_GMC,
        127, 3, 512, 768, 4, 100, 6, 8, 9));
    assert(!pe_r200_aic_blit_signature(PE_R200_AIC_BLIT_GMC,
        127, 3, 640, 768, 4, 4, 65536, 8, 9));
    assert(!pe_r200_aic_blit_signature(PE_R200_AIC_BLIT_GMC,
        1, UINT32_MAX, 4, 4, 4, 0, 0, 0, 0));
    assert(!pe_r200_aic_blit_signature(PE_R200_AIC_BLIT_GMC,
        0x4000, 1, 0x10000, 0x10000, 4, 0, 0, 0, 0));
    assert(!pe_r200_aic_blit_signature(PE_R200_AIC_BLIT_GMC,
        1, 5000, 4, 4, 4, 0, 0, 0, 0));
    assert(!pe_r200_aic_blit_signature(PE_R200_AIC_BLIT_GMC,
        UINT32_MAX, 1, UINT32_MAX, UINT32_MAX, 4, 0, 0, 0, 0));

    memset(packed, 0xee, sizeof(packed));
    assert(pe_r200_aic_blit_pack_rows(source, sizeof(source), 3,
                                      12, 8, 3, packed, sizeof(packed)));
    assert(!memcmp(packed, source + 3, 8));
    assert(!memcmp(packed + 8, source + 15, 8));
    assert(!memcmp(packed + 16, source + 27, 8));

    memset(packed, 0xee, sizeof(packed));
    assert(!pe_r200_aic_blit_pack_rows(source, 34, 3,
                                       12, 8, 3, packed, sizeof(packed)));
    for (unsigned i = 0; i < sizeof(packed); i++) assert(packed[i] == 0xee);
    assert(!pe_r200_aic_blit_pack_rows(source, sizeof(source), 0,
                                       0, 8, 3, packed, sizeof(packed)));
    assert(!pe_r200_aic_blit_pack_rows(source, sizeof(source), 0,
                                       12, 8, 3, packed, sizeof(packed) - 1));

    PPCMacGPUPhysicalSpan all[2] = {{100, 0, 4}, {200, 4, 4}};
    PPCMacGPUPhysicalSpan row[2] = {{300, 0, 4}, {400, 4, 4}};
    size_t count = 0;
    assert(pe_r200_aic_append_spans(all, 2, &count, row, 2, 8));
    assert(count == 2 && all[0].offset == 8 && all[1].offset == 12);
    PPCMacGPUPhysicalSpan before[2];
    memcpy(before, all, sizeof(all));
    assert(!pe_r200_aic_append_spans(all, 2, &count, row, 1, 16));
    assert(count == 2 && !memcmp(before, all, sizeof(all)));
    assert(pe_r200_aic_ranges_overlap(100, 20, 119, 10));
    assert(!pe_r200_aic_ranges_overlap(100, 20, 120, 10));
    assert(pe_r200_aic_ranges_overlap(100, 20, 105, 2));
    assert(pe_r200_aic_ranges_overlap(0xa6f000, 0x800u * 220u,
                                       0xa6f000 + 0x800u * 100u + 64u,
                                       4096));
    assert(pe_r200_aic_ranges_overlap(UINT64_MAX - 4, 8, 0, 1));
    puts("R200 AIC production signature/row-pack fixture PASS");
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix='pe-r200-aic-blit-') as tmp:
    c = Path(tmp) / 'test.c'
    exe = Path(tmp) / 'test'
    c.write_text(test)
    subprocess.run([os.environ.get('CC', 'clang'), '-std=c11', '-g', '-O1',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    '-I', str(root), str(c), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
