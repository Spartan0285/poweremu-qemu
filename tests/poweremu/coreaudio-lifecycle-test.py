#!/usr/bin/env python3
"""Verify IOProc ownership when a shared CoreAudio device is already running."""
import argparse
from pathlib import Path
import subprocess
import tempfile
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--source',type=Path)
a=parser.parse_args()
root=Path(__file__).resolve().parents[2]
s=(a.source or root/'audio/coreaudio.m').read_text()
sig='static void update_device_playback_state('
start=s.index(sig);end=s.index('{',start)+1;depth=1
while depth:
    depth+=(s[end]=='{')-(s[end]=='}');end+=1
func=s[start:end]
code=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#define QEMU_CLOCK_REALTIME 0
static int64_t qemu_clock_get_ns(int clock){return 0;}
typedef int OSStatus;
typedef uint32_t UInt32;
enum {kAudioHardwareNoError=0,kAudioHardwareBadObjectError=1,kAudioHardwareBadDeviceError=2};
typedef struct {bool enabled,ioproc_started,debug_audio;int outputDeviceID;void *ioprocid;} coreaudioVoiceOut;
static unsigned starts,stops;
static OSStatus start_result,stop_result;
static bool other_client_running=true;
static OSStatus coreaudio_get_isrunning(int id,UInt32 *running){*running=other_client_running;return 0;}
static OSStatus AudioDeviceStart(int id,void *proc){assert(id&&proc);starts++;return start_result;}
static OSStatus AudioDeviceStop(int id,void *proc){assert(id&&proc);stops++;return stop_result;}
#define coreaudio_logerr(...) ((void)0)
#define qemu_log(...) ((void)0)
'''+func+r'''
int main(void){
    coreaudioVoiceOut c={.enabled=true,.outputDeviceID=9,.ioprocid=(void *)1};
    update_device_playback_state(&c);
    /* DeviceIsRunning may already be true for another client: our IOProc
     * still needs its own start. The original implementation fails here. */
    assert(starts==1&&c.ioproc_started);
    update_device_playback_state(&c);assert(starts==1);
    c.enabled=false;update_device_playback_state(&c);assert(stops==1&&!c.ioproc_started);
    update_device_playback_state(&c);assert(stops==1);
    assert(other_client_running);
    c.enabled=true;start_result=17;update_device_playback_state(&c);assert(!c.ioproc_started&&starts==2);
    start_result=0;update_device_playback_state(&c);assert(c.ioproc_started&&starts==3);
    c.enabled=false;stop_result=17;update_device_playback_state(&c);assert(c.ioproc_started&&stops==2);
    stop_result=0;update_device_playback_state(&c);assert(!c.ioproc_started&&stops==3);
    c.enabled=true;c.ioprocid=NULL;update_device_playback_state(&c);assert(starts==3);
    c.ioprocid=(void *)1;c.outputDeviceID=0;update_device_playback_state(&c);assert(starts==3);
    c.outputDeviceID=10;update_device_playback_state(&c);assert(c.ioproc_started&&starts==4);
    c.enabled=false;stop_result=kAudioHardwareBadDeviceError;update_device_playback_state(&c);assert(!c.ioproc_started);
    puts("PASS shared-device IOProc start, idempotent enable/disable, failure retry, absent/replaced device");
}
'''
with tempfile.TemporaryDirectory(prefix='poweremu-lifecycle-test-') as d:
    p=Path(d)/'test.c';p.write_text(code);exe=Path(d)/'test'
    subprocess.run(['clang','-std=c11','-O1','-g','-fsanitize=address,undefined',str(p),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
