# hakios-mali

The beginning of a Metal back end for the X6826B's Mali-G52 (Bifrost, architecture v7) that writes the GPU's job
descriptors itself: no Gallium, no Panfrost driver, no Mesa at run time. It talks to the kernel's own GPU service,
HakiosMali (`kernel/overlay/iokit/Kernel/HakiosMali.cpp`, brought up by `bsd/dev/mtk_mali.c` with boot-arg `mali=1`),
in the requests of Linux's panfrost DRM driver: buffers made, given a GPU address and mapped, job chains submitted to
the fragment slot or the other, waits by the job's number. It follows the method of `metal/` in xnu-kernel-qcom, which
drives the Adreno the same way: every descriptor checked on this machine against a fake GPU, every refusal a sentence
that names what is wrong, and a probe that measures on the phone what the fake cannot.

## What there is

| Part | What it does | Checked by |
|---|---|---|
| `v7.h`, `v7.c` | The descriptors: Job Header, Write Value job, Fragment job, Framebuffer (Parameters, Padding), Render Target, Tiler Context and Heap, the sample positions, the tile size and the clear colour. Packed and unpacked by Mesa's own `genxml/v7_pack.h`, which the build makes from Mesa's `v7.xml` with Mesa's `gen_pack.py`: no field position is written by hand. What each one is filled with is what `pan_desc.c` (`GENX(pan_emit_fbd)`, `pan_prepare_rt`, `GENX(pan_select_tile_size)`), `pan_jm.c` (`GENX(jm_emit_fragment_job)`, `jm_emit_tiler_desc`), `pan_clear.c` and `pan_samples.c` write for the same frame. The readers refuse a bit outside every field with the generated unpacker's own sentence. | `tests/host_test.cpp`: every word of the 64x64 RGBA8 frame against literals worked out by hand from `v7.xml`, field by field |
| `mali.hpp`, `gpu.cpp` | `Gpu` and `Bo`: parameters, buffers (create, GPU address, mapping, close), submissions with the buffers they use, waits (HakiosMali's request 0x7d), the names of the GPU's exception codes | the fake GPU; on the phone, `mali-probe` stages `buffer` and `write-value` |
| `iokit_transport.cpp` | the only part that knows IOKit: HakiosMali's user client, its client memory per handle | the phone |
| `pass.hpp`, `pass.cpp` | `Device` (the GPU checked to be a G52, product 0x7212; the sample positions; the tiler heap), `ClearPass` (one colour target, load action Clear, nothing drawn: one Fragment job over a Framebuffer Descriptor and its Render Target, the frame Panfrost submits for a clear-only batch -- on v7 without a Tiler Context, which `jm_emit_tiler_desc` makes only for a draw; optionally with one), `WriteValue` | the fake GPU, which carries the frame out; the refusals by sentence; on the phone, `mali-probe` |
| `fake_gpu.hpp`, `fake_gpu.cpp` | HakiosMali's requests answered in the process and the job chains run as strictly as the Mali takes them: every job through Mesa's unpackers, a job outside every buffer or on the wrong slot, a framebuffer pointer whose tag disagrees with its descriptor, tiles past the frame (TILE_RANGE_FAULT), a target's rows outside its buffer, an effective tile size or colour buffer allocation the hardware does not take -- each stops the chain with its sentence and the Exception Status the GPU would write. It carries out Write Value and the cleared frame (only for targets with Clean Pixel Write Enable: a tile nothing was drawn into is "clean", and the GPU does not write it back otherwise) | itself, by `host_test`'s fifteen mutation twins: an accepted frame changed in one place in GPU memory, each refused with its sentence and its code |
| `probe.cpp` | `mali-probe`: the stages below against HakiosMali (or `--fake`), each a line `mali-probe: STAGE PASS|FAIL ...` on the console and the same with `!!` through `kern.hakios.say`, kept for the host | the phone |

| `tools/bishaders.c`, `tools/mk-shaders.sh`, `shaders_v7.h` | The first triangle's shaders, built as NIR and compiled on the Mac by Mesa's Bifrost compiler for the phone's 0x7402 (no IDVS; the viewport transform's scale and offset made constants of a 64x64 viewport), with their Renderer State words as Panfrost prepares them (`pan_shader_prepare_rsd`; for the fragment shader merged as `panfrost_emit_frag_shader` merges it for this draw's fixed state) and the target's Blend descriptor (`panfrost_emit_blend`). The phone's library carries the header and no compiler | on the phone, `mali-probe` stage `triangle` |
| `TrianglePass` (`pass.hpp`), the draw packers of `v7.c` | One draw as Panfrost submits it on v7 without IDVS: a vertex job (Compute Job layout) and a tiler job depending on it on slot 1, then the Fragment job on slot 0 over a framebuffer that names the Tiler Context; Attribute and Attribute Buffer tables, the position varying (`MALI_SNAP_4`), Viewport, Local Storage, all through Mesa's packers | the fake GPU checks the jobs (it does not rasterize); on the phone, `triangle` |

## What the phone said

Runs 500 to 502, the 27 release with `mali=1 metal=1`, `mali-probe` as launchd's first job (`userland/rc.jobs.maliprobe`):

- The GPU is a **Mali-G52 r1**: product 0x7402 (not 0x7212, which run 500's probe asked for and refused by name), r1p0,
  two shader cores, `TILER_FEATURES` 0x209 (2^9-byte bins, **two** hierarchy levels), `THREAD_TLS_ALLOC` 0x400.
- `clear64`, the first milestone: one Fragment job of ours cleared the 64x64 RGBA8 target, 4096 of 4096 pixels
  `33 66 99 cc`, every canary byte after it kept, the job DONE (runs 501 and 502).
- Also as the fake GPU says: Write Value, a frame with a Tiler Context, 48x20 at an offset with padded rows (nothing
  of the padding written), BGRA8, the panel's 750x1624 (about 1 ms), no write without Clean Pixel Write Enable;
  a 64x64 clear and its wait take about 90 us.
- Where the fake GPU was wrong, and was changed: the Fragment job's tile bounds do **not** limit what is written (a job
  over tiles (1,1)-(2,2) wrote the whole frame); the framebuffer's Bound Min/Max does, to the pixel, also off the tile
  grid ((5,7)-(40,50)).
- `triangle` (run 502), the first draw: vertex, tiler and fragment jobs DONE, 420 of 420 pixels more than a pixel
  inside red, 3304 of 3304 more than a pixel outside the black clear, the red where the viewport puts it (wide at row
  20, the apex at row 48), the vertex job's position for vertex 0 (16, 16, 0.5, 1).

Not yet, refused by name where it would be asked for: shaders other than the triangle's, textures, uniforms and
push constants, varyings other than the position, depth and stencil, MSAA, AFBC and tiled targets, formats other
than RGBA8Unorm and BGRA8Unorm, sRGB, more than one target per frame, a pre-frame shader (load action Load). The Metal
objects over this (an `MTLDevice` plug-in, as `userland/src/hakiosmetal.m` is over Mesa) are not written.

## mali-probe's stages

1. `open`: HakiosMali's user client (waits up to 60 s for the service: mtk_mali.c starts it once the GPU is up).
2. `params`: the GPU's product (0x7212 for the G52), revision, shader cores, TILER_FEATURES (hierarchy levels), TLS.
3. `device`: the same checked, the sample positions uploaded.
4. `buffer`: a 64 KB buffer, its GPU address below 4 GB, every word written and read back through the mapping.
5. `write-value`: the smallest job, a 32-bit write, and the job's Exception Status DONE (1).
6. `clear64` **(the first milestone)**: one Fragment job clears a 64x64 RGBA8Unorm target to (0.2, 0.4, 0.6, 0.8),
   which is 33 66 99 cc in memory; every pixel compared, every other byte of the buffer a canary that must stay.
7. `clear64-tiler`: the same frame with a Tiler Context and heap, as a frame with draws has them.
8. `clear-48x20`: a target whose size is not a whole number of tiles, at an offset, with row padding (the GPU writes
   up to the frame's width and height only, and nothing of the padding).
9. `clear64-bgra`: BGRA8Unorm, the render target's swizzle (B first in memory).
10. `clear64-tiles`: a Fragment job over tiles (1,1)-(2,2) only: the whole frame is written all the same.
11. `clear64-fbbounds`, `clear64-both`, `clear64-fbbounds-odd`: the framebuffer's Bound Min/Max, alone, with the
    job's tiles, and off the tile grid: what is written is exactly those pixels.
12. `clear-750x1624`: a frame of the panel's size.
13. `clear64-noclean`: Clean Pixel Write Enable off on a frame without primitives: nothing written.
14. `triangle`: the first draw (TrianglePass), judged pixel by pixel away from the edges; the target is also printed
    as `mali-picture` lines (`tools/probe-picture.py LOG OUT.png triangle`).
15. `timing`: twenty clears of 64x64, a submission and its wait each.

The expected pixel is computed in the probe from the colour alone (round to nearest even of c x 255, as `pan_clear.c`
rounds), not by the library.

## Building and running

On this machine (macOS or Linux), against the fake GPU:

```sh
make -C userland/src/mali test MESA=$PWD/kernel/deps/build/mesa/mesa-25.0.7
```

For the phone (arm64e, the 27 release), then the small image with the probe and a job list that runs it:

```sh
system27/mtk27.sh mali                        # system27/userland/mali27.sh: build/ios27/obj/mali/mali-probe
RC_JOBS=userland/rc.jobs.maliprobe system27/mtk27.sh root
```

`root26-sys.sh` puts `mali-probe` into `/hakios/bin` when `MALI27` names its directory (`mtk27.sh root` does so when
the `mali` step has run). Booted with `mali=1`, the job list runs it as soon as HakiosMali is there and again 90 s
later with `--dump` (every frame's descriptors printed by Mesa's printers). Its verdicts are in the run's log, and
the `!!mali-probe:` lines at its end, in what the kernel kept for the host.
