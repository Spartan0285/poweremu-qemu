#!/usr/bin/env python3
"""Exercise production bounded producer trace, disabled path and stopped flush."""
from pathlib import Path
import subprocess, tempfile
root = Path(__file__).resolve().parents[2]
s = (root / 'hw/audio/screamer.c').read_text()
a = s.index('#define SCREAMER_TRACE_CAPACITY')
b = s.index('static struct {\n    int64_t last;', a)
helpers = s[a:b].replace('#define SCREAMER_TRACE_CAPACITY 4000000',
                         '#define SCREAMER_TRACE_CAPACITY 4')
stub = r'''#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <unistd.h>
#include <stdarg.h>
#define QEMU_CLOCK_REALTIME 0
#define QEMU_CLOCK_VIRTUAL 1
#define FRAME_CNT_REG 5
#define g_try_malloc0_n(n,s) calloc(n,s)
#define g_free free
#define qemu_log(...) ((void)0)
typedef struct Notifier { void (*notify)(struct Notifier *, void *); } Notifier;
static Notifier *exit_notify;
static void qemu_add_exit_notifier(Notifier *n) { exit_notify = n; }
static char *g_strdup_printf(const char *fmt, ...) {
 char *r; va_list a; va_start(a, fmt); assert(vasprintf(&r, fmt, a) >= 0);
 va_end(a); return r;
}
static int clock_reads;
static int64_t qemu_clock_get_ns(int c) { clock_reads++; return c ? 77 : 88; }
typedef struct { uint64_t addr; uint32_t len; } IO;
typedef struct { IO io; uint32_t regs[6], wpos, rpos, rate; } ScreamerState;
'''
main = r'''
int main(int argc, char **argv) {
 _Static_assert(sizeof(ScreamerTraceRecord) == 56, "record format");
 ScreamerState s = {.io={0x1234, 512}, .regs={0,0,0,0,0,42},
                    .wpos=70, .rpos=20, .rate=44100};
 unsetenv("POWEREMU_SCREAMER_TRACE"); screamer_trace_init();
 screamer_trace_event(&s, STRACE_TX, 44, 7);
 assert(!screamer_trace.records && !clock_reads && !exit_notify);
 setenv("POWEREMU_SCREAMER_TRACE", "relative", 1); screamer_trace_init();
 assert(!screamer_trace.records);
 setenv("POWEREMU_SCREAMER_TRACE", argv[1], 1); screamer_trace_init();
 assert(screamer_trace.records && exit_notify);
 char *path = strdup(screamer_trace.path);
 for (int i=0;i<6;i++) screamer_trace_event(&s, STRACE_TX, i, i+1);
 assert(screamer_trace.count==4 && screamer_trace.dropped==2);
 assert(access(path, F_OK)!=0); /* No recording file during playback. */
 exit_notify->notify(exit_notify, NULL);
 assert(!screamer_trace.records && !screamer_trace.path);
 FILE *f=fopen(path,"rb"); assert(f);
 ScreamerTraceRecord records[4];
 assert(fread(records,sizeof(records),1,f)==1 && fgetc(f)==EOF);
 fclose(f); unlink(path);
 char *meta_path=g_strdup_printf("%s.json",path);
 f=fopen(meta_path,"r"); assert(f); char meta[256]={0};
 assert(fread(meta,1,255,f)>0); fclose(f);
 assert(strstr(meta,"\"dropped\":2") && strstr(meta,"\"flush_ok\":true"));
 unlink(meta_path); free(meta_path); free(path);
 for(int i=0;i<4;i++) {
  assert(records[i].wall_ns==88 && records[i].virtual_ns==77);
  assert(records[i].dma_addr==0x1234 && records[i].dma_len==512);
  assert(records[i].frame_count==42 && records[i].queued==50);
  assert(records[i].rate==44100 && records[i].kind==STRACE_TX);
  assert(records[i].arg==i && records[i].zero_frames==i+1);
 }
 puts("bounded producer trace: PASS");
}
'''
with tempfile.TemporaryDirectory() as d:
 d=Path(d); source=d/'trace.c'; source.write_text(stub+helpers+main)
 binary=d/'trace'
 subprocess.run(['clang','-D_GNU_SOURCE','-g','-fsanitize=address,undefined',
                 str(source),'-o',str(binary)],check=True)
 subprocess.run([str(binary),str(d/'capture')],check=True)
