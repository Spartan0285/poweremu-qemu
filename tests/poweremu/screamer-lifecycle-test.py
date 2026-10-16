#!/usr/bin/env python3
"""Replay production DMA transfers/flushes across partial pause, stop and reset.

Uses the real Screamer transfer/flush helpers with bounded RAM and the actual
DBDMA pre-status-commit flush ordering. No emulator or guest disk is opened.
"""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
s = (root / 'hw/audio/screamer.c').read_text()
def function(signature):
    start = s.index(signature)
    end = s.index('{', start) + 1
    depth = 1
    while depth:
        depth += (s[end] == '{') - (s[end] == '}')
        end += 1
    return s[start:end]
stub = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#define MIN(a,b) ((a)<(b)?(a):(b))
#define MAX(a,b) ((a)>(b)?(a):(b))
#define RUN 0x8000
#define PAUSE 0x4000
#define FLUSH 0x2000
#define DEAD 0x0800
#define ACTIVE 0x0400
#define DBDMA_CONTROL 0
#define DBDMA_STATUS 1
#define DBDMA_CMDPTR_LO 3
#define COMMAND_MASK 0xf000
#define OUTPUT_MORE 0
#define OUTPUT_LAST 0x1000
#define INPUT_MORE 0x2000
#define INPUT_LAST 0x3000
#define FRAME_CNT_REG 5
#define MEMTXATTRS_UNSPECIFIED 0
#define SCREAMER_DPRINTF(...) ((void)0)
#define screamer_slot_transfer(...) ((void)0)
#define screamer_pcm_capture(...) ((void)0)
#define screamer_trace_event(...) ((void)0)
#define cpu_to_le16(x) (x)
#define le16_to_cpu(x) (x)
typedef uint64_t hwaddr;
typedef struct {uint16_t req_count,command;uint32_t phy_addr,cmd_dep;uint16_t res_count,xfer_status;} dbdma_cmd;
typedef struct DBDMA_io {void *opaque,*channel;hwaddr addr;int len;bool processing;void (*dma_end)(struct DBDMA_io *);} DBDMA_io;
typedef struct {uint32_t regs[16];DBDMA_io io;dbdma_cmd current;} DBDMA_channel;
typedef struct {DBDMA_io io,in_io;int shift,samples,wpos,rpos;uint32_t regs[6];uint8_t *mixbuf,*inbuf;uint32_t in_rpos,in_wpos;int in_samples;bool active,in_running;void *voice;} ScreamerState;
static bool screamer_lifecycle_fix;
static unsigned reads,writes,completed;
static uint8_t ram[1024];
static int address_space_memory,sdbg_on;
static struct {void *records;} screamer_trace;
static uint64_t sdbg_exact_run;
static struct {uint64_t overflow_frames,pulled,exact_zero_frames,exact_zero_runs,exact_zero_max,zr,zero_runs,zero_frames,tx_descriptor_completions;} sdbg;
static void dma_memory_read(void *as,hwaddr addr,void *buf,size_t len,int attrs) {assert(addr+len<=sizeof ram);memcpy(buf,ram+addr,len);reads++;}
static void dma_memory_write(void *as,hwaddr addr,const void *buf,size_t len,int attrs) {assert(addr+len<=sizeof ram);memcpy(ram+addr,buf,len);writes++;}
static void dma_memory_set(void *as,hwaddr addr,int value,size_t len,int attrs) {assert(addr+len<=sizeof ram);memset(ram+addr,value,len);writes++;}
static void AUD_set_active_out(void *voice,bool active) {}
static void finish(DBDMA_io *io) {completed++;((DBDMA_channel *)io->channel)->io.processing=false;}
'''
source = stub + '\n'.join(function(sig) for sig in (
    'static bool screamer_dma_can_transfer(', 'static void screamer_dma_sync(',
    'static void screamer_dma_flush(', 'static int pmac_screamer_tx_transfer(',
    'static int pmac_screamer_rx_transfer(ScreamerState *s, int max)\n{', 'static void pmac_screamer_tx_flush(',
    'static void pmac_screamer_rx_flush('))
source += r'''
static uint8_t ring[32],input[32];
static void start(ScreamerState *s,DBDMA_channel *ch,bool rx) {
 memset(ch,0,sizeof *ch); memset(s,0,sizeof *s);
 ch->regs[DBDMA_STATUS]=RUN|ACTIVE;ch->regs[DBDMA_CMDPTR_LO]=512;
 ch->current.command=rx?INPUT_MORE:OUTPUT_MORE;ch->current.req_count=16;
 ch->io=(DBDMA_io){.opaque=s,.channel=ch,.addr=128,.len=16,.processing=true,.dma_end=finish};
 s->shift=2;s->samples=s->in_samples=8;s->mixbuf=ring;s->inbuf=input;
 if(rx){s->in_io=ch->io;s->in_running=true;}else{s->io=ch->io;}
}
static int transfer(ScreamerState *s,bool rx,int frames) {return rx?pmac_screamer_rx_transfer(s,frames):pmac_screamer_tx_transfer(s,frames);}
static void control(ScreamerState *s,DBDMA_channel *ch,bool rx,unsigned mask,unsigned value) {
 ch->regs[DBDMA_CONTROL]=(mask<<16)|value;
 /* Matches mac_dbdma: flush sees old status, then status is committed. */
 if(rx)pmac_screamer_rx_flush(&ch->io);else pmac_screamer_tx_flush(&ch->io);
 unsigned status=(ch->regs[DBDMA_STATUS]&~(mask&(RUN|PAUSE)))|(value&mask&(RUN|PAUSE));
 if(!(status&RUN)||(status&PAUSE))status&=~ACTIVE;
 ch->regs[DBDMA_STATUS]=status;
}
int main(void) {
 for(unsigned i=0;i<sizeof ram;i++)ram[i]=(uint8_t)i;
 ScreamerState s;DBDMA_channel ch;
 screamer_lifecycle_fix=true;
 for(int rx=0;rx<2;rx++) {
  start(&s,&ch,rx);unsigned done=completed;
  assert(transfer(&s,rx,2)==2&&ch.io.addr==136&&ch.io.len==8);
  assert(s.regs[FRAME_CNT_REG]==(rx?0:2));
  control(&s,&ch,rx,PAUSE,PAUSE);
  assert(ch.io.processing&&ch.current.res_count==8);
  assert(ch.current.xfer_status==(RUN|ACTIVE));
  unsigned before_r=reads,before_w=writes,counter=s.regs[FRAME_CNT_REG];
  for(int tick=0;tick<10;tick++)assert(transfer(&s,rx,44)==0);
  assert(reads==before_r&&writes==before_w&&s.regs[FRAME_CNT_REG]==counter&&completed==done);
  /* Resume keeps descriptor processing: DBDMA_run must not re-fetch it. */
  ch.regs[DBDMA_STATUS]=RUN|ACTIVE;
  assert(ch.io.processing&&transfer(&s,rx,2)==2&&ch.io.addr==144&&ch.io.len==0&&completed==done+1);
  start(&s,&ch,rx);assert(transfer(&s,rx,2)==2);
  control(&s,&ch,rx,FLUSH,FLUSH);
  assert(ch.io.processing&&ch.current.res_count==8);
  assert(transfer(&s,rx,2)==2); /* explicit FLUSH cannot replay partial bytes */
  start(&s,&ch,rx);assert(transfer(&s,rx,2)==2);done=completed;
  control(&s,&ch,rx,RUN,0);
  assert(!ch.io.processing&&ch.current.res_count==8);
  assert((rx?s.in_io.len:s.io.len)==0);
  for(int tick=0;tick<10;tick++)assert(transfer(&s,rx,44)==0);
  assert(completed==done); /* stop is not a descriptor completion/IRQ */
  start(&s,&ch,rx);assert(transfer(&s,rx,2)==2);
  /* Actual mac_dbdma reset clears regs/processing/len; stale copy remains. */
  memset(ch.regs,0,sizeof ch.regs);ch.io.processing=false;ch.io.len=0;
  assert(transfer(&s,rx,44)==0);
  start(&s,&ch,rx);assert(transfer(&s,rx,4)==4); /* fresh restart */
  start(&s,&ch,rx);ch.regs[DBDMA_STATUS]=RUN|ACTIVE|DEAD;
  assert(transfer(&s,rx,44)==0);
  ch.regs[DBDMA_STATUS]=RUN;assert(transfer(&s,rx,44)==0);
 }
 /* Default-off reproduces original stale-copy behavior; opt-in is isolated. */
 screamer_lifecycle_fix=false;start(&s,&ch,false);ch.regs[DBDMA_STATUS]=0;
 assert(transfer(&s,false,2)==2&&ch.io.len==16);
 puts("Screamer partial pause/resume/stop/reset/residual/FLUSH: PASS");
}
'''
with tempfile.TemporaryDirectory() as directory:
    directory = Path(directory)
    src = directory / 'lifecycle.c'; src.write_text(source)
    binary = directory / 'lifecycle'
    subprocess.run(['clang', '-g', '-fsanitize=address,undefined', str(src), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
