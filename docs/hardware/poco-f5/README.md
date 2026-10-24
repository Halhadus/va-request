# POCO F5

Tests ran on Alpine aarch64 with kernel `7.2.8` and Mesa 26.2.5 (Freedreno FD730). The recorded builds used commit `6027a1e8e6ce34da17345220827e7d4bd5806a85` with local changes.

## Test results

| Area | Latest result |
| --- | --- |
| Native musl build and unit tests | `run-mbmgr47q`: all 59 tests passed without compiler aliases or skipped tests |
| Annex-B reconstruction | Six H.264/HEVC streams, 48 frames identical to the originals after software decoding |
| Decoder discovery | `run-w43vz9fm`: H.264 Constrained Baseline/Main/High and HEVC Main advertised |
| Hardware decode | Not tested: FFmpeg was missing from the hardware run |
| Renderer | `run-w43vz9fm`: 19 PASS / 2 FAIL in each of four export/storage modes |
| Failing formats | Composed I420/YV12 EGL imports returned `0x3003` with pitches 64/32/32 |

The latest run contains unit tests only. It does not test Iris decoding, native decoder DMA-BUF export or EGL rendering. The hardware run's 12 failures comprise eight I420/YV12 cases and four group gates. No later renderer result is available.

The allocator aligns owned planar chroma rows to 64 bytes. Retest I420/YV12 imports and rendered pixels with this alignment; imported buffers retain their supplied pitches.

## Report identities

| Report | Driver SHA256 |
| --- | --- |
| `run-mbmgr47q` (unit) | `34e970cafbed9f1c017448a6b3aad0b94876be89c2516257b490647036307d3c` |
| `run-w43vz9fm` (hardware) | `ce168b9568fa0d98cad448e5768317974891c0476a5d8b1319be97aa1337ce61` |

## Hardware retest

Install FFmpeg and v4l-utils, then run:

```sh
VAV4L2_PROBE_NOCACHE=1 python3 tools/test.py --suite hardware --render-node /dev/dri/renderD128
```

Use the render node for the device and attach the resulting report archive and capability inventory. Check H.264/HEVC pixels, queue completion, native DMA-BUF export and I420/YV12 rendering.
