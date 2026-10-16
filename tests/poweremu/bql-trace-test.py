#!/usr/bin/env python3
"""Standalone recorder bounds plus real pthread contention/condition release."""
from pathlib import Path
import subprocess, tempfile, json
root = Path(__file__).resolve().parents[2]
source = r'''
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include "system/poweremu-bql-trace.h"
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond=PTHREAD_COND_INITIALIZER;
static PEBQLTrace t;
static atomic_bool held;
static bool awakened;
static void *holder(void *arg) {
    pthread_mutex_lock(&lock);
    uint64_t begin=pe_bql_clock();
    PEBQLSegment s=pe_bql_segment(&t,begin,"holder",23,0x1234,99,2);
    atomic_store(&held,true); pthread_cond_broadcast(&cond);
    struct timespec delay={0,30000000}; nanosleep(&delay,NULL);
    pe_bql_event(&t,&s,begin,pe_bql_clock(),2,0);
    pthread_mutex_unlock(&lock); return NULL;
}
static void *wake(void *arg) {
    struct timespec delay={0,50000000}; nanosleep(&delay,NULL);
    pthread_mutex_lock(&lock); awakened=true;
    pthread_cond_broadcast(&cond); pthread_mutex_unlock(&lock); return NULL;
}
int main(void) {
    t.events=calloc(PE_BQL_CAPACITY,sizeof(*t.events)); assert(t.events);
    t.start_ns=10; t.deadline_ns=10000000000ULL;
    PEBQLSegment s=pe_bql_segment(&t,20,"fixture",77,0x5678,42,-1);
    pe_bql_event(&t,&s,20,2000020,1,7); assert(t.count==0);
    pe_bql_event(&t,&s,20,2000021,1,7); assert(t.count==1);
    assert(t.events[0].owner_hint==7 && t.events[0].line==77 &&
           t.events[0].kind==1 && t.events[0].tid==42);
    pe_bql_event(&t,&s,40,30,2,0); assert(t.clock_errors==1);
    pe_bql_event(&t,&s,0,40,2,0); assert(t.clock_errors==2);
    pe_bql_event(&t,&s,9990000000ULL,11000000000ULL,2,0);
    assert(t.count==2 && t.events[1].end_ns==t.deadline_ns &&
           t.events[1].flags==2);
    pe_bql_event(&t,&s,t.deadline_ns+1,t.deadline_ns+3000000,2,0);
    assert(t.count==2);
    char longpath[256]; memset(longpath,'x',255); longpath[255]=0;
    s.file=longpath; pe_bql_event(&t,&s,20,3000020,2,0);
    assert(t.events[2].flags==1 && t.events[2].file[127]==0);
    for (unsigned i=3;i<PE_BQL_CAPACITY;i++)
        pe_bql_event(&t,&s,20,3000020,2,0);
    pe_bql_event(&t,&s,20,3000020,2,0);
    assert(t.count==PE_BQL_CAPACITY && t.drops==1);
    for (unsigned i=0;i<t.count;i++) assert(t.events[i].serial==i);
    /* Real contested mutex: a wait and the owning hold overlap. */
    t.count=0; t.start_ns=pe_bql_clock(); t.deadline_ns=t.start_ns+1000000000;
    pthread_t th; pthread_create(&th,NULL,holder,NULL);
    /* Wait until holder owns lock without adding a bogus condition hold. */
    struct timespec setup={0,100000};
    while (!atomic_load(&held)) nanosleep(&setup,NULL);
    uint64_t begin=pe_bql_clock(); pthread_mutex_lock(&lock);
    uint64_t end=pe_bql_clock(); assert(held);
    s=pe_bql_segment(&t,end,"waiter",99,0xabcd,100,3);
    pe_bql_event(&t,&s,begin,end,1,1);
    pthread_mutex_unlock(&lock); pthread_join(th,NULL);
    assert(t.count==2 && t.events[0].kind==2 && t.events[1].kind==1);
    assert(t.events[1].end_ns-t.events[1].begin_ns>2000000);
    assert(t.events[0].begin_ns<t.events[1].end_ns &&
           t.events[0].end_ns>t.events[1].begin_ns);
    /* Condition wait releases the lock: split pre/post segments explicitly. */
    t.count=0; pthread_create(&th,NULL,wake,NULL); pthread_mutex_lock(&lock);
    begin=pe_bql_clock(); s=pe_bql_segment(&t,begin,"before-cond",1,0,1,-1);
    struct timespec small={0,4000000}; nanosleep(&small,NULL);
    pe_bql_event(&t,&s,begin,pe_bql_clock(),2,0);
    while (!awakened) pthread_cond_wait(&cond,&lock);
    begin=pe_bql_clock(); s=pe_bql_segment(&t,begin,"after-cond",2,0,1,-1);
    nanosleep(&small,NULL); pe_bql_event(&t,&s,begin,pe_bql_clock(),2,0);
    pthread_mutex_unlock(&lock); pthread_join(th,NULL);
    assert(t.count==2);
    for(unsigned i=0;i<2;i++) assert(t.events[i].end_ns-t.events[i].begin_ns<30000000);
    PEBQLMMIOTrace mt = {.events = calloc(PE_BQL_MMIO_CAPACITY,
                                         sizeof(PEBQLMMIOEvent))};
    assert(mt.events);
    PEBQLMMIOEvent me = {.begin_ns=20,.end_ns=3000021,.acquisition=77,
        .handler=0x1234,.addr=0x50,.tid=42,.cpu=3,.size=4,.write=1,
        .with_attrs=1,.result=2,.depth=2};
    pe_bql_mmio_event(&mt,&me,"GPU-MMIO",10,10000000);
    assert(mt.count==1 && mt.events[0].acquisition==77 &&
           mt.events[0].handler==0x1234 && mt.events[0].addr==0x50 &&
           mt.events[0].write==1 && mt.events[0].result==2 &&
           mt.events[0].depth==2 && !strcmp(mt.events[0].region,"GPU-MMIO"));
    me.end_ns=2000020; pe_bql_mmio_event(&mt,&me,"short",10,10000000);
    assert(mt.count==1);
    me.end_ns=11000000; pe_bql_mmio_event(&mt,&me,longpath,10,10000000);
    assert(mt.events[1].flags==3 && mt.events[1].end_ns==10000000);
    me.end_ns=3000021;
    for(unsigned i=2;i<PE_BQL_MMIO_CAPACITY;i++)
        pe_bql_mmio_event(&mt,&me,"attrs-read",10,10000000);
    pe_bql_mmio_event(&mt,&me,"overflow",10,10000000);
    assert(mt.drops==1 && mt.count==PE_BQL_MMIO_CAPACITY);
    me.end_ns=0; pe_bql_mmio_event(&mt,&me,"bad-clock",10,10000000);
    assert(mt.clock_errors==1);
    free(mt.events); free(t.events);
    puts("bounds, overflow, contention, condition-split, MMIO attribution PASS");
}
'''
with tempfile.TemporaryDirectory() as td:
    f=Path(td)/'test.c'; f.write_text(source); exe=Path(td)/'test'
    subprocess.run(['clang','-std=gnu11','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-pthread','-fsanitize=address,undefined','-I',str(root),str(f),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
# Integration gate: only wrapped condition functions may touch the BQL directly.
cpus=(root/'system/cpus.c').read_text()
assert cpus.count('qemu_cond_wait(cond, &bql);')==1
assert cpus.count('qemu_cond_timedwait(cond, &bql, ms);')==1
assert 'qemu_cond_wait(cpu->halt_cond, &bql)' not in cpus
assert 'qemu_cond_wait(&qemu_pause_cond, &bql)' not in cpus
assert 'qemu_cond_wait(&qemu_cpu_cond, &bql)' not in cpus
assert 'if (!prefix || !*prefix)' in cpus
assert cpus.index('if (!prefix || !*prefix)') < cpus.index('pe_bql_trace.start_ns = pe_bql_clock();')
memory=(root/'system/memory.c').read_text()
assert memory.count('poweremu_bql_mmio_begin() : 0;')==4
assert memory.count('poweremu_bql_mmio_end(pe_begin, memory_region_name(mr)')==4
assert 'POWEREMU_BQL_MMIO_TRACE' in cpus and 'mmio && !strcmp(mmio, "1")' in cpus
assert 's.acquisition != get_pe_bql_current().acquisition' in cpus
print(json.dumps({'valid':True,'sanitizers':['ASan','UBSan'],'actual_pthread_contention':True,'condition_sleep_excluded':True,'disabled_init_before_clock_or_allocation':True,'backend_built':False}))
