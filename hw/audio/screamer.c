/*
 * QEMU PowerMac Awacs Screamer device support
 *
 * Copyright (c) 2016 Mark Cave-Ayland
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"
#include "audio/audio.h"
#include "hw/hw.h"
#include "hw/irq.h"
#include "audio/audio.h"
#include "hw/audio/screamer.h"
#include "hw/qdev-properties.h"
#include "qemu/timer.h"
#include "hw/ppc/mac_dbdma.h"
#include "system/system.h"
#include "qemu/cutils.h"
#include "qemu/log.h"
#include "qemu/typedefs.h"

/* debug screamer */
//#define DEBUG_SCREAMER

#ifdef DEBUG_SCREAMER
#define SCREAMER_DPRINTF(fmt, ...)                                  \
    do { printf("SCREAMER: " fmt , ## __VA_ARGS__); } while (0)
#else
#define SCREAMER_DPRINTF(fmt, ...)
#endif

/* chip registers */
#define SND_CTRL_REG   0x0
#define CODEC_CTRL_REG 0x1
#define CODEC_STAT_REG 0x2
#define CLIP_CNT_REG   0x3
#define BYTE_SWAP_REG  0x4
#define FRAME_CNT_REG  0x5

#define CODEC_CTRL_MASKECMD        (0x1 << 24)
#define CODEC_CTRL1_RECALIBRATE    0x4

#define CODEC_STAT_MANUFACTURER_CRYSTAL    0x100
#define CODEC_STAT_MANUFACTURER_NATIONAL   0x200
#define CODEC_STAT_AWACS_REVISION          0x3000
#define CODEC_STAT_MASK_VALID              (0x1 << 22)

/*
 * Sound input, in the codec's own terms.
 *
 * Codec control register 0 carries the whole of it: which source the input
 * multiplexer is listening to, and how much gain to give each channel.
 * Apple's AppleScreamerAudio, Linux's sound/ppc/awacs.h and NetBSD's
 * awacs.c agree on every bit; the names below are Apple's.
 */
#define CODEC0_GAIN_RIGHT                  0x00f
#define CODEC0_GAIN_LEFT                   0x0f0
#define CODEC0_NOT_MIC_PREAMP              0x100
#define CODEC0_INPUT_MUX                   0xe00   /* CD 0x200, mic 0x400,
                                                      line 0x800 */
/*
 * The sound control register's input subframe.  Apple writes subframe 0 for
 * input and for output once, at initialisation; a guest that has not chosen
 * an input subframe is not recording, whatever else it has set.
 */
#define SND_CTRL_IN_SUBFRAME               0x00f

/*
 * The sense nibble of the status register says what is plugged in, and the
 * driver reads it through a bit reversal (AppleScreamerAudio's
 * sndHWGetInSenseBits).  The device tree this machine publishes turns bit 1
 * into "a microphone is connected", which is the one that makes Mac OS X
 * offer an input at all.
 */
#define CODEC_STAT_LINE_IN_SENSE           0x2

/* Audio */
static const char *s_spk = "screamer";

/*
 * mixbuf is a ring of s->samples frames; wpos/rpos count frames written by
 * DMA and consumed by the audio backend (wpos - rpos = queued).  DMA refills
 * whenever there is room and the callback drains what is queued, so output
 * never alternates between a full buffer and an empty one.  The old scheme -
 * a buffer the size of one backend period that DMA could only refill once
 * it was completely drained - left a gap every period (crackling, far worse
 * when the guest was busy).
 */
/*
 * Keep only a small lead over real time: Mac OS X's audio engine times the
 * stream from DMA progress (it timestamps each wrap of its circular buffer),
 * so letting DMA run hundreds of ms ahead stalls it (dropouts).
 */
#define SCREAMER_RING_FRAMES 8192       /* ~186 ms at 44.1 kHz */
/* Jitter buffer: start (and restart after an underrun) with this much queued,
 * so the backend's timer and the DMA pacing timer can drift in phase. */
#define SCREAMER_PRIME_MS 40

static struct {
    int64_t last;
    uint64_t pulled, written, underruns, zero_runs, zero_frames, reads[16];
    uint64_t pace_max_ns, pace_discarded, overflow_frames;
    uint64_t tx_budget_lost, tx_descriptor_empty, tx_descriptor_completions;
    uint64_t prime_waits;
    uint64_t exact_zero_frames, exact_zero_runs, exact_zero_max;
    uint64_t host_step_max_ns;
    int64_t host_step_begin, host_step_end;
    int zr;
} sdbg;

/* Preserve a run across reporting intervals; these are exact stereo zeros,
 * unlike the historical left-channel low-amplitude counter above. */
static uint64_t sdbg_exact_run;
static int64_t sdbg_pace_host_last;

static int sdbg_on = -1;                /* SCREAMER_DEBUG=1: per-second stats */

static void sdbg_tick(ScreamerState *s)
{
    if (sdbg_on < 0) {
        sdbg_on = getenv("SCREAMER_DEBUG") != NULL;
    }
    if (!sdbg_on) {
        return;
    }
    int64_t now = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
    if (now - sdbg.last >= 1000) {
        qemu_log("screamer: pulled %" PRIu64 " written %" PRIu64 " underruns %"
                 PRIu64 " dma-silent-runs %" PRIu64 " (%" PRIu64 " frames) queued %d\n",
                 sdbg.pulled, sdbg.written, sdbg.underruns, sdbg.zero_runs,
                 sdbg.zero_frames, (int)(s->wpos - s->rpos));
        qemu_log("screamer: timer-max-us %" PRIu64 " catchup-discarded %"
                 PRIu64 " ring-overflow %" PRIu64 " frames\n",
                 sdbg.pace_max_ns / 1000, sdbg.pace_discarded,
                 sdbg.overflow_frames);
        qemu_log("screamer-timing: wall-ns %" PRId64
                 " virtual-ns %" PRId64 " host-step-max-us %" PRIu64
                 " step-begin-ns %" PRId64 " step-end-ns %" PRId64
                 " dma-exact-zero-frames %" PRIu64
                 " runs-ge64 %" PRIu64 " max-run-frames %" PRIu64 "\n",
                 qemu_clock_get_ns(QEMU_CLOCK_REALTIME),
                 qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),
                 sdbg.host_step_max_ns / 1000,
                 sdbg.host_step_begin, sdbg.host_step_end,
                 sdbg.exact_zero_frames, sdbg.exact_zero_runs,
                 sdbg.exact_zero_max);
        qemu_log("screamer: reads");
        for (int r = 0; r < 16; r++) {
            if (sdbg.reads[r]) {
                qemu_log(" r%d=%" PRIu64, r, sdbg.reads[r]);
            }
        }
        qemu_log("\n");
        qemu_log("screamer: tx-budget-lost %" PRIu64
                 " descriptor-empty-ticks %" PRIu64
                 " descriptor-completions %" PRIu64
                 " prime-waits %" PRIu64 "\n",
                 sdbg.tx_budget_lost, sdbg.tx_descriptor_empty,
                 sdbg.tx_descriptor_completions, sdbg.prime_waits);
        memset(&sdbg, 0, sizeof(sdbg));
        sdbg.last = now;
    }
}

static int pmac_screamer_rx_transfer(ScreamerState *s, int max);

/* Move up to max frames of the current TX descriptor into the ring. */
static int pmac_screamer_tx_transfer(ScreamerState *s, int max)
{
    DBDMA_io *io = &s->io;
    int samples;

    /* The guest hardware clock must not stop when the host output stalls.
     * Retain the newest frames and discard only stale queued host audio. */
    samples = MIN(MIN(io->len >> s->shift, max), s->samples);
    int overflow = samples - (s->samples - (s->wpos - s->rpos));
    if (overflow > 0) {
        sdbg.overflow_frames += overflow;
        s->rpos += overflow;
    }
    if (s->rpos >= s->samples) {
        int base = s->rpos - s->rpos % s->samples;
        s->rpos -= base;
        s->wpos -= base;
    }
    for (int done = 0; done < samples;) {
        int at = (s->wpos + done) % s->samples;
        int n = MIN(samples - done, s->samples - at);
        dma_memory_read(&address_space_memory, io->addr + (done << s->shift),
                        &s->mixbuf[at << s->shift], n << s->shift,
                        MEMTXATTRS_UNSPECIFIED);
        done += n;
    }

    SCREAMER_DPRINTF("DMA actually transferred 0x%x, wpos is %d\n", samples << s->shift, s->wpos);
    s->regs[FRAME_CNT_REG] += samples;   /* frames "played": DMA-paced time */
    sdbg.pulled += samples;
    for (int k = 0; sdbg_on > 0 && k < samples; k++) {
        const uint8_t *f = &s->mixbuf[((s->wpos + k) % s->samples) << s->shift];
        int16_t l = (int16_t)((f[0] << 8) | f[1]);
        if (!(f[0] | f[1] | f[2] | f[3])) {
            sdbg.exact_zero_frames++;
            if (++sdbg_exact_run == 64) {
                sdbg.exact_zero_runs++;
            }
            sdbg.exact_zero_max = MAX(sdbg.exact_zero_max, sdbg_exact_run);
        } else {
            sdbg_exact_run = 0;
        }
        if (l > -200 && l < 200) {
            if (++sdbg.zr == 64) {
                sdbg.zero_runs++;
            }
            sdbg.zero_frames++;
        } else {
            sdbg.zr = 0;
        }
    }

    io->addr += (samples << s->shift);
    io->len -= (samples << s->shift);
    s->wpos += samples;

    /*
     * There is sound to play again, so take part in mixing again.  See the
     * matching deactivation in the callback.
     */
    if (samples && !s->active) {
        s->active = true;
        AUD_set_active_out(s->voice, true);
    }

    /* Continue DBDMA if we have completed the transfer, otherwise defer */
    if (io->len == 0 && samples) {
        sdbg.tx_descriptor_completions++;
        SCREAMER_DPRINTF("-> End of transfer\n");
        io->dma_end(io);
    }
    return samples;
}

/*
 * The real Screamer consumes TX DMA at exactly the sample rate, and Mac OS
 * X's audio engine keeps its clock from that (it timestamps each wrap of
 * its DMA ring).  Pull DMA here at the sample rate on the virtual clock; the
 * audio backend drains the ring independently.  Pacing DMA by backend
 * buffer events instead either underran (gaps) or let the guest engine's
 * clock drift so it overwrote audio before it was read (lost samples).
 */
#define SCREAMER_PACE_NS (1 * SCALE_MS)

static void screamer_pace_cb(void *opaque)
{
    ScreamerState *s = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    if (sdbg_on > 0) {
        int64_t host_now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
        if (sdbg_pace_host_last &&
            host_now - sdbg_pace_host_last > sdbg.host_step_max_ns) {
            sdbg.host_step_max_ns = host_now - sdbg_pace_host_last;
            sdbg.host_step_begin = sdbg_pace_host_last;
            sdbg.host_step_end = host_now;
        }
        sdbg_pace_host_last = host_now;
        sdbg.pace_max_ns = MAX(sdbg.pace_max_ns, now - s->pace_last);
    }
    s->pace_frac += (now - s->pace_last) * (int64_t)s->rate;
    s->pace_last = now;
    int64_t due = s->pace_frac / NANOSECONDS_PER_SECOND;
    s->pace_frac -= due * NANOSECONDS_PER_SECOND;
    if (due > (int64_t)s->rate / 20) {
        sdbg.pace_discarded += due - (int64_t)s->rate / 20;
    }
    due = MIN(due, (int64_t)s->rate / 20);    /* at most 50 ms after a stall */

    /* A dispatched timer is no longer pending. Completing a DMA descriptor
     * below may synchronously start the next one, whose TX/RX entry point
     * uses timer_pending() to detect a genuinely idle hardware clock. Arm
     * the next tick first so that continuation cannot reset pace_frac and
     * throw away the fractional frames accumulated on every tick. */
    timer_mod(s->pace_timer, now + SCREAMER_PACE_NS);
    int moved = 0;
    int64_t out_left = due, in_left = due;
    while (out_left > 0 && s->io.len) {
        int n = pmac_screamer_tx_transfer(s, out_left);
        if (n == 0) {
            break;                          /* ring full: backend is behind */
        }
        out_left -= n;
        moved += n;
    }
    /*
     * Input is spent from the same budget on the same tick.
     *
     * There is one chip and one clock here, and Mac OS X believes it: both
     * streams belong to a single IOAudioEngine whose clock is the frame
     * counter the output transfer above advances, and whose timestamps are
     * taken on the output interrupt.  Pacing input from a timer of its own
     * let the two drift, and a drifting input is read either before it has
     * been written or after it has been overwritten.
     */
    while (in_left > 0 && s->in_running && s->in_io.len) {
        int n = pmac_screamer_rx_transfer(s, in_left);
        if (n == 0) {
            break;
        }
        in_left -= n;
        moved += n;
    }
    if (moved) {
        /* DBDMA may defer the next descriptor to a bottom half. Preserve
         * the unused budget across that boundary instead of losing a
         * fraction of a timer tick on every descriptor completion. */
        s->pace_frac += MIN(out_left, in_left) * NANOSECONDS_PER_SECOND;
        s->pace_idle = 0;
    } else if (s->io.len || (s->in_running && s->in_io.len)) {
        /* A full host queue is backpressure, not an idle guest DMA engine.
         * Keep polling so output resumes when the backend drains again. */
        s->pace_idle = 0;
    } else if (++s->pace_idle > 200) {      /* ~200 ms without DMA: stop */
        sdbg_pace_host_last = 0;
        sdbg_exact_run = 0;
        timer_del(s->pace_timer);
        return;
    }
    if (sdbg_on > 0) {
        int64_t carried = moved ? MIN(out_left, in_left) : 0;
        sdbg.tx_budget_lost += out_left - carried;
        sdbg.tx_descriptor_empty += due > 0 && !s->io.len;
    }
}

static void pmac_screamer_tx(DBDMA_io *io)
{
    ScreamerState *s = io->opaque;

    SCREAMER_DPRINTF("DMA TX transfer: addr %" HWADDR_PRIx
                     " len: %x\n", io->addr, io->len);

    memcpy(&s->io, io, sizeof(DBDMA_io));
    s->pace_idle = 0;
    if (!timer_pending(s->pace_timer)) {
        s->pace_last = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        s->pace_frac = 0;
        timer_mod(s->pace_timer, s->pace_last + SCREAMER_PACE_NS);
    }
}

static void pmac_screamer_tx_flush(DBDMA_io *io)
{
    DBDMA_channel *ch = io->channel;
    dbdma_cmd *current = &ch->current;
    uint16_t cmd;

    SCREAMER_DPRINTF("DMA TX flush!\n");
#if 0
    cmd = le16_to_cpu(current->command) & COMMAND_MASK;
    if (cmd == OUTPUT_MORE || cmd == OUTPUT_LAST ||
        cmd == INPUT_MORE || cmd == INPUT_LAST) {
        current->xfer_status = cpu_to_le16(ch->regs[DBDMA_STATUS]);
        current->res_count = cpu_to_le16(io->len);

        dma_memory_write(&address_space_memory, ch->regs[DBDMA_CMDPTR_LO],
                         &ch->current, sizeof(dbdma_cmd),
                         MEMTXATTRS_UNSPECIFIED);
    }
#endif

    ch->io.processing = false;

    cmd = le16_to_cpu(current->command) & COMMAND_MASK;
    if (cmd == INPUT_MORE || cmd == INPUT_LAST) {
        current->xfer_status = cpu_to_le16(ch->regs[DBDMA_STATUS]);
        current->res_count = cpu_to_le16(io->len);

            dma_memory_write(&address_space_memory, ch->regs[DBDMA_CMDPTR_LO],
                         &ch->current, sizeof(dbdma_cmd),
                         MEMTXATTRS_UNSPECIFIED);
    }

}

/*
 * Sound input.
 *
 * The real Screamer fills input DMA at exactly the sample rate, and Mac OS X
 * reads its input stream's clock from how far that DMA has got -- the same
 * arrangement as the output side, and it needs the same treatment: a timer on
 * the virtual clock moves frames at the sample rate, and the host's capture
 * callback fills a ring behind it.
 *
 * What the ring does not have, it makes up as silence rather than waiting.
 * A host that is slow, or has no microphone, or whose owner has not given
 * this Mac permission to listen, must not stop the guest's input clock:
 * Mac OS X would read that as the stream stalling, and the application
 * recording from it hangs rather than recording quiet.
 */
static int pmac_screamer_rx_transfer(ScreamerState *s, int max)
{
    DBDMA_io *io = &s->in_io;
    int frames = MIN(io->len >> s->shift, max);
    int queued = s->in_wpos - s->in_rpos;
    int done;

    if (frames <= 0) {
        return 0;
    }
    for (done = 0; done < frames;) {
        int at = (s->in_rpos + done) % s->in_samples;
        int n = MIN(frames - done, s->in_samples - at);

        if (done < queued) {
            n = MIN(n, queued - done);
            dma_memory_write(&address_space_memory,
                             io->addr + ((hwaddr)done << s->shift),
                             &s->inbuf[(size_t)at << s->shift],
                             (size_t)n << s->shift, MEMTXATTRS_UNSPECIFIED);
        } else {
            /* Nothing captured: hand the guest silence, on time. */
            n = frames - done;
            dma_memory_set(&address_space_memory,
                           io->addr + ((hwaddr)done << s->shift), 0,
                           (size_t)n << s->shift, MEMTXATTRS_UNSPECIFIED);
        }
        done += n;
    }

    s->in_rpos += MIN(frames, queued);
    if (s->in_rpos >= (uint32_t)s->in_samples) {
        uint32_t base = s->in_rpos - s->in_rpos % s->in_samples;
        s->in_rpos -= base;
        s->in_wpos -= base;
    }

    io->addr += (hwaddr)frames << s->shift;
    io->len -= frames << s->shift;

    if (io->len == 0) {
        s->in_running = false;
        io->dma_end(io);
    }
    return frames;
}

/* The host has frames for us: take what fits and drop the oldest if the
 * guest is not reading (an input nobody listens to must not back up). */
static void screamer_in_callback(void *opaque, int avail)
{
    ScreamerState *s = opaque;
    int frames = avail >> s->shift;

    if (!s->inbuf) {
        return;
    }
    while (frames > 0) {
        int at = s->in_wpos % s->in_samples;
        int n = MIN(frames, s->in_samples - at);
        int got = AUD_read(s->in_voice, &s->inbuf[(size_t)at << s->shift],
                           n << s->shift) >> s->shift;
        if (got <= 0) {
            break;
        }
        s->in_wpos += got;
        frames -= got;
        if ((int)(s->in_wpos - s->in_rpos) > s->in_samples) {
            s->in_rpos = s->in_wpos - s->in_samples;
        }
    }
}

static void pmac_screamer_rx(DBDMA_io *io)
{
    ScreamerState *s = io->opaque;
    DBDMA_channel *ch = io->channel;

    SCREAMER_DPRINTF("DMA RX transfer: addr %" HWADDR_PRIx
                     " len: %x\n", io->addr, io->len);

    if (!s->input || !s->in_voice) {
        /*
         * No input on this machine.  Stop the channel after the status
         * update, as this has always done: Mac OS 9 freezes if an input
         * channel it started neither runs nor stops.
         */
        ch->regs[DBDMA_STATUS] &= ~RUN;
        io->dma_end(io);
        return;
    }

    memcpy(&s->in_io, io, sizeof(DBDMA_io));
    s->in_running = true;
    s->pace_idle = 0;
    if (!timer_pending(s->pace_timer)) {
        s->pace_last = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        s->pace_frac = 0;
        timer_mod(s->pace_timer, s->pace_last + SCREAMER_PACE_NS);
    }
}

static void pmac_screamer_rx_flush(DBDMA_io *io)
{
    DBDMA_channel *ch = io->channel;
    dbdma_cmd *current = &ch->current;
    uint16_t cmd;

    SCREAMER_DPRINTF("DMA RX flush!\n");
#if 1
    cmd = le16_to_cpu(current->command) & COMMAND_MASK;
    if (cmd == INPUT_MORE || cmd == INPUT_LAST) {
        current->xfer_status = cpu_to_le16(ch->regs[DBDMA_STATUS]);
        current->res_count = cpu_to_le16(io->len);

        dma_memory_write(&address_space_memory, ch->regs[DBDMA_CMDPTR_LO],
                         &ch->current, sizeof(dbdma_cmd),
                         MEMTXATTRS_UNSPECIFIED);
    }
#endif
}

void macio_screamer_register_dma(ScreamerState *s, void *dbdma, int txchannel, int rxchannel)
{
    s->dbdma = dbdma;
    DBDMA_register_channel(dbdma, txchannel, s->dma_tx_irq,
                           pmac_screamer_tx, pmac_screamer_tx_flush, s);
    DBDMA_register_channel(dbdma, rxchannel, s->dma_rx_irq,
                           pmac_screamer_rx, pmac_screamer_rx_flush, s);
}

static void screamerspk_callback(void *opaque, int free_b)
{
    ScreamerState *s = opaque;
    int samples, generated;

    if (free_b == 0) {
        return;
    }

    if (s->wpos - s->rpos == 0) {
        if (s->primed) {
            sdbg.underruns++;
        }
        s->primed = false;                  /* underrun: refill first */
        sdbg_tick(s);
        /*
         * Stop taking part in mixing while there is nothing to play.
         *
         * QEMU mixes by taking the *minimum* of what every active voice
         * has supplied, so a voice that stays active and supplies nothing
         * holds every other voice on the same backend at zero.  This one
         * was activated once and never deactivated, which left the other
         * sound device on this machine playing at about one per cent of
         * real time -- it only advanced when the screamer happened to
         * contribute.  The DMA path turns this back on when the guest
         * gives it something to play.
         */
        if (s->active) {
            s->active = false;
            AUD_set_active_out(s->voice, false);
        }
        return;
    }
    if (!s->primed) {
        if (s->wpos - s->rpos < s->rate * SCREAMER_PRIME_MS / 1000) {
            sdbg.prime_waits++;
            return;
        }
        s->primed = true;
    }

    samples = MIN(s->samples, free_b >> s->shift);
    generated = 0;
    while (generated < samples && s->rpos < s->wpos) {
        int at = s->rpos % s->samples;
        int n = MIN(MIN(samples - generated, s->wpos - s->rpos), s->samples - at);
        int w = AUD_write(s->voice, s->mixbuf + ((uintptr_t)at << s->shift),
                          n << s->shift) >> s->shift;
        s->rpos += w;
        generated += w;
        if (w < n) {
            break;
        }
    }

    SCREAMER_DPRINTF("  - generated %d, wpos %d, rpos %d\n", generated, s->wpos, s->rpos);
    sdbg.written += generated;
    sdbg_tick(s);

    if (s->rpos >= s->samples) {          /* keep the counters small */
        int base = s->rpos - s->rpos % s->samples;
        s->rpos -= base;
        s->wpos -= base;
    }

}

static void screamer_update_settings(ScreamerState *s)
{
    struct audsettings as = { s->rate, 2, AUDIO_FORMAT_S16,
        1 };

    s->voice = AUD_open_out(&s->card, s->voice, s_spk, s, screamerspk_callback, &as);
    if (!s->voice) {
        AUD_log(s_spk, "Could not open voice\n");
        return;
    }

    s->shift = 2;
    if (!s->mixbuf) {
        /* SCREAMER_RING=<frames> overrides the default (for tuning) */
        const char *e = getenv("SCREAMER_RING");
        s->samples = e ? MAX(atoi(e), 256) : SCREAMER_RING_FRAMES;
        s->mixbuf = g_malloc0(s->samples << s->shift);
    }

    s->active = true;
    AUD_set_active_out(s->voice, true);
}

static void screamer_update_volume(ScreamerState *s)
{
    uint8_t muted = s->codec_ctrl_regs[0x1] & 0x80 ? 1 : 0;
    uint8_t att_left = (s->codec_ctrl_regs[0x4] & 0xf);
    uint8_t att_right = (s->codec_ctrl_regs[0x4] & 0x3c0) >> 6;

    SCREAMER_DPRINTF("setting mute: %d, attenuation L: %d R: %d\n",
                     muted, att_left, att_right);

    AUD_set_volume_out(s->voice, muted, (0xf - att_left) << 4,
                       (0xf - att_right) << 4);
}

/*
 * Follow codec register 0 and the sound control register: start or stop
 * listening, and set the capture gain.
 */
static void screamer_update_input(ScreamerState *s)
{
    uint32_t mux = s->codec_ctrl_regs[0x0] & CODEC0_INPUT_MUX;
    uint8_t right = s->codec_ctrl_regs[0x0] & CODEC0_GAIN_RIGHT;
    uint8_t left = (s->codec_ctrl_regs[0x0] & CODEC0_GAIN_LEFT) >> 4;
    bool armed = mux && (s->regs[SND_CTRL_REG] & SND_CTRL_IN_SUBFRAME);

    s->in_armed = armed;
    if (!s->input) {
        return;
    }
    /*
     * Open the capture voice the first time the guest chooses an input, not
     * when the machine is built.
     *
     * Opening it asks this Mac for permission to listen, and that question
     * is not one to ask while a virtual Mac is still starting -- on a Mac
     * that has not answered it, the request does not come back, and the
     * emulator would hang before the guest had drawn anything.  A guest that
     * never records never reaches here.
     */
    if (!s->in_voice && armed) {
        struct audsettings as = { s->rate, 2, AUDIO_FORMAT_S16, 1 };

        s->in_voice = AUD_open_in(&s->card, s->in_voice, "screamer.in", s,
                                  screamer_in_callback, &as);
        if (!s->in_voice) {
            AUD_log(s_spk, "Could not open the sound input\n");
            return;
        }
        if (!s->inbuf) {
            s->in_samples = SCREAMER_RING_FRAMES;
            s->inbuf = g_malloc0((size_t)s->in_samples << s->shift);
        }
    }
    if (!s->in_voice) {
        return;
    }
    /* The codec's gain is 0..15 over about 22.5 dB; the mixer wants 0..255. */
    AUD_set_volume_in(s->in_voice, 0, left << 4, right << 4);
    AUD_set_active_in(s->in_voice, armed);
}

static void screamer_reset(DeviceState *dev)
{
    ScreamerState *s = SCREAMER(dev);
    
    memset(s->regs, 0, sizeof(s->regs));
    memset(s->codec_ctrl_regs, 0, sizeof(s->codec_ctrl_regs));
    memset(&s->io, 0, sizeof(DBDMA_io));

    s->rate = 44100;
    screamer_update_settings(s);

    s->bpos = 0;
    s->wpos = s->rpos = 0;
    s->ppos = 0;
    s->in_wpos = s->in_rpos = 0;
    s->in_running = false;

    return;
}

static void screamer_realizefn(DeviceState *dev, Error **errp)
{
    ScreamerState *s = SCREAMER(dev);

    if (!AUD_register_card(s_spk, &s->card, errp)) {
        return;
    }

    s->rate = 44100;
    s->pace_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, screamer_pace_cb, s);
    screamer_update_settings(s);
}

static void screamer_control_write(ScreamerState *s, uint32_t val)
{
    uint32_t old_rate = s->rate;

    SCREAMER_DPRINTF("%s: val %" PRId32 "\n", __func__, val);
        
    /* Basic rate selection */
    switch ((val & 0x700) >> 8) {
    case 0x00:
        s->rate = 44100;
        break;
    case 0x1:
        s->rate = 29400;
        break;
    case 0x2:
        s->rate = 22050;
        break;
    case 0x3:
        s->rate = 17640;
        break;
    case 0x4:
        s->rate = 14700;
        break;
    case 0x5:
        s->rate = 11025;
        break;
    case 0x6:
        s->rate = 8820;
        break;
    case 0x7:
        s->rate = 7350;
        break;
    }

    SCREAMER_DPRINTF("basic rate: %d\n", s->rate);
    /*
     * Reopening the host voice tears down and rebuilds a CoreAudio stream,
     * with whatever latency that takes, inside a guest MMIO store -- and it
     * drops the priming.  The guest writes this register for reasons that
     * have nothing to do with the rate, so do it only when the rate actually
     * moved.  Both other emulators of this chip switch lazily for the same
     * reason.
     */
    if (s->rate != old_rate) {
        screamer_update_settings(s);
    }

    s->regs[0] = val;
    screamer_update_input(s);
}

static void screamer_codec_write(ScreamerState *s, hwaddr addr, uint64_t val)
{
    /* Register 3 is not a register (Apple: kInvalidRegNumber = 3). */
    if (addr == 3 || addr >= ARRAY_SIZE(s->codec_ctrl_regs)) {
        return;
    }

    //SCREAMER_DPRINTF("%s: addr " HWADDR_PRIx " val %" PRIx64 "\n", __func__, addr, val);

    switch (addr) {
    case 0x0:
        /*
         * Input: which source, and how loud.
         *
         * The three multiplexer bits are a source selector, and which jack
         * each one stands for differs between this chip's revisions -- Apple,
         * Linux and NetBSD do not even agree on the names.  So any source at
         * all counts as "listening", and the host's one input is handed over
         * whichever was picked, rather than pretending to a wiring loom this
         * machine does not have.
         */
        s->codec_ctrl_regs[addr] = val;
        screamer_update_input(s);
        return;

    case 0x1:
        /* Clear recalibrate if set */
        val = val & ~CODEC_CTRL1_RECALIBRATE;    

        /* Update volume in case mute set */
        screamer_update_volume(s);
        break;

    case 0x4:
        /* Speaker attenuation */
        screamer_update_volume(s);
        break;
    }
    
    s->codec_ctrl_regs[addr] = val;
}

/*
 * SCREAMER_TRACE=1: every register access, in order, to stderr.  For
 * chasing a guest driver that crashes on what the device told it.
 */
static int strace_on = -1;
static void strace(const char *dir, uint32_t reg, uint32_t val)
{
    if (strace_on < 0) {
        strace_on = getenv("SCREAMER_TRACE") != NULL;
    }
    if (strace_on) {
        fprintf(stderr, "SCRTRACE %s reg=%u val=0x%08x\n", dir, reg, val);
    }
}

static uint64_t screamer_read(void *opaque, hwaddr addr, unsigned size)
{
    ScreamerState *s = opaque;
    uint32_t val;

    addr = addr >> 4;
    sdbg.reads[addr & 15]++;
    switch (addr) {
    case SND_CTRL_REG:
        val = s->regs[addr];
        break;
    case CODEC_CTRL_REG:
        val = s->regs[addr] & ~CODEC_CTRL_MASKECMD;
        break;
    case CODEC_STAT_REG:
        if (s->codec_ctrl_regs[7] & 1) {
            /*
             * Screamer read-back.  Codec register 7 bit 0 enables it and bits
             * 3:1 select the register -- three bits, so eight registers.
             * (Masking with 0xe reached 8, 10, 12 and 14 and read past the
             * end of the array.)
             *
             * The value comes back at bits 15:4 of the status register, not
             * at the bottom: Apple's driver reads it as (status >> 4) & 0xfff
             * (AppleScreamerAudio, Screamer_readCodecControlReg) and Mac OS 9
             * uses the same shift, kAWACsReadbackDataShift = 4.  Returning
             * the bare value handed the driver a sixteenth of what it wrote.
             *
             * kValidData stays set in read-back mode too.  Every Screamer
             * codec write is preceded by a wait on that bit -- including the
             * write that turns read-back back off -- so clearing it here
             * stalls the driver until its own timeout expires and it takes an
             * error path.  Mac OS X never noticed: it spins on the bit with
             * no timeout at all, where Mac OS 9's wait is bounded.
             */
            val = CODEC_STAT_MASK_VALID |
                  ((s->codec_ctrl_regs[(s->codec_ctrl_regs[7] >> 1) & 0x7] & 0xfff) << 4);
        } else {
            /*
             * Built from constants: the real register is read-only, and
             * letting the guest's writes survive into it handed the driver
             * back its own bits in the error and overflow fields.
             *
             * The sense nibble is left at zero, which Apple's awacs_hw.h
             * reads as a microphone being plugged in (bit 0 is inverted,
             * kNotMicSense).  Reporting nothing connected instead was tried
             * and measured worse -- 3 boots in 6 against 12 in 12 -- so it
             * stays as it is until there is evidence rather than inference
             * behind changing it.  The other emulators of this chip that do
             * boot Mac OS 9 also return zero here.
             */
            val = CODEC_STAT_MASK_VALID | CODEC_STAT_AWACS_REVISION |
                  CODEC_STAT_MANUFACTURER_NATIONAL;
            /*
             * Say a microphone is plugged in, but only when this machine
             * actually has an input to offer.  Mac OS X polls this once a
             * second and builds its list of input devices from it, so
             * claiming one that cannot record would put a dead input in the
             * Sound preference pane.
             */
            if (s->input) {
                val |= CODEC_STAT_LINE_IN_SENSE;
            }
        }
        break;
    case CLIP_CNT_REG:
    case BYTE_SWAP_REG:
    case FRAME_CNT_REG:
        val = s->regs[addr];
        break;
    default:
        qemu_log_mask(LOG_UNIMP,
                  "screamer: Unimplemented register read "
                  "reg 0x%" HWADDR_PRIx " size 0x%x\n",
                  addr, size);
        val = 0;
        break;
    }

    SCREAMER_DPRINTF("%s: addr " HWADDR_FMT_plx " -> %x\n", __func__, addr, val);

    strace("RD", (uint32_t)addr, val);
    return val;
}

static void screamer_write(void *opaque, hwaddr addr,
                           uint64_t val, unsigned size)
{
    ScreamerState *s = opaque;
    uint32_t codec_addr;

    addr = addr >> 4;

    SCREAMER_DPRINTF("%s: addr " HWADDR_FMT_plx " val %" PRIx64 "\n", __func__, addr, val);
    strace("WR", (uint32_t)addr, (uint32_t)val);

    switch (addr) {
    case SND_CTRL_REG:
        screamer_control_write(s, val & 0xffffffff);
        break;
    case CODEC_CTRL_REG:
        s->regs[addr] = val & 0xffffffff;
        codec_addr = (val & 0x7fff) >> 12;
        screamer_codec_write(s, codec_addr, val & 0xfff);
        break;
    case CODEC_STAT_REG:
        break;                  /* read-only on the real chip */
    case CLIP_CNT_REG:
    case BYTE_SWAP_REG:
        s->regs[addr] = val & 0xffffffff;
        break;
    case FRAME_CNT_REG:
        /*
         * The driver zeroes this at every engine start and then reads it as
         * the audio clock.  The write used to fall through to the
         * unimplemented path and be dropped, so the counter carried on from
         * wherever the last stream had left it.
         */
        s->regs[addr] = val & 0xffffffff;
        break;
    default:
        qemu_log_mask(LOG_UNIMP,
                  "screamer: Unimplemented register write "
                  "reg 0x%" HWADDR_PRIx " size 0x%x value 0x%" PRIx64 "\n",
                  addr, size, val);
        break;
    }

    return;
}

static const MemoryRegionOps screamer_ops = {
    .read = screamer_read,
    .write = screamer_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    }
};

static void screamer_initfn(Object *obj)
{
    SysBusDevice *d = SYS_BUS_DEVICE(obj);
    ScreamerState *s = SCREAMER(obj);

    memory_region_init_io(&s->mem, obj, &screamer_ops, s, "screamer", 0x1000);
    sysbus_init_mmio(d, &s->mem);
    sysbus_init_irq(d, &s->irq);
    sysbus_init_irq(d, &s->dma_tx_irq);
    sysbus_init_irq(d, &s->dma_rx_irq);
}

static const Property screamer_properties[] = {
    DEFINE_AUDIO_PROPERTIES(ScreamerState, card),
    /*
     * Off unless the machine is given one.  An input the guest never asked
     * for costs a capture stream on the host -- and on this Mac, opening one
     * is what makes the microphone indicator light up, which should not
     * happen because somebody started a virtual Mac.
     */
    DEFINE_PROP_BOOL("input", ScreamerState, input, false),
};

static void screamer_class_init(ObjectClass *oc, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = screamer_realizefn;
    device_class_set_legacy_reset(dc, screamer_reset);
    device_class_set_props(dc, screamer_properties);
}

static const TypeInfo screamer_type_info = {
    .name = TYPE_SCREAMER,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(ScreamerState),
    .instance_init = screamer_initfn,
    .class_init = screamer_class_init,
};

static void screamer_register_types(void)
{
    type_register_static(&screamer_type_info);
}

type_init(screamer_register_types)
