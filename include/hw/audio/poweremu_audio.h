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
    /*
     * The ring lives in a RAM BAR of the device rather than in guest
     * memory.  A driver loaded at PCI discovery can only use libraries
     * that live in ROM, which rules out allocating a buffer and resolving
     * its physical address; and a RAM BAR is mapped straight into the
     * guest, so the guest's writes are ordinary stores with no exit and
     * the device reads its own memory with no DMA.
     */
    MemoryRegion ring;
    uint8_t *ring_ptr;

    QEMUSoundCard card;
    SWVoiceOut *voice;

    uint32_t ring_base, ring_size, rate, format, control, status;
    uint32_t write_ptr, read_ptr;
    uint32_t open_rate;
};

#endif
