#!/usr/bin/env python3
"""Exercise production bounded interrupt trace, disabled path and stopped flush."""
from pathlib import Path
import subprocess, tempfile
root = Path(__file__).resolve().parents[2]
s = (root / 'hw/intc/openpic.c').read_text()
a = s.index('#define SCREAMER_IRQ_TRACE_CAPACITY')
b = s.index('static inline void IRQ_setbit', a)
helpers = s[a:b].replace('#define SCREAMER_IRQ_TRACE_CAPACITY 65536',
                         '#define SCREAMER_IRQ_TRACE_CAPACITY 4')
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
#define g_try_malloc0_n(n,s) calloc(n,s)
#define g_free free
typedef struct Notifier { void (*notify)(struct Notifier *, void *); } Notifier;
static Notifier *exit_notify;
static void qemu_add_exit_notifier(Notifier *n) { exit_notify = n; }
static char *g_strdup_printf(const char *fmt, ...) {
 char *r; va_list a; va_start(a, fmt); assert(vasprintf(&r, fmt, a) >= 0);
 va_end(a); return r;
}
static int clock_reads;
static int64_t qemu_clock_get_ns(int c) { clock_reads++; return c ? 77 : 88; }
enum { OPENPIC_MODEL_KEYLARGO = 7 };
#define IVPR_VECTOR(opp,v) ((v)&255)
typedef struct { uint32_t ivpr; } IRQSource;
typedef struct { int model; IRQSource src[16]; } OpenPICState;
'''
main = r'''
int main(int argc, char **argv) {
 _Static_assert(sizeof(ScreamerIRQRecord)==40,"record format");
 OpenPICState s={.model=OPENPIC_MODEL_KEYLARGO};s.src[9].ivpr=0x123;
 unsetenv("POWEREMU_SCREAMER_IRQ_TRACE");screamer_irq_trace_init(&s);
 screamer_irq_trace_event(&s,9,2,SIRQ_IACK,0);
 assert(!screamer_irq_trace.records && !clock_reads && !exit_notify);
 setenv("POWEREMU_SCREAMER_IRQ_TRACE","relative",1);screamer_irq_trace_init(&s);
 assert(!screamer_irq_trace.records);
 setenv("POWEREMU_SCREAMER_IRQ_TRACE",argv[1],1);
 s.model=0;screamer_irq_trace_init(&s);assert(!screamer_irq_trace.records);
 s.model=OPENPIC_MODEL_KEYLARGO;screamer_irq_trace_init(&s);
 assert(screamer_irq_trace.records && exit_notify);
 char *path=strdup(screamer_irq_trace.path);
 screamer_irq_trace_event(&s,8,0,SIRQ_LEVEL,1);assert(!clock_reads);
 for(int i=0;i<6;i++)screamer_irq_trace_event(&s,9,i,SIRQ_IACK,0);
 assert(screamer_irq_trace.count==4 && screamer_irq_trace.dropped==2);
 assert(access(path,F_OK)!=0);
 exit_notify->notify(exit_notify,NULL);
 FILE *f=fopen(path,"rb");assert(f);ScreamerIRQRecord records[4];
 assert(fread(records,sizeof(records),1,f)==1 && fgetc(f)==EOF);fclose(f);
 for(int i=0;i<4;i++) {
  assert(records[i].wall_ns==88 && records[i].virtual_ns==77);
  assert(records[i].kind==SIRQ_IACK && records[i].irq==9);
  assert(records[i].cpu==i && records[i].vector==0x23);
 }
 char *meta_path=g_strdup_printf("%s.json",path);f=fopen(meta_path,"r");
 assert(f);char meta[256]={0};assert(fread(meta,1,255,f)>0);fclose(f);
 assert(strstr(meta,"\"dropped\":2") && strstr(meta,"\"flush_ok\":true"));
 assert(!screamer_irq_trace.records && !screamer_irq_trace.path);
 unlink(path);unlink(meta_path);free(path);free(meta_path);
 puts("bounded Screamer interrupt trace: PASS");
}
'''
with tempfile.TemporaryDirectory() as d:
 d=Path(d); source=d/'trace.c'; source.write_text(stub+helpers+main)
 binary=d/'trace'
 subprocess.run(['clang','-D_GNU_SOURCE','-g','-fsanitize=address,undefined',
                 str(source),'-o',str(binary)],check=True)
 subprocess.run([str(binary),str(d/'capture')],check=True)
