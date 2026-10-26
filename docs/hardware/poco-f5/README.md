# POCO F5

The latest report, `run-5m068n2n`, ran on Alpine aarch64 with kernel `7.2.8` and the Iris decoder. The source was commit `717bee42b78f93ea854d86140af199a36eabb5ea` with a clean working tree. Driver SHA256: `2db95c3f99e090aac4afb664ada12b0537a5c241dce95ce5811acf8ab36f60b3`. GPU copy was disabled.

## Test results

| Area | Latest result |
| --- | --- |
| Native musl build and unit tests | 69/69 passed, including the packet oracle and stateful queues |
| Decoder discovery | H.264 Constrained Baseline/Main/High, HEVC Main/Main10 and VP9 Profile 0/2 advertised |
| Hardware decode | H.264 wrote three pictures and HEVC Main/Main10 four each, identical to the software reference, before timing out. VP9 Profile 0/2 produced no pixels |
| Failure point | H.264 and HEVC stop making progress after the first completed drain/resume cycle. VP9 returns a CAPTURE buffer with ERROR (`flags=0x4040`) and zero payload |
| Renderer | 21/21 cases passed in each of the native and GBM export modes, including I420/YV12, Y210 and GPU fence writeback |
| Native decoder DMA-BUF | Not qualified by these partial CPU readback results |
| Overall | 131 PASS / 5 FAIL / 14 SKIP; complete report, exit 1 |

The skips cover unavailable VP8/MPEG-2, JPEG/VPP and acceptance groups that require those processing interfaces. Renderer success tests VA surface storage, EGL import and rendered pixels. Decoder completion and the optional GPU copy path still need device tests.

H.264, HEVC Main/Main10 and VP9 remain unresolved. The recorded decoder menu has no display-delay controls. Partial matching pixels do not qualify decoder completion, native DMA-BUF export or subsequent source changes.
