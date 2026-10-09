#!/usr/bin/env python3
"""Bootless Radeon PM4/fill checks and R350 reset checks against built QEMU."""
import argparse
import socket
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--qemu', required=True)
parser.add_argument('--firmware', required=True, help='Directory containing openbios-ppc')
parser.add_argument('--card', choices=['r350', 'r200'], default='r350')
args = parser.parse_args()
qemu = str(Path(args.qemu).resolve())
d = tempfile.TemporaryDirectory(prefix='pe-pm4-')
root = Path(d.name)
log = (root / 'qemu.log').open('w')
p = subprocess.Popen([
    qemu, '-L', str(Path(args.firmware).resolve()),
    '-machine', 'mac99', '-m', '64', '-cpu', '7400', '-nodefaults',
    '-vga', 'none', '-audio', 'none', '-display', 'none', '-S',
    '-device', ('ppc-mac-r350-probe' if args.card == 'r350' else 'ppc-mac-gpu') +
               ',id=gpu0,addr=e,vgamem_mb=32',
    '-qtest', f'unix:{root}/test.sock,server=on,wait=off',
], stdout=log, stderr=log)
try:
    for _ in range(100):
        if (root/'test.sock').exists() and p.poll() is None:
            time.sleep(0.05)
            break
        if p.poll() is not None:
            raise RuntimeError((root / 'qemu.log').read_text())
        time.sleep(.05)
    s = socket.socket(socket.AF_UNIX)
    s.settimeout(10)
    s.connect(str(root / 'test.sock'))
    f = s.makefile('rwb')
    def qt(cmd):
        f.write((cmd + '\n').encode())
        f.flush()
        while True:
            line = f.readline()
            if not line:
                raise RuntimeError('QEMU closed the qtest connection')
            r = line.decode().strip()
            if r.startswith('OK'):
                return r
            if r.startswith('FAIL'):
                raise RuntimeError(r)
    def wl(a, v):
        return qt(f'writel {a:#x} {v:#x}')
    def rl(a):
        return int(qt(f'readl {a:#x}').split()[1], 0)
    def swap(v):
        return int.from_bytes(v.to_bytes(4, 'little'), 'big')
    # UniNorth's config and Radeon register windows are little-endian.
    wl(0xf2800000,swap(0x80007000))
    identity = 0x4e481002 if args.card == 'r350' else 0x59601002
    assert swap(rl(0xf2c00000)) == identity, 'Radeon PCI identity'
    # qtest writes use PPC byte order; swap the LE register value.
    def cfg(reg, v):
        wl(0xf2800000, swap(0x80007000 | reg))
        wl(0xf2c00000, swap(v))
    cfg(0x10,0x88000000)
    cfg(0x18,0x90000000)
    cfg(0x04,7)
    def mw(reg, v):
        wl(0x90000000 + reg, swap(v))
    def mr(reg):
        return swap(rl(0x90000000 + reg))
    mw(0x7d0,0x10000000)
    assert mr(0x7d0)==0x10000000
    print('PASS real CP_ME_CNTL readback')
    # A reset initializes the ring pointers while the CP is disabled. The old
    # contents of the ring must not execute during this pointer initialization.
    qt('write 0x2000 4 0x01300000')  # LE PTE -> physical RAM page 0x3000
    if args.card == 'r350':
        words=[0x14c//4,0x17ff1000,0x80000000,0x80000000]
        payload=b''.join(struct.pack('>I',v) for v in words)
        qt(f'write 0x3000 {len(payload)} 0x{payload.hex()}')
        mw(0x1d0,1)
        mw(0x1d8,0x2000)
        mw(0x1dc,0x10000000)
        mw(0x1e0,0x1000ffff)
        mw(0x14c,0)
        mw(0x7d0,0)
        mw(0x740,0)
        mw(0x700,0x10000000)
        mw(0x704,0x80000001)
        mw(0x71c,2)
        mw(0x714,2)
        assert mr(0x14c)==0, 'disabled CP executed stale ring contents'
        assert mr(0x710)==2, 'writable RPTR was not transferred'
        print('PASS disabled ring reset does not execute stale commands')
        mw(0x704,1)
        mw(0x740,0x40000000)
        mw(0x714,0)
        assert mr(0x14c)==0 and mr(0x710)==0
        mw(0x714,2)
        assert mr(0x14c)==0x17ff1000 and mr(0x710)==2
        print('PASS enabled ring executes newly submitted commands across wrap')
        mw(0x170c,1)
        mw(0x170c,0)
        assert mr(0x710)==0 and mr(0x714)==0
        print('PASS R300 CP soft reset clears both queue pointers')
    else:
        # Preserve the older R200 CP reset behavior: this comparison checks
        # PAINT_MULTI, while disabled-CP reset checks remain R350-specific.
        mw(0x1d0,1)
        mw(0x1d8,0x2000)
        mw(0x1dc,0x10000000)
        mw(0x1e0,0x1000ffff)
        mw(0x700,0x10000000)
    # A clipped PAINT_MULTI clear must preserve byte order, adjacent pixels
    # and row padding. Exercise the packet path rather than calling the row
    # helper directly, so clip/format decode and dirty notifications run too.
    def paint(offset, pitch, datatype, color, x, y, width, height, clip=None):
        gmc = (0xf0 << 16) | (datatype << 8) | 0xd0 | 2
        body = [gmc, (pitch << 16) | (offset >> 10)]
        if clip:
            body[0] |= 8
            x0,y0,x1,y1 = clip
            body += [(y0 << 16) | x0, (y1 << 16) | x1]
        body += [color, (x << 16) | y, (width << 16) | height]
        packet = [0xc0009a00 | ((len(body)-1) << 16)] + body
        data = b''.join(struct.pack('>I',v) for v in packet)
        mw(0x740,0)
        mw(0x714,0)
        mw(0x710,0)
        mw(0x704,5)
        qt(f'write 0x3000 {len(data)} 0x{data.hex()}')
        mw(0x7d0,0)
        mw(0x740,0x40000000)
        mw(0x714,len(packet))
        assert mr(0x710)==len(packet), 'paint packet did not complete'
    def read_bytes(address, size):
        return bytes.fromhex(qt(f'read {address:#x} {size}').split()[1][2:])
    offset,pitch,rows = 0x10000,128,8
    sentinel = bytes([0xa5]) * (pitch * rows)
    qt(f'write {0x88000000+offset:#x} {len(sentinel)} 0x{sentinel.hex()}')
    paint(offset,pitch,6,0x12345678,1,1,12,6,(3,2,9,5))
    expected = bytearray(sentinel)
    for y in range(2,5):
        expected[y*pitch+3*4:y*pitch+9*4] = bytes.fromhex('12345678') * 6
    assert read_bytes(0x88000000+offset,len(sentinel))==expected, 'clipped 32-bit fill changed pixels or padding'
    print('PASS PM4 row fill preserves clipping, big-endian color and padding')
    paint(offset,pitch,4,0x12345678,2,6,5,1)
    expected[6*pitch+2*2:6*pitch+7*2] = bytes.fromhex('5678') * 5
    assert read_bytes(0x88000000+offset,len(sentinel))==expected, '16-bit fill fallback changed pixels or padding'
    print('PASS PM4 16-bit fill retains pixel fallback')
finally:
    p.terminate()
    try:
        p.wait(timeout=5)
    except subprocess.TimeoutExpired:
        p.kill()
        p.wait()
    log.close()
    if sys.exc_info()[0]:
        print((root/'qemu.log').read_text()[-1600:], file=sys.stderr)
    d.cleanup()
