# Environment variables

Normal use requires only driver selection. Decode, encode, processing and eligible DMA-BUF paths are discovered automatically. AV1 profile advertisement is experimental and requires an explicit opt-in.

## Driver selection and discovery

| Variable | Default | Effect |
| --- | --- | --- |
| `LIBVA_DRIVER_NAME` | libva selection | Set `v4l2request` to select this driver |
| `LIBVA_DRIVERS_PATH` | libva search path | Directory containing `v4l2request_drv_video.so`; use the build directory for an uninstalled driver |
| `XDG_RUNTIME_DIR` | No probe cache when absent or unsuitable | Private runtime directory used for the discovery cache; ownership and permissions are checked |
| `VAREQ_PROBE_NOCACHE` | Unset | Any set value bypasses cache reads and writes, including an empty value or `0` |
| `VAREQ_RENDER_NODE` | DRM connection of the libva display; render-node discovery otherwise | Override the DRM node used for GBM backing. Does not select a video codec node |
| `VAREQ_ADVERTISE_AV1` | Unset | Exactly `1` includes the experimental AV1 profile in VA profile queries when a usable decoder exists |
| `VAREQ_DEBUG` | Unset | Any set value enables driver diagnostics, including an empty value or `0`; read at driver initialization |

## Buffer transport

| Variable | Default | Values and behavior |
| --- | --- | --- |
| `VAREQ_EXPBUF_EXPORT` | Enabled | A value starting with `0` disables native decoder EXPBUF export and selects copy backing. Read once per process at first use |
| `VAREQ_VPP_DMABUF` | `auto` | `0`: staged processing; `required`: fail if direct transport is unavailable; other values: automatic eligible direct transport with staging fallback |
| `VAREQ_JPEG_DMABUF` | `auto` | `0`: staged encoder input; `required`: require direct input; other values: automatic selection |
| `VAREQ_CAPTURE_DMABUF` | `auto` | `0`: native/internal decoder storage; `required`: require direct external targets; `1`: also permit an undeclared dynamic target pool whose layouts the client guarantees are compatible; other values: direct capture only for an eligible declared pool |
| `VAREQ_CAPTURE_POOL_DMABUF` | `auto` | `0`: disable the internal DMA-BUF pool; `required`: require that pool when selecting decoder-owned storage; other values: allow it when native CAPTURE allocation is insufficient. Eligible direct external targets take precedence |
| `VAREQ_SHARED_STORAGE` | `auto` | `coherent`: restrict compatibility allocation to the coherent-provider policy; other values: normal provider ranking |
| `VAREQ_VPP_DMABUF_SESSION` | Retain compatible sessions | `reconfigure`: rebuild queues for each direct job; `isolated`: reopen the node for each direct job; other values: normal session reuse |

`required` modes are diagnostic constraints and return errors instead of staging. Direct transport still requires the negotiated format, stride, object span and ownership. See [memory and fallback](memory.md).

## Trace files

These variables name append-only trace files. They are disabled when unset or empty. The parent directory must already exist. Acceptance tools assign paths inside their report directory.

| Variable | Contents |
| --- | --- |
| `VAREQ_ALLOC_TRACE` | Allocation provider, requested span, returned geometry and errors |
| `VAREQ_STORAGE_TRACE` | Surface storage allocation, GBM import and backing changes |
| `VAREQ_DECODE_TRACE` | Decoder submission, capture pools, references and image readback |
| `VAREQ_CAPTURE_TRACE` | Direct decoder CAPTURE layout, binding and queue completion |
| `VAREQ_VPP_TRACE` | Processing operations, selected node and regions |
| `VAREQ_DMABUF_TRACE` | Direct VPP setup, object layout, queue state and errors |
| `VAREQ_JPEG_TRACE` | JPEG input transport and encoder completion |

## Acceptance tools

Prefer the command-line options in [testing](testing.md). The shell acceptance profiles and test adapters also accept these settings:

| Variable | Consumer | Effect |
| --- | --- | --- |
| `VAREQ_BUILD_DIR` | Feature and hardware completion scripts | Build directory; defaults to `builddir` when the scripts run directly |
| `VAREQ_REPORT_FILE` | Hardware completion script | Output archive; defaults to `verify/hardware-report.tar.gz` |
| `FF` | Full matrix and hardware completion scripts | FFmpeg executable; defaults to `/usr/bin/ffmpeg` |
| `BUILDDIR` | Full matrix script | Build directory; defaults to `builddir` |
| `CLIPDIR` | Full matrix script | Generated clips directory; defaults to `verify/clips` |
| `OUTDIR` | Full matrix script | Results directory; defaults to `verify/matrix` |
| `DRM_DEVICE` | Full matrix script | Render node; defaults to `/dev/dri/renderD128` |
| `GST_H264_DECODER` | Full matrix script | Override the GStreamer decoder factory; otherwise the script matches a factory to the selected H.264 video node |
| `VAREQ_TEST_CAPTURE_POOL` | Full matrix script | Exactly `1` adds required internal-pool cases |
| `VAREQ_TEST_AV1_TILES` | Full matrix script | Exactly `1` adds experimental tiled AV1 cases; also requires `VAREQ_ADVERTISE_AV1=1` |
| `VAREQ_HEVC_PACKET_MODE` | HEVC test adapter | `whole`, `offset`, `fragments`, `array` or `split-prefix` submission shape |
| `VAREQ_HEVC_PACKET_LOG` | HEVC test adapter | Packet trace output; required to activate the adapter |
| `VAREQ_PRIME_TRACE_DIR` | PRIME test adapter | Trace directory; setting it enables imported test allocations |
| `VAREQ_PRIME_CANONICAL` | PRIME test adapter | Exactly `1` selects the canonical NV12 test allocation |

The test adapters are not installed with the driver. Acceptance profiles set some variables explicitly for each case; an inherited value does not override those case settings. `tools/test.py` uses `--build-dir`, `--output-dir` and `--render-node` for its own configuration.

Build tools also honor their standard compiler, pkg-config and sanitizer environment settings. See [build and packaging](building.md) for native and cross builds.
