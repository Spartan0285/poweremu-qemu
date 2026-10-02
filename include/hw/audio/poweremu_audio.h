/*
 * poweremu-audio -- a sound device designed to be easy to drive.
 * See hw/audio/poweremu_audio.c and PowerEmu's docs/PARAVIRTUAL-SOUND.md.
 */
#ifndef HW_AUDIO_POWEREMU_AUDIO_H
#define HW_AUDIO_POWEREMU_AUDIO_H

#include "hw/pci/pci_device.h"
#include "audio/audio.h"
#include "qom/object.h"

#define TYPE_POWEREMU_AUDIO "poweremu-audio"
OBJECT_DECLARE_SIMPLE_TYPE(PowerEmuAudioState, POWEREMU_AUDIO)

struct PowerEmuAudioState {
    PCIDevice parent_obj;

    MemoryRegion mem;
    QEMUSoundCard card;
    SWVoiceOut *voice;

    uint32_t ring_base, ring_size, rate, format, control, status;
    uint32_t write_ptr, read_ptr;
    uint32_t open_rate;
};

#endif
