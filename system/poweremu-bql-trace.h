/* Optional bounded BQL diagnostics. All mutations require the BQL. */
#ifndef POWEREMU_BQL_TRACE_H
#define POWEREMU_BQL_TRACE_H

#define PE_BQL_THRESHOLD_NS UINT64_C(2000000)
#define PE_BQL_CAPACITY 65536

typedef struct PEBQLEvent {
    uint64_t begin_ns, end_ns, acquisition, owner_hint, callsite, tid, serial;
    int32_t cpu;
    uint32_t line, kind, flags; /* kind 1 wait, 2 hold; flags 1 path cut, 2 epoch cut */
    char file[128];
} PEBQLEvent;

typedef struct PEBQLSegment {
    uint64_t begin_ns, acquisition, callsite, tid;
    int32_t cpu;
    uint32_t line;
    const char *file;
} PEBQLSegment;

typedef struct PEBQLTrace {
    PEBQLEvent *events;
    uint64_t start_ns, deadline_ns, next_acquisition, count, drops, clock_errors;
    uint64_t cond_reacquires;
} PEBQLTrace;

static uint64_t pe_bql_clock(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) {
        return 0;
    }
    return (uint64_t)t.tv_sec * UINT64_C(1000000000) + t.tv_nsec;
}

/* Called only after acquiring BQL, or before releasing it. No IO/allocation. */
static void pe_bql_event(PEBQLTrace *t, const PEBQLSegment *s,
                         uint64_t begin, uint64_t end, unsigned kind,
                         uint64_t owner_hint)
{
    PEBQLEvent *e;
    size_t n = 0;
    uint32_t flags = 0;

    if (!begin || !end || end < begin) {
        t->clock_errors++;
        return;
    }
    if (begin >= t->deadline_ns || begin < t->start_ns) {
        return;
    }
    if (end > t->deadline_ns) {
        end = t->deadline_ns;
        flags |= 2;
    }
    if (end - begin <= PE_BQL_THRESHOLD_NS) {
        return;
    }
    if (t->count == PE_BQL_CAPACITY) {
        t->drops++;
        return;
    }
    e = &t->events[t->count];
    *e = (PEBQLEvent) {
        .begin_ns = begin, .end_ns = end, .acquisition = s->acquisition,
        .owner_hint = owner_hint, .callsite = s->callsite, .tid = s->tid,
        .serial = t->count, .cpu = s->cpu, .line = s->line, .kind = kind,
    };
    while (s->file && s->file[n] && n + 1 < sizeof(e->file)) {
        e->file[n] = s->file[n];
        n++;
    }
    if (s->file && s->file[n]) {
        flags |= 1;
    }
    e->flags = flags;
    t->count++;
}

static PEBQLSegment pe_bql_segment(PEBQLTrace *t, uint64_t ns,
                                  const char *file, unsigned line,
                                  uint64_t callsite, uint64_t tid, int cpu)
{
    return (PEBQLSegment) {
        .begin_ns = ns, .acquisition = ++t->next_acquisition,
        .file = file, .line = line, .callsite = callsite, .tid = tid, .cpu = cpu,
    };
}

#define PE_BQL_MMIO_CAPACITY 16384
typedef struct PEBQLMMIOEvent {
    uint64_t begin_ns, end_ns, acquisition, handler, addr, tid, serial;
    int32_t cpu;
    uint32_t size, write, with_attrs, result, depth, flags, reserved;
    char region[128];
} PEBQLMMIOEvent;
typedef struct PEBQLMMIOTrace {
    PEBQLMMIOEvent *events;
    uint64_t count, drops, clock_errors;
} PEBQLMMIOTrace;
static void pe_bql_mmio_event(PEBQLMMIOTrace *t, const PEBQLMMIOEvent *source,
                              const char *region, uint64_t start,
                              uint64_t deadline)
{
    PEBQLMMIOEvent *e;
    size_t n = 0;
    uint64_t end = source->end_ns;
    if (!source->begin_ns || !end || end < source->begin_ns) {
        t->clock_errors++;
        return;
    }
    if (source->begin_ns < start || source->begin_ns >= deadline) {
        return;
    }
    if (end > deadline) {
        end = deadline;
    }
    if (end - source->begin_ns <= PE_BQL_THRESHOLD_NS) {
        return;
    }
    if (t->count == PE_BQL_MMIO_CAPACITY) {
        t->drops++;
        return;
    }
    e = &t->events[t->count];
    *e = *source;
    e->end_ns = end;
    e->serial = t->count;
    if (end != source->end_ns) {
        e->flags |= 2;
    }
    while (region && region[n] && n + 1 < sizeof(e->region)) {
        e->region[n] = region[n];
        n++;
    }
    if (region && region[n]) {
        e->flags |= 1;
    }
    t->count++;
}
#endif
