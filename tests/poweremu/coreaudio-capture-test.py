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
#include <errno.h>
#include <unistd.h>
#define QEMU_CLOCK_REALTIME 0
static int64_t qemu_clock_get_ns(int clock) {static int64_t ns;return ++ns;}
typedef struct {int64_t wall_ns;uint64_t pcm_offset,bytes,copied_bytes;} CoreaudioCapturePacket;
#define MIN(a,b) ((a)<(b)?(a):(b))
#define qatomic_load_acquire(p) __atomic_load_n(p,__ATOMIC_ACQUIRE)
#define qatomic_store_release(p,v) __atomic_store_n(p,v,__ATOMIC_RELEASE)
#define qatomic_fetch_add(p,v) __atomic_fetch_add(p,v,__ATOMIC_SEQ_CST)
#define g_free free
#define dolog(...) ((void)0)
typedef struct {uint32_t mNumberChannels,mDataByteSize;void *mData;} AudioBuffer;
typedef struct {uint32_t mNumberBuffers;AudioBuffer mBuffers[1];} AudioBufferList;
typedef struct {uint8_t *capture_pcm;size_t capture_size;uint64_t capture_written,capture_read,capture_dropped;FILE *capture_file;bool capture_deferred;int64_t capture_last_ms;CoreaudioCapturePacket *capture_packets;size_t capture_packet_count,capture_packet_capacity;uint64_t capture_packet_dropped;FILE *capture_packet_file;} coreaudioVoiceOut;
'''
code += '\n'.join(function(sig) for sig in (
    'static void coreaudio_capture_push(',
    'static void coreaudio_capture_drain(',
    'static void coreaudio_capture_end(',
    'static unsigned coreaudio_capture_seconds(',
    'static void coreaudio_capture_poll('))
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
        coreaudio_capture_push(&core,&b,b.mBuffers[0].mDataByteSize);
        size_t got=core.capture_written-before;
        memcpy(expected+accepted,pcm,got);accepted+=got;
        if((i%13)==0)sched_yield();
    }
    qatomic_store_release(&finished,true);return NULL;
}
int main(void) {
    unsetenv("POWEREMU_COREAUDIO_CAPTURE_SECONDS");
    assert(coreaudio_capture_seconds()==0);
    const char *invalid[]={"", "0", "-1", "+1", " 1", "601", "180x", "9999999999999999999999999"};
    for(unsigned i=0;i<sizeof(invalid)/sizeof(invalid[0]);i++){
        setenv("POWEREMU_COREAUDIO_CAPTURE_SECONDS",invalid[i],1);
        assert(coreaudio_capture_seconds()==0);
    }
    setenv("POWEREMU_COREAUDIO_CAPTURE_SECONDS","180",1);
    assert(coreaudio_capture_seconds()==180);
    setenv("POWEREMU_COREAUDIO_CAPTURE_SECONDS","600",1);
    assert(coreaudio_capture_seconds()==600);
    core.capture_size=4096;core.capture_pcm=malloc(core.capture_size);core.capture_file=tmpfile();assert(core.capture_file);
    /* Full ring, overflow, partial space and wrap are explicit, independent
     * of scheduling in the concurrent test. */
    uint8_t pcm[2048];memset(pcm,0xa7,sizeof pcm);
    AudioBufferList b={1,{{2,sizeof pcm,pcm}}};
    coreaudio_capture_push(&core,&b,b.mBuffers[0].mDataByteSize);coreaudio_capture_push(&core,&b,b.mBuffers[0].mDataByteSize);coreaudio_capture_push(&core,&b,b.mBuffers[0].mDataByteSize);
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
    /* In bounded mode, main-thread polling never writes during playback.
     * After overflow, stop flushes exactly the retained prefix in order. */
    core=(coreaudioVoiceOut){.capture_size=4096,.capture_pcm=malloc(4096),
        .capture_file=tmpfile(),.capture_deferred=true};
    assert(core.capture_file);
    core.capture_packet_capacity=2;
    core.capture_packets=calloc(2,sizeof(*core.capture_packets));
    core.capture_packet_file=tmpfile();assert(core.capture_packet_file);
    int metadata_fd=dup(fileno(core.capture_packet_file));assert(metadata_fd>=0);
    coreaudio_capture_push(&core,&b,b.mBuffers[0].mDataByteSize);coreaudio_capture_poll(&core,100);
    coreaudio_capture_push(&core,&b,b.mBuffers[0].mDataByteSize);coreaudio_capture_poll(&core,200);
    coreaudio_capture_push(&core,&b,b.mBuffers[0].mDataByteSize);coreaudio_capture_poll(&core,300);
    assert(ftell(core.capture_file)==0&&core.capture_read==0);
    assert(ftell(core.capture_packet_file)==0&&core.capture_packet_count==2);
    assert(core.capture_packet_dropped==1);
    assert(core.capture_packets[0].pcm_offset==0&&core.capture_packets[1].pcm_offset==2048);
    assert(core.capture_packets[0].copied_bytes==2048);
    assert(core.capture_packets[0].bytes==2048&&core.capture_packets[1].wall_ns>core.capture_packets[0].wall_ns);
    assert(core.capture_written==4096&&core.capture_dropped==2048);
    int saved=dup(fileno(core.capture_file));assert(saved>=0);
    coreaudio_capture_end(&core);
    assert(!core.capture_deferred&&!core.capture_file&&!core.capture_pcm);
    FILE *record=fdopen(saved,"rb");assert(record);
    rewind(record);uint8_t captured[4096];assert(fread(captured,1,4096,record)==4096);
    for(unsigned i=0;i<4096;i++)assert(captured[i]==0xa7);
    fclose(record);
    puts("PASS diagnostic PCM ring: full/overflow/wrap, concurrent callback-to-file ordering, exact drop count, stopped cleanup");
    FILE *metadata=fdopen(metadata_fd,"rb");assert(metadata);rewind(metadata);
    CoreaudioCapturePacket packets[2];assert(fread(packets,sizeof(packets[0]),2,metadata)==2);
    assert(packets[0].pcm_offset==0&&packets[1].pcm_offset==2048);
    assert(fgetc(metadata)==EOF);fclose(metadata);
    assert(!core.capture_packets&&!core.capture_packet_file);
    puts("PASS bounded capture: option validation, no playback file writes, overflow accounting and complete stopped flush");
}
'''
with tempfile.TemporaryDirectory(prefix='poweremu-capture-test-') as tmp:
    path=Path(tmp)/'test.c';path.write_text(code);exe=Path(tmp)/'test'
    subprocess.run(['clang','-std=c11','-O1','-g','-pthread',f'-fsanitize={args.sanitizer}',str(path),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
