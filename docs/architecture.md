# Architecture and discovery

| Module | Responsibility |
| --- | --- |
| `src/api/` | Typed libva callbacks; config/context/surface/buffer lifecycle, separate Image transfer and unsupported-operation modules, shared frontend serialization and API validation |
| `src/codecs/` | Decode/encode parameter translation, packet assembly and reference validation |
| `src/codecs/bytestream.c` | H.264/HEVC Annex-B parameter sets reconstructed from VA decode buffers |
| `src/v4l2/codec.c` | Codec descriptors, required-control policy and queue capability classification |
| `src/v4l2/probe.c` | Numeric video-node enumeration, profile/format discovery, exact media-interface matching and cache validation |
| `src/v4l2/device.c` | Shared device access, queue allocation, capture geometry and storage attachment |
| `src/v4l2/stateless.c` | Request initialization, codec dispatch, synchronous submission, capture reconfiguration and recovery |
| `src/v4l2/stateful.c` | Independent bytestream OUTPUT/CAPTURE, source changes, timestamp routing, drain/resume and pending surface lifetime |
| `src/compat/` | Runtime module matching, processing exceptions and additional storage providers |
| `src/memory/` | Capture pools, format conversion, LINEAR PRIME validation, FD ownership, synchronization and generic GBM storage |
| `src/vpp/` | Per-job raw-node selection, controls, TRY_FMT geometry, crop/compose and MMAP/DMABUF sessions |
| `tests/` | Hardware-free fault regressions, public VA clients, packet adapters and hardware pixel oracles |
| `tools/` | Read-only capability inventory and standard report orchestration |

## Discovery

Discovery naturally sorts numeric `/dev/video*` paths, including indices above 63. It holds up to 64 candidate nodes, 64 formats per queue and 16 VPP candidates. Node enumeration returns EOVERFLOW when more than 64 nodes are present.

The opened node's `device_caps` is authoritative when V4L2_CAP_DEVICE_CAPS is present. Both M2M_MPLANE and STREAMING are required. Decoder discovery distinguishes stateful bytestream FourCCs from stateless parsed-frame/slice FourCCs.

Each decoder requires a compiled translator and its coded OUTPUT format. Stateless paths require mandatory writable controls. Stateless H.264/HEVC additionally require frame-based mode and Annex-B start-code menu entries. For Request decoding, the OUTPUT queue must expose SUPPORTS_REQUESTS via nonallocating CREATE_BUFS(count=0), and the associated media node must allocate a request. Media association uses the graph's V4L video interface major/minor pair.

Stateful candidates require SOURCE_CHANGE subscription and decoder STOP command support; they do not require a media node or request controls. H.264 candidates are ranked by advertised extended profiles and frame area. Available H.264/HEVC profile menus constrain VA profile advertisement; sequence setup validates each stream. Each codec selects one node. Incompatible context or stream reconfiguration returns an error. Stateful CAPTURE is configured after sequence parsing; its OUTPUT stays streaming during CAPTURE reconfiguration. See [decoding](decoding.md).

Raw M2M processing is detected from implemented input/output formats independently of transform controls. Every job checks its actual format pair, dimensions, requested controls and compatibility restrictions; exact TRY_FMT may reject a candidate. Capabilities are checked on the selected node.

JPEG discovery selects coded CAPTURE before enumerating its raw OUTPUT menu and requires an implemented input layout and writable quality control. Encoding uses raw OUTPUT and JPEG CAPTURE queues.

## Cache

Discovery results may be cached in a private, owned, mode-0700 XDG_RUNTIME_DIR. Cache files are created with unique mode-0600 temporary files and atomically renamed; reads refuse symlinks, nonregular files, wrong owners, broad permissions, oversized/truncated records, invalid paths and stale timestamps.

A cache key includes the boot ID and numeric video-node identity/metadata. Selected paths are reopened and checked against current capabilities, formats and required controls before reuse. Topology changes, removed controls/formats, cache age above 60 seconds or `VAV4L2_PROBE_NOCACHE=1` trigger discovery. The age limit also refreshes negative capability entries. Existing VA configs are snapshots; applications must recreate/reinitialize after kernel/driver changes.

CAPTURE format depends on coded OUTPUT and sequence controls. Final format, depth/chroma, dimensions, physical plane count, pitches and sizeimage are validated during stream setup. Unsupported layouts or unavailable capabilities return an error. Software codec fallback is handled by the application.

## Extending hardware support

See [Contributing](../CONTRIBUTING.md) for interface changes, compatibility modules and hardware qualification.

## Build and runtime boundaries

Codec, memory and processing modules are built into the driver. EGL/GLES acceptance clients test exported buffers; see [build options](building.md).

GBM backing is scoped to each libva display and uses an owned duplicate of its DRM FD. For displays without a DRM connection, render nodes are enumerated through libdrm. An explicit render-node override applies only to storage selection. Storage allocation tries eligible V4L2 providers and generic GBM backing.

For stateless decoding, direct external decoder capture is selected automatically for a declared pool whose targets all match the negotiated allocation. An unknown dynamic pool keeps independent native DPB storage, because a later target may have another layout. The diagnostic direct mode allows clients with a guaranteed uniform dynamic pool to exercise that path.

## Compatibility handling

Shared paths implement standard V4L2 formats, controls and queue capabilities. Compatibility modules describe implementation exceptions and additional storage providers.

Compatibility source modules register in `src/compat/meson.build`. Meson generates the registry; all bundled modules are linked into the same driver. Runtime matching uses the individual node's driver, card, bus identity and version. Standard capability negotiation follows module matching.

VPP policies can reject a particular job, adjust candidate preference and independently select mirror mapping, pre-queue streaming, retained DMA streams and import-span validation. Allocation providers supply their geometry through a shared validating export helper. Provider failures advance to the next allocation candidate.

The versioned source interface is described in [Compatibility modules](compatibility.md), including registration and regression coverage. The [module catalog](compat/README.md) documents individual implementations and their purpose.
