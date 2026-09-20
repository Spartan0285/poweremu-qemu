#!/usr/bin/env python3
"""Summarise a macOS `sample` of qemu-system-ppc by what the emulator was doing.

    host-sample-report.py qemu.sample.txt [top]

`sample` reports self time per symbol, which is the right measurement but the
wrong granularity: a hundred rows of helper_* and tlb_* say where cycles went
without saying which *job* spent them. This groups them into the jobs the
emulator actually has -- translating guest addresses, finding translation
blocks, running generated code, modelling the GPU, talking to Metal -- so the
output can be compared against the cost of changing any of them.

Grouping is by symbol name, so it is only as good as the patterns below; the
"other" bucket is printed with its largest members so anything big that the
patterns miss is visible rather than quietly absorbed.
"""
import re
import sys
from collections import Counter

# Order matters: the first pattern that matches wins, so specific groups come
# before the general ones they would otherwise be swallowed by.
GROUPS = [
    ('guest code (JIT)',   r'^(\?\?\?|0x[0-9a-f]+)$'),
    ('softmmu TLB',        r'tlb_|victim_tlb|memory_region_section|iotlb'),
    ('address translation', r'mmu|get_physical|ppc_xlate|hash|segment|bat_|'
                            r'ppc_hash32|ppc_radix'),
    ('TB lookup/chain',    r'tb_lookup|tb_find|tb_htable|tb_jmp|tb_link|'
                           r'tb_page|do_tb_phys|tb_flush|tb_gen|lookup_tb_ptr'),
    ('TB execution',       r'cpu_tb_exec|cpu_exec|tcg_qemu_tb_exec|cpu_loop'),
    ('translation (codegen)', r'tcg_gen|tcg_op|tcg_reg|tcg_out|translate|'
                              r'gen_intermediate|tcg_func|liveness|optimize'),
    ('float helpers',      r'float|softfloat|helper_f|round_|packFloat'),
    ('GPU device model',   r'ppc_mac_gpu|r200|pm4|gpu_|radeon'),
    ('Metal / display',    r'[Mm]etal|MTL|IOGPU|AGX|surface|dpy_|pixman|'
                           r'poweremu_display|gfx_'),
    ('other helpers',      r'^helper_',
     ),
    ('memory/alloc',       r'malloc|free|memcpy|memmove|memset|g_slice|'
                           r'nanov2|tiny_|szone'),
    ('locking/sync',       r'mutex|lock|cond_|pthread|futex|bql'),
    ('kernel/syscall',     r'^mach_|^__|syscall|kevent|semaphore|thread_'),
]


def classify(sym, binary):
    for name, pat in GROUPS:
        if re.search(pat, sym, re.I):
            return name
    return 'other'


def cpu_thread_selftime(text):
    """Self time per symbol for the busiest vCPU thread only.

    `sample` counts every thread, and most of QEMU's are blocked in select
    or a condition variable the whole time -- which is why a naive summary
    reports ~88% "syscall" and says nothing. The emulation cost lives in the
    thread running tcg_cpu_exec, so that subtree is measured on its own.

    Self time is a node's count minus its children's, computed from the tree
    rather than from the flat "top of stack" section, which cannot be
    filtered by thread.
    """
    lines = text.splitlines()
    start = None
    for i, line in enumerate(lines):
        if re.match(r'\s+\d+ Thread_', line):
            if start is not None:
                yield_block = lines[start:i]
                if any('tcg_cpu_exec' in x for x in yield_block[:12]):
                    return _selftime(yield_block)
            start = i
    if start is not None:
        block = lines[start:]
        if any('tcg_cpu_exec' in x for x in block[:12]):
            return _selftime(block)
    return None


def _selftime(block):
    nodes = []            # (indent, count, symbol)
    for line in block:
        m = re.match(r'^(\D*?)(\d+) (.+?)(?:\s+\(in ([^)]*)\))?'
                     r'(?:\s+\+ [\d,]+)?\s*(?:\[[^\]]*\])?\s*$', line)
        if not m:
            continue
        indent, count, sym = len(m.group(1)), int(m.group(2)), m.group(3)
        sym = re.sub(r'\s+\(in .*$', '', sym).strip()
        nodes.append((indent, count, sym))

    # Self time is a node's samples minus its direct children's. Children are
    # the nodes at the first deeper indent reached before the tree returns to
    # this node's level; grandchildren sit deeper still and must not be
    # subtracted twice.
    self_time = Counter()
    for i, (ind, cnt, sym) in enumerate(nodes):
        child, child_indent = 0, None
        for j in range(i + 1, len(nodes)):
            if nodes[j][0] <= ind:
                break
            if child_indent is None:
                child_indent = nodes[j][0]
            if nodes[j][0] == child_indent:
                child += nodes[j][1]
        self_time[sym] += max(cnt - child, 0)
    return self_time


def main(path, ntop=25):
    text = open(path, errors='replace').read()

    st = cpu_thread_selftime(text)
    if st:
        tot = sum(st.values()) or 1
        groups = Counter()
        for sym, n in st.items():
            groups[classify(sym, '')] += n
        print('=== vCPU thread only: %d samples ===\n' % tot)
        print('  %-24s %7s' % ('what the emulator was doing', 'share'))
        for g, n in groups.most_common():
            print('  %-24s %6.2f%%' % (g, 100.0 * n / tot))
        print('\n  top symbols (self time)')
        for sym, n in st.most_common(ntop):
            print('    %-46s %6.2f%%  [%s]'
                  % (sym[:46], 100.0 * n / tot, classify(sym, '')))
        print()


    # The section that reports self time. Its absence usually means the
    # sample was too short to have any symbol reach the >= 5 threshold.
    m = re.search(r'Sort by top of stack[^\n]*:\n(.*?)(\n\n|\nBinary Images)',
                  text, re.S)
    if not m:
        raise SystemExit('no "sort by top of stack" section: sample too short?')

    rows, total = [], 0
    for line in m.group(1).splitlines():
        r = re.match(r'\s*(.+?)\s+\(in ([^)]*)\)\s+(\d+)\s*$', line)
        if not r:
            r2 = re.match(r'\s*(\S+)\s+(\d+)\s*$', line)
            if not r2:
                continue
            sym, binary, n = r2.group(1), '?', int(r2.group(2))
        else:
            sym, binary, n = r.group(1), r.group(2), int(r.group(3))
        rows.append((sym, binary, n))
        total += n

    total = total or 1
    groups, members = Counter(), {}
    for sym, binary, n in rows:
        g = classify(sym, binary)
        groups[g] += n
        members.setdefault(g, Counter())[sym] += n

    print('%d samples attributed across %d symbols\n' % (total, len(rows)))
    print('  %-24s %7s' % ('what the emulator was doing', 'share'))
    for g, n in groups.most_common():
        print('  %-24s %6.2f%%' % (g, 100.0 * n / total))

    print('\n  top symbols')
    for sym, binary, n in sorted(rows, key=lambda r: -r[2])[:ntop]:
        short = sym if len(sym) <= 52 else sym[:49] + '...'
        print('    %-52s %6.2f%%  [%s]' % (short, 100.0 * n / total,
                                           classify(sym, binary)))

    if 'other' in members:
        print('\n  largest ungrouped (patterns to extend if these matter)')
        for sym, n in members['other'].most_common(8):
            print('    %-52s %6.2f%%' % (sym[:52], 100.0 * n / total))


if __name__ == '__main__':
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    main(sys.argv[1], int(sys.argv[2]) if len(sys.argv) > 2 else 25)
