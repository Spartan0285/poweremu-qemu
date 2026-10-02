/*
 * poweremu-audio -- a sound device designed to be easy to drive.
 *
 * The emulated AWACS Screamer is faithful and, for Mac OS 9, unreliable:
 * Apple Audio Extension bombs on roughly one boot in six, which is a
 * five-year-old open problem in QEMU's screamer device.  Everything that
 * breaks is protocol -- a serial codec link with a busy handshake,
 * Screamer read-back, recalibration ramps with manufacturer-dependent
 * delays, and DBDMA channel programs whose stop protocol depends on a
 * conditional branch reaching STOP.
 *
 * This device has none of it.  The guest writes frames into a ring and
 * advances a pointer; the device drains the ring at the sample rate and
 * hands frames to the audio backend.  Nothing the guest does can leave the
 * device waiting on it: an empty ring plays silence and sets a sticky
 * underrun bit, and that is the whole error model.
 *
 * It is paired with a guest driver of ours -- the same arrangement as
 * ppc-ndrvloader for video, where the guest binds to a driver we supply
 * rather than to a period card's ROM.  See docs/PARAVIRTUAL-SOUND.md in
 * the PowerEmu tree.  The Screamer stays: boot-time sound comes from the
 * Mac OS ROM before any extension loads, and a guest without our driver
 * installed has to keep working.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "hw/sysbus.h"
#include "hw/qdev-properties.h"
#include "migration/vmstate.h"
#include "audio/audio.h"
#include "system/dma.h"
#include "qom/object.h"
#include "hw/audio/poweremu_audio.h"

/* Registers, little-endian, one page. */
#define PEAU_ID         0x00    /* r   'PEAU'                        */
#define PEAU_VERSION    0x04    /* r   1                             */
#define PEAU_CAPS       0x08    /* r   see PEAU_CAP_*                */
#define PEAU_RING_BASE  0x10    /* rw  guest physical address        */
#define PEAU_RING_SIZE  0x14    /* rw  bytes, a power of two         */
#define PEAU_RATE       0x18    /* rw  frames a second               */
#define PEAU_FORMAT     0x1c    /* rw  see PEAU_FMT_*                */
#define PEAU_CONTROL    0x20    /* rw  bit 0 run, bit 1 flush        */
#define PEAU_WRITE_PTR  0x24    /* rw  guest's producer, in frames   */
#define PEAU_READ_PTR   0x28    /* r   device's consumer, in frames  */
#define PEAU_STATUS     0x2c    /* r   see PEAU_ST_*                 */

#define PEAU_MAGIC      0x50454155u     /* 'PEAU' */
#define PEAU_VERSION_1  1

#define PEAU_CAP_S16_STEREO  (1u << 0)
#define PEAU_CAP_MAX_RING    (1u << 20)  /* 1 MB, and the cap is the value */

#define PEAU_FMT_S16_STEREO  0

#define PEAU_CTL_RUN    (1u << 0)
#define PEAU_CTL_FLUSH  (1u << 1)

#define PEAU_ST_RUNNING  (1u << 0)
#define PEAU_ST_UNDERRUN (1u << 1)      /* sticky; cleared by writing it */

#define PEAU_FRAME_BYTES 4              /* 16-bit signed stereo */

static uint32_t peau_ring_frames(PowerEmuAudioState *s)
{
    return s->ring_size / PEAU_FRAME_BYTES;
}

/* Frames the guest has produced and the device has not yet consumed. */
static uint32_t peau_queued(PowerEmuAudioState *s)
{
    uint32_t frames = peau_ring_frames(s);
    uint32_t q;

    if (!frames) {
        return 0;
    }
    q = s->write_ptr - s->read_ptr;     /* wraps, which is the point */
    return q > frames ? frames : q;     /* the guest ran away: clamp */
}

static void peau_open_voice(PowerEmuAudioState *s)
{
    struct audsettings as = {
        .freq = s->rate ? s->rate : 44100,
        .nchannels = 2,
        .fmt = AUDIO_FORMAT_S16,
        .endianness = 1,                /* the guest is big-endian */
    };

    s->voice = AUD_open_out(&s->card, s->voice, TYPE_POWEREMU_AUDIO,
                            s, NULL, &as);
    s->open_rate = as.freq;
}

/*
 * The backend asks for what it can take.  Everything here is driven by
 * that: there is no timer, no pacing of our own, and nothing to get out of
 * step with.  An empty ring plays silence rather than stalling.
 */
static void peau_callback(void *opaque, int avail)
{
    PowerEmuAudioState *s = opaque;
    uint8_t buf[4096];

    while (avail > 0) {
        uint32_t queued = peau_queued(s);
        int want = avail > (int)sizeof(buf) ? (int)sizeof(buf) : avail;
        int frames = want / PEAU_FRAME_BYTES;
        int wrote;

        if (!(s->control & PEAU_CTL_RUN) || !s->ring_base || !s->ring_size) {
            return;
        }
        if (!queued) {
            s->status |= PEAU_ST_UNDERRUN;
            return;                     /* silence, not a stall */
        }
        if ((uint32_t)frames > queued) {
            frames = queued;
        }

        /* One frame at a time over the wrap; the ring is a power of two. */
        for (int i = 0; i < frames; i++) {
            uint32_t idx = (s->read_ptr + i) % peau_ring_frames(s);
            dma_memory_read(&address_space_memory,
                            s->ring_base + idx * PEAU_FRAME_BYTES,
                            &buf[i * PEAU_FRAME_BYTES], PEAU_FRAME_BYTES,
                            MEMTXATTRS_UNSPECIFIED);
        }

        wrote = AUD_write(s->voice, buf, frames * PEAU_FRAME_BYTES);
        if (wrote <= 0) {
            return;
        }
        s->read_ptr += wrote / PEAU_FRAME_BYTES;
        avail -= wrote;
    }
}

/* PEAU_TRACE=1: every register access, for bringing the device up. */
static int peau_trace = -1;
static void peau_log(const char *dir, hwaddr addr, uint32_t val)
{
    if (peau_trace < 0) {
        peau_trace = getenv("PEAU_TRACE") != NULL;
    }
    if (peau_trace) {
        fprintf(stderr, "PEAU %s 0x%02x = 0x%08x\n", dir, (unsigned)addr, val);
    }
}

static uint64_t peau_read(void *opaque, hwaddr addr, unsigned size)
{
    PowerEmuAudioState *s = opaque;

    switch (addr) {
    case PEAU_ID:        return PEAU_MAGIC;
    case PEAU_VERSION:   return PEAU_VERSION_1;
    case PEAU_CAPS:      return PEAU_CAP_S16_STEREO | PEAU_CAP_MAX_RING;
    case PEAU_RING_BASE: return s->ring_base;
    case PEAU_RING_SIZE: return s->ring_size;
    case PEAU_RATE:      return s->rate;
    case PEAU_FORMAT:    return s->format;
    case PEAU_CONTROL:   return s->control;
    case PEAU_WRITE_PTR: return s->write_ptr;
    case PEAU_READ_PTR:  return s->read_ptr;
    case PEAU_STATUS:    return s->status;
    default:
        qemu_log_mask(LOG_UNIMP, "poweremu-audio: read 0x%" HWADDR_PRIx "\n", addr);
        return 0;
    }
}

static uint64_t peau_read_traced(void *opaque, hwaddr addr, unsigned size)
{
    uint64_t v = peau_read(opaque, addr, size);
    peau_log("RD", addr, (uint32_t)v);
    return v;
}

static void peau_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    PowerEmuAudioState *s = opaque;
    uint32_t v = val;

    peau_log("WR", addr, v);

    switch (addr) {
    case PEAU_RING_BASE:
        s->ring_base = v;
        break;
    case PEAU_RING_SIZE:
        /* A power of two, capped: the consumer index wraps by masking. */
        if (v > PEAU_CAP_MAX_RING) {
            v = PEAU_CAP_MAX_RING;
        }
        v &= ~(PEAU_FRAME_BYTES - 1);
        s->ring_size = (v && !(v & (v - 1))) ? v : 0;
        break;
    case PEAU_RATE:
        if (v >= 8000 && v <= 48000) {
            s->rate = v;
        }
        break;
    case PEAU_FORMAT:
        s->format = PEAU_FMT_S16_STEREO;        /* the only one, for now */
        break;
    case PEAU_CONTROL:
        if (v & PEAU_CTL_FLUSH) {
            s->read_ptr = s->write_ptr;
            v &= ~PEAU_CTL_FLUSH;
        }
        s->control = v & PEAU_CTL_RUN;
        if (s->control & PEAU_CTL_RUN) {
            /* Only disturb the host voice when the rate really moved. */
            if (!s->voice || s->open_rate != s->rate) {
                peau_open_voice(s);
            }
            s->status |= PEAU_ST_RUNNING;
        } else {
            s->status &= ~PEAU_ST_RUNNING;
        }
        if (s->voice) {
            AUD_set_active_out(s->voice, !!(s->control & PEAU_CTL_RUN));
        }
        break;
    case PEAU_WRITE_PTR:
        s->write_ptr = v;
        break;
    case PEAU_STATUS:
        s->status &= ~(v & PEAU_ST_UNDERRUN);   /* write 1 to clear */
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "poweremu-audio: write 0x%" HWADDR_PRIx
                      " = 0x%x\n", addr, v);
        break;
    }
}

static const MemoryRegionOps peau_ops = {
    .read = peau_read_traced,
    .write = peau_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void peau_reset(DeviceState *dev)
{
    PowerEmuAudioState *s = POWEREMU_AUDIO(dev);

    s->ring_base = s->ring_size = 0;
    s->rate = 44100;
    s->format = PEAU_FMT_S16_STEREO;
    s->control = s->status = 0;
    s->write_ptr = s->read_ptr = 0;
    if (s->voice) {
        AUD_set_active_out(s->voice, 0);
    }
}

static void peau_realize(DeviceState *dev, Error **errp)
{
    PowerEmuAudioState *s = POWEREMU_AUDIO(dev);

    if (!AUD_register_card(TYPE_POWEREMU_AUDIO, &s->card, errp)) {
        return;
    }
    s->rate = 44100;
    peau_open_voice(s);
}

static void peau_unrealize(DeviceState *dev)
{
    PowerEmuAudioState *s = POWEREMU_AUDIO(dev);

    if (s->voice) {
        AUD_close_out(&s->card, s->voice);
        s->voice = NULL;
    }
    AUD_remove_card(&s->card);
}

static void peau_init(Object *obj)
{
    PowerEmuAudioState *s = POWEREMU_AUDIO(obj);

    memory_region_init_io(&s->mem, obj, &peau_ops, s, TYPE_POWEREMU_AUDIO, 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mem);
}

static const VMStateDescription vmstate_peau = {
    .name = TYPE_POWEREMU_AUDIO,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(ring_base, PowerEmuAudioState),
        VMSTATE_UINT32(ring_size, PowerEmuAudioState),
        VMSTATE_UINT32(rate, PowerEmuAudioState),
        VMSTATE_UINT32(format, PowerEmuAudioState),
        VMSTATE_UINT32(control, PowerEmuAudioState),
        VMSTATE_UINT32(status, PowerEmuAudioState),
        VMSTATE_UINT32(write_ptr, PowerEmuAudioState),
        VMSTATE_UINT32(read_ptr, PowerEmuAudioState),
        VMSTATE_END_OF_LIST()
    }
};

static const Property peau_properties[] = {
    DEFINE_AUDIO_PROPERTIES(PowerEmuAudioState, card),
};

static void peau_class_init(ObjectClass *oc, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = peau_realize;
    dc->unrealize = peau_unrealize;
    device_class_set_legacy_reset(dc, peau_reset);
    dc->vmsd = &vmstate_peau;
    device_class_set_props(dc, peau_properties);
}

static const TypeInfo peau_type_info = {
    .name = TYPE_POWEREMU_AUDIO,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(PowerEmuAudioState),
    .instance_init = peau_init,
    .class_init = peau_class_init,
};

static void peau_register_types(void)
{
    type_register_static(&peau_type_info);
}

type_init(peau_register_types)
