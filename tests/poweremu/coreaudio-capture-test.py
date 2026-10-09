#!/usr/bin/env python3
"""Check the optional callback-to-file diagnostic ring using production code."""
import argparse
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--sanitizer', choices=['address,undefined', 'thread'], default='address,undefined')
args = parser.parse_args()
root = Path(__file__).resolve().parents[2]
source = (root / 'audio/coreaudio.m').read_text()
def function(signature):
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]
code = r'''
#include <assert.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sched.h>
#define MIN(a,b) ((a)<(b)?(a):(b))
#define qatomic_load_acquire(p) __atomic_load_n(p,__ATOMIC_ACQUIRE)
#define qatomic_store_release(p,v) __atomic_store_n(p,v,__ATOMIC_RELEASE)
#define qatomic_fetch_add(p,v) __atomic_fetch_add(p,v,__ATOMIC_SEQ_CST)
#define g_free free
#define dolog(...) ((void)0)
typedef struct {uint32_t mNumberChannels,mDataByteSize;void *mData;} AudioBuffer;
typedef struct {uint32_t mNumberBuffers;AudioBuffer mBuffers[1];} AudioBufferList;
typedef struct {uint8_t *capture_pcm;size_t capture_size;uint64_t capture_written,capture_read,capture_dropped;FILE *capture_file;} coreaudioVoiceOut;
'''
code += '\n'.join(function(sig) for sig in (
    'static void coreaudio_capture_push(',
    'static void coreaudio_capture_drain(',
    'static void coreaudio_capture_end('))
code += r'''
#define PACKETS 20000
static coreaudioVoiceOut core;
static uint8_t expected[PACKETS*256];
static size_t accepted;
static bool finished;
static void *producer(void *unused) {
    uint8_t pcm[256];
    for (unsigned i=0;i<PACKETS;i++) {
        size_t n=8*(1+(i%32));
        for(size_t j=0;j<n;j++)pcm[j]=(uint8_t)(i+j);
        AudioBufferList b={1,{{2,(uint32_t)n,pcm}}};
        uint64_t before=core.capture_written;
        coreaudio_capture_push(&core,&b);
        size_t got=core.capture_written-before;
        memcpy(expected+accepted,pcm,got);accepted+=got;
        if((i%13)==0)sched_yield();
    }
    qatomic_store_release(&finished,true);return NULL;
}
int main(void) {
    core.capture_size=4096;core.capture_pcm=malloc(core.capture_size);core.capture_file=tmpfile();assert(core.capture_file);
    /* Full ring, overflow, partial space and wrap are explicit, independent
     * of scheduling in the concurrent test. */
    uint8_t pcm[2048];memset(pcm,0xa7,sizeof pcm);
    AudioBufferList b={1,{{2,sizeof pcm,pcm}}};
    coreaudio_capture_push(&core,&b);coreaudio_capture_push(&core,&b);coreaudio_capture_push(&core,&b);
    assert(core.capture_written==4096&&core.capture_dropped==2048);
    coreaudio_capture_drain(&core);assert(core.capture_read==4096);
    assert(ftell(core.capture_file)==4096);
    fclose(core.capture_file);core.capture_file=tmpfile();assert(core.capture_file);
    core.capture_written=core.capture_read=core.capture_dropped=0;
    pthread_t worker;assert(!pthread_create(&worker,NULL,producer,NULL));
    while(!qatomic_load_acquire(&finished)){coreaudio_capture_drain(&core);sched_yield();}
    pthread_join(worker,NULL);coreaudio_capture_drain(&core);
    assert(core.capture_written==core.capture_read);
    assert((size_t)ftell(core.capture_file)==accepted);
    rewind(core.capture_file);uint8_t *actual=malloc(accepted);assert(fread(actual,1,accepted,core.capture_file)==accepted);
    assert(!memcmp(actual,expected,accepted));free(actual);
    size_t total=0;for(unsigned i=0;i<PACKETS;i++)total+=8*(1+(i%32));
    assert(accepted+core.capture_dropped==total);
    coreaudio_capture_end(&core);assert(!core.capture_pcm&&!core.capture_file);
    puts("PASS diagnostic PCM ring: full/overflow/wrap, concurrent callback-to-file ordering, exact drop count, stopped cleanup");
}
'''
with tempfile.TemporaryDirectory(prefix='poweremu-capture-test-') as tmp:
    path=Path(tmp)/'test.c';path.write_text(code);exe=Path(tmp)/'test'
    subprocess.run(['clang','-std=c11','-O1','-g','-pthread',f'-fsanitize={args.sanitizer}',str(path),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
