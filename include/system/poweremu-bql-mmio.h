/* Default-off timing of actual MMIO callbacks while BQL is owned. */
#ifndef POWEREMU_BQL_MMIO_H
#define POWEREMU_BQL_MMIO_H
extern bool poweremu_bql_mmio_trace_enabled;
uint64_t poweremu_bql_mmio_begin(void);
void poweremu_bql_mmio_end(uint64_t begin, const char *region, uint64_t addr,
                          unsigned size, bool write, bool with_attrs,
                          uintptr_t handler, unsigned result);
#endif
