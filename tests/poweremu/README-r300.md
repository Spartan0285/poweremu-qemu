# R300/R350 experimental renderer tests

This is an isolated bring-up path, **not a working Radeon 9800 or a Final Cut Studio compatibility claim**. The production Radeon 9200 path remains separate. The experimental device is not exposed in the app configuration.

## Host tests

On a Mac with Metal:

```sh
tests/poweremu/run-r300-metal-test.sh
tests/poweremu/run-r300-replay-test.sh /path/to/captured/fixture
```

Both scripts build temporary executables and run on the real host GPU. They do not open guest disks. The replay fixture contains `draw1.json` (captured registers, PVS bank and immediate packet) and `texture0.bin` (the first referenced guest texture).

The shader tests validate native R300 vertex and fragment instruction translation, texture sampling, simultaneous RGB/alpha operations, saturation, independent source banks, real Leopard shader words, and all eight alpha comparisons. Negative cases reject unsupported encodings and incomplete state. Data tests cover S16E7 constants, immediate vertex stream layouts, swizzles, normalization and bounds.

The replay suite uses an actual Leopard draw's geometry, shaders, constants and texture. It compares the entire 1024×768 destination against independent expected pixels, including untouched pixels. Additional cases alter constants and textures while reusing cached pipelines, check scissor and color masks, and confirm that rejected shaders leave the destination unchanged. This validates the linear rendering contract, not physical R350 tiling or complete guest presentation.

## Current implementation limits

- Vertex programs: straight-line DOT/MUL/ADD/MAD subset, validated source components and constants; no branches.
- Fragment programs: one node, TEX/TXP and MAD subset, independent RGB/alpha operands, temporary registers, swizzles, saturation and alpha testing. Unsupported instructions are rejected.
- Metal backend: synchronous RGBA8 render, GPU completion and readback before modifying the caller's target; bounded pipeline cache.
- Experimental device: immediate triangle lists/quads, one verified interpolator route, normalized RGBA8 textures, scissor and channel masks. No blending, depth/stencil, broad format coverage or complete primitive support yet.
- Target buffers follow the emulator's existing linear 2D storage even when the guest requests macrotiled buffers. Physical R350 tiling is **not implemented**. This is an explicit experiment requiring mixed 2D/3D and scanout validation.

## Isolated guest experiment

The experimental QEMU properties are:

```text
ppc-mac-r350-probe,x-r350-bridge-aic=on,x-r350-linear-render=on,x-r350-shader-snapshots=on
```

Use a disposable snapshot and compatible probe firmware. All properties default off. The device cannot migrate. `x-r350-capture-dir` optionally captures VRAM at draw 1; shader/packet diagnostics capture the first draws and the first occurrence of distinct rejection reasons with bounded output.

On 2026-09-29, Leopard's stock driver reached Finder and the first textured draws completed through Metal. Later unsupported drawing paths still produced a visibly corrupt desktop. Hardware-acceleration labels in System Profiler reflect driver attachment, **not verified rendering support**. Do not use this path for real documents or installations yet.
