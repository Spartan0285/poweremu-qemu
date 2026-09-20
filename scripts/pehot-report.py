#!/usr/bin/env python3
"""Rank a pehot raw dump (contrib/plugins/pehot.c) for 32-bit PowerPC guests.

    pehot-report.py prefix.N.raw [top-blocks]

Prints the window summary, the instruction-class and mnemonic mix, the
busiest 64 KB regions, retranslated blocks and the top blocks, decoded.
"""
import sys
from collections import Counter, defaultdict

PRIMARY = {
    3: "twi", 7: "mulli", 8: "subfic", 10: "cmpli", 11: "cmpi", 12: "addic",
    13: "addic.", 14: "addi", 15: "addis", 16: "bc", 17: "sc", 18: "b",
    20: "rlwimi", 21: "rlwinm", 23: "rlwnm", 24: "ori", 25: "oris",
    26: "xori", 27: "xoris", 28: "andi.", 29: "andis.", 32: "lwz",
    33: "lwzu", 34: "lbz", 35: "lbzu", 36: "stw", 37: "stwu", 38: "stb",
    39: "stbu", 40: "lhz", 41: "lhzu", 42: "lha", 43: "lhau", 44: "sth",
    45: "sthu", 46: "lmw", 47: "stmw", 48: "lfs", 49: "lfsu", 50: "lfd",
    51: "lfdu", 52: "stfs", 53: "stfsu", 54: "stfd", 55: "stfdu",
}
OP19 = {0: "mcrf", 16: "bclr", 33: "crnor", 50: "rfi", 129: "crandc",
        150: "isync", 193: "crxor", 225: "crnand", 257: "crand",
        289: "creqv", 417: "crorc", 449: "cror", 528: "bcctr"}
OP31 = {
    0: "cmp", 4: "tw", 6: "lvsl", 7: "lvebx", 8: "subfc", 10: "addc",
    11: "mulhwu", 19: "mfcr", 20: "lwarx", 23: "lwzx", 24: "slw",
    26: "cntlzw", 28: "and", 32: "cmpl", 38: "lvsr", 39: "lvehx",
    40: "subf", 54: "dcbst", 55: "lwzux", 60: "andc", 71: "lvewx",
    75: "mulhw", 83: "mfmsr", 86: "dcbf", 87: "lbzx", 103: "lvx",
    104: "neg", 119: "lbzux", 124: "nor", 135: "stvebx", 136: "subfe",
    138: "adde", 144: "mtcrf", 146: "mtmsr", 150: "stwcx.", 151: "stwx",
    167: "stvehx", 183: "stwux", 199: "stvewx", 200: "subfze",
    202: "addze", 210: "mtsr", 215: "stbx", 231: "stvx", 232: "subfme",
    234: "addme", 235: "mullw", 242: "mtsrin", 246: "dcbtst", 247: "stbux",
    266: "add", 278: "dcbt", 279: "lhzx", 284: "eqv", 306: "tlbie",
    311: "lhzux", 316: "xor", 339: "mfspr", 342: "dst", 343: "lhax",
    359: "lvxl", 371: "mftb", 375: "lhaux", 407: "sthx", 412: "orc",
    439: "sthux", 444: "or", 459: "divwu", 467: "mtspr", 470: "dcbi",
    476: "nand", 487: "stvxl", 491: "divw", 512: "mcrxr", 533: "lswx",
    534: "lwbrx", 535: "lfsx", 536: "srw", 566: "tlbsync", 567: "lfsux",
    595: "mfsr", 597: "lswi", 598: "sync", 599: "lfdx", 631: "lfdux",
    659: "mfsrin", 661: "stswx", 662: "stwbrx", 663: "stfsx", 695: "stfsux",
    725: "stswi", 727: "stfdx", 759: "stfdux", 790: "lhbrx", 792: "sraw",
    822: "dss", 824: "srawi", 854: "eieio", 918: "sthbrx", 922: "extsh",
    954: "extsb", 982: "icbi", 983: "stfiwx", 1014: "dcbz",
}
OP59 = {18: "fdivs", 20: "fsubs", 21: "fadds", 22: "fsqrts", 24: "fres",
        25: "fmuls", 28: "fmsubs", 29: "fmadds", 30: "fnmsubs", 31: "fnmadds"}
OP63A = {18: "fdiv", 20: "fsub", 21: "fadd", 22: "fsqrt", 23: "fsel",
         25: "fmul", 26: "frsqrte", 28: "fmsub", 29: "fmadd", 30: "fnmsub",
         31: "fnmadd"}
OP63X = {0: "fcmpu", 12: "frsp", 14: "fctiw", 15: "fctiwz", 32: "fcmpo",
         38: "mtfsb1", 40: "fneg", 64: "mcrfs", 70: "mtfsb0", 72: "fmr",
         134: "mtfsfi", 136: "fnabs", 264: "fabs", 583: "mffs", 711: "mtfsf"}
OP4A = {32: "vmhaddshs", 33: "vmhraddshs", 34: "vmladduhm", 36: "vmsumubm",
        37: "vmsummbm", 38: "vmsumuhm", 39: "vmsumuhs", 40: "vmsumshm",
        41: "vmsumshs", 42: "vsel", 43: "vperm", 44: "vsldoi", 46: "vmaddfp",
        47: "vnmsubfp"}
OP4X = {0: "vaddubm", 10: "vaddfp", 12: "vmrghb", 64: "vadduhm",
        74: "vsubfp", 76: "vmrghh", 128: "vadduwm", 140: "vmrghw",
        266: "vrefp", 268: "vmrglb", 330: "vrsqrtefp", 332: "vmrglh",
        396: "vmrglw", 524: "vspltb", 588: "vsplth", 652: "vspltw",
        778: "vcfux", 780: "vspltisb", 842: "vcfsx", 844: "vspltish",
        906: "vctuxs", 908: "vspltisw", 970: "vctsxs", 1028: "vand",
        1034: "vmaxfp", 1092: "vandc", 1098: "vminfp", 1156: "vor",
        1220: "vxor", 1284: "vnor", 1540: "mfvscr", 1604: "mtvscr"}

SPR = {1: "xer", 8: "lr", 9: "ctr", 268: "tbl", 269: "tbu", 256: "vrsave"}


def mnemonic(w):
    op = w >> 26
    if op == 19:
        return OP19.get((w >> 1) & 0x3ff, "op19_%d" % ((w >> 1) & 0x3ff))
    if op == 31:
        xo = (w >> 1) & 0x3ff
        m = OP31.get(xo) or OP31.get(xo & 0x1ff)
        if m is None:
            return "op31_%d" % xo
        if m in ("mfspr", "mtspr"):
            n = ((w >> 16) & 0x1f) | (((w >> 11) & 0x1f) << 5)
            return "%s %s" % (m, SPR.get(n, n))
        return m + ("." if (w & 1) and not m.endswith(".") else "")
    if op == 59:
        return OP59.get((w >> 1) & 0x1f, "op59_%d" % ((w >> 1) & 0x1f))
    if op == 63:
        a = (w >> 1) & 0x1f
        if a >= 16 and a in OP63A:
            return OP63A[a]
        xo = (w >> 1) & 0x3ff
        return OP63X.get(xo, "op63_%d" % xo)
    if op == 4:
        if (w & 0x3f) >= 32 and (w & 0x3f) in OP4A:
            return OP4A[w & 0x3f]
        xo = w & 0x7ff
        if (xo & 0x3ff) in (198, 454, 710, 966, 70, 134, 6, 390, 518, 582, 646, 774, 838, 902):
            return "vcmp%d" % (xo & 0x3ff)
        return OP4X.get(xo, "v_%d" % xo)
    return PRIMARY.get(op, "op%d" % op)


def klass(m):
    if m.startswith(("lf", "stf")):
        return "fp load/store"
    if m[0] == "f" or m in ("mffs", "mtfsf", "mtfsfi", "mtfsb0", "mtfsb1", "mcrfs"):
        return "fp arith"
    if m[0] == "v" or m.startswith(("lv", "stv", "mfvscr", "mtvscr", "dst", "dss")):
        return "altivec"
    if m.startswith(("lwarx", "stwcx", "sync", "isync", "eieio", "dcb", "icbi", "tlb", "rfi", "sc", "mtmsr", "mfmsr", "mtsr", "mfsr")):
        return "system/sync"
    if m.startswith(("mfspr", "mtspr", "mftb", "mfcr", "mtcrf", "mcrf", "cr", "mcrxr")):
        return "spr/cr"
    if m.startswith(("l", "st")):
        return "int load/store"
    if m.startswith(("b", "tw")):
        return "branch"
    if m.startswith(("mul", "div")):
        return "int mul/div"
    return "int alu"


def operands(w, pc):
    op = w >> 26
    rt, ra, rb = (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31
    d = w & 0xffff
    if d & 0x8000:
        d -= 0x10000
    if op == 18:
        li = w & 0x03fffffc
        if li & 0x02000000:
            li -= 0x04000000
        t = li if w & 2 else pc + li
        return "0x%x%s" % (t & 0xffffffff, " (call)" if w & 1 else "")
    if op == 16:
        bd = w & 0xfffc
        if bd & 0x8000:
            bd -= 0x10000
        t = bd if w & 2 else pc + bd
        return "bo=%d bi=%d 0x%x" % (rt, ra, t & 0xffffffff)
    if 32 <= op <= 55:
        fr = "f" if op >= 48 else "r"
        return "%s%d, %d(r%d)" % (fr, rt, d, ra)
    if op in (14, 15, 7, 8, 12, 13, 10, 11):
        return "r%d, r%d, %d" % (rt, ra, d)
    if op in (24, 25, 26, 27, 28, 29):
        return "r%d, r%d, 0x%x" % (ra, rt, w & 0xffff)
    if op in (20, 21, 23):
        return "r%d, r%d, %d,%d,%d" % (ra, rt, rb, (w >> 6) & 31, (w >> 1) & 31)
    if op in (59, 63):
        return "f%d, f%d, f%d, f%d" % (rt, ra, rb, (w >> 6) & 31)
    if op == 4:
        return "v%d, v%d, v%d" % (rt, ra, rb)
    if op == 31:
        return "r%d, r%d, r%d" % (rt, ra, rb)
    return ""


def main():
    path = sys.argv[1]
    ntop = int(sys.argv[2]) if len(sys.argv) > 2 else 60
    blocks = []
    with open(path) as f:
        head = f.readline().split()
        window, trans = float(head[1]), int(head[3])
        for line in f:
            p = line.split()
            words = [int(x, 16) for x in p[4].split(",")] if len(p) > 4 else []
            blocks.append((int(p[0], 16), int(p[1]), int(p[2]), int(p[3]), words))

    total = sum(c * n for _, c, n, _, _ in blocks) or 1
    runs = sum(c for _, c, _, _, _ in blocks)
    print("window %.1f s: %.1f M guest insns (%.0f M/s), %.1f M block runs "
          "(%.1f insns/block), %d translations, %d blocks" % (
              window, total / 1e6, total / window / 1e6, runs / 1e6,
              total / max(runs, 1), trans, len(blocks)))

    mn, kl = Counter(), Counter()
    regions = Counter()
    for pc, c, n, _, words in blocks:
        regions[pc & ~0xffff] += c * n
        for w in words:
            m = mnemonic(w)
            mn[m] += c
            kl[klass(m)] += c
        if n > len(words) and words:        # extrapolate the tail
            kl["(unsampled tail)"] += c * (n - len(words))

    print("\n== instruction classes ==")
    for k, v in kl.most_common():
        print("  %-18s %5.1f%%" % (k, 100.0 * v / total))
    print("\n== mnemonics ==")
    for k, v in mn.most_common(50):
        print("  %-16s %5.2f%%" % (k, 100.0 * v / total))
    print("\n== 64 KB regions ==")
    for k, v in regions.most_common(40):
        print("  %08x %5.2f%%" % (k, 100.0 * v / total))
    print("\n== retranslated blocks ==")
    for pc, c, n, t, _ in sorted(blocks, key=lambda b: -b[3])[:20]:
        if t > 2:
            print("  %08x x%d (%d insns, %d runs)" % (pc, t, n, c))
    print("\n== top blocks ==")
    for i, (pc, c, n, t, words) in enumerate(
            sorted(blocks, key=lambda b: -b[1] * b[2])[:ntop]):
        print("\n#%d %08x %5.2f%% runs %d insns %d trans %d" % (
            i + 1, pc, 100.0 * c * n / total, c, n, t))
        for j, w in enumerate(words):
            a = pc + 4 * j
            print("  %08x  %08x  %-10s %s" % (a, w, mnemonic(w), operands(w, a)))


main()
