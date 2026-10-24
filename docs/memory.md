# Memory, DMA-BUF and fallback

## Ownership

VA surfaces own their current backing and bookkeeping. Imported descriptor FDs remain caller-owned; the driver retains independent CLOEXEC references. Exported FDs belong to the caller and remain valid after surface, context or driver destruction. Partial import failure does not publish surface IDs, and partial export failure does not publish a descriptor.

The last-writer marker tracks CPU backing, memfd snapshots and GBM/imported DMA objects. Readback uses the latest storage. Decode references retain native CAPTURE ownership. Timestamp zero is valid.

DeriveImage borrows writable surface backing. GetImage/PutImage handle detached transfers. Live borrows can make conflicting operations busy; failed writes preserve the surface error state.

## Decoder queues

Compressed OUTPUT is MMAP-backed and bound to a media request; CAPTURE is queued independently. Sequence controls, request controls and queue completion are separate responsibilities. OUTPUT is dequeued before synchronous submission returns. Initial allocation pressure may reduce the spare OUTPUT slot count from four to two to one while preserving coded-buffer size and retained references. Non-memory errors stop immediately.

Native CAPTURE allocation may return fewer buffers than requested. Eligible codecs can fall back to an independently allocated DMA-BUF pool before accepting a smaller native pool. Negotiated decoder sizeimage, including auxiliary storage, determines capacity. Pool transport uses MEMORY_DMABUF; its capacity is checked against decoder requirements.

Internal pools currently allocate through registered V4L2 storage providers. Without an eligible provider, decoding uses the available native CAPTURE buffers; streams requiring a larger reference pool may fail setup.

External direct decoder targets are selected automatically for compatible declared pools: canonical, LINEAR, compatible single-object NV12, exact negotiated pitch/aligned height/full sizeimage, and safe ownership are required. Packed NV15/NV20, incompatible imported layouts and undeclared dynamic pools use native storage and transfer. Explicit direct mode is available for clients that guarantee homogeneous dynamic targets. Reconfiguration releases buffers only after ownership is resolved; uncertain STREAMOFF/REQBUFS cleanup closes the device and requires context recreation.

## Export and processing

Native NV12 EXPBUF export avoids the capture-to-GBM copy when eligible. CPU-written content, unsupported capture/export layouts or explicit EXPBUF opt-out use the implemented coherent/GBM copy path. Exported objects retain their identity across updates. Pixel conversion and application hwdownload involve copies.

VPP direct transport uses actual DMA-BUF OUTPUT/CAPTURE queues with retained descriptor references. Direct processing queues the exported objects without CPU pixel staging. Compatible jobs retain queues; new FDs are rebound with correct ownership. Setup may try another importer or staging only before unsafe submission. Submitted job failures propagate to the caller.

Completed native NV12 decoder buffers can feed VPP through EXPBUF when the live CAPTURE slot, pitch, chroma offset and full object size match the processing layout. A fresh full-frame output can allocate shared backing for this path without uploading pixels. Padded capture heights and other native layouts use the existing transfer path when direct processing is unavailable.

JPEG direct transport similarly keeps eligible input FDs until both queues complete. Its coded CAPTURE remains MMAP and its VACodedBufferSegment is a host copy.

## Allocation and compatibility

GBM backing follows the DRM connection of each libva display. A validated LINEAR import that GBM cannot accept can use mapped PRIME storage with CPU synchronization instead.

Compatibility modules can register additional V4L2 allocation providers. The allocator tries eligible providers in preference order and validates returned formats, physical planes, allocation spans and FD flags. A failed provider advances to the next allocation candidate. The [module catalog](compat/README.md) describes the implementation-specific allocation paths.

## Synchronization

Reservation-fence waits precede device access. CPU access additionally brackets mappings with DMA_BUF_IOCTL_SYNC START/END using READ/WRITE directions. EINTR/EAGAIN retries use a bounded monotonic budget. Clients must coordinate external writers/readers before invoking conflicting VA operations.

See [testing](testing.md) for fence, lifetime and pixel-validation coverage.

## Runtime policy

Transport overrides and trace settings are listed in [environment variables](environment.md). Normal operation selects eligible hardware paths and uses staging where supported.

Sources: [DMA-BUF API](https://docs.kernel.org/driver-api/dma-buf.html), [V4L2 DMA import](https://docs.kernel.org/userspace-api/media/v4l/dmabuf.html), [V4L2 Request API](https://docs.kernel.org/userspace-api/media/mediactl/request-api.html).
