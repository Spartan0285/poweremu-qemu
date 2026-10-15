/* Diagnostic mapping learned only from actual transfers, never guessed strides. */
#define SLOT_MAP_LIMIT 65536
static struct {
    uint64_t *addresses, *sorted;
    uint32_t length, count, origin, next, stable;
    uint32_t rate, shift;
    int64_t last;
    bool enabled, started, mature;
} slot_map;

static void slot_map_reset(void)
{
    slot_map.length = slot_map.count = slot_map.stable = 0;
    slot_map.started = slot_map.mature = false;
}

static int slot_map_address_compare(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

static bool slot_map_unique(void)
{
    memcpy(slot_map.sorted, slot_map.addresses,
           slot_map.length * sizeof(uint64_t));
    qsort(slot_map.sorted, slot_map.length, sizeof(uint64_t),
          slot_map_address_compare);
    for (uint32_t i = 1; i < slot_map.length; i++) {
        if (slot_map.sorted[i] == slot_map.sorted[i - 1]) { return false; }
    }
    return true;
}

/* Returns false on a change; the caller invalidates every outstanding ticket. */
static bool slot_map_transfer(uint32_t frame, uint64_t address, uint32_t frames,
                              uint32_t rate, uint32_t shift, int64_t wall)
{
    if (!slot_map.enabled || !frames) { return true; }
    if (shift > 2 || !rate ||
        address > UINT64_MAX - ((uint64_t)frames << shift)) { return false; }
    if (slot_map.started &&
        (frame != slot_map.next || rate != slot_map.rate ||
         shift != slot_map.shift ||
         (slot_map.length && wall - slot_map.last >
          2LL * slot_map.length * NANOSECONDS_PER_SECOND / rate))) {
        return false;
    }
    if (!slot_map.started) {
        slot_map.started = true;
        slot_map.origin = slot_map.next = frame;
        slot_map.rate = rate; slot_map.shift = shift;
    }
    for (uint32_t i = 0; i < frames; i++) {
        uint64_t a = address + ((uint64_t)i << shift);
        if (!slot_map.length && slot_map.count &&
            a == slot_map.addresses[0]) {
            slot_map.length = slot_map.count;
            if (slot_map.length < 4096 || !slot_map_unique()) { return false; }
            slot_map.count = 0; slot_map.stable = 1;
        }
        if (!slot_map.length) {
            if (slot_map.count == SLOT_MAP_LIMIT) { return false; }
            slot_map.addresses[slot_map.count++] = a;
        } else {
            uint32_t phase = (frame + i - slot_map.origin) % slot_map.length;
            if (a != slot_map.addresses[phase] || phase != slot_map.count) {
                return false;
            }
            if (++slot_map.count == slot_map.length) {
                slot_map.count = 0;
                if (slot_map.stable < 3) { slot_map.stable++; }
                slot_map.mature = slot_map.stable >= 3;
            }
        }
    }
    slot_map.next = frame + frames;
    slot_map.last = wall;
    return true;
}

static bool slot_map_target(uint32_t frame, unsigned lead, unsigned frames,
                            uint64_t *address)
{
    if (!slot_map.enabled || !slot_map.mature || frame != slot_map.next) {
        return false;
    }
    uint32_t phase = (frame - slot_map.origin + lead) % slot_map.length;
    uint64_t a = slot_map.addresses[phase];
    if (a > UINT64_MAX - ((uint64_t)frames << slot_map.shift)) { return false; }
    for (unsigned i = 1; i < frames; i++) {
        if (slot_map.addresses[(phase + i) % slot_map.length] !=
            a + ((uint64_t)i << slot_map.shift)) { return false; }
    }
    *address = a;
    return true;
}
