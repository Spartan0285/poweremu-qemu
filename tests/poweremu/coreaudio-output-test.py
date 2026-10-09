#!/usr/bin/env python3
"""Exercise the production CoreAudio callback with deterministic host buffers.

Extract the callback and ring-position helper, compiling against minimal device
stubs so empty/partial/wrapped playback can be checked without live speakers.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--source', type=Path, help='Alternate coreaudio.m for regression comparison')
parser.add_argument('--sanitizer', default='address,undefined', choices=['address,undefined', 'thread'])
args = parser.parse_args()
root = Path(__file__).resolve().parents[2]
source = (args.source or root / 'audio/coreaudio.m').read_text()


def function(text, signature):
    start = text.index(signature)
    opening = text.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


callback = function(source, 'static OSStatus audioDeviceIOProc(')
spsc = ''
if 'static OSStatus coreaudio_spsc_output(' in source:
    spsc = '\n'.join(function(source, signature) for signature in (
        'static size_t coreaudio_buffer_get_free(',
        'static void *coreaudio_get_buffer_out(',
        'static size_t coreaudio_put_buffer_out(',
        'static size_t coreaudio_write(',
        'static OSStatus coreaudio_spsc_output('))

ring = function((root / 'audio/audio_int.h').read_text(), 'static inline size_t audio_ring_posb(')
stubs = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <sched.h>
#include <stdlib.h>
#define MIN(a,b) ((a) < (b) ? (a) : (b))
#define qatomic_fetch_add(p,v) __atomic_fetch_add(p,v,__ATOMIC_SEQ_CST)
typedef uint32_t UInt32;
typedef uint32_t AudioDeviceID;
typedef int OSStatus;
typedef struct { int unused; } AudioTimeStamp;
typedef struct { UInt32 mNumberChannels, mDataByteSize; void *mData; } AudioBuffer;
typedef struct { UInt32 mNumberBuffers; AudioBuffer mBuffers[1]; } AudioBufferList;
typedef struct {
    struct { size_t bytes_per_frame; } info;
    size_t pending_emul, pos_emul, size_emul;
    uint8_t *buf_emul;
} HWVoiceOut;
typedef struct {
    HWVoiceOut hw;
    AudioDeviceID outputDeviceID;
    UInt32 audioDevicePropertyBufferFrameSize;
    bool spsc_output;
    uint64_t output_written, output_read;
    bool debug_audio;
    FILE *capture_file;
    uint64_t debug_callbacks, debug_empty_callbacks, debug_missing_frames;
    uint64_t debug_invocations, debug_unusable_callbacks;
} coreaudioVoiceOut;
static int coreaudio_buf_lock(coreaudioVoiceOut *c, const char *name) { assert(!c->spsc_output); return 0; }
static int coreaudio_buf_unlock(coreaudioVoiceOut *c, const char *name) { return 0; }
static void coreaudio_capture_push(coreaudioVoiceOut *c, const AudioBufferList *b) {}
'''
spsc_stubs = r'''
#define qatomic_load_acquire(p) __atomic_load_n(p, __ATOMIC_ACQUIRE)
#define qatomic_store_release(p,v) __atomic_store_n(p,v,__ATOMIC_RELEASE)
#define qatomic_fetch_add(p,v) __atomic_fetch_add(p,v,__ATOMIC_SEQ_CST)
static void coreaudio_debug_tick(coreaudioVoiceOut *c) {}
static void coreaudio_debug_produced(coreaudioVoiceOut *c, const void *p, size_t n) {}
static size_t coreaudio_legacy_buffer_get_free(HWVoiceOut *h) { abort(); }
static void *coreaudio_legacy_get_buffer_out(HWVoiceOut *h, size_t *n) { abort(); }
static size_t coreaudio_legacy_put_buffer_out(HWVoiceOut *h, void *p, size_t n) { abort(); }
static size_t coreaudio_legacy_write(HWVoiceOut *h, void *p, size_t n) { abort(); }
static size_t audio_generic_write(HWVoiceOut *h, void *p, size_t n);
'''
spsc_tests = r'''
static size_t audio_generic_write(HWVoiceOut *h, void *p, size_t n)
{
    size_t done = 0;
    size_t free = coreaudio_buffer_get_free(h);
    n = MIN(n, free);
    while (done < n) {
        size_t bytes;
        void *dst = coreaudio_get_buffer_out(h, &bytes);
        bytes = MIN(bytes, n - done);
        memcpy(dst, (uint8_t *)p + done, bytes);
        done += coreaudio_put_buffer_out(h, dst, bytes);
    }
    return done;
}
#define STRESS_FRAMES 200000
static coreaudioVoiceOut stress;
static void *producer(void *unused)
{
    for (uint32_t f = 1; f <= STRESS_FRAMES;) {
        uint32_t pcm[46];
        unsigned count = MIN(23, STRESS_FRAMES - f + 1);
        for (unsigned i = 0; i < count; i++) {
            pcm[i*2] = f+i;
            pcm[i*2+1] = (f+i) ^ 0x80000000u;
        }
        size_t bytes = coreaudio_write(&stress.hw, pcm, count*8);
        assert(bytes % 8 == 0);
        f += bytes/8;
        if (!bytes) sched_yield();
    }
    return NULL;
}
static void *consumer(void *unused)
{
    uint32_t next = 1;
    unsigned periods = 0;
    while (next <= STRESS_FRAMES) {
        struct { uint32_t before, pcm[62], after; } out;
        memset(&out, 0xa5, sizeof(out));
        unsigned frames = 1 + periods++ % 31;
        AudioBufferList b = { 1, {{2, frames*8, out.pcm}} };
        audioDeviceIOProc(7, NULL, NULL, NULL, &b, NULL, &stress);
        for (unsigned i = 0; i < frames; i++) {
            if (!out.pcm[i*2]) {
                assert(!out.pcm[i*2+1]);
            } else {
                assert(out.pcm[i*2] == next);
                assert(out.pcm[i*2+1] == (next ^ 0x80000000u));
                next++;
            }
        }
        assert(out.before == 0xa5a5a5a5 && out.after == 0xa5a5a5a5);
        for (unsigned i = frames*2; i < 62; i++) assert(out.pcm[i] == 0xa5a5a5a5);
        sched_yield();
    }
    return NULL;
}
static void test_spsc(void)
{
    uint8_t ring[256];
    stress = (coreaudioVoiceOut) { .hw = {
        .info = {8}, .size_emul = sizeof(ring), .buf_emul = ring },
        .outputDeviceID = 7, .spsc_output = true, .debug_audio = true };
    pthread_t p, c;
    assert(!pthread_create(&p, NULL, producer, NULL));
    assert(!pthread_create(&c, NULL, consumer, NULL));
    assert(!pthread_join(p, NULL)); assert(!pthread_join(c, NULL));
    assert(stress.output_written == STRESS_FRAMES*8ull);
    assert(stress.output_read == stress.output_written);
    assert(coreaudio_buffer_get_free(&stress.hw) == sizeof(ring));
    /* An obsolete device and disabled stream must not consume queued PCM. */
    uint32_t pcm[2] = {123,456}, out[2] = {9,9};
    assert(coreaudio_write(&stress.hw, pcm, 8) == 8);
    AudioBufferList b = {1, {{2,8,out}}};
    audioDeviceIOProc(8,NULL,NULL,NULL,&b,NULL,&stress);
    assert(!out[0] && !out[1]);
    assert(stress.output_written - stress.output_read == 8);
    b.mBuffers[0].mData = NULL;
    audioDeviceIOProc(7,NULL,NULL,NULL,&b,NULL,&stress);
    assert(stress.output_written - stress.output_read == 8);
    b.mNumberBuffers = 0;
    audioDeviceIOProc(7,NULL,NULL,NULL,&b,NULL,&stress);
    assert(stress.output_written - stress.output_read == 8);
    puts("PASS SPSC concurrent 200,000 ordered stereo frames, variable periods, wrap, bounds, old device and disabled buffers; callback cannot acquire mutex");
}
'''
tests = r'''
int main(void)
{
    /* Distinct stereo float PCM words make order, byte identity and padding
     * observable independently of the playback implementation. */
    uint32_t pcm[16];
    for (int i = 0; i < 16; i++) pcm[i] = 0x3e800000 + i * 0x10000;
    coreaudioVoiceOut c = { .hw = { .info = { 8 }, .size_emul = sizeof(pcm),
                                   .buf_emul = (uint8_t *)pcm },
        .outputDeviceID = 7, .audioDevicePropertyBufferFrameSize = 4,
        .debug_audio = true };
    struct { uint32_t before, pcm[8], after; } out;
    AudioBufferList buffers = { 1, {{ 2, sizeof(out.pcm), out.pcm }} };
    struct { size_t pending, pos, remaining; int frames[4]; } cases[] = {
        { 4, 5, 0, { 1, 2, 3, 4 } },  /* full */
        { 2, 4, 0, { 2, 3, -1, -1 } }, /* partial */
        { 3, 1, 0, { 6, 7, 0, -1 } },  /* wrapped partial */
        { 0, 3, 0, { -1, -1, -1, -1 } }, /* empty */
        { 6, 2, 2, { 4, 5, 6, 7 } },   /* retain next period */
        { 2, 2, 0, { 0, 1, -1, -1 } }, /* exact remaining tail */
    };
    for (unsigned i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
        memset(&out, 0xa5, sizeof(out));
        c.hw.pending_emul = cases[i].pending * 8;
        c.hw.pos_emul = cases[i].pos * 8;
        assert(audioDeviceIOProc(7, NULL, NULL, NULL, &buffers, NULL, &c) == 0);
        for (int f = 0; f < 4; f++) {
            for (int ch = 0; ch < 2; ch++) {
                uint32_t expected = cases[i].frames[f] < 0 ? 0 :
                    pcm[cases[i].frames[f] * 2 + ch];
                assert(out.pcm[f * 2 + ch] == expected);
            }
        }
        assert(c.hw.pending_emul == cases[i].remaining * 8);
        assert(c.hw.pos_emul == cases[i].pos * 8);
        assert(out.before == 0xa5a5a5a5 && out.after == 0xa5a5a5a5);
    }
    assert(c.debug_callbacks == 6 && c.debug_empty_callbacks == 4);
    assert(c.debug_missing_frames == 9);
    puts("PASS CoreAudio full/partial/empty/wrapped stereo playback, retained tail, bounds and counters");

    /* A smaller actual period must retain queued PCM and never overwrite
     * the memory after the host's supplied buffer, even with a stale cache. */
    memset(&out, 0xa5, sizeof(out));
    buffers.mBuffers[0].mDataByteSize = 16;
    c.hw.pending_emul = 32; c.hw.pos_emul = 32;
    audioDeviceIOProc(7,NULL,NULL,NULL,&buffers,NULL,&c);
    assert(c.hw.pending_emul == 16);
    for(int j=0;j<4;j++)assert(out.pcm[j]==pcm[j]);
    for(int j=4;j<8;j++)assert(out.pcm[j]==0xa5a5a5a5);
    uint32_t wide[12];memset(wide,0xa5,sizeof wide);
    buffers.mBuffers[0].mDataByteSize=sizeof wide;buffers.mBuffers[0].mData=wide;
    c.hw.pending_emul=32;c.hw.pos_emul=32;
    audioDeviceIOProc(7,NULL,NULL,NULL,&buffers,NULL,&c);
    for(int j=0;j<8;j++)assert(wide[j]==pcm[j]);
    for(int j=8;j<12;j++)assert(wide[j]==0);
    assert(c.hw.pending_emul==0);
    puts("PASS legacy callback respects actual smaller/larger host periods");
    TEST_SPSC();
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='pe-coreaudio-test-') as d:
    path = Path(d)
    (path / 'test.c').write_text(stubs + (spsc_stubs if spsc else '') + ring + '\n' + spsc + '\n' + callback + '\n' + (spsc_tests if spsc else '') + '\n#define TEST_SPSC() ' + ('test_spsc()' if spsc else '((void)0)') + '\n' + tests)
    subprocess.run([os.environ.get('CC', 'clang'), '-std=gnu11', '-O2',
                    '-pthread', '-fsanitize=' + args.sanitizer, str(path / 'test.c'),
                    '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True, timeout=30)
