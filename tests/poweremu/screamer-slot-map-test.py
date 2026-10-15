#!/usr/bin/env python3
"""Sanitizer checks for actual-transfer ring inference and future addressing."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
stub='''#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
#define NANOSECONDS_PER_SECOND 1000000000LL
'''
main=r'''
static uint64_t address(uint32_t frame) {
 unsigned p=frame%16384, chunk=p/32;
 return 0x100000 + ((chunk*37)%512)*128 + (p%32)*4;
}
static void learn(uint32_t origin) {
 slot_map_reset();
 for(unsigned i=0;i<3*16384;i+=32) {
  assert(slot_map_transfer(origin+i,address(i),32,44100,2,i*22676LL));
  if(i<3*16384-32)assert(!slot_map.mature);
 }
 assert(slot_map.length==16384 && slot_map.stable==3 && slot_map.mature);
 uint64_t a;
 assert(slot_map_target(origin+3*16384,1024,8,&a));assert(a==address(1024));
 /* Disjoint descriptors must use learned addresses, never base+lead. */
 assert(a!=address(0)+1024*4);
 assert(!slot_map_target(origin+3*16384,1050,8,&a));
}
int main(void) {
 assert(slot_map_transfer(0,0,32,44100,2,0));assert(!slot_map.started);
 slot_map.addresses=calloc(SLOT_MAP_LIMIT,8);slot_map.sorted=calloc(SLOT_MAP_LIMIT,8);slot_map.enabled=true;
 learn(0);
 assert(!slot_map_transfer(3*16384,address(0)+4,32,44100,2,3*16384*22676LL));
 learn(0xffff8000U); /* Counter rollover while learning. */
 assert(!slot_map_transfer(0xffff8000U+3*16384,address(0),32,48000,2,3*16384*22676LL));
 learn(0);assert(!slot_map_transfer(3*16384,address(0),32,44100,2,10000000000LL));
 learn(0);assert(!slot_map_transfer(3*16384+1,address(0),32,44100,2,3*16384*22676LL));
 slot_map_reset();assert(!slot_map_target(0,1024,8,&(uint64_t){0}));
 assert(slot_map_transfer(0,0x100000,4096,44100,2,0));
 /* A repeated interior address makes the inferred candidate ambiguous. */
 assert(slot_map_transfer(4096,0x100004,4096,44100,2,100000000));
 assert(!slot_map_transfer(8192,0x100000,1,44100,2,200000000));
 slot_map_reset();assert(slot_map_transfer(0,0x100000,SLOT_MAP_LIMIT,44100,2,0));
 assert(!slot_map_transfer(SLOT_MAP_LIMIT,0x900000,1,44100,2,1));
 free(slot_map.addresses);free(slot_map.sorted);puts("DMA learned ring mapping: PASS");
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d);src=p/'map.c';src.write_text(stub+(root/'hw/audio/screamer-slot-map.h').read_text()+main)
 subprocess.run(['clang','-g','-fsanitize=address,undefined',str(src),'-o',str(p/'map')],check=True)
 subprocess.run([str(p/'map')],check=True)
