# PowerEmu QEMU

The emulator behind PowerEmu: QEMU 10.0.2 as patched by
[UTM](https://github.com/utmapp/qemu) (see the first commit for the exact
upstream revision), plus PowerEmu's work to run Mac OS X 10.4/10.5 on the
`mac99` PowerPC machine with accelerated graphics:

| Area | Files | What |
|---|---|---|
| GPU | `hw/display/ppc_mac_gpu*`, `include/hw/display/ppc_mac_gpu.h` | ATI Radeon 9000 (RV280/R200) for Mac OS X's ATI drivers; 2D engine, R200 3D pipeline rendered with Metal, AGP/GART, EDID, hardware cursor |
| AGP | `hw/pci-host/uninorth.c` | AGP capability and GART on the uni-north bridge (Quartz Extreme) |
| Audio | `hw/audio/screamer.c` | Real-time DMA pacing, DMA-based position reporting |
| USB | `hw/usb/hcd-ohci.c` | No livelock when the host is overloaded |
| CPU | `accel/tcg/*`, `target/ppc/*` | Bigger TLB floor, vCPU QoS, inline lmw/stmw, host-FPU fast path |
| UI | `ui/cocoa.m` | Layer presentation, hardware cursor, full-panel/below-notch fullscreen |
| Firmware | `pc-bios/openbios-ppc`, `pc-bios/ppc-ndrvloader` | OpenBIOS patched for the RV280; NDRV loader |

The display driver the guest loads (`qemu_vga_hwc.ndrv`, the QEMU VGA NDRV
patched for the hardware cursor and extra modes) and the launcher live in the
PowerEmu repository.

## Building (Apple silicon, Homebrew)

    brew install meson ninja pkgconf glib pixman libslirp libusb sdl2 gnutls jpeg-turbo libpng zstd
    mkdir build && cd build
    ../configure --target-list=ppc-softmmu --disable-docs --enable-plugins \
        -Dqom_cast_debug=false -Doptimization=3 -Db_lto=true
    ninja qemu-system-ppc

`qemu-system-ppc` is signed with `accel/hvf/entitlements.plist` by the build;
copy it with copy+rename (not in place) or macOS kills the next launch for an
invalid code signature.

## Useful switches

`PPCGPU_ASYNC_FENCE` (0 sync, 1 deferred, 2 hybrid), `PPCGPU_R200_DIRECT=0`,
`PPCGPU_SEQ_LOG=1`, `PPCGPU_DEBUG_LOG=1`, `PPC_STRICT_FP=1`,
`PPC_FAULT_WATCH=lo-hi`, `SCREAMER_DEBUG=1`, `QEMU_PPC_NDRV=path`.

## License

GPL-2.0-or-later, like QEMU (see `COPYING`).
