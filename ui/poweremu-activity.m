/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "poweremu-activity.h"
#import <Foundation/Foundation.h>

/* The embedded display runs with -display none, so cocoa_display_init()
 * never acquires its activity token. Keep the helper's virtual-clock timers
 * and CPU work active even when the host app's window is obscured. */
void *poweremu_activity_begin(void)
{
    @autoreleasepool {
        return [[[NSProcessInfo processInfo]
            beginActivityWithOptions:NSActivityUserInitiated |
                                     NSActivityLatencyCritical
                              reason:@"Running a PowerEmu virtual machine"] retain];
    }
}

void poweremu_activity_end(void *activity)
{
    if (activity) {
        @autoreleasepool {
            [[NSProcessInfo processInfo] endActivity:(id)activity];
            [(id)activity release];
        }
    }
}
