/*
 * QEMU System Emulator
 *
 * Copyright (c) 2003-2008 Fabrice Bellard
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
#include "monitor/monitor.h"
#include "qemu/coroutine-tls.h"
#include "qapi/error.h"
#include "qapi/qapi-commands-machine.h"
#include "qapi/qapi-commands-misc.h"
#include "qapi/qapi-events-run-state.h"
#include "qapi/qmp/qerror.h"
#include "exec/gdbstub.h"
#include "system/accel-ops.h"
#include "system/hw_accel.h"
#include "exec/cpu-common.h"
#include "qemu/thread.h"
#include "qemu/main-loop.h"
#include "qemu/plugin.h"
#include "system/cpus.h"
#include "qemu/guest-random.h"
#include "hw/nmi.h"
#include "system/replay.h"
#include "system/runstate.h"
#include "system/cpu-timers.h"
#include "system/whpx.h"
#include "hw/boards.h"
#include "hw/hw.h"
#include "trace.h"
#include "system/system.h"
#include "poweremu-bql-trace.h"
#include "system/poweremu-bql-mmio.h"

#ifdef CONFIG_LINUX

#include <sys/prctl.h>

#ifndef PR_MCE_KILL
#define PR_MCE_KILL 33
#endif

#ifndef PR_MCE_KILL_SET
#define PR_MCE_KILL_SET 1
#endif

#ifndef PR_MCE_KILL_EARLY
#define PR_MCE_KILL_EARLY 1
#endif

#endif /* CONFIG_LINUX */

/* The Big QEMU Lock (BQL) */
static QemuMutex bql;

/* Default-off: no clock calls, memory allocation or logging unless requested. */
static bool pe_bql_enabled;
static uint64_t pe_bql_owner;
static PEBQLTrace pe_bql_trace;
bool poweremu_bql_mmio_trace_enabled;
static PEBQLMMIOTrace pe_bql_mmio_trace;
static char *pe_bql_prefix;
static void pe_bql_init(void);

/*
 * The chosen accelerator is supposed to register this.
 */
static const AccelOpsClass *cpus_accel;

bool cpu_is_stopped(CPUState *cpu)
{
    return cpu->stopped || !runstate_is_running();
}

bool cpu_work_list_empty(CPUState *cpu)
{
    return QSIMPLEQ_EMPTY_ATOMIC(&cpu->work_list);
}

bool cpu_thread_is_idle(CPUState *cpu)
{
    if (cpu->stop || !cpu_work_list_empty(cpu)) {
        return false;
    }
    if (cpu_is_stopped(cpu)) {
        return true;
    }
    if (!cpu->halted || cpu_has_work(cpu)) {
        return false;
    }
    if (cpus_accel->cpu_thread_is_idle) {
        return cpus_accel->cpu_thread_is_idle(cpu);
    }
    return true;
}

bool all_cpu_threads_idle(void)
{
    CPUState *cpu;

    CPU_FOREACH(cpu) {
        if (!cpu_thread_is_idle(cpu)) {
            return false;
        }
    }
    return true;
}

/***********************************************************/
void hw_error(const char *fmt, ...)
{
    va_list ap;
    CPUState *cpu;

    va_start(ap, fmt);
    fprintf(stderr, "qemu: hardware error: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    CPU_FOREACH(cpu) {
        fprintf(stderr, "CPU #%d:\n", cpu->cpu_index);
        cpu_dump_state(cpu, stderr, CPU_DUMP_FPU);
    }
    va_end(ap);
    abort();
}

void cpu_synchronize_all_states(void)
{
    CPUState *cpu;

    CPU_FOREACH(cpu) {
        cpu_synchronize_state(cpu);
    }
}

void cpu_synchronize_all_post_reset(void)
{
    CPUState *cpu;

    CPU_FOREACH(cpu) {
        cpu_synchronize_post_reset(cpu);
    }
}

void cpu_synchronize_all_post_init(void)
{
    CPUState *cpu;

    CPU_FOREACH(cpu) {
        cpu_synchronize_post_init(cpu);
    }
}

void cpu_synchronize_all_pre_loadvm(void)
{
    CPUState *cpu;

    CPU_FOREACH(cpu) {
        cpu_synchronize_pre_loadvm(cpu);
    }
}

void cpu_synchronize_state(CPUState *cpu)
{
    if (cpus_accel->synchronize_state) {
        cpus_accel->synchronize_state(cpu);
    }
}

void cpu_synchronize_post_reset(CPUState *cpu)
{
    if (cpus_accel->synchronize_post_reset) {
        cpus_accel->synchronize_post_reset(cpu);
    }
}

void cpu_synchronize_post_init(CPUState *cpu)
{
    if (cpus_accel->synchronize_post_init) {
        cpus_accel->synchronize_post_init(cpu);
    }
}

void cpu_synchronize_pre_loadvm(CPUState *cpu)
{
    if (cpus_accel->synchronize_pre_loadvm) {
        cpus_accel->synchronize_pre_loadvm(cpu);
    }
}

bool cpus_are_resettable(void)
{
    if (cpus_accel->cpus_are_resettable) {
        return cpus_accel->cpus_are_resettable();
    }
    return true;
}

void cpu_exec_reset_hold(CPUState *cpu)
{
    if (cpus_accel->cpu_reset_hold) {
        cpus_accel->cpu_reset_hold(cpu);
    }
}

int64_t cpus_get_virtual_clock(void)
{
    /*
     * XXX
     *
     * need to check that cpus_accel is not NULL, because qcow2 calls
     * qemu_get_clock_ns(CLOCK_VIRTUAL) without any accel initialized and
     * with ticks disabled in some io-tests:
     * 030 040 041 060 099 120 127 140 156 161 172 181 191 192 195 203 229 249 256 267
     *
     * is this expected?
     *
     * XXX
     */
    if (cpus_accel && cpus_accel->get_virtual_clock) {
        return cpus_accel->get_virtual_clock();
    }
    return cpu_get_clock();
}

/*
 * Signal the new virtual time to the accelerator. This is only needed
 * by accelerators that need to track the changes as we warp time.
 */
void cpus_set_virtual_clock(int64_t new_time)
{
    if (cpus_accel && cpus_accel->set_virtual_clock) {
        cpus_accel->set_virtual_clock(new_time);
    }
}

/*
 * return the time elapsed in VM between vm_start and vm_stop.  Unless
 * icount is active, cpus_get_elapsed_ticks() uses units of the host CPU cycle
 * counter.
 */
int64_t cpus_get_elapsed_ticks(void)
{
    if (cpus_accel->get_elapsed_ticks) {
        return cpus_accel->get_elapsed_ticks();
    }
    return cpu_get_ticks();
}

static void generic_handle_interrupt(CPUState *cpu, int mask)
{
    cpu->interrupt_request |= mask;

    if (!qemu_cpu_is_self(cpu)) {
        qemu_cpu_kick(cpu);
    }
}

void cpu_interrupt(CPUState *cpu, int mask)
{
    if (cpus_accel->handle_interrupt) {
        cpus_accel->handle_interrupt(cpu, mask);
    } else {
        generic_handle_interrupt(cpu, mask);
    }
}

/*
 * True if the vm was previously suspended, and has not been woken or reset.
 */
static int vm_was_suspended;

void vm_set_suspended(bool suspended)
{
    vm_was_suspended = suspended;
}

bool vm_get_suspended(void)
{
    return vm_was_suspended;
}

static int do_vm_stop(RunState state, bool send_stop)
{
    int ret = 0;
    RunState oldstate = runstate_get();

    if (runstate_is_live(oldstate)) {
        vm_was_suspended = (oldstate == RUN_STATE_SUSPENDED);
        runstate_set(state);
        cpu_disable_ticks();
        if (oldstate == RUN_STATE_RUNNING) {
            pause_all_vcpus();
        }
        vm_state_notify(0, state);
        if (send_stop) {
            qapi_event_send_stop();
        }
    }

    bdrv_drain_all();
    ret = bdrv_flush_all();
    trace_vm_stop_flush_all(ret);

    return ret;
}

/* Special vm_stop() variant for terminating the process.  Historically clients
 * did not expect a QMP STOP event and so we need to retain compatibility.
 */
int vm_shutdown(void)
{
    return do_vm_stop(RUN_STATE_SHUTDOWN, false);
}

bool cpu_can_run(CPUState *cpu)
{
    if (cpu->stop) {
        return false;
    }
    if (cpu_is_stopped(cpu)) {
        return false;
    }
    return true;
}

void cpu_handle_guest_debug(CPUState *cpu)
{
    if (replay_running_debug()) {
        if (!cpu->singlestep_enabled) {
            /*
             * Report about the breakpoint and
             * make a single step to skip it
             */
            replay_breakpoint();
            cpu_single_step(cpu, SSTEP_ENABLE);
        } else {
            cpu_single_step(cpu, 0);
        }
    } else {
        gdb_set_stop_cpu(cpu);
        qemu_system_debug_request();
        cpu->stopped = true;
    }
}

#ifdef CONFIG_LINUX
static void sigbus_reraise(void)
{
    sigset_t set;
    struct sigaction action;

    memset(&action, 0, sizeof(action));
    action.sa_handler = SIG_DFL;
    if (!sigaction(SIGBUS, &action, NULL)) {
        raise(SIGBUS);
        sigemptyset(&set);
        sigaddset(&set, SIGBUS);
        pthread_sigmask(SIG_UNBLOCK, &set, NULL);
    }
    perror("Failed to re-raise SIGBUS!");
    abort();
}

static void sigbus_handler(int n, siginfo_t *siginfo, void *ctx)
{
    if (siginfo->si_code != BUS_MCEERR_AO && siginfo->si_code != BUS_MCEERR_AR) {
        sigbus_reraise();
    }

    if (current_cpu) {
        /* Called asynchronously in VCPU thread.  */
        if (kvm_on_sigbus_vcpu(current_cpu, siginfo->si_code, siginfo->si_addr)) {
            sigbus_reraise();
        }
    } else {
        /* Called synchronously (via signalfd) in main thread.  */
        if (kvm_on_sigbus(siginfo->si_code, siginfo->si_addr)) {
            sigbus_reraise();
        }
    }
}

static void qemu_init_sigbus(void)
{
    struct sigaction action;

    /*
     * ALERT: when modifying this, take care that SIGBUS forwarding in
     * qemu_prealloc_mem() will continue working as expected.
     */
    memset(&action, 0, sizeof(action));
    action.sa_flags = SA_SIGINFO;
    action.sa_sigaction = sigbus_handler;
    sigaction(SIGBUS, &action, NULL);

    prctl(PR_MCE_KILL, PR_MCE_KILL_SET, PR_MCE_KILL_EARLY, 0, 0);
}
#else /* !CONFIG_LINUX */
static void qemu_init_sigbus(void)
{
}
#endif /* !CONFIG_LINUX */

static QemuThread io_thread;

/* cpu creation */
static QemuCond qemu_cpu_cond;
/* system init */
static QemuCond qemu_pause_cond;

void qemu_init_cpu_loop(void)
{
    qemu_init_sigbus();
    qemu_cond_init(&qemu_cpu_cond);
    qemu_cond_init(&qemu_pause_cond);
    qemu_mutex_init(&bql);
    pe_bql_init();

    qemu_thread_get_self(&io_thread);
}

void run_on_cpu(CPUState *cpu, run_on_cpu_func func, run_on_cpu_data data)
{
    do_run_on_cpu(cpu, func, data, &bql);
}

static void qemu_cpu_stop(CPUState *cpu, bool exit)
{
    g_assert(qemu_cpu_is_self(cpu));
    cpu->stop = false;
    cpu->stopped = true;
    if (exit) {
        cpu_exit(cpu);
    }
    qemu_cond_broadcast(&qemu_pause_cond);
}

void qemu_wait_io_event_common(CPUState *cpu)
{
    qatomic_set_mb(&cpu->thread_kicked, false);
    if (cpu->stop) {
        qemu_cpu_stop(cpu, false);
    }
    process_queued_cpu_work(cpu);
}

void qemu_wait_io_event(CPUState *cpu)
{
    bool slept = false;

    while (cpu_thread_is_idle(cpu)) {
        if (!slept) {
            slept = true;
            qemu_plugin_vcpu_idle_cb(cpu);
        }
        qemu_cond_wait_bql(cpu->halt_cond);
    }
    if (slept) {
        qemu_plugin_vcpu_resume_cb(cpu);
    }

    qemu_wait_io_event_common(cpu);
}

void cpus_kick_thread(CPUState *cpu)
{
    if (cpu->thread_kicked) {
        return;
    }
    cpu->thread_kicked = true;

#ifndef _WIN32
    int err = pthread_kill(cpu->thread->thread, SIG_IPI);
    if (err && err != ESRCH) {
        fprintf(stderr, "qemu:%s: %s", __func__, strerror(err));
        exit(1);
    }
#else
    qemu_sem_post(&cpu->sem);
#endif
}

void qemu_cpu_kick(CPUState *cpu)
{
    qemu_cond_broadcast(cpu->halt_cond);
    if (cpus_accel->kick_vcpu_thread) {
        cpus_accel->kick_vcpu_thread(cpu);
    } else { /* default */
        cpus_kick_thread(cpu);
    }
}

void qemu_cpu_kick_self(void)
{
    assert(current_cpu);
    cpus_kick_thread(current_cpu);
}

bool qemu_cpu_is_self(CPUState *cpu)
{
    return qemu_thread_is_self(cpu->thread);
}

bool qemu_in_vcpu_thread(void)
{
    return current_cpu && qemu_cpu_is_self(current_cpu);
}

QEMU_DEFINE_STATIC_CO_TLS(bool, bql_locked)
QEMU_DEFINE_STATIC_CO_TLS(PEBQLSegment, pe_bql_current)
typedef struct PEBQLMMIOStack {
    uint32_t depth;
    PEBQLSegment segments[64];
} PEBQLMMIOStack;
QEMU_DEFINE_STATIC_CO_TLS(PEBQLMMIOStack, pe_bql_mmio_stack)
static uint64_t pe_bql_mmio_acquisition_changes;

uint64_t poweremu_bql_mmio_begin(void)
{
    uint64_t ns;
    if (!qatomic_read(&poweremu_bql_mmio_trace_enabled) || !bql_locked()) {
        return 0;
    }
    ns = pe_bql_clock();
    if (!ns) {
        pe_bql_mmio_trace.clock_errors++;
        return 0;
    }
    if (ns >= pe_bql_trace.deadline_ns || !get_pe_bql_current().acquisition) {
        return 0;
    }
    PEBQLMMIOStack *stack = get_ptr_pe_bql_mmio_stack();
    if (stack->depth == G_N_ELEMENTS(stack->segments)) {
        pe_bql_mmio_trace.drops++;
        return 0;
    }
    stack->segments[stack->depth++] = get_pe_bql_current();
    return ns;
}

void poweremu_bql_mmio_end(uint64_t begin, const char *region, uint64_t addr,
                          unsigned size, bool write, bool with_attrs,
                          uintptr_t handler, unsigned result)
{
    if (begin) {
        PEBQLMMIOStack *stack = get_ptr_pe_bql_mmio_stack();
        uint32_t depth = stack->depth;
        g_assert(depth);
        PEBQLSegment s = stack->segments[--stack->depth];
        bool changed = !bql_locked() ||
                       s.acquisition != get_pe_bql_current().acquisition;
        if (changed) {
            qatomic_inc(&pe_bql_mmio_acquisition_changes);
            return;
        }
        PEBQLMMIOEvent e = {
            .begin_ns = begin, .end_ns = pe_bql_clock(),
            .acquisition = s.acquisition, .handler = handler, .addr = addr,
            .tid = s.tid, .cpu = s.cpu, .size = size, .write = write,
            .with_attrs = with_attrs, .result = result, .depth = depth,
        };
        pe_bql_mmio_event(&pe_bql_mmio_trace, &e, region,
                         pe_bql_trace.start_ns, pe_bql_trace.deadline_ns);
    }
}

static void pe_bql_flush(Notifier *notifier, void *data)
{
    bool owned = bql_locked();
    bool ok;
    FILE *f;
    char *path;

    /* Exit-only serialization: no producer can append while files flush. */
    if (!owned) {
        bql_lock();
    }
    PEBQLSegment s = get_pe_bql_current();
    if (s.begin_ns) {
        pe_bql_event(&pe_bql_trace, &s, s.begin_ns, pe_bql_clock(), 2, 0);
    }
    qatomic_set(&pe_bql_enabled, false);
    qatomic_set(&poweremu_bql_mmio_trace_enabled, false);
    qatomic_set(&pe_bql_owner, 0);
    path = g_strdup_printf("%s.bql.bin", pe_bql_prefix);
    f = fopen(path, "wb");
    ok = f != NULL;
    if (f) {
        if (fwrite(pe_bql_trace.events, sizeof(PEBQLEvent),
                   pe_bql_trace.count, f) != pe_bql_trace.count) {
            ok = false;
        }
        if (fclose(f)) {
            ok = false;
        }
    }
    g_free(path);
    if (pe_bql_mmio_trace.events) {
        path = g_strdup_printf("%s.bql-mmio.bin", pe_bql_prefix);
        f = fopen(path, "wb");
        if (!f) {
            ok = false;
        } else {
            if (fwrite(pe_bql_mmio_trace.events, sizeof(PEBQLMMIOEvent),
                       pe_bql_mmio_trace.count, f) != pe_bql_mmio_trace.count) {
                ok = false;
            }
            if (fclose(f)) {
                ok = false;
            }
        }
        g_free(path);
    }
    path = g_strdup_printf("%s.bql.json", pe_bql_prefix);
    f = fopen(path, "w");
    if (f) {
        int n = fprintf(f,
            "{\"schema\":1,\"clock\":\"CLOCK_MONOTONIC\","
            "\"record_bytes\":%zu,\"records\":%" PRIu64 ","
            "\"capacity\":%u,\"drops\":%" PRIu64 ","
            "\"clock_errors\":%" PRIu64 ",\"start_ns\":%" PRIu64 ","
            "\"deadline_ns\":%" PRIu64 ",\"threshold_ns\":%" PRIu64 ","
            "\"condition_reacquires\":%" PRIu64 ","
            "\"acquisitions\":%" PRIu64 ","
            "\"bql_lock_impl_runtime\":%" PRIu64 ","
            "\"return_address_kind\":\"acquisition-caller\","
            "\"binary_flush_ok\":%s,"
            "\"mmio_enabled\":%s,\"mmio_record_bytes\":%zu,"
            "\"mmio_records\":%" PRIu64 ",\"mmio_capacity\":%u,"
            "\"mmio_drops\":%" PRIu64 ",\"mmio_clock_errors\":%" PRIu64 ","
            "\"mmio_acquisition_changes\":%" PRIu64 ","
            "\"mmio_addr_is_region_relative\":true,"
            "\"owner_hint_is_wait_start_only\":true,"
            "\"condition_sleep_is_not_hold\":true}\n",
            sizeof(PEBQLEvent), pe_bql_trace.count, PE_BQL_CAPACITY,
            pe_bql_trace.drops, pe_bql_trace.clock_errors,
            pe_bql_trace.start_ns, pe_bql_trace.deadline_ns,
            PE_BQL_THRESHOLD_NS, pe_bql_trace.cond_reacquires,
            pe_bql_trace.next_acquisition, (uint64_t)(uintptr_t)bql_lock_impl,
            ok ? "true" : "false", pe_bql_mmio_trace.events ? "true" : "false",
            sizeof(PEBQLMMIOEvent), pe_bql_mmio_trace.count,
            PE_BQL_MMIO_CAPACITY, pe_bql_mmio_trace.drops,
            pe_bql_mmio_trace.clock_errors,
            qatomic_read(&pe_bql_mmio_acquisition_changes));
        if (n < 0 || ferror(f)) {
            ok = false;
        }
        if (fclose(f)) {
            ok = false;
        }
    } else {
        ok = false;
    }
    g_free(path);
    path = g_strdup_printf("%s.bql.flush.json", pe_bql_prefix);
    f = fopen(path, "w");
    if (!f || fprintf(f, "{\"all_outputs_ok\":%s}\n",
                      ok ? "true" : "false") < 0) {
        error_report("PowerEmu BQL trace final receipt failed");
    }
    if (f && fclose(f)) {
        error_report("PowerEmu BQL trace final receipt close failed");
    }
    g_free(path);
    if (!ok) {
        error_report("PowerEmu BQL trace flush failed");
    }
    if (!owned) {
        bql_unlock();
    }
}

static Notifier pe_bql_exit = { .notify = pe_bql_flush };

static void pe_bql_init(void)
{
    const char *prefix = getenv("POWEREMU_BQL_TRACE");
    if (!prefix || !*prefix) {
        return;
    }
    pe_bql_trace.start_ns = pe_bql_clock();
    if (!pe_bql_trace.start_ns) {
        error_report("PowerEmu BQL trace clock initialization failed");
        return;
    }
    pe_bql_trace.deadline_ns = pe_bql_trace.start_ns +
                              UINT64_C(600000000000);
    pe_bql_trace.events = g_try_new0(PEBQLEvent, PE_BQL_CAPACITY);
    if (!pe_bql_trace.events) {
        error_report("PowerEmu BQL trace allocation failed");
        return;
    }
    /* Commit pages before the hot path. */
    for (size_t i = 0; i < sizeof(PEBQLEvent) * PE_BQL_CAPACITY; i += 4096) {
        ((volatile unsigned char *)pe_bql_trace.events)[i] = 0;
    }
    const char *mmio = getenv("POWEREMU_BQL_MMIO_TRACE");
    if (mmio && !strcmp(mmio, "1")) {
        pe_bql_mmio_trace.events = g_try_new0(PEBQLMMIOEvent,
                                             PE_BQL_MMIO_CAPACITY);
        if (!pe_bql_mmio_trace.events) {
            error_report("PowerEmu BQL MMIO trace allocation failed");
        } else {
            for (size_t i = 0; i < sizeof(PEBQLMMIOEvent) *
                 PE_BQL_MMIO_CAPACITY; i += 4096) {
                ((volatile unsigned char *)pe_bql_mmio_trace.events)[i] = 0;
            }
            qatomic_set(&poweremu_bql_mmio_trace_enabled, true);
        }
    }
    pe_bql_prefix = g_strdup(prefix);
    qemu_add_exit_notifier(&pe_bql_exit);
    qatomic_set(&pe_bql_enabled, true);
}

static uint32_t bql_unlock_blocked;

void bql_block_unlock(bool increase)
{
    uint32_t new_value;

    assert(bql_locked());

    /* check for overflow! */
    new_value = bql_unlock_blocked + increase - !increase;
    assert((new_value > bql_unlock_blocked) == increase);
    bql_unlock_blocked = new_value;
}

bool bql_locked(void)
{
    return get_bql_locked();
}

bool qemu_in_main_thread(void)
{
    return bql_locked();
}

void rust_bql_mock_lock(void)
{
    error_report("This function should be used only from tests");
    abort();
}

/*
 * The BQL is taken from so many places that it is worth profiling the
 * callers directly, instead of funneling them all through a single function.
 */
void bql_lock_impl(const char *file, int line)
{
    QemuMutexLockFunc bql_lock_fn = qatomic_read(&bql_mutex_lock_func);

    uint64_t begin = 0, owner = 0;
    bool tracing = qatomic_read(&pe_bql_enabled);

    g_assert(!bql_locked());
    if (tracing) {
        begin = pe_bql_clock();
        owner = qatomic_read(&pe_bql_owner);
    }
    bql_lock_fn(&bql, file, line);
    set_bql_locked(true);
    if (tracing && qatomic_read(&pe_bql_enabled)) {
        uint64_t end = pe_bql_clock();
        PEBQLSegment s = pe_bql_segment(&pe_bql_trace, end, file, line,
            (uintptr_t)__builtin_return_address(0), qemu_get_thread_id(),
            current_cpu ? current_cpu->cpu_index : -1);
        pe_bql_event(&pe_bql_trace, &s, begin, end, 1, owner);
        set_pe_bql_current(s);
        qatomic_set(&pe_bql_owner, s.acquisition);
    }
}

void bql_unlock(void)
{
    g_assert(bql_locked());
    g_assert(!bql_unlock_blocked);
    if (qatomic_read(&pe_bql_enabled)) {
        PEBQLSegment s = get_pe_bql_current();
        if (s.begin_ns) {
            pe_bql_event(&pe_bql_trace, &s, s.begin_ns, pe_bql_clock(), 2, 0);
        }
        set_pe_bql_current((PEBQLSegment) {0});
        qatomic_set(&pe_bql_owner, 0);
    }
    set_bql_locked(false);
    qemu_mutex_unlock(&bql);
}

static void pe_bql_cond_release(void)
{
    if (qatomic_read(&pe_bql_enabled)) {
        PEBQLSegment s = get_pe_bql_current();
        if (s.begin_ns) {
            pe_bql_event(&pe_bql_trace, &s, s.begin_ns, pe_bql_clock(), 2, 0);
        }
        set_pe_bql_current((PEBQLSegment) {0});
        qatomic_set(&pe_bql_owner, 0);
    }
}

static void pe_bql_cond_acquired(const char *file, int line, uintptr_t callsite)
{
    if (qatomic_read(&pe_bql_enabled)) {
        PEBQLSegment s = pe_bql_segment(&pe_bql_trace, pe_bql_clock(),
            file, line, callsite, qemu_get_thread_id(),
            current_cpu ? current_cpu->cpu_index : -1);
        pe_bql_trace.cond_reacquires++;
        set_pe_bql_current(s);
        qatomic_set(&pe_bql_owner, s.acquisition);
    }
}

void qemu_cond_wait_bql(QemuCond *cond)
{
    pe_bql_cond_release();
    qemu_cond_wait(cond, &bql);
    pe_bql_cond_acquired("condition-reacquire", 0,
                         (uintptr_t)__builtin_return_address(0));
}

void qemu_cond_timedwait_bql(QemuCond *cond, int ms)
{
    pe_bql_cond_release();
    qemu_cond_timedwait(cond, &bql, ms);
    pe_bql_cond_acquired("timed-condition-reacquire", 0,
                         (uintptr_t)__builtin_return_address(0));
}

/* signal CPU creation */
void cpu_thread_signal_created(CPUState *cpu)
{
    cpu->created = true;
    qemu_cond_signal(&qemu_cpu_cond);
}

/* signal CPU destruction */
void cpu_thread_signal_destroyed(CPUState *cpu)
{
    cpu->created = false;
    qemu_cond_signal(&qemu_cpu_cond);
}

void cpu_pause(CPUState *cpu)
{
    if (qemu_cpu_is_self(cpu)) {
        qemu_cpu_stop(cpu, true);
    } else {
        cpu->stop = true;
        qemu_cpu_kick(cpu);
    }
}

void cpu_resume(CPUState *cpu)
{
    cpu->stop = false;
    cpu->stopped = false;
    qemu_cpu_kick(cpu);
}

static bool all_vcpus_paused(void)
{
    CPUState *cpu;

    CPU_FOREACH(cpu) {
        if (!cpu->stopped) {
            return false;
        }
    }

    return true;
}

void pause_all_vcpus(void)
{
    CPUState *cpu;

    qemu_clock_enable(QEMU_CLOCK_VIRTUAL, false);
    CPU_FOREACH(cpu) {
        cpu_pause(cpu);
    }

    /* We need to drop the replay_lock so any vCPU threads woken up
     * can finish their replay tasks
     */
    replay_mutex_unlock();

    while (!all_vcpus_paused()) {
        qemu_cond_wait_bql(&qemu_pause_cond);
        CPU_FOREACH(cpu) {
            qemu_cpu_kick(cpu);
        }
    }

    bql_unlock();
    replay_mutex_lock();
    bql_lock();
}

void resume_all_vcpus(void)
{
    CPUState *cpu;

    if (!runstate_is_running()) {
        return;
    }

    qemu_clock_enable(QEMU_CLOCK_VIRTUAL, true);
    CPU_FOREACH(cpu) {
        cpu_resume(cpu);
    }
}

void cpu_remove_sync(CPUState *cpu)
{
    cpu->stop = true;
    cpu->unplug = true;
    qemu_cpu_kick(cpu);
    bql_unlock();
    qemu_thread_join(cpu->thread);
    bql_lock();
}

void cpus_register_accel(const AccelOpsClass *ops)
{
    assert(ops != NULL);
    assert(ops->create_vcpu_thread != NULL); /* mandatory */
    cpus_accel = ops;
}

const AccelOpsClass *cpus_get_accel(void)
{
    /* broken if we call this early */
    assert(cpus_accel);
    return cpus_accel;
}

void qemu_init_vcpu(CPUState *cpu)
{
    MachineState *ms = MACHINE(qdev_get_machine());

    cpu->nr_threads =  ms->smp.threads;
    cpu->stopped = true;
    cpu->random_seed = qemu_guest_random_seed_thread_part1();

    if (!cpu->as) {
        /* If the target cpu hasn't set up any address spaces itself,
         * give it the default one.
         */
        cpu->num_ases = 1;
        cpu_address_space_init(cpu, 0, "cpu-memory", cpu->memory);
    }

    /* accelerators all implement the AccelOpsClass */
    g_assert(cpus_accel != NULL && cpus_accel->create_vcpu_thread != NULL);
    cpus_accel->create_vcpu_thread(cpu);

    while (!cpu->created) {
        qemu_cond_wait_bql(&qemu_cpu_cond);
    }
}

void cpu_stop_current(void)
{
    if (current_cpu) {
        current_cpu->stop = true;
        cpu_exit(current_cpu);
    }
}

int vm_stop(RunState state)
{
    if (qemu_in_vcpu_thread()) {
        qemu_system_vmstop_request_prepare();
        qemu_system_vmstop_request(state);
        /*
         * FIXME: should not return to device code in case
         * vm_stop() has been requested.
         */
        cpu_stop_current();
        return 0;
    }

    return do_vm_stop(state, true);
}

/**
 * Prepare for (re)starting the VM.
 * Returns 0 if the vCPUs should be restarted, -1 on an error condition,
 * and 1 otherwise.
 */
int vm_prepare_start(bool step_pending)
{
    int ret = vm_was_suspended ? 1 : 0;
    RunState state = vm_was_suspended ? RUN_STATE_SUSPENDED : RUN_STATE_RUNNING;
    RunState requested;

    qemu_vmstop_requested(&requested);
    if (runstate_is_running() && requested == RUN_STATE__MAX) {
        return -1;
    }

    /* Ensure that a STOP/RESUME pair of events is emitted if a
     * vmstop request was pending.  The BLOCK_IO_ERROR event, for
     * example, according to documentation is always followed by
     * the STOP event.
     */
    if (runstate_is_running()) {
        qapi_event_send_stop();
        qapi_event_send_resume();
        return -1;
    }

    /*
     * WHPX accelerator needs to know whether we are going to step
     * any CPUs, before starting the first one.
     */
    if (cpus_accel->synchronize_pre_resume) {
        cpus_accel->synchronize_pre_resume(step_pending);
    }

    /* We are sending this now, but the CPUs will be resumed shortly later */
    qapi_event_send_resume();

    cpu_enable_ticks();
    runstate_set(state);
    vm_state_notify(1, state);
    vm_was_suspended = false;
    return ret;
}

void vm_start(void)
{
    if (!vm_prepare_start(false)) {
        resume_all_vcpus();
    }
}

void vm_resume(RunState state)
{
    if (runstate_is_live(state)) {
        vm_start();
    } else {
        runstate_set(state);
    }
}

/* does a state transition even if the VM is already stopped,
   current state is forgotten forever */
int vm_stop_force_state(RunState state)
{
    if (runstate_is_live(runstate_get())) {
        return vm_stop(state);
    } else {
        int ret;
        runstate_set(state);

        bdrv_drain_all();
        /* Make sure to return an error if the flush in a previous vm_stop()
         * failed. */
        ret = bdrv_flush_all();
        trace_vm_stop_flush_all(ret);
        return ret;
    }
}

void qmp_memsave(uint64_t addr, uint64_t size, const char *filename,
                 bool has_cpu, int64_t cpu_index, Error **errp)
{
    FILE *f;
    uint64_t l;
    CPUState *cpu;
    uint8_t buf[1024];
    uint64_t orig_addr = addr, orig_size = size;

    if (!has_cpu) {
        cpu_index = 0;
    }

    cpu = qemu_get_cpu(cpu_index);
    if (cpu == NULL) {
        error_setg(errp, QERR_INVALID_PARAMETER_VALUE, "cpu-index",
                   "a CPU number");
        return;
    }

    f = fopen(filename, "wb");
    if (!f) {
        error_setg_file_open(errp, errno, filename);
        return;
    }

    while (size != 0) {
        l = sizeof(buf);
        if (l > size)
            l = size;
        if (cpu_memory_rw_debug(cpu, addr, buf, l, 0) != 0) {
            error_setg(errp, "Invalid addr 0x%016" PRIx64 "/size %" PRIu64
                             " specified", orig_addr, orig_size);
            goto exit;
        }
        if (fwrite(buf, 1, l, f) != l) {
            error_setg(errp, "writing memory to '%s' failed",
                       filename);
            goto exit;
        }
        addr += l;
        size -= l;
    }

exit:
    fclose(f);
}

void qmp_pmemsave(uint64_t addr, uint64_t size, const char *filename,
                  Error **errp)
{
    FILE *f;
    uint64_t l;
    uint8_t buf[1024];

    f = fopen(filename, "wb");
    if (!f) {
        error_setg_file_open(errp, errno, filename);
        return;
    }

    while (size != 0) {
        l = sizeof(buf);
        if (l > size)
            l = size;
        cpu_physical_memory_read(addr, buf, l);
        if (fwrite(buf, 1, l, f) != l) {
            error_setg(errp, "writing memory to '%s' failed",
                       filename);
            goto exit;
        }
        addr += l;
        size -= l;
    }

exit:
    fclose(f);
}

void qmp_inject_nmi(Error **errp)
{
    nmi_monitor_handle(monitor_get_cpu_index(monitor_cur()), errp);
}

