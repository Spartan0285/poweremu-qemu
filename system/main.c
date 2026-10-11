/*
 * QEMU System Emulator
 *
 * Copyright (c) 2003-2020 Fabrice Bellard
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
#include "qemu-main.h"
#include "qemu/main-loop.h"
#include "system/replay.h"
#include "system/system.h"

#ifdef CONFIG_DARWIN
#include <pthread/qos.h>
#endif

#ifdef CONFIG_SDL
/*
 * SDL insists on wrapping the main() function with its own implementation on
 * some platforms; it does so via a macro that renames our main function, so
 * <SDL.h> must be #included here even with no SDL code called from this file.
 */
#include <SDL.h>
#endif

#ifdef CONFIG_DARWIN
#include <CoreFoundation/CoreFoundation.h>
#endif

static void *qemu_default_main(void *opaque)
{
    int status;

#ifdef CONFIG_DARWIN
    /* The Darwin event loop runs on a separate thread. Test matching the
     * vCPU QoS here, on the thread that actually dispatches DMA timers. */
    const char *audio_qos = getenv("POWEREMU_MAINLOOP_QOS");
    if (audio_qos && !strcmp(audio_qos, "1")) {
        int result = pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
        qos_class_t actual_qos = QOS_CLASS_UNSPECIFIED;
        pthread_get_qos_class_np(pthread_self(), &actual_qos, NULL);
        fprintf(stderr, "POWEREMU_MAINLOOP_QOS result=%d class=%u\n", result,
                (unsigned)actual_qos);
    }
#endif

    replay_mutex_lock();
    bql_lock();
    status = qemu_main_loop();
    qemu_cleanup(status);
    bql_unlock();
    replay_mutex_unlock();

    exit(status);
}

int (*qemu_main)(void);

#ifdef CONFIG_DARWIN
static int os_darwin_cfrunloop_main(void)
{
    CFRunLoopRun();
    g_assert_not_reached();
}
int (*qemu_main)(void) = os_darwin_cfrunloop_main;
#endif

int main(int argc, char **argv)
{
    qemu_init(argc, argv);
    bql_unlock();
    replay_mutex_unlock();
    if (qemu_main) {
        QemuThread main_loop_thread;
        qemu_thread_create(&main_loop_thread, "qemu_main",
                           qemu_default_main, NULL, QEMU_THREAD_DETACHED);
        return qemu_main();
    } else {
        qemu_default_main(NULL);
        g_assert_not_reached();
    }
}
