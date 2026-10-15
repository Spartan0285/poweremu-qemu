/* Opt-in read-only DMA slot observer. Serialized by BQL, flush only at exit. */
#include "screamer-slot-map.h"
#define SLOT_BYTES (32 * 1024 * 1024)
#define SLOT_RECORDS 65536
#define SLOT_FRAMES 8
#define SLOT_INTERVAL_NS (2 * SCALE_MS)
enum { SLOT_PRE = 1, SLOT_POST = 2, SLOT_PARTIAL = 3, SLOT_INVALID = 4 };
typedef struct ScreamerSlotRecord {
    uint64_t ticket, epoch, address, payload;
    int64_t first_wall, last_wall, first_virtual, last_virtual;
    uint32_t target_frame, first_frame, last_frame, rate;
    uint32_t shift, frames, phase, repeats;
} ScreamerSlotRecord;
static struct {
    uint8_t *bytes;
    ScreamerSlotRecord *records;
    size_t used, count;
    uint64_t ticket, epoch, dropped, invalid, incomplete, undersampled;
    uint64_t address;
    uint32_t target, rate, shift, pre_reads;
    int64_t next, post_until, begin, delay, duration;
    bool tracked, consumed, learning;
    char *path;
    Notifier exit;
} screamer_slot;

static void screamer_slot_exit(Notifier *n, void *data)
{
    if (screamer_slot.tracked) {
        screamer_slot.incomplete++;
    }
    bool bytes_ok = screamer_pcm_flush(screamer_slot.path,
                                       screamer_slot.bytes, screamer_slot.used);
    char *events = g_strdup_printf("%s.events", screamer_slot.path);
    bool events_ok = screamer_pcm_flush(events, screamer_slot.records,
                           screamer_slot.count * sizeof(ScreamerSlotRecord));
    char *meta = g_strdup_printf("%s.json", screamer_slot.path);
    FILE *f = fopen(meta, "w");
    if (f) {
        fprintf(f, "{\"record_bytes\":%zu,\"bytes\":%zu,\"records\":%zu,"
                "\"dropped\":%" PRIu64 ",\"invalid\":%" PRIu64
                ",\"incomplete\":%" PRIu64 ",\"undersampled\":%" PRIu64
                ",\"ring_mode\":%s,\"ring_length\":%u,\"ring_mature\":%s,\"pcm_flush_ok\":%s,\"events_flush_ok\":%s}\n",
                sizeof(ScreamerSlotRecord), screamer_slot.used,
                screamer_slot.count, screamer_slot.dropped,
                screamer_slot.invalid, screamer_slot.incomplete,
                screamer_slot.undersampled, slot_map.enabled ? "true" : "false",
                slot_map.length, slot_map.mature ? "true" : "false",
                bytes_ok ? "true" : "false",
                events_ok ? "true" : "false");
        fclose(f);
    }
    g_free(events); g_free(meta);
    g_free(screamer_slot.bytes); g_free(screamer_slot.records);
    g_free(screamer_slot.path);
    screamer_slot.bytes = NULL; screamer_slot.records = NULL;
    g_free(slot_map.addresses); g_free(slot_map.sorted);
    slot_map.addresses = slot_map.sorted = NULL; slot_map.enabled = false;
}

static void screamer_slot_init(void)
{
    const char *prefix = getenv("POWEREMU_SCREAMER_SLOT_CAPTURE");
    if (!prefix || prefix[0] != '/' || screamer_slot.bytes) {
        return;
    }
    uint8_t *bytes = g_try_malloc0_n(SLOT_BYTES, 1);
    ScreamerSlotRecord *records = g_try_malloc0_n(SLOT_RECORDS, sizeof(*records));
    if (!bytes || !records) {
        g_free(bytes); g_free(records); return;
    }
    volatile uint8_t *pages = bytes;
    for (size_t i = 0; i < SLOT_BYTES; i += 4096) { pages[i] = 0; }
    pages = (volatile uint8_t *)records;
    for (size_t i = 0; i < SLOT_RECORDS * sizeof(*records); i += 4096) {
        pages[i] = 0;
    }
    const char *learn = getenv("POWEREMU_SCREAMER_SLOT_LEARN_RING");
    if (learn && !strcmp(learn, "1")) {
        slot_map.addresses = g_try_malloc0_n(SLOT_MAP_LIMIT, sizeof(uint64_t));
        slot_map.sorted = g_try_malloc0_n(SLOT_MAP_LIMIT, sizeof(uint64_t));
        if (!slot_map.addresses || !slot_map.sorted) {
            g_free(slot_map.addresses); g_free(slot_map.sorted);
            slot_map.addresses = slot_map.sorted = NULL;
            g_free(bytes); g_free(records); return;
        }
        volatile uint8_t *map_pages = (volatile uint8_t *)slot_map.addresses;
        volatile uint8_t *sort_pages = (volatile uint8_t *)slot_map.sorted;
        for (size_t i = 0; i < SLOT_MAP_LIMIT * sizeof(uint64_t); i += 4096) {
            map_pages[i] = sort_pages[i] = 0;
        }
        slot_map.enabled = true;
    }
    screamer_slot.bytes = bytes; screamer_slot.records = records;
    screamer_slot.path = g_strdup_printf("%s.%d.screamer-slot", prefix, getpid());
    /* Explicit host-time arming window, validated once outside callbacks. */
    const char *delay = getenv("POWEREMU_SCREAMER_SLOT_DELAY_SECONDS");
    unsigned d = 0;
    if (delay) {
        char *end; unsigned long v = strtoul(delay, &end, 10);
        if (*delay && !*end && v <= 480) { d = v; }
    }
    screamer_slot.delay = (int64_t)d * NANOSECONDS_PER_SECOND;
    screamer_slot.duration = 60LL * NANOSECONDS_PER_SECOND;
    screamer_slot.begin = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    screamer_slot.exit.notify = screamer_slot_exit;
    qemu_add_exit_notifier(&screamer_slot.exit);
}

static void screamer_slot_invalidate(void)
{
    if (!screamer_slot.bytes) { return; }
    if (screamer_slot.tracked) {
        screamer_slot.invalid++;
        for (size_t i = screamer_slot.count; i > 0; i--) {
            if (screamer_slot.records[i - 1].ticket != screamer_slot.ticket) {
                break;
            }
            screamer_slot.records[i - 1].phase = SLOT_INVALID;
        }
    }
    screamer_slot.tracked = false;
    screamer_slot.epoch++;
    slot_map_reset();
}

static void screamer_slot_transfer(ScreamerState *s, unsigned frames)
{
    if (!slot_map.enabled || !screamer_slot.learning) { return; }
    if (!slot_map_transfer(s->regs[FRAME_CNT_REG], s->io.addr, frames,
                          s->rate, s->shift,
                          qemu_clock_get_ns(QEMU_CLOCK_REALTIME))) {
        screamer_slot_invalidate();
    }
}

static void screamer_slot_tick(ScreamerState *s)
{
    if (!screamer_slot.bytes) { return; }
    int64_t wall = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    DBDMA_channel *channel = s->io.channel;
    if (slot_map.enabled && slot_map.started && channel &&
        (!(channel->regs[DBDMA_STATUS] & RUN) ||
         (channel->regs[DBDMA_STATUS] & (PAUSE | DEAD)))) {
        screamer_slot_invalidate();
    }
    if (slot_map.enabled && slot_map.mature &&
        wall - slot_map.last > 2LL * slot_map.length *
                               NANOSECONDS_PER_SECOND / slot_map.rate) {
        screamer_slot_invalidate();
    }
    int64_t age = wall - screamer_slot.begin;
    if (age < screamer_slot.delay) { return; }
    if (age >= screamer_slot.delay + screamer_slot.duration) {
        if (screamer_slot.tracked) { screamer_slot.incomplete++; }
        screamer_slot.tracked = false;
        screamer_slot.learning = false;
        return;
    }
    screamer_slot.learning = true;
    if (wall < screamer_slot.next) { return; }
    screamer_slot.next = wall + SLOT_INTERVAL_NS;
    uint32_t current = s->regs[FRAME_CNT_REG];
    if (screamer_slot.tracked &&
        (s->rate != screamer_slot.rate || s->shift != screamer_slot.shift)) {
        screamer_slot_invalidate();
    }
    if (screamer_slot.tracked && screamer_slot.consumed &&
        wall >= screamer_slot.post_until) { screamer_slot.tracked = false; }
    if (!screamer_slot.tracked) {
        if (s->io.len <= 0 || s->shift < 0 || s->shift > 2) { return; }
        /* Tiger uses 32-frame descriptors: retain strict descriptor bounds.
         * This short lookahead principally tests post-read late filling;
         * it cannot exclude an erase before the first observation. */
        unsigned lead = slot_map.enabled ?
            ((screamer_slot.ticket & 1) ? 2048 : 1024) :
            ((screamer_slot.ticket & 1) ? 16 : 8);
        uint64_t offset = (uint64_t)lead << s->shift;
        uint64_t bytes = (uint64_t)SLOT_FRAMES << s->shift;
        uint64_t target_address;
        if (slot_map.enabled) {
            if (!slot_map_target(current, lead, SLOT_FRAMES, &target_address)) {
                return;
            }
        } else {
            if (offset + bytes > s->io.len ||
                s->io.addr > UINT64_MAX - offset - bytes) { return; }
            target_address = s->io.addr + offset;
        }
        screamer_slot.ticket++;
        screamer_slot.address = target_address;
        screamer_slot.target = current + lead;
        screamer_slot.rate = s->rate; screamer_slot.shift = s->shift;
        screamer_slot.tracked = true; screamer_slot.consumed = false;
        screamer_slot.pre_reads = 0;
    }
    uint32_t phase = SLOT_PRE;
    if ((int32_t)(current - (screamer_slot.target + SLOT_FRAMES)) >= 0) {
        phase = SLOT_POST;
        if (!screamer_slot.consumed) {
            if (screamer_slot.pre_reads < 2) { screamer_slot.undersampled++; }
            screamer_slot.consumed = true;
            screamer_slot.post_until = wall + 20 * SCALE_MS;
        }
    }
    if (phase == SLOT_PRE && (int32_t)(current - screamer_slot.target) >= 0) {
        phase = SLOT_PARTIAL;
    }
    if (phase == SLOT_PRE) { screamer_slot.pre_reads++; }
    size_t bytes = (size_t)SLOT_FRAMES << screamer_slot.shift;
    hwaddr translated, length = bytes;
    MemoryRegion *mr = address_space_translate(&address_space_memory,
        screamer_slot.address, &translated, &length, false,
        MEMTXATTRS_UNSPECIFIED);
    /* Never touch device MMIO. No assumption of adjacent descriptors. */
    if (!memory_region_is_ram(mr) || length < bytes) {
        screamer_slot_invalidate(); return;
    }
    uint8_t snapshot[SLOT_FRAMES * 4];
    if (dma_memory_read(&address_space_memory, screamer_slot.address,
                        snapshot, bytes, MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
        screamer_slot_invalidate(); return;
    }
    int64_t virt = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    ScreamerSlotRecord *last = screamer_slot.count ?
        &screamer_slot.records[screamer_slot.count - 1] : NULL;
    if (last && last->ticket == screamer_slot.ticket && last->phase == phase &&
        !memcmp(screamer_slot.bytes + last->payload, snapshot, bytes)) {
        last->last_wall = wall; last->last_virtual = virt;
        last->last_frame = current; last->repeats++; return;
    }
    if (screamer_slot.count == SLOT_RECORDS ||
        bytes > SLOT_BYTES - screamer_slot.used) {
        screamer_slot.dropped++; return;
    }
    ScreamerSlotRecord *r = &screamer_slot.records[screamer_slot.count++];
    *r = (ScreamerSlotRecord) {
        .ticket = screamer_slot.ticket, .epoch = screamer_slot.epoch,
        .address = screamer_slot.address, .payload = screamer_slot.used,
        .first_wall = wall, .last_wall = wall,
        .first_virtual = virt, .last_virtual = virt,
        .target_frame = screamer_slot.target, .first_frame = current,
        .last_frame = current, .rate = s->rate, .shift = s->shift,
        .frames = SLOT_FRAMES, .phase = phase, .repeats = 1,
    };
    memcpy(screamer_slot.bytes + screamer_slot.used, snapshot, bytes);
    screamer_slot.used += bytes;
}
