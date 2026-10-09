#!/usr/bin/env python3
"""Replay production pacing and descriptor entry points with synchronous DMA.

The timer dispatcher clears pending before calling the pacer, as QEMU does.
New 32-frame descriptors arrive synchronously during the DMA transfer. This
checks conservation of fractional frames, TX/RX continuation, idle and restart.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--source', type=Path)
a = p.parse_args()
root = Path(__file__).resolve().parents[2]
s = (a.source or root / 'hw/audio/screamer.c').read_text()
def function(signature):
    start = s.index(signature)
    end = s.index('{', start) + 1
    depth = 1
    while depth:
        depth += (s[end] == '{') - (s[end] == '}')
        end += 1
    return s[start:end]
source = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#define MIN(a,b) ((a)<(b)?(a):(b))
#define MAX(a,b) ((a)>(b)?(a):(b))
#define SCALE_MS 1000000LL
#define NANOSECONDS_PER_SECOND 1000000000LL
#define SCREAMER_PACE_NS SCALE_MS
#define QEMU_CLOCK_REALTIME 0
#define QEMU_CLOCK_VIRTUAL 1
#define SCREAMER_DPRINTF(...) ((void)0)
#define DBDMA_STATUS 0
#define RUN 1
#define AUD_log(...) ((void)0)
typedef struct { bool pending; int64_t deadline; } QEMUTimer;
typedef struct { unsigned regs[4]; } DBDMA_channel;
typedef struct { void *opaque; DBDMA_channel *channel; int len; void (*dma_end)(void *); } DBDMA_io;
typedef struct {
    QEMUTimer *pace_timer;
    int64_t pace_last, pace_frac, pace_idle;
    int rate; DBDMA_io io, in_io; bool input, in_running; void *in_voice;
} ScreamerState;
static int64_t now;
static int64_t qemu_clock_get_ns(int clock) { return now; }
static bool timer_pending(QEMUTimer *t) { return t->pending; }
static void timer_mod(QEMUTimer *t, int64_t deadline) { t->pending=true; t->deadline=deadline; }
static void timer_del(QEMUTimer *t) { t->pending=false; }
static int sdbg_on;
static uint64_t sdbg_exact_run;
static int64_t sdbg_pace_host_last;
static struct {
    uint64_t pace_max_ns, pace_discarded, host_step_max_ns;
    int64_t host_step_begin, host_step_end;
    uint64_t tx_budget_lost, tx_descriptor_empty;
} sdbg;
static void pmac_screamer_tx(DBDMA_io *io);
static void pmac_screamer_rx(DBDMA_io *io);
static uint64_t tx_frames, rx_frames;
static bool host_queue_full;
static int pmac_screamer_tx_transfer(ScreamerState *s, int max)
{
    if (host_queue_full) return 0;
    int n = MIN(max, s->io.len/4);
    tx_frames += n; s->io.len -= n*4;
    if (!s->io.len) {
        DBDMA_io next = {.opaque=s, .len=32*4};
        pmac_screamer_tx(&next);
    }
    return n;
}
static int pmac_screamer_rx_transfer(ScreamerState *s, int max)
{
    int n = MIN(max, s->in_io.len/4);
    rx_frames += n; s->in_io.len -= n*4;
    if (!s->in_io.len) {
        DBDMA_channel ch = {0};
        DBDMA_io next = {.opaque=s, .channel=&ch, .len=32*4};
        pmac_screamer_rx(&next);
    }
    return n;
}
'''
source += '\n'.join(function(sig) for sig in (
    'static void screamer_pace_cb(', 'static void pmac_screamer_tx(DBDMA_io *io)',
    'static void pmac_screamer_rx(DBDMA_io *io)'))
source += r'''
int main(void)
{
    int rates[] = {22050,32000,44100,48000};
    for (unsigned r=0; r<4; r++) for (unsigned duplex=0; duplex<2; duplex++) {
        QEMUTimer timer = {0};
        ScreamerState s = {.pace_timer=&timer, .rate=rates[r],
            .input=duplex, .in_voice=&timer, .in_running=duplex};
        DBDMA_io next = {.opaque=&s, .len=32*4};
        now=0; tx_frames=rx_frames=0; sdbg_on=1; memset(&sdbg,0,sizeof(sdbg));
        pmac_screamer_tx(&next); s.in_io=next;
        for (unsigned tick=0; tick<10000; tick++) {
            assert(timer.pending);
            now += tick%3==0 ? 1100000 : tick%3==1 ? 1734567 : 2400123;
            timer.pending=false; /* production timer dispatch semantics */
            screamer_pace_cb(&s);
            assert(timer.pending);
            uint64_t expected = now*s.rate/NANOSECONDS_PER_SECOND;
            assert(tx_frames == expected);
            if (duplex) assert(rx_frames == expected);
            assert(!sdbg.pace_discarded && !sdbg.tx_budget_lost);
        }

        if (!duplex) {
            uint64_t before=tx_frames;
            host_queue_full=true;
            for(unsigned tick=0;tick<500;tick++) {
                now+=SCALE_MS;timer.pending=false;screamer_pace_cb(&s);
                assert(timer.pending && s.io.len);
            }
            assert(tx_frames==before);
            host_queue_full=false;
            now+=SCALE_MS;timer.pending=false;screamer_pace_cb(&s);
            assert(timer.pending && tx_frames>before);
        }
        s.io.len=s.in_io.len=0;
        for (unsigned tick=0; tick<201; tick++) {
            now += SCALE_MS; timer.pending=false; screamer_pace_cb(&s);
        }
        assert(!timer.pending);
        now += 10*NANOSECONDS_PER_SECOND;
        pmac_screamer_tx(&next);
        assert(timer.pending && s.pace_last==now && !s.pace_frac);
    }
    puts("PASS exact fractional-frame conservation over 10,000 irregular ticks at four rates, synchronous TX/RX descriptors, idle stop/restart, 500 ms host backpressure and recovery");
}
'''
with tempfile.TemporaryDirectory(prefix='pe-screamer-pacing-') as d:
    d=Path(d);(d/'test.c').write_text(source)
    subprocess.run(['clang','-O2','-fsanitize=address,undefined',str(d/'test.c'),'-o',str(d/'test')],check=True)
    subprocess.run([str(d/'test')],check=True,timeout=30)
