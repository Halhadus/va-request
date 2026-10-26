# Memory, DMA-BUF and fallback

## Ownership

VA surfaces own their current backing and bookkeeping. Imported descriptor FDs remain caller-owned; the driver retains independent CLOEXEC references. Exported FDs belong to the caller and remain valid after surface, context or driver destruction. Partial import failure does not publish surface IDs, and partial export failure does not publish a descriptor.

The last-writer marker tracks CPU backing, memfd snapshots and GBM/imported DMA objects. Readback uses the latest storage. Decode references retain native CAPTURE ownership. Timestamp zero is valid.

DeriveImage borrows writable surface backing. GetImage/PutImage handle detached transfers. Live borrows can make conflicting operations busy; failed writes preserve the surface error state.

## Decoder queues

Compressed OUTPUT is MMAP-backed. Stateless submissions bind OUTPUT to a media request; stateful submissions queue a bytestream without a request. Native Request jobs retain their coded buffers, CAPTURE reservations and reference surfaces until completion; reusing a target waits for outstanding readers before detaching its backing; imported CAPTURE and previously exported targets preserve synchronous slot ownership. CAPTURE is queued independently. Sequence controls, request controls and queue completion are separate responsibilities. Request OUTPUT is dequeued before synchronous submission returns; pending native jobs release it after both queues and the request complete. Stateful OUTPUT completion only releases coded storage; a surface becomes Ready after matching CAPTURE completion. Initial allocation pressure may reduce the spare OUTPUT slot count from four to two to one while preserving coded-buffer size and retained references. Non-memory errors stop immediately.

Stateful native CAPTURE slots retain a fixed index/backing association while streaming. Surfaces retain dequeued pixels until reuse or destruction; queued slots are never exposed as completed. Stateful setup requires the reported decoder minimum plus spare slots and keeps client export backing independent of CAPTURE index selection.

For Request decoding, native CAPTURE allocation may return fewer buffers than requested. Eligible codecs can fall back to an independently allocated DMA-BUF pool before accepting a smaller native pool. Negotiated decoder sizeimage, including auxiliary storage, determines capacity. Pool transport uses MEMORY_DMABUF; its capacity is checked against decoder requirements.

Internal pools try registered V4L2 storage providers, then the standard system DMA heap when available. Heap allocations retain CPU cache synchronization and do not satisfy an explicit coherent-provider requirement. Without eligible storage, decoding uses the available native CAPTURE buffers; streams requiring a larger reference pool may fail setup.

For Request decoding, external direct decoder targets are selected automatically for compatible declared pools: canonical, LINEAR, compatible single-object NV12, exact negotiated pitch/aligned height/full sizeimage, and safe ownership are required. Packed NV15/NV20, incompatible imported layouts and undeclared dynamic pools use native storage and transfer. Explicit direct mode is available for clients that guarantee homogeneous dynamic targets. Reconfiguration releases buffers only after ownership is resolved; uncertain STREAMOFF/REQBUFS cleanup closes the device and requires context recreation.

## Export and processing

Native NV12 EXPBUF export avoids the capture-to-GBM copy when eligible. CPU-written content, unsupported capture/export layouts or explicit EXPBUF opt-out use the implemented coherent/GBM copy path. Persistent imported/GBM backing retains its identity across updates. Native Request exports share a CAPTURE allocation; queue reconfiguration ends that allocation association while existing FDs keep the old object alive. Stateful exports use persistent surface backing because CAPTURE completion can select a different slot. Pixel conversion and application hwdownload involve copies.

Read-only export can precede `vaSyncSurface()`. If a submitted decoder job still owns the target, export resolves its completion before selecting the backing. Completion errors leave the caller's descriptor unchanged. An unfinished VA picture or a writable export conflicting with decoder work remains busy. Applications must still call `vaSyncSurface()` before reading exported pixels.

VPP direct transport uses actual DMA-BUF OUTPUT/CAPTURE queues with retained descriptor references. Direct processing queues the exported objects without CPU pixel staging. Compatible jobs retain queues; new FDs are rebound with correct ownership. Setup may try another importer or staging only before unsafe submission. Submitted job failures propagate to the caller.

Completed native NV12 decoder buffers can feed VPP through EXPBUF when the live CAPTURE slot, pitch, chroma offset and full object size match the processing layout. A fresh full-frame output can allocate shared backing for this path without uploading pixels. Padded capture heights and other native layouts use the existing transfer path when direct processing is unavailable.

JPEG direct transport similarly keeps eligible input FDs until both queues complete. Its coded CAPTURE remains MMAP and its VACodedBufferSegment is a host copy.

## Allocation and compatibility

GBM backing follows the DRM connection of each libva display. A validated LINEAR import that GBM cannot accept can use mapped PRIME storage with CPU synchronization instead.

Compatibility modules can register additional V4L2 allocation providers. The allocator tries eligible providers in preference order and validates returned formats, physical planes, allocation spans and FD flags. A failed provider advances to the next allocation candidate. The [module catalog](compat/README.md) describes the implementation-specific allocation paths.

## Synchronization

Reservation-fence waits precede device access. Native exported CAPTURE objects retain private FD references; every physical plane waits for readers before the slot is queued for writing again. Teardown closes the private references without closing caller-owned exports. CPU access additionally brackets mappings with DMA_BUF_IOCTL_SYNC START/END using READ/WRITE directions. EINTR/EAGAIN retries use a bounded monotonic budget. Clients must coordinate external writers/readers before invoking conflicting VA operations.

See [testing](testing.md) for fence, lifetime and pixel-validation coverage.

## Runtime policy

Transport overrides and trace settings are listed in [environment variables](environment.md). Normal operation selects eligible hardware paths and uses staging where supported.

Sources: [DMA-BUF API](https://docs.kernel.org/driver-api/dma-buf.html), [V4L2 DMA import](https://docs.kernel.org/userspace-api/media/v4l/dmabuf.html), [V4L2 Request API](https://docs.kernel.org/userspace-api/media/mediactl/request-api.html).

For display storage on MSM DRM devices, the allocator first requests cached coherent GEM memory. Unsupported cache modes may fall back to write-combined storage; allocation and permission errors are returned. `VAV4L2_SHARED_STORAGE=coherent` requires the coherent mode for this provider. Selection uses the actual display DRM fd and does not depend on a decoder or board name. Native decoder CAPTURE remains V4L2-managed.

## Native planes and GPU transfers

Native planar CAPTURE keeps the kernel allocations and pitches separate. LINEAR NV12M EXPBUF descriptors preserve each object's size and pitch in composed or separate layers. Other implemented planar and tiled layouts assemble canonical NV12/NV16 rows before surface transfer; they retain the kernel DPB backing. Nonzero CAPTURE data offsets and unimplemented native layouts are rejected.

`VAV4L2_GPU_COPY=1` selects an optional copy path for matching LINEAR NV12/P010 capture and imported/GBM targets.

A worker owns a private GBM display and requests a GLES 3.0 context. It selects image copying from GLES 3.2 core support or the advertised `GL_EXT_copy_image` / `GL_OES_copy_image` extension; a function address alone is not treated as support.

Read-only exports of driver-owned NV12/P010 targets use the coherent-provider policy when GPU copying is selected, unless the current pixels were CPU-written. Writable exports and VPP allocations retain their normal provider policy. Existing and imported objects keep their identity.

It imports byte-preserving RGBA8 views, waits for source and target reservations, copies both planes and completes the GPU work before publishing the target. Successful copying preserves the native CAPTURE reference through CPU readback; a CPU write invalidates that reference. It does not bind or terminate the caller's EGL display or context. Aliased source/target objects, unsupported modifiers and views exceeding an object are rejected. This is a GPU copy, not zero-copy; native EXPBUF export still shares the original capture objects.
