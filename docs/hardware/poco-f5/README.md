# POCO F5

The latest report, `run-0r003yl_`, ran on Alpine aarch64 with kernel `7.2.8` and the Iris decoder. The source was commit `2743db3c36c961a7df40cf10150ba161ca591506` with local changes. Driver SHA256: `62f252746a8c1cc080d2d714f5ecb9665278f3d5b95fb385074a1fe69e78923f`. GPU copy was disabled.

## Test results

| Area | Latest result |
| --- | --- |
| Native musl build and unit tests | 68/68 passed, including the packet oracle and stateful queues |
| Decoder discovery | H.264 Constrained Baseline/Main/High, HEVC Main/Main10 and VP9 Profile 0/2 advertised |
| Hardware decode | HEVC Main decoded eight pictures identical to the software reference. H.264 wrote five exact pictures and HEVC Main10 four before timing out. VP9 Profile 0/2 produced no pixels |
| Failure point | H.264 and HEVC Main10 stop making progress after partial output; HEVC Main completes. VP9 returns a CAPTURE buffer with ERROR (`flags=0x4040`) and zero payload |
| Renderer | 21/21 cases passed in each of the native and GBM export modes, including I420/YV12, Y210 and GPU fence writeback |
| Native decoder DMA-BUF | Not qualified by these partial CPU readback results |
| Overall | 131 PASS / 4 FAIL / 14 SKIP; complete report, exit 1 |

The skips cover unavailable VP8/MPEG-2, JPEG/VPP and acceptance groups that require those processing interfaces. Renderer success tests VA surface storage, EGL import and rendered pixels. Decoder completion and the optional GPU copy path still need device tests.

H.264, HEVC Main10 and VP9 remain unresolved. HEVC Main success applies to the recorded fixture and binary; it does not qualify native decoder DMA-BUF export or subsequent source changes.
