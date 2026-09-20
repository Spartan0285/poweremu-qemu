#!/usr/bin/env python3
"""Attribute a pehot profile to the guest libraries it ran in.

    pehot-attribute.py prefix.N.raw vmmap.txt

pehot-report.py ranks guest code by address, which answers "where is the
time" but not "whose code is that".  This joins the profile to a `vmmap` of
the process taken while it was running, so the answer comes out as library
names.

That distinction is the whole point here: deciding whether to paravirtualise
OpenGL depends on how much guest time is inside GLEngine and the ATI driver,
and an address alone cannot say.

Every block is attributed, not just the top ones, so the percentages sum to
the whole window rather than to whatever the top-N happened to cover.  A
block outside every mapped region is reported as "(unmapped)" rather than
dropped: dyld moves things and a silently missing 20% would be the most
misleading number on the page.
"""
import re
import sys
from collections import Counter


def load_map(path):
    """[(start, end, name)] from a Tiger `vmmap` dump, executable regions."""
    regions = []
    for line in open(path, errors='replace'):
        m = re.search(r'\b([0-9a-f]{8})-([0-9a-f]{8})\b', line)
        if not m:
            continue
        start, end = int(m.group(1), 16), int(m.group(2), 16)
        if end <= start:
            continue
        # Only executable regions can contain code we translated; without
        # this, a library's __DATA would claim blocks from whatever was
        # mapped next to it.
        # Current *or* maximum permissions, not current alone. CFM/PEF code
        # -- which is what Warcraft III and CarbonLib are -- is mapped
        # "r--/rwx": not executable as it sits, executed through the CFM
        # runtime. Requiring x in the current permissions drops the game's
        # own code, and with it most of the profile.
        #
        # Including data regions this way costs nothing: a region only
        # receives weight if blocks actually ran in it, and none do in data.
        #
        # No \b before the field: it can begin with '-', which is not a word
        # character, so \b would fail and __PAGEZERO (---/---) would be kept
        # as though it held code.
        perms = re.search(r'(?:^|\s)([-r][-w][-x])/([-r][-w][-x])', line)
        if not perms or ('x' not in perms.group(1) and
                         'x' not in perms.group(2)):
            continue
        # The path may contain spaces -- "Warcraft III.app" does -- so it is
        # taken as everything after the SM= field rather than as one token.
        path_m = re.search(r'SM=\S+\s+(/.*?)\s*$', line)
        name = path_m.group(1) if path_m else None
        if not name:
            # Anonymous executable memory: JIT, or a region vmmap could not
            # name. Worth seeing separately rather than folded into a guess.
            tag = line.split()[0] if line.split() else 'anon'
            name = '(%s)' % tag
        regions.append((start, end, name))
    regions.sort()
    return regions


def lookup(regions, pc):
    lo, hi = 0, len(regions) - 1
    while lo <= hi:
        mid = (lo + hi) // 2
        s, e, n = regions[mid]
        if pc < s:
            hi = mid - 1
        elif pc >= e:
            lo = mid + 1
        else:
            return n
    return None


def main(raw, vmmap):
    regions = load_map(vmmap)
    if not regions:
        raise SystemExit('no regions parsed from %s' % vmmap)

    by_lib, by_lib_blocks = Counter(), Counter()
    total = 0
    with open(raw) as f:
        head = f.readline().split()
        window = float(head[1])
        for line in f:
            p = line.split()
            if len(p) < 3:
                continue
            pc, count, insns = int(p[0], 16), int(p[1]), int(p[2])
            weight = count * insns
            total += weight
            name = lookup(regions, pc) or '(unmapped)'
            by_lib[name] += weight
            by_lib_blocks[name] += 1

    total = total or 1
    print('window %.1f s, %.1f M guest instructions, %d mapped regions\n'
          % (window, total / 1e6, len(regions)))
    print('  %-58s %7s %8s' % ('library', 'share', 'blocks'))
    for name, w in by_lib.most_common(30):
        short = name if len(name) <= 58 else '...' + name[-55:]
        print('  %-58s %6.2f%% %8d' % (short, 100.0 * w / total,
                                       by_lib_blocks[name]))

    # The number this was built to produce.
    gl = sum(w for n, w in by_lib.items()
             if re.search(r'GLEngine|OpenGL|GLDriver|GLRenderer|ATIRadeon',
                          n, re.I))
    print('\n  OpenGL + GPU driver total: %.2f%% of guest instructions'
          % (100.0 * gl / total))
    unmapped = by_lib.get('(unmapped)', 0)
    if unmapped:
        print('  (unmapped: %.2f%% -- vmmap taken at a different moment?)'
              % (100.0 * unmapped / total))


if __name__ == '__main__':
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    main(sys.argv[1], sys.argv[2])
