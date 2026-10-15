#!/usr/bin/env python3
"""Exercise the production R200 RAM span planner/copyback with sanitizers.

The memory API is a fixture, not QEMU: this proves planner edge cases and
captured-address use, not real address-space dirty tracking or concurrency.
"""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'hw/display/ppc_mac_gpu.c').read_text()
start = source.index('typedef PPCMacGPUPhysicalSpan R200HostSpan;')
end = source.index('\n/* A very narrow, opt-in implementation of the captured Tiger window', start)
production = source[start:end]
stub = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hw/display/ppc_mac_gpu_target.h"
typedef struct { bool ram, device, readonly; } MemoryRegion;
typedef uint64_t hwaddr;
typedef struct { MemoryRegion vram; } PPCMacGPUState;
#define MIN(a,b) ((a) < (b) ? (a) : (b))
#define MEMTXATTRS_UNSPECIFIED 0
#define MEMTX_OK 0
static int address_space_memory, reads, writes, aic_calls, agp_calls;
static uint8_t ram[0x10000];
static uint64_t pages[4];
static bool valid[4];
static MemoryRegion regions[16];
static MemoryRegion *vram_region;
static bool translate(uint32_t a, hwaddr *p) {
    if (a < 0xfc00000 || a >= 0xfc04000) return false;
    unsigned page = (a - 0xfc00000) / 4096;
    if (!valid[page]) return false;
    *p = pages[page] + (a & 4095); return true;
}
static bool ppc_mac_gpu_gart_translate(PPCMacGPUState *s, uint32_t a, hwaddr *p) {
    (void)s; aic_calls++; return translate(a,p);
}
static bool ppc_mac_gpu_agp_translate(PPCMacGPUState *s, uint32_t a, hwaddr *p) {
    (void)s; agp_calls++; return translate(a,p);
}
static MemoryRegion *address_space_translate(int *as, hwaddr p, hwaddr *x,
                                            hwaddr *n, bool wr, int attr) {
    (void)as; (void)attr; assert(wr); assert(p < sizeof(ram));
    *x = p; *n = MIN(*n, 4096 - (p & 4095));
    return vram_region && p / 4096 == 4 ? vram_region : &regions[p / 4096];
}
static bool memory_region_is_ram(MemoryRegion *m) { return m->ram; }
static bool memory_region_is_ram_device(MemoryRegion *m) { return m->device; }
static int address_space_read(int *as, hwaddr p, int attr, void *b, size_t n) {
    (void)as; (void)attr; assert(p+n <= sizeof(ram)); reads++;
    memcpy(b, ram+p, n); return MEMTX_OK;
}
static int address_space_write(int *as, hwaddr p, int attr, const void *b, size_t n) {
    (void)as; (void)attr; assert(p+n <= sizeof(ram)); writes++;
    memcpy(ram+p,b,n); return MEMTX_OK;
}
static void reset(void) {
    memset(ram, 0, sizeof(ram));
    for (unsigned i=0;i<16;i++) regions[i]=(MemoryRegion){true,false,false};
    for (unsigned i=0;i<4;i++) {valid[i]=true; pages[i]=(4-i)*4096;}
    vram_region=NULL;
    reads=writes=aic_calls=agp_calls=0;
}
'''
main = r'''
int main(void) {
    PPCMacGPUState s={0}; R200HostSpan spans[R200_HOST_RT_MAX_SPANS]; size_t n=0;
    uint8_t out[5000], input[5000];
    for(unsigned i=0;i<sizeof(input);i++) input[i]=(uint8_t)(i*37);
    reset();
    /* Page-unaligned target, reverse physical ordering, preserved offsets. */
    assert(r200_host_rt_spans(&s,0xfc00ff0,PPC_MAC_GPU_TARGET_AIC,
                              sizeof(input),spans,&n));
    assert(n==3 && aic_calls==3 && agp_calls==0);
    assert(r200_host_rt_write(spans,n,input) && writes==3);
    assert(r200_host_rt_read(spans,n,out) && !memcmp(input,out,sizeof(out)));
    /* Changing page-table mappings after planning cannot redirect copyback. */
    hwaddr saved=spans[0].physical; size_t off=spans[0].offset;
    memset(input,0xa7,sizeof(input)); for(unsigned i=0;i<4;i++) pages[i]=0;
    assert(r200_host_rt_write(spans,n,input)); assert(ram[saved]==input[off]);
    reset(); valid[1]=false;
    assert(!r200_host_rt_spans(&s,0xfc00ff0,PPC_MAC_GPU_TARGET_AIC,
                               sizeof(input),spans,&n));
    assert(agp_calls==0 && reads==0 && writes==0);
    /* Full validation rejects later invalid destinations before any write. */
    for(unsigned kind=0;kind<3;kind++) {
        reset(); MemoryRegion *m=&regions[pages[1]/4096];
        if(kind==0)m->readonly=true; if(kind==1)m->device=true; if(kind==2)m->ram=false;
        assert(!r200_host_rt_spans(&s,0xfc00000,PPC_MAC_GPU_TARGET_AIC,
                                   sizeof(input),spans,&n));
        assert(reads==0 && writes==0);
    }
    reset(); pages[1]=pages[0];
    assert(!r200_host_rt_spans(&s,0xfc00000,PPC_MAC_GPU_TARGET_AIC,
                               sizeof(input),spans,&n)); assert(writes==0);
    reset(); assert(r200_host_rt_spans(&s,0xfc00000,PPC_MAC_GPU_TARGET_AGP,
                                      sizeof(input),spans,&n));
    assert(agp_calls==2 && aic_calls==0);
    reset(); vram_region=&s.vram;
    assert(!r200_host_rt_spans(&s,0xfc00000,PPC_MAC_GPU_TARGET_AIC,
                               sizeof(input),spans,&n));
    assert(reads==0 && writes==0);
    puts("R200 production span planner/copyback fixture PASS");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='pe-r200-target-') as tmp:
    c = Path(tmp) / 'test.c'
    exe = Path(tmp) / 'test'
    c.write_text(stub + production + main)
    subprocess.run([os.environ.get('CC', 'clang'), '-std=c11', '-g', '-O1',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    '-I', str(root), str(c), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
