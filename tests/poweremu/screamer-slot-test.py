#!/usr/bin/env python3
"""Replay actual read-only slot observer: bounds, phases, resets, dedup and flush."""
from pathlib import Path
import ast, subprocess, tempfile
root = Path(__file__).resolve().parents[2]
tree = ast.parse((root/'tests/poweremu/screamer-trace-test.py').read_text())
stub = next(ast.literal_eval(n.value) for n in tree.body if isinstance(n,ast.Assign) and any(isinstance(t,ast.Name) and t.id=='stub' for t in n.targets))
stub = stub.replace('static int64_t qemu_clock_get_ns(int c) { clock_reads++; return c ? 77 : 88; }','static int64_t wall; static int64_t qemu_clock_get_ns(int c) { clock_reads++; return wall; }')
stub = stub.replace('uint32_t regs[6], wpos, rpos, rate;','uint32_t regs[6], wpos, rpos, rate; int shift;')
stub = stub.replace('uint64_t addr; uint32_t len;', 'uint64_t addr; uint32_t len; void *channel;')
stub += r'''
typedef struct { uint32_t regs[16]; } DBDMA_channel;
#define DBDMA_STATUS 1
#define RUN 0x8000
#define PAUSE 0x4000
#define DEAD 0x0800
#define SCALE_MS 1000000LL
#define NANOSECONDS_PER_SECOND 1000000000LL
typedef uint64_t hwaddr;
typedef struct { int dummy; } MemoryRegion;
static MemoryRegion ram_region; static int address_space_memory;
#define MEMTXATTRS_UNSPECIFIED 0
#define MEMTX_OK 0
static bool ram_ok=true; static int reads;
static uint8_t memory[131072];
static MemoryRegion *address_space_translate(void *as,hwaddr addr,hwaddr *out,hwaddr *len,bool w,int attr) {
 *out=addr; if(addr>=sizeof(memory)) *len=0; else if(*len>sizeof(memory)-addr) *len=sizeof(memory)-addr;
 return &ram_region;
}
static bool memory_region_is_ram(MemoryRegion *m) { return ram_ok; }
static int dma_memory_read(void *as,hwaddr addr,void *out,size_t bytes,int attr) {
 assert(addr+bytes<=sizeof(memory));reads++;memcpy(out,memory+addr,bytes);return MEMTX_OK;
}
static bool screamer_pcm_flush(const char *path,const void *data,size_t bytes) {
 FILE *f=fopen(path,"wb");if(!f)return false;bool ok=fwrite(data,1,bytes,f)==bytes;return fclose(f)==0 && ok;
}
'''
helpers=(root/'hw/audio/screamer-slot.h').read_text().replace('#include "screamer-slot-map.h"',(root/'hw/audio/screamer-slot-map.h').read_text()).replace('(32 * 1024 * 1024)','8192').replace('#define SLOT_RECORDS 65536','#define SLOT_RECORDS 16')
main=r'''
static void tick(ScreamerState *s) { wall+=2*SCALE_MS;screamer_slot_tick(s); }
int main(int argc,char **argv) {
 _Static_assert(sizeof(ScreamerSlotRecord)==96,"record ABI");
 ScreamerState s={.io={0,16384},.rate=44100,.shift=2};
 if (argc > 2) {
  setenv("POWEREMU_SCREAMER_SLOT_CAPTURE",argv[1],1);
  setenv("POWEREMU_SCREAMER_SLOT_LEARN_RING","1",1);
  screamer_slot_init();tick(&s);assert(screamer_slot.learning);
  memset(memory,42,sizeof(memory));
  for(unsigned i=0;i<3*16384;i+=32) {
   s.regs[5]=i;s.io.addr=((i%16384)/32*37%512)*128;
   s.io.len=128;screamer_slot_transfer(&s,32);
  }
  s.regs[5]=3*16384;s.io.addr=0;tick(&s);
  assert(slot_map.mature && screamer_slot.ticket==1);
  assert(screamer_slot.target==3*16384+1024);
  assert(screamer_slot.address==(32*37%512)*128);
  for(unsigned i=3*16384;i<3*16384+1056;i+=32) {
   s.regs[5]=i;s.io.addr=((i%16384)/32*37%512)*128;
   screamer_slot_transfer(&s,32);
  }
  s.regs[5]=3*16384+1056;tick(&s);
  assert(screamer_slot.consumed && screamer_slot.records[1].phase==SLOT_POST);
  s.io.addr=7;screamer_slot_transfer(&s,32);
  assert(!slot_map.mature && !screamer_slot.tracked);
  assert(screamer_slot.records[0].phase==SLOT_INVALID);
  assert(screamer_slot.records[1].phase==SLOT_INVALID);
  DBDMA_channel channel={.regs={0,PAUSE}};
  slot_map.started=true;s.io.channel=&channel;tick(&s);
  assert(!slot_map.started);
  exit_notify->notify(exit_notify,NULL);
  puts("Learned ring observer integration: PASS");return 0;
 }
 unsetenv("POWEREMU_SCREAMER_SLOT_CAPTURE");screamer_slot_init();tick(&s);
 assert(clock_reads==0 && reads==0);
 setenv("POWEREMU_SCREAMER_SLOT_CAPTURE",argv[1],1);screamer_slot_init();
 char *path=strdup(screamer_slot.path);
 s.io.len=32;tick(&s);assert(reads==0 && screamer_slot.ticket==0);
 s.io.len=16384;memset(memory+32,42,32);tick(&s);
 assert(screamer_slot.ticket==1 && screamer_slot.target==8 && screamer_slot.count==1);
 assert(screamer_slot.records[0].address==32 && screamer_slot.records[0].phase==SLOT_PRE);
 tick(&s);assert(screamer_slot.count==1 && screamer_slot.records[0].repeats==2);
 /* Correct source then erased before consumption. */
 memset(memory+32,0,32);tick(&s);assert(screamer_slot.count==2);
 assert(screamer_slot.bytes[0]==42 && screamer_slot.bytes[32]==0);
 s.regs[5]=14;tick(&s);assert(screamer_slot.records[2].phase==SLOT_PARTIAL);
 s.regs[5]=16;tick(&s);assert(screamer_slot.records[3].phase==SLOT_POST);
 /* An after-read fill is captured without changing any guest byte. */
 memset(memory+32,17,32);tick(&s);assert(screamer_slot.records[4].phase==SLOT_POST);
 assert(screamer_slot.bytes[4*32]==17 && memory[32]==17);
 screamer_slot_invalidate();assert(!screamer_slot.tracked && screamer_slot.epoch==1);
 ram_ok=false;int before=reads;tick(&s);assert(!screamer_slot.tracked && reads==before);ram_ok=true;
 /* Reset/format changes do not deduplicate across epochs/tickets. */
 s.regs[5]=0;s.io.addr=0;tick(&s);assert(screamer_slot.tracked);
 uint64_t old=screamer_slot.ticket;s.shift=1;tick(&s);assert(screamer_slot.ticket>old);
 /* Capacity failures are explicit, no writes past allocated pools. */
 screamer_slot.count=SLOT_RECORDS;tick(&s);assert(screamer_slot.dropped>0);
 screamer_slot.count=6;screamer_slot.used=SLOT_BYTES-1;tick(&s);assert(screamer_slot.dropped>1);
 screamer_slot.used=160;screamer_slot.count=5;
 assert(access(path,F_OK)!=0);exit_notify->notify(exit_notify,NULL);
 FILE *f=fopen(path,"rb");assert(f);fclose(f);
 char *events=g_strdup_printf("%s.events",path);char *meta=g_strdup_printf("%s.json",path);
 f=fopen(meta,"r");assert(f);char json[1024]={0};fread(json,1,1023,f);fclose(f);
 assert(strstr(json,"\"pcm_flush_ok\":true") && strstr(json,"\"events_flush_ok\":true"));
 unlink(path);unlink(events);unlink(meta);free(path);free(events);free(meta);
 puts("DMA slot observer: PASS");
}
'''
with tempfile.TemporaryDirectory() as d:
 d=Path(d);src=d/'slot.c';src.write_text(stub+helpers+main);binary=d/'slot'
 subprocess.run(['clang','-D_GNU_SOURCE','-g','-fsanitize=address,undefined',str(src),'-o',str(binary)],check=True)
 subprocess.run([str(binary),str(d/'capture')],check=True)
 subprocess.run([str(binary),str(d/'learned'),'learn'],check=True)
