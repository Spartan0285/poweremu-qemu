/*
 * QEMU OS X CoreAudio audio driver
 *
 * Copyright (c) 2005 Mike Kronenberg
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
#include <CoreAudio/CoreAudio.h>
#include <pthread.h>            /* pthread_X */

#include "qemu/main-loop.h"
#include "qemu/atomic.h"
#include "qemu/module.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "audio.h"

#define AUDIO_CAP "coreaudio"
#include "audio_int.h"

typedef struct CoreaudioCapturePacket {
    int64_t wall_ns;
    uint64_t pcm_offset;
    uint64_t bytes;
    uint64_t copied_bytes;
} CoreaudioCapturePacket;

typedef struct coreaudioVoiceOut {
    HWVoiceOut hw;
    pthread_mutex_t buf_mutex;
    AudioDeviceID outputDeviceID;
    int frameSizeSetting;
    uint32_t bufferCount;
    UInt32 audioDevicePropertyBufferFrameSize;
    AudioDeviceIOProcID ioprocid;
    bool enabled;
    bool ioproc_started;
    /* BQL serializes the producer; the device IOProc is the sole consumer.
     * Release publishes copied PCM / consumed space, acquire observes it.
     * Reset only with the device stopped and its IOProc destroyed. */
    bool spsc_output;
    /* Optional diagnostic PCM handoff, callback -> main thread. */
    uint8_t *capture_pcm;
    size_t capture_size;
    uint64_t capture_written, capture_read, capture_dropped;
    FILE *capture_file;
    unsigned capture_generation;
    int64_t capture_last_ms;
    bool capture_deferred;
    CoreaudioCapturePacket *capture_packets;
    size_t capture_packet_count, capture_packet_capacity;
    uint64_t capture_packet_dropped;
    FILE *capture_packet_file;
    uint64_t output_written, output_read;
    bool debug_audio;
    int64_t debug_last_ms;
    uint64_t debug_callbacks, debug_empty_callbacks, debug_missing_frames;
    uint64_t debug_invocations, debug_unusable_callbacks;
    /* Producer-side PCM diagnostics; BQL owns these counters. */
    uint64_t debug_producer_frames, debug_producer_zero_frames;
    uint64_t debug_producer_zero_run, debug_producer_zero_max;
} coreaudioVoiceOut;

/*
 * Capture.
 *
 * The playback half below drives the default output device directly with an
 * IOProc.  Input is the mirror image of it: an IOProc on the default input
 * device copies what the microphone hears into the same ring buffer the
 * generic layer would have filled, and `read` drains it.
 *
 * This Mac asks its owner for permission the first time the input IOProc
 * runs, so a process that never opens a capture voice never raises the
 * microphone indicator.
 */
typedef struct coreaudioVoiceIn {
    HWVoiceIn hw;
    pthread_mutex_t buf_mutex;
    AudioDeviceID inputDeviceID;
    int frameSizeSetting;
    uint32_t bufferCount;
    UInt32 audioDevicePropertyBufferFrameSize;
    AudioDeviceIOProcID ioprocid;
    bool enabled;
} coreaudioVoiceIn;

static const AudioObjectPropertyAddress voice_addr = {
    kAudioHardwarePropertyDefaultOutputDevice,
    kAudioObjectPropertyScopeGlobal,
    kAudioObjectPropertyElementMain
};

static const AudioObjectPropertyAddress in_voice_addr = {
    kAudioHardwarePropertyDefaultInputDevice,
    kAudioObjectPropertyScopeGlobal,
    kAudioObjectPropertyElementMain
};

static OSStatus coreaudio_get_voice(AudioDeviceID *id)
{
    UInt32 size = sizeof(*id);

    return AudioObjectGetPropertyData(kAudioObjectSystemObject,
                                      &voice_addr,
                                      0,
                                      NULL,
                                      &size,
                                      id);
}

static OSStatus coreaudio_get_framesizerange(AudioDeviceID id,
                                             AudioValueRange *framerange)
{
    UInt32 size = sizeof(*framerange);
    AudioObjectPropertyAddress addr = {
        kAudioDevicePropertyBufferFrameSizeRange,
        kAudioDevicePropertyScopeOutput,
        kAudioObjectPropertyElementMain
    };

    return AudioObjectGetPropertyData(id,
                                      &addr,
                                      0,
                                      NULL,
                                      &size,
                                      framerange);
}

static OSStatus coreaudio_get_framesize(AudioDeviceID id, UInt32 *framesize)
{
    UInt32 size = sizeof(*framesize);
    AudioObjectPropertyAddress addr = {
        kAudioDevicePropertyBufferFrameSize,
        kAudioDevicePropertyScopeOutput,
        kAudioObjectPropertyElementMain
    };

    return AudioObjectGetPropertyData(id,
                                      &addr,
                                      0,
                                      NULL,
                                      &size,
                                      framesize);
}

static OSStatus coreaudio_set_framesize(AudioDeviceID id, UInt32 *framesize)
{
    UInt32 size = sizeof(*framesize);
    AudioObjectPropertyAddress addr = {
        kAudioDevicePropertyBufferFrameSize,
        kAudioDevicePropertyScopeOutput,
        kAudioObjectPropertyElementMain
    };

    return AudioObjectSetPropertyData(id,
                                      &addr,
                                      0,
                                      NULL,
                                      size,
                                      framesize);
}

static OSStatus coreaudio_set_streamformat(AudioDeviceID id,
                                           AudioStreamBasicDescription *d)
{
    UInt32 size = sizeof(*d);
    AudioObjectPropertyAddress addr = {
        kAudioDevicePropertyStreamFormat,
        kAudioDevicePropertyScopeOutput,
        kAudioObjectPropertyElementMain
    };

    return AudioObjectSetPropertyData(id,
                                      &addr,
                                      0,
                                      NULL,
                                      size,
                                      d);
}

static void coreaudio_logstatus (OSStatus status)
{
    const char *str = "BUG";

    switch (status) {
    case kAudioHardwareNoError:
        str = "kAudioHardwareNoError";
        break;

    case kAudioHardwareNotRunningError:
        str = "kAudioHardwareNotRunningError";
        break;

    case kAudioHardwareUnspecifiedError:
        str = "kAudioHardwareUnspecifiedError";
        break;

    case kAudioHardwareUnknownPropertyError:
        str = "kAudioHardwareUnknownPropertyError";
        break;

    case kAudioHardwareBadPropertySizeError:
        str = "kAudioHardwareBadPropertySizeError";
        break;

    case kAudioHardwareIllegalOperationError:
        str = "kAudioHardwareIllegalOperationError";
        break;

    case kAudioHardwareBadDeviceError:
        str = "kAudioHardwareBadDeviceError";
        break;

    case kAudioHardwareBadStreamError:
        str = "kAudioHardwareBadStreamError";
        break;

    case kAudioHardwareUnsupportedOperationError:
        str = "kAudioHardwareUnsupportedOperationError";
        break;

    case kAudioDeviceUnsupportedFormatError:
        str = "kAudioDeviceUnsupportedFormatError";
        break;

    case kAudioDevicePermissionsError:
        str = "kAudioDevicePermissionsError";
        break;

    default:
        AUD_log (AUDIO_CAP, "Reason: status code %" PRId32 "\n", (int32_t)status);
        return;
    }

    AUD_log (AUDIO_CAP, "Reason: %s\n", str);
}

static void G_GNUC_PRINTF (2, 3) coreaudio_logerr (
    OSStatus status,
    const char *fmt,
    ...
    )
{
    va_list ap;

    va_start (ap, fmt);
    AUD_log (AUDIO_CAP, fmt, ap);
    va_end (ap);

    coreaudio_logstatus (status);
}

static void G_GNUC_PRINTF (3, 4) coreaudio_logerr2 (
    OSStatus status,
    const char *typ,
    const char *fmt,
    ...
    )
{
    va_list ap;

    AUD_log (AUDIO_CAP, "Could not initialize %s\n", typ);

    va_start (ap, fmt);
    AUD_vlog (AUDIO_CAP, fmt, ap);
    va_end (ap);

    coreaudio_logstatus (status);
}

#define coreaudio_playback_logerr(status, ...) \
    coreaudio_logerr2(status, "playback", __VA_ARGS__)

static int coreaudio_buf_lock (coreaudioVoiceOut *core, const char *fn_name)
{
    int err;

    err = pthread_mutex_lock (&core->buf_mutex);
    if (err) {
        dolog ("Could not lock voice for %s\nReason: %s\n",
               fn_name, strerror (err));
        return -1;
    }
    return 0;
}

static int coreaudio_buf_unlock (coreaudioVoiceOut *core, const char *fn_name)
{
    int err;

    err = pthread_mutex_unlock (&core->buf_mutex);
    if (err) {
        dolog ("Could not unlock voice for %s\nReason: %s\n",
               fn_name, strerror (err));
        return -1;
    }
    return 0;
}

/* Capture the actual output packets without doing file IO in the IOProc.
 * This is driver-input PCM, not a recording of the physical speakers. */
static void coreaudio_capture_push(coreaudioVoiceOut *core,
                                   const AudioBufferList *buffers,
                                   size_t copied_bytes)
{
    if (!core->capture_pcm || !buffers->mNumberBuffers ||
        !buffers->mBuffers[0].mData) {
        return;
    }
    size_t bytes = buffers->mBuffers[0].mDataByteSize;
    uint64_t written = core->capture_written;
    if (core->capture_packets) {
        if (core->capture_packet_count < core->capture_packet_capacity) {
            CoreaudioCapturePacket *packet =
                &core->capture_packets[core->capture_packet_count++];
            packet->wall_ns = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
            packet->pcm_offset = written;
            packet->bytes = bytes;
            packet->copied_bytes = copied_bytes;
        } else {
            core->capture_packet_dropped++;
        }
    }
    uint64_t read = qatomic_load_acquire(&core->capture_read);
    size_t free = core->capture_size - (written - read);
    size_t n = MIN(bytes, free);
    size_t at = written % core->capture_size;
    size_t first = MIN(n, core->capture_size - at);
    const uint8_t *pcm = buffers->mBuffers[0].mData;
    memcpy(core->capture_pcm + at, pcm, first);
    memcpy(core->capture_pcm, pcm + first, n - first);
    qatomic_store_release(&core->capture_written, written + n);
    if (bytes > n) {
        qatomic_fetch_add(&core->capture_dropped, bytes - n);
    }
}

static void coreaudio_capture_drain(coreaudioVoiceOut *core)
{
    if (!core->capture_file) {
        return;
    }
    uint64_t read = core->capture_read;
    uint64_t written = qatomic_load_acquire(&core->capture_written);
    size_t bytes = written - read;
    size_t at = read % core->capture_size;
    size_t first = MIN(bytes, core->capture_size - at);
    if ((first && fwrite(core->capture_pcm + at, 1, first, core->capture_file) != first) ||
        (bytes > first && fwrite(core->capture_pcm, 1, bytes - first,
                                 core->capture_file) != bytes - first)) {
        dolog("Diagnostic PCM capture write failed\n");
    }
    qatomic_store_release(&core->capture_read, written);
}

static void coreaudio_capture_end(coreaudioVoiceOut *core)
{
    /* Called only after AudioDeviceStop / DestroyIOProcID. */
    if (core->capture_file) {
        coreaudio_capture_drain(core);
        fclose(core->capture_file);
        dolog("Diagnostic PCM capture dropped %" PRIu64 " bytes\n",
              core->capture_dropped);
        core->capture_file = NULL;
    }
    if (core->capture_packet_file) {
        if (fwrite(core->capture_packets, sizeof(*core->capture_packets),
                   core->capture_packet_count, core->capture_packet_file) !=
            core->capture_packet_count) {
            dolog("Diagnostic callback timing write failed\n");
        }
        fclose(core->capture_packet_file);
        core->capture_packet_file = NULL;
        dolog("Diagnostic callback timing dropped %" PRIu64 " packets\n",
              core->capture_packet_dropped);
    }
    g_free(core->capture_packets);
    core->capture_packets = NULL;
    core->capture_packet_count = core->capture_packet_capacity = 0;
    g_free(core->capture_pcm);
    core->capture_pcm = NULL;
    core->capture_deferred = false;
}

/* QA can retain a bounded recording until the IOProc stops. This avoids
 * synchronous recording writes on the emulation thread during playback.
 * Invalid settings retain the existing four-second streaming capture. */
static unsigned coreaudio_capture_seconds(void)
{
    const char *value = getenv("POWEREMU_COREAUDIO_CAPTURE_SECONDS");
    char *end;
    unsigned long seconds;

    if (!value || !*value || *value < '0' || *value > '9') {
        return 0;
    }
    errno = 0;
    seconds = strtoul(value, &end, 10);
    return !errno && !*end && seconds >= 1 && seconds <= 600 ? seconds : 0;
}

static void coreaudio_capture_poll(coreaudioVoiceOut *core, int64_t now)
{
    if (core->capture_file && !core->capture_deferred &&
        now - core->capture_last_ms >= 100) {
        coreaudio_capture_drain(core);
        core->capture_last_ms = now;
    }
}

static void coreaudio_capture_begin(coreaudioVoiceOut *core)
{
    const char *prefix = getenv("POWEREMU_COREAUDIO_CAPTURE");
    if (!prefix || !*prefix) {
        return;
    }
    char *path = g_strdup_printf("%s.%d.%u-%d.f32", prefix, getpid(),
                                 ++core->capture_generation, core->hw.info.freq);
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) {
        dolog("Cannot create diagnostic PCM capture: %s\n", strerror(errno));
        g_free(path);
        return;
    }
    core->capture_file = fdopen(fd, "wb");
    if (!core->capture_file) {
        close(fd);
        g_free(path);
        return;
    }
    unsigned seconds = coreaudio_capture_seconds();
    core->capture_deferred = seconds != 0;
    core->capture_size = (size_t)core->hw.info.freq *
                        core->hw.info.bytes_per_frame * (seconds ? seconds : 4);
    core->capture_pcm = g_malloc(core->capture_size);
    /* Commit diagnostic storage before IO starts, so the callback does not
     * fault in a large untouched allocation as it records the first sound. */
    for (size_t at = 0; at < core->capture_size; at += 4096) {
        ((volatile uint8_t *)core->capture_pcm)[at] = 0;
    }
    core->capture_written = core->capture_read = core->capture_dropped = 0;
    dolog("Diagnostic PCM %s: float32 native endian, %d Hz, %d channels\n",
          path, core->hw.info.freq, core->hw.info.nchannels);
    if (core->capture_deferred) {
        /* Fixed-size native-endian records: monotonic ns, retained PCM byte
         * offset, requested bytes and copied bytes. Allocate before starting IO;
         * write only after stopping it. Full metadata is explicitly counted. */
        char *packet_path = g_strdup_printf("%s.callbacks", path);
        int packet_fd = open(packet_path, O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (packet_fd >= 0) {
            core->capture_packet_file = fdopen(packet_fd, "wb");
            if (!core->capture_packet_file) {
                close(packet_fd);
            } else {
                core->capture_packet_capacity =
                    (size_t)core->hw.info.freq * seconds / 32 + 1024;
                core->capture_packets = g_new(CoreaudioCapturePacket,
                                              core->capture_packet_capacity);
                size_t packet_bytes = core->capture_packet_capacity *
                                      sizeof(*core->capture_packets);
                for (size_t at = 0; at < packet_bytes; at += 4096) {
                    ((volatile uint8_t *)core->capture_packets)[at] = 0;
                }
                core->capture_packet_count = core->capture_packet_dropped = 0;
            }
        }
        g_free(packet_path);
        dolog("Diagnostic PCM retains %u seconds in memory until output stops\n",
              seconds);
    }
    g_free(path);
}

/* Snapshot counters on the main audio thread; SPSC counters are atomic,
 * legacy counters use the buffer lock. Never log or take BQL in the IOProc. */
static void coreaudio_debug_tick(coreaudioVoiceOut *core)
{
    uint64_t callbacks, empty, missing;
    int64_t now;

    now = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
    coreaudio_capture_poll(core, now);
    if (!core->debug_audio) {
        return;
    }
    if (now - core->debug_last_ms < 1000) {
        return;
    }
    qemu_log("coreaudio-producer: wall-ns %" PRId64
             " frames %" PRIu64 " exact-zero-frames %" PRIu64
             " max-zero-run %" PRIu64 "\n",
             qemu_clock_get_ns(QEMU_CLOCK_REALTIME),
             core->debug_producer_frames, core->debug_producer_zero_frames,
             core->debug_producer_zero_max);
    core->debug_producer_frames = core->debug_producer_zero_frames = 0;
    core->debug_producer_zero_max = 0;
    qemu_log("coreaudio-io: invoked %" PRIu64 " unusable %" PRIu64 "\n",
             qatomic_xchg(&core->debug_invocations, 0),
             qatomic_xchg(&core->debug_unusable_callbacks, 0));
    if (core->spsc_output) {
        callbacks = qatomic_xchg(&core->debug_callbacks, 0);
        empty = qatomic_xchg(&core->debug_empty_callbacks, 0);
        missing = qatomic_xchg(&core->debug_missing_frames, 0);
        core->debug_last_ms = now;
        size_t queued = (core->output_written -
                         qatomic_load_acquire(&core->output_read)) /
                        core->hw.info.bytes_per_frame;
        qemu_log("coreaudio: callbacks %" PRIu64 " empty %" PRIu64
                 " missing-frames %" PRIu64 " spsc 1 queued-frames %zu"
                 " capacity-frames %zu\n", callbacks, empty, missing, queued,
                 core->hw.size_emul / core->hw.info.bytes_per_frame);
        return;
    }
    if (coreaudio_buf_lock(core, "debug")) {
        return;
    }
    callbacks = core->debug_callbacks;
    empty = core->debug_empty_callbacks;
    missing = core->debug_missing_frames;
    core->debug_callbacks = 0;
    core->debug_empty_callbacks = 0;
    core->debug_missing_frames = 0;
    core->debug_last_ms = now;
    coreaudio_buf_unlock(core, "debug");
    qemu_log("coreaudio: callbacks %" PRIu64 " empty %" PRIu64
             " missing-frames %" PRIu64 "\n", callbacks, empty, missing);
}

#define COREAUDIO_WRAPPER_FUNC(name, ret_type, args_decl, args) \
    static ret_type glue(coreaudio_legacy_, name)args_decl             \
    {                                                           \
        coreaudioVoiceOut *core = (coreaudioVoiceOut *) hw;     \
        ret_type ret;                                           \
                                                                \
        if (coreaudio_buf_lock(core, "coreaudio_" #name)) {         \
            return 0;                                           \
        }                                                       \
                                                                \
        ret = glue(audio_generic_, name)args;                   \
                                                                \
        coreaudio_buf_unlock(core, "coreaudio_" #name);             \
        coreaudio_debug_tick(core);                              \
        return ret;                                             \
    }
COREAUDIO_WRAPPER_FUNC(buffer_get_free, size_t, (HWVoiceOut *hw), (hw))
COREAUDIO_WRAPPER_FUNC(get_buffer_out, void *, (HWVoiceOut *hw, size_t *size),
                       (hw, size))
COREAUDIO_WRAPPER_FUNC(write, size_t, (HWVoiceOut *hw, void *buf, size_t size),
                       (hw, buf, size))
#undef COREAUDIO_WRAPPER_FUNC

/* Observe the already clipped PCM before publishing it to CoreAudio. This
 * distinguishes source/mixer silence from a shortage in the device callback.
 * Optional diagnostics only; no scanning on ordinary playback builds. */
static void coreaudio_debug_produced(coreaudioVoiceOut *core,
                                     const void *buf, size_t size)
{
    if (!core->debug_audio) {
        return;
    }
    size_t bpf = core->hw.info.bytes_per_frame;
    const uint8_t *pcm = buf;
    for (size_t at = 0; at + bpf <= size; at += bpf) {
        uint8_t nonzero = 0;
        for (size_t ch = 0; ch < bpf; ch++) {
            nonzero |= pcm[at + ch];
        }
        core->debug_producer_frames++;
        if (!nonzero) {
            core->debug_producer_zero_frames++;
            core->debug_producer_zero_run++;
            core->debug_producer_zero_max = MAX(core->debug_producer_zero_max,
                                               core->debug_producer_zero_run);
        } else {
            core->debug_producer_zero_run = 0;
        }
    }
}

/* No allocation, lock, logging, guest access or BQL in this handoff.
 * hw->pos_emul/pending_emul belong to the generic legacy ring only. */
static size_t coreaudio_legacy_put_buffer_out(HWVoiceOut *hw,
                                               void *buf, size_t size)
{
    coreaudioVoiceOut *core = (coreaudioVoiceOut *)hw;
    if (coreaudio_buf_lock(core, "coreaudio_put_buffer_out")) {
        return 0;
    }
    coreaudio_debug_produced(core, buf, size);
    size_t result = audio_generic_put_buffer_out(hw, buf, size);
    coreaudio_buf_unlock(core, "coreaudio_put_buffer_out");
    coreaudio_debug_tick(core);
    return result;
}

static size_t coreaudio_buffer_get_free(HWVoiceOut *hw)
{
    coreaudioVoiceOut *core = (coreaudioVoiceOut *)hw;
    if (!core->spsc_output) {
        return coreaudio_legacy_buffer_get_free(hw);
    }
    /* Report even when a full queue prevents further put-buffer calls. */
    if (core->debug_audio || core->capture_file) {
        coreaudio_debug_tick(core);
    }
    uint64_t read = qatomic_load_acquire(&core->output_read);
    return hw->size_emul - (core->output_written - read);
}

static void *coreaudio_get_buffer_out(HWVoiceOut *hw, size_t *size)
{
    coreaudioVoiceOut *core = (coreaudioVoiceOut *)hw;
    if (!core->spsc_output) {
        return coreaudio_legacy_get_buffer_out(hw, size);
    }
    if (!hw->buf_emul || !hw->size_emul) {
        *size = 0;
        return NULL;
    }
    size_t at = core->output_written % hw->size_emul;
    size_t free = coreaudio_buffer_get_free(hw);
    *size = MIN(free, hw->size_emul - at);
    return hw->buf_emul + at;
}

static size_t coreaudio_put_buffer_out(HWVoiceOut *hw, void *buf, size_t size)
{
    coreaudioVoiceOut *core = (coreaudioVoiceOut *)hw;
    if (!core->spsc_output) {
        return coreaudio_legacy_put_buffer_out(hw, buf, size);
    }
    size_t at = core->output_written % hw->size_emul;
    assert(buf == hw->buf_emul + at && size <= hw->size_emul - at &&
           size <= coreaudio_buffer_get_free(hw));
    coreaudio_debug_produced(core, buf, size);
    qatomic_store_release(&core->output_written, core->output_written + size);
    coreaudio_debug_tick(core);
    return size;
}

static size_t coreaudio_write(HWVoiceOut *hw, void *buf, size_t size)
{
    coreaudioVoiceOut *core = (coreaudioVoiceOut *)hw;
    return core->spsc_output ? audio_generic_write(hw, buf, size) :
                             coreaudio_legacy_write(hw, buf, size);
}

static OSStatus coreaudio_spsc_output(AudioDeviceID device,
                                     AudioBufferList *buffers,
                                     coreaudioVoiceOut *core)
{
    HWVoiceOut *hw = &core->hw;
    /* Silence all supplied storage, including an obsolete-device callback.
     * Never assume the device still requests its initialization period. */
    for (UInt32 i = 0; i < buffers->mNumberBuffers; i++) {
        if (buffers->mBuffers[i].mData) {
            memset(buffers->mBuffers[i].mData, 0,
                   buffers->mBuffers[i].mDataByteSize);
        }
    }
    if (device != core->outputDeviceID || !buffers->mNumberBuffers ||
        !buffers->mBuffers[0].mData || !hw->buf_emul || !hw->size_emul) {
        if (core->debug_audio) {
            qatomic_fetch_add(&core->debug_unusable_callbacks, 1);
        }
        return 0;
    }
    size_t bpf = hw->info.bytes_per_frame;
    size_t requested = buffers->mBuffers[0].mDataByteSize / bpf * bpf;
    uint64_t read = core->output_read;
    uint64_t written = qatomic_load_acquire(&core->output_written);
    size_t available = written - read;
    size_t bytes = MIN(requested, available);
    size_t at = read % hw->size_emul;
    size_t first = MIN(bytes, hw->size_emul - at);
    uint8_t *out = buffers->mBuffers[0].mData;
    memcpy(out, hw->buf_emul + at, first);
    memcpy(out + first, hw->buf_emul, bytes - first);
    qatomic_store_release(&core->output_read, read + bytes);
    coreaudio_capture_push(core, buffers, bytes);
    if (core->debug_audio) {
        qatomic_fetch_add(&core->debug_callbacks, 1);
        if (bytes < requested) {
            qatomic_fetch_add(&core->debug_empty_callbacks, 1);
            qatomic_fetch_add(&core->debug_missing_frames,
                              (requested - bytes) / bpf);
        }
    }
    return 0;
}

/*
 * callback to feed audiooutput buffer. called without BQL.
 * allowed to lock "buf_mutex", but disallowed to have any other locks.
 */
static OSStatus audioDeviceIOProc(
    AudioDeviceID inDevice,
    const AudioTimeStamp *inNow,
    const AudioBufferList *inInputData,
    const AudioTimeStamp *inInputTime,
    AudioBufferList *outOutputData,
    const AudioTimeStamp *inOutputTime,
    void *hwptr)
{
    coreaudioVoiceOut *spsc = hwptr;
    if (spsc->debug_audio) {
        qatomic_fetch_add(&spsc->debug_invocations, 1);
    }
    if (!outOutputData) {
        if (spsc->debug_audio) {
            qatomic_fetch_add(&spsc->debug_unusable_callbacks, 1);
        }
        return 0;
    }
    if (spsc->spsc_output) {
        return coreaudio_spsc_output(inDevice, outOutputData, spsc);
    }
    UInt32 frameCount, pending_frames;
    HWVoiceOut *hw = hwptr;
    coreaudioVoiceOut *core = (coreaudioVoiceOut *) hwptr;
    size_t len;

    for (UInt32 i = 0; i < outOutputData->mNumberBuffers; i++) {
        if (outOutputData->mBuffers[i].mData) {
            memset(outOutputData->mBuffers[i].mData, 0,
                   outOutputData->mBuffers[i].mDataByteSize);
        }
    }
    if (!outOutputData->mNumberBuffers || !outOutputData->mBuffers[0].mData) {
        if (core->debug_audio) {
            qatomic_fetch_add(&core->debug_unusable_callbacks, 1);
        }
        return 0;
    }
    void *out = outOutputData->mBuffers[0].mData;

    if (coreaudio_buf_lock (core, "audioDeviceIOProc")) {
        inInputTime = 0;
        return 0;
    }

    if (inDevice != core->outputDeviceID) {
        coreaudio_buf_unlock (core, "audioDeviceIOProc(old device)");
        return 0;
    }

    /* The device can request a different period after initialization. Never
     * write past its supplied storage or leave a larger period unfilled. */
    frameCount = outOutputData->mBuffers[0].mDataByteSize /
                 hw->info.bytes_per_frame;
    pending_frames = hw->pending_emul / hw->info.bytes_per_frame;
    if (core->debug_audio) {
        core->debug_callbacks++;
        if (pending_frames < frameCount) {
            core->debug_empty_callbacks++;
            core->debug_missing_frames += frameCount - pending_frames;
        }
    }

    /* A partial period still contains valid audio. Consume it in order
     * rather than withholding the entire period for a small shortage. */
    len = MIN(frameCount, pending_frames) * hw->info.bytes_per_frame;
    while (len) {
        size_t write_len, start;

        start = audio_ring_posb(hw->pos_emul, hw->pending_emul, hw->size_emul);
        assert(start < hw->size_emul);

        write_len = MIN(MIN(hw->pending_emul, len),
                        hw->size_emul - start);

        memcpy(out, hw->buf_emul + start, write_len);
        hw->pending_emul -= write_len;
        len -= write_len;
        out += write_len;
    }

    if (pending_frames < frameCount) {
        memset(out, 0, (frameCount - pending_frames) * hw->info.bytes_per_frame);
    }

    coreaudio_capture_push(core, outOutputData,
                           MIN(frameCount, pending_frames) *
                           hw->info.bytes_per_frame);
    coreaudio_buf_unlock (core, "audioDeviceIOProc");
    return 0;
}

static OSStatus init_out_device(coreaudioVoiceOut *core)
{
    OSStatus status;
    AudioDeviceID deviceID;
    AudioValueRange frameRange;
    UInt32 audioDevicePropertyBufferFrameSize;
    AudioDeviceIOProcID ioprocid;

    AudioStreamBasicDescription streamBasicDescription = {
        .mBitsPerChannel = core->hw.info.bits,
        .mBytesPerFrame = core->hw.info.bytes_per_frame,
        .mBytesPerPacket = core->hw.info.bytes_per_frame,
        .mChannelsPerFrame = core->hw.info.nchannels,
        .mFormatFlags = kLinearPCMFormatFlagIsFloat,
        .mFormatID = kAudioFormatLinearPCM,
        .mFramesPerPacket = 1,
        .mSampleRate = core->hw.info.freq
    };

    status = coreaudio_get_voice(&deviceID);
    if (status != kAudioHardwareNoError) {
        coreaudio_playback_logerr (status,
                                   "Could not get default output Device\n");
        return status;
    }
    if (deviceID == kAudioDeviceUnknown) {
        dolog ("Could not initialize playback - Unknown Audiodevice\n");
        return status;
    }

    /* get minimum and maximum buffer frame sizes */
    status = coreaudio_get_framesizerange(deviceID, &frameRange);
    if (status == kAudioHardwareBadObjectError) {
        return 0;
    }
    if (status != kAudioHardwareNoError) {
        coreaudio_playback_logerr (status,
                                    "Could not get device buffer frame range\n");
        return status;
    }

    if (frameRange.mMinimum > core->frameSizeSetting) {
        audioDevicePropertyBufferFrameSize = (UInt32) frameRange.mMinimum;
        dolog ("warning: Upsizing Buffer Frames to %f\n", frameRange.mMinimum);
    } else if (frameRange.mMaximum < core->frameSizeSetting) {
        audioDevicePropertyBufferFrameSize = (UInt32) frameRange.mMaximum;
        dolog ("warning: Downsizing Buffer Frames to %f\n", frameRange.mMaximum);
    } else {
        audioDevicePropertyBufferFrameSize = core->frameSizeSetting;
    }

    /* set Buffer Frame Size */
    status = coreaudio_set_framesize(deviceID,
                                     &audioDevicePropertyBufferFrameSize);
    if (status == kAudioHardwareBadObjectError) {
        return 0;
    }
    if (status != kAudioHardwareNoError) {
        coreaudio_playback_logerr (status,
                                    "Could not set device buffer frame size %" PRIu32 "\n",
                                    (uint32_t)audioDevicePropertyBufferFrameSize);
        return status;
    }

    /* get Buffer Frame Size */
    status = coreaudio_get_framesize(deviceID,
                                     &audioDevicePropertyBufferFrameSize);
    if (status == kAudioHardwareBadObjectError) {
        return 0;
    }
    if (status != kAudioHardwareNoError) {
        coreaudio_playback_logerr (status,
                                    "Could not get device buffer frame size\n");
        return status;
    }

    /* set Samplerate */
    status = coreaudio_set_streamformat(deviceID, &streamBasicDescription);
    if (status == kAudioHardwareBadObjectError) {
        return 0;
    }
    if (status != kAudioHardwareNoError) {
        coreaudio_playback_logerr (status,
                                   "Could not set samplerate %lf\n",
                                   streamBasicDescription.mSampleRate);
        return status;
    }

    /*
     * set Callback.
     *
     * On macOS 11.3.1, Core Audio calls AudioDeviceIOProc after calling an
     * internal function named HALB_Mutex::Lock(), which locks a mutex in
     * HALB_IOThread::Entry(void*). HALB_Mutex::Lock() is also called in
     * AudioObjectGetPropertyData, which is called by coreaudio driver.
     * Therefore, the specified callback must be designed to avoid a deadlock
     * with the callers of AudioObjectGetPropertyData.
     */
    ioprocid = NULL;
    status = AudioDeviceCreateIOProcID(deviceID,
                                       audioDeviceIOProc,
                                       &core->hw,
                                       &ioprocid);
    if (status == kAudioHardwareBadDeviceError) {
        return 0;
    }
    if (status != kAudioHardwareNoError || ioprocid == NULL) {
        coreaudio_playback_logerr (status, "Could not set IOProc\n");
        return status;
    }

    core->outputDeviceID = deviceID;
    core->audioDevicePropertyBufferFrameSize = audioDevicePropertyBufferFrameSize;
    core->hw.samples = core->bufferCount * core->audioDevicePropertyBufferFrameSize;
    audio_generic_initialize_buffer_out(&core->hw);
    core->output_written = core->output_read = 0;
    coreaudio_capture_begin(core);
    core->ioprocid = ioprocid;
    core->ioproc_started = false;

    return 0;
}

static void fini_out_device(coreaudioVoiceOut *core)
{
    OSStatus status;

    /* DeviceIsRunning describes all clients, not this IOProc. */
    if (core->ioproc_started) {
        status = AudioDeviceStop(core->outputDeviceID, core->ioprocid);
        if (status != kAudioHardwareBadDeviceError && status != kAudioHardwareNoError) {
            coreaudio_logerr(status, "Could not stop playback\n");
        }
    }

    /* remove callback */
    status = AudioDeviceDestroyIOProcID(core->outputDeviceID,
                                        core->ioprocid);
    if (status != kAudioHardwareBadDeviceError && status != kAudioHardwareNoError) {
        coreaudio_logerr(status, "Could not remove IOProc\n");
    }
    coreaudio_capture_end(core);
    core->ioproc_started = false;
    core->outputDeviceID = kAudioDeviceUnknown;
}

static void update_device_playback_state(coreaudioVoiceOut *core)
{
    OSStatus status;
    static int timing = -1;
    if (timing < 0) timing = getenv("POWEREMU_AUDIO_TIMING") != NULL;

    if (!core->outputDeviceID || !core->ioprocid) {
        return;
    }

    if (core->enabled) {
        /* Another app may already be playing on this device. Starting our
         * registered IOProc is still necessary to receive callbacks. */
        if (!core->ioproc_started) {
            int64_t began = timing ? qemu_clock_get_ns(QEMU_CLOCK_REALTIME) : 0;
            status = AudioDeviceStart(core->outputDeviceID, core->ioprocid);
            if (timing) {
                int64_t ended = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
                qemu_log("audio-transition: operation=start begin-ns %" PRId64
                         " end-ns %" PRId64 " duration-us %" PRId64
                         " status %d\n", began, ended, (ended - began) / 1000, status);
            }
            if (core->debug_audio) {
                qemu_log("coreaudio-device: start device %u status %d\n",
                         core->outputDeviceID, status);
            }
            if (status == kAudioHardwareNoError) {
                core->ioproc_started = true;
            }
            if (status != kAudioHardwareBadDeviceError && status != kAudioHardwareNoError) {
                coreaudio_logerr (status, "Could not resume playback\n");
            }
        }
    } else {
        /* stop playback */
        if (core->ioproc_started) {
            int64_t began = timing ? qemu_clock_get_ns(QEMU_CLOCK_REALTIME) : 0;
            status = AudioDeviceStop(core->outputDeviceID,
                                     core->ioprocid);
            if (timing) {
                int64_t ended = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
                qemu_log("audio-transition: operation=stop begin-ns %" PRId64
                         " end-ns %" PRId64 " duration-us %" PRId64
                         " status %d\n", began, ended, (ended - began) / 1000, status);
            }
            if (core->debug_audio) {
                qemu_log("coreaudio-device: stop device %u status %d\n",
                         core->outputDeviceID, status);
            }
            if (status == kAudioHardwareNoError || status == kAudioHardwareBadDeviceError) {
                core->ioproc_started = false;
            }
            if (status != kAudioHardwareBadDeviceError && status != kAudioHardwareNoError) {
                coreaudio_logerr(status, "Could not pause playback\n");
            }
        }
    }
}

/* called without BQL. */
static OSStatus handle_voice_change(
    AudioObjectID in_object_id,
    UInt32 in_number_addresses,
    const AudioObjectPropertyAddress *in_addresses,
    void *in_client_data)
{
    coreaudioVoiceOut *core = in_client_data;

    bql_lock();

    if (core->outputDeviceID) {
        fini_out_device(core);
    }

    init_out_device(core);

    if (core->outputDeviceID) {
        update_device_playback_state(core);
    }

    bql_unlock();
    return 0;
}

static int coreaudio_init_out(HWVoiceOut *hw, struct audsettings *as,
                              void *drv_opaque)
{
    OSStatus status;
    coreaudioVoiceOut *core = (coreaudioVoiceOut *) hw;
    int err;
    Audiodev *dev = drv_opaque;
    AudiodevCoreaudioPerDirectionOptions *cpdo = dev->u.coreaudio.out;
    struct audsettings obt_as;

    /* create mutex */
    err = pthread_mutex_init(&core->buf_mutex, NULL);
    if (err) {
        dolog("Could not create mutex\nReason: %s\n", strerror (err));
        return -1;
    }

    obt_as = *as;
    as = &obt_as;
    as->fmt = AUDIO_FORMAT_F32;
    audio_pcm_init_info (&hw->info, as);

    core->frameSizeSetting = audio_buffer_frames(
        qapi_AudiodevCoreaudioPerDirectionOptions_base(cpdo), as, 11610);

    core->bufferCount = cpdo->has_buffer_count ? cpdo->buffer_count : 4;
    core->debug_audio = getenv("SCREAMER_DEBUG") != NULL;
    /* Opt-in until matched live testing validates device changes and audio.
     * The existing mutex path remains a same-binary control. */
    const char *spsc = getenv("POWEREMU_COREAUDIO_SPSC");
    core->spsc_output = spsc && !strcmp(spsc, "1");

    status = AudioObjectAddPropertyListener(kAudioObjectSystemObject,
                                            &voice_addr, handle_voice_change,
                                            core);
    if (status != kAudioHardwareNoError) {
        coreaudio_playback_logerr (status,
                                   "Could not listen to voice property change\n");
        return -1;
    }

    if (init_out_device(core)) {
        status = AudioObjectRemovePropertyListener(kAudioObjectSystemObject,
                                                   &voice_addr,
                                                   handle_voice_change,
                                                   core);
        if (status != kAudioHardwareNoError) {
            coreaudio_playback_logerr(status,
                                      "Could not remove voice property change listener\n");
        }

        return -1;
    }

    return 0;
}

static void coreaudio_fini_out (HWVoiceOut *hw)
{
    OSStatus status;
    int err;
    coreaudioVoiceOut *core = (coreaudioVoiceOut *) hw;

    status = AudioObjectRemovePropertyListener(kAudioObjectSystemObject,
                                               &voice_addr,
                                               handle_voice_change,
                                               core);
    if (status != kAudioHardwareNoError) {
        coreaudio_logerr(status, "Could not remove voice property change listener\n");
    }

    fini_out_device(core);

    /* destroy mutex */
    err = pthread_mutex_destroy(&core->buf_mutex);
    if (err) {
        dolog("Could not destroy mutex\nReason: %s\n", strerror (err));
    }
}

static void coreaudio_enable_out(HWVoiceOut *hw, bool enable)
{
    coreaudioVoiceOut *core = (coreaudioVoiceOut *) hw;

    core->enabled = enable;
    update_device_playback_state(core);
}

/* ---- capture ---- */

#define coreaudio_record_logerr(status, ...) \
    coreaudio_logerr2(status, "record", __VA_ARGS__)

static int coreaudio_in_lock(coreaudioVoiceIn *core, const char *fn_name)
{
    int err = pthread_mutex_lock(&core->buf_mutex);

    if (err) {
        dolog("Could not lock voice for %s\nReason: %s\n",
              fn_name, strerror(err));
        return -1;
    }
    return 0;
}

static void coreaudio_in_unlock(coreaudioVoiceIn *core, const char *fn_name)
{
    int err = pthread_mutex_unlock(&core->buf_mutex);

    if (err) {
        dolog("Could not unlock voice for %s\nReason: %s\n",
              fn_name, strerror(err));
    }
}

/*
 * What the microphone heard, called without the BQL on Core Audio's own
 * thread.  Same locking rule as the playback IOProc: buf_mutex and nothing
 * else.
 */
static OSStatus audioDeviceIOProcIn(
    AudioDeviceID inDevice,
    const AudioTimeStamp *inNow,
    const AudioBufferList *inInputData,
    const AudioTimeStamp *inInputTime,
    AudioBufferList *outOutputData,
    const AudioTimeStamp *inOutputTime,
    void *hwptr)
{
    HWVoiceIn *hw = hwptr;
    coreaudioVoiceIn *core = (coreaudioVoiceIn *) hwptr;
    const uint8_t *in;
    size_t len;

    if (!inInputData || inInputData->mNumberBuffers < 1) {
        return 0;
    }
    in = inInputData->mBuffers[0].mData;
    len = inInputData->mBuffers[0].mDataByteSize;
    if (!in || !len) {
        return 0;
    }

    if (coreaudio_in_lock(core, "audioDeviceIOProcIn")) {
        return 0;
    }
    if (inDevice != core->inputDeviceID || !hw->buf_emul) {
        coreaudio_in_unlock(core, "audioDeviceIOProcIn(old device)");
        return 0;
    }

    /*
     * Keep the newest sound rather than the oldest.  If nothing is reading
     * -- the guest has an input open but is not recording -- the ring would
     * otherwise fill once and then hold the same half-second of audio for
     * as long as the machine runs.
     */
    if (len > hw->size_emul) {
        in += len - hw->size_emul;
        len = hw->size_emul;
    }
    while (len) {
        size_t n = MIN(len, hw->size_emul - hw->pos_emul);

        memcpy((uint8_t *)hw->buf_emul + hw->pos_emul, in, n);
        hw->pos_emul = (hw->pos_emul + n) % hw->size_emul;
        hw->pending_emul = MIN(hw->pending_emul + n, hw->size_emul);
        in += n;
        len -= n;
    }

    coreaudio_in_unlock(core, "audioDeviceIOProcIn");
    return 0;
}

static size_t coreaudio_read(HWVoiceIn *hw, void *buf, size_t size)
{
    coreaudioVoiceIn *core = (coreaudioVoiceIn *) hw;
    size_t total = 0;

    if (coreaudio_in_lock(core, "coreaudio_read")) {
        return 0;
    }
    while (total < size && hw->pending_emul) {
        size_t start = audio_ring_posb(hw->pos_emul, hw->pending_emul,
                                       hw->size_emul);
        size_t n = MIN(MIN(size - total, hw->pending_emul),
                       hw->size_emul - start);

        memcpy((uint8_t *)buf + total, (uint8_t *)hw->buf_emul + start, n);
        hw->pending_emul -= n;
        total += n;
    }
    coreaudio_in_unlock(core, "coreaudio_read");
    return total;
}

static OSStatus init_in_device(coreaudioVoiceIn *core)
{
    OSStatus status;
    AudioDeviceID deviceID;
    AudioValueRange frameRange;
    UInt32 framesize;
    AudioDeviceIOProcID ioprocid;
    UInt32 size = sizeof(deviceID);
    AudioObjectPropertyAddress addr = {
        kAudioDevicePropertyBufferFrameSizeRange,
        kAudioDevicePropertyScopeInput,
        kAudioObjectPropertyElementMain
    };

    status = AudioObjectGetPropertyData(kAudioObjectSystemObject,
                                        &in_voice_addr, 0, NULL,
                                        &size, &deviceID);
    if (status != kAudioHardwareNoError) {
        coreaudio_record_logerr(status,
                                "Could not get the default input device\n");
        return status;
    }
    if (deviceID == kAudioDeviceUnknown) {
        dolog("Could not initialize recording - no input device\n");
        return kAudioHardwareBadDeviceError;
    }

    size = sizeof(frameRange);
    status = AudioObjectGetPropertyData(deviceID, &addr, 0, NULL,
                                        &size, &frameRange);
    if (status == kAudioHardwareBadObjectError) {
        return 0;
    }
    if (status != kAudioHardwareNoError) {
        coreaudio_record_logerr(status,
                                "Could not get the input buffer frame range\n");
        return status;
    }
    if (frameRange.mMinimum > core->frameSizeSetting) {
        framesize = (UInt32) frameRange.mMinimum;
    } else if (frameRange.mMaximum < core->frameSizeSetting) {
        framesize = (UInt32) frameRange.mMaximum;
    } else {
        framesize = core->frameSizeSetting;
    }

    addr.mSelector = kAudioDevicePropertyBufferFrameSize;
    status = AudioObjectSetPropertyData(deviceID, &addr, 0, NULL,
                                        sizeof(framesize), &framesize);
    if (status != kAudioHardwareNoError &&
        status != kAudioHardwareBadObjectError) {
        coreaudio_record_logerr(status,
                                "Could not set the input buffer frame size\n");
        return status;
    }
    size = sizeof(framesize);
    status = AudioObjectGetPropertyData(deviceID, &addr, 0, NULL,
                                        &size, &framesize);
    if (status != kAudioHardwareNoError) {
        coreaudio_record_logerr(status,
                                "Could not get the input buffer frame size\n");
        return status;
    }

    /*
     * Ask the device for the shape this voice wants.  Unlike playback, a
     * failure here is not fatal: an input device that will not take the
     * format still delivers float frames at its own rate, and Core Audio's
     * own conversion is better than refusing to record at all.
     */
    {
        AudioStreamBasicDescription d = {
            .mBitsPerChannel = core->hw.info.bits,
            .mBytesPerFrame = core->hw.info.bytes_per_frame,
            .mBytesPerPacket = core->hw.info.bytes_per_frame,
            .mChannelsPerFrame = core->hw.info.nchannels,
            .mFormatFlags = kLinearPCMFormatFlagIsFloat,
            .mFormatID = kAudioFormatLinearPCM,
            .mFramesPerPacket = 1,
            .mSampleRate = core->hw.info.freq
        };
        addr.mSelector = kAudioDevicePropertyStreamFormat;
        status = AudioObjectSetPropertyData(deviceID, &addr, 0, NULL,
                                            sizeof(d), &d);
        if (status != kAudioHardwareNoError) {
            coreaudio_record_logerr(status,
                                    "Could not set the input samplerate\n");
        }
    }

    ioprocid = NULL;
    status = AudioDeviceCreateIOProcID(deviceID, audioDeviceIOProcIn,
                                       &core->hw, &ioprocid);
    if (status != kAudioHardwareNoError || ioprocid == NULL) {
        coreaudio_record_logerr(status, "Could not set the input IOProc\n");
        return status ? status : kAudioHardwareUnspecifiedError;
    }

    core->inputDeviceID = deviceID;
    core->audioDevicePropertyBufferFrameSize = framesize;
    core->hw.samples = core->bufferCount * framesize;
    audio_generic_initialize_buffer_in(&core->hw);
    core->ioprocid = ioprocid;
    return 0;
}

static void fini_in_device(coreaudioVoiceIn *core)
{
    OSStatus status;

    if (core->ioprocid != NULL) {
        status = AudioDeviceStop(core->inputDeviceID, core->ioprocid);
        if (status != kAudioHardwareNoError &&
            status != kAudioHardwareBadDeviceError) {
            coreaudio_record_logerr(status, "Could not stop recording\n");
        }
        status = AudioDeviceDestroyIOProcID(core->inputDeviceID,
                                            core->ioprocid);
        if (status != kAudioHardwareNoError &&
            status != kAudioHardwareBadDeviceError) {
            coreaudio_record_logerr(status, "Could not remove the IOProc\n");
        }
        core->ioprocid = NULL;
    }
    core->inputDeviceID = kAudioDeviceUnknown;
}

static int coreaudio_init_in(HWVoiceIn *hw, struct audsettings *as,
                             void *drv_opaque)
{
    coreaudioVoiceIn *core = (coreaudioVoiceIn *) hw;
    Audiodev *dev = drv_opaque;
    AudiodevCoreaudioPerDirectionOptions *cpdo = dev->u.coreaudio.in;
    struct audsettings obt_as;
    int err;

    err = pthread_mutex_init(&core->buf_mutex, NULL);
    if (err) {
        dolog("Could not create mutex\nReason: %s\n", strerror(err));
        return -1;
    }

    obt_as = *as;
    as = &obt_as;
    as->fmt = AUDIO_FORMAT_F32;
    audio_pcm_init_info(&hw->info, as);

    core->frameSizeSetting = audio_buffer_frames(
        qapi_AudiodevCoreaudioPerDirectionOptions_base(cpdo), as, 11610);
    core->bufferCount = cpdo->has_buffer_count ? cpdo->buffer_count : 4;

    if (init_in_device(core)) {
        pthread_mutex_destroy(&core->buf_mutex);
        return -1;
    }
    return 0;
}

static void coreaudio_fini_in(HWVoiceIn *hw)
{
    coreaudioVoiceIn *core = (coreaudioVoiceIn *) hw;
    int err;

    fini_in_device(core);
    err = pthread_mutex_destroy(&core->buf_mutex);
    if (err) {
        dolog("Could not destroy mutex\nReason: %s\n", strerror(err));
    }
}

static void coreaudio_enable_in(HWVoiceIn *hw, bool enable)
{
    coreaudioVoiceIn *core = (coreaudioVoiceIn *) hw;
    OSStatus status;

    if (core->enabled == enable || core->ioprocid == NULL) {
        core->enabled = enable;
        return;
    }
    core->enabled = enable;
    if (enable) {
        /*
         * This is where this Mac asks its owner whether the virtual Mac may
         * listen.  Until a guest selects an input and starts its input DMA,
         * nothing here runs and nothing is recorded.
         */
        status = AudioDeviceStart(core->inputDeviceID, core->ioprocid);
        if (status != kAudioHardwareNoError) {
            coreaudio_record_logerr(status, "Could not start recording\n");
            /*
             * Most often this Mac has not been asked for the microphone yet,
             * or was asked and refused.  Do not remember the failure: the
             * guest arms its input again every time it records, and if
             * permission is given in between, the next attempt should work.
             */
            core->enabled = false;
        }
    } else {
        status = AudioDeviceStop(core->inputDeviceID, core->ioprocid);
        if (status != kAudioHardwareNoError &&
            status != kAudioHardwareBadDeviceError) {
            coreaudio_record_logerr(status, "Could not stop recording\n");
        }
        if (!coreaudio_in_lock(core, "coreaudio_enable_in")) {
            hw->pending_emul = 0;
            hw->pos_emul = 0;
            coreaudio_in_unlock(core, "coreaudio_enable_in");
        }
    }
}

static void *coreaudio_audio_init(Audiodev *dev, Error **errp)
{
    return dev;
}

static void coreaudio_audio_fini (void *opaque)
{
}

static struct audio_pcm_ops coreaudio_pcm_ops = {
    .init_out = coreaudio_init_out,
    .fini_out = coreaudio_fini_out,
  /* wrapper for audio_generic_write */
    .write    = coreaudio_write,
  /* wrapper for audio_generic_buffer_get_free */
    .buffer_get_free = coreaudio_buffer_get_free,
  /* wrapper for audio_generic_get_buffer_out */
    .get_buffer_out = coreaudio_get_buffer_out,
  /* wrapper for audio_generic_put_buffer_out */
    .put_buffer_out = coreaudio_put_buffer_out,
    .enable_out = coreaudio_enable_out,
    .init_in  = coreaudio_init_in,
    .fini_in  = coreaudio_fini_in,
    .read     = coreaudio_read,
    .enable_in = coreaudio_enable_in
};

static struct audio_driver coreaudio_audio_driver = {
    .name           = "coreaudio",
    .descr          = "CoreAudio http://developer.apple.com/audio/coreaudio.html",
    .init           = coreaudio_audio_init,
    .fini           = coreaudio_audio_fini,
    .pcm_ops        = &coreaudio_pcm_ops,
    .max_voices_out = 1,
    .max_voices_in  = 1,
    .voice_size_out = sizeof (coreaudioVoiceOut),
    .voice_size_in  = sizeof (coreaudioVoiceIn)
};

static void register_audio_coreaudio(void)
{
    audio_driver_register(&coreaudio_audio_driver);
}
type_init(register_audio_coreaudio);
