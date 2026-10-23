# Pixel formats and layout contracts

Image support, PRIME representation, decoder CAPTURE and raw VPP queues are distinct capabilities. The bridge implements the following 18 public VA layouts. All listed formats support implemented Image transfers and validated LINEAR PRIME2/3 representation; a successful descriptor import is not proof that the same representation can be queued directly to a device.

`W`/`H` are visible dimensions. Row spans below describe pixel bytes, not padding. Each object is independently bounded using actual FD size, offset, pitch, row count and occupied bytes. YUV requires even W; 4:2:0 also requires even H for PRIME import. CPU Image helpers separately handle supported odd-edge readback. All byte-order descriptions assume the supported little-endian Linux layout.

| VA FourCC | Storage and logical plane order | Visible pixel rows | Composed DRM FourCC | VPP queue mapping |
| --- | --- | --- | --- | --- |
| NV12 | Y + interleaved U,V; 8-bit 4:2:0 | W × H; W × H/2 | NV12 | NV12 or eligible NV12M |
| NV21 | Y + interleaved V,U; 8-bit 4:2:0 | W × H; W × H/2 | NV21 | NV21 or eligible NV21M |
| I420 | Y, U, V; planar 8-bit 4:2:0 | W × H; W/2 × H/2 twice | YUV420 | YUV420 |
| YV12 | Y, V, U; planar 8-bit 4:2:0 | W × H; W/2 × H/2 twice | YVU420 | Canonical YUV420 transport with logical-plane mapping; staging for incompatible external YVU order |
| P010 | Y + UV; 10-bit samples in upper bits of 16-bit words | 2W × H; 2W × H/2 | P010 | Not implemented as a raw VPP format |
| YUY2 | Y0,U,Y1,V; packed 8-bit 4:2:2 | 2W × H | YUYV | YUYV |
| UYVY | U,Y0,V,Y1; packed 8-bit 4:2:2 | 2W × H | UYVY | UYVY |
| VYUY | V,Y0,U,Y1; packed 8-bit 4:2:2 | 2W × H | VYUY | VYUY |
| YVYU | Y0,V,Y1,U; packed 8-bit 4:2:2 | 2W × H | YVYU | YVYU |
| 422H | Y,U,V; planar 8-bit 4:2:2 | W × H; W/2 × H twice | YUV422 | NV16 for staged transport; eligible direct planar storage uses YUV422P |
| YV16 | Y,V,U; planar 8-bit 4:2:2 | W × H; W/2 × H twice | YVU422 | NV61 for staged transport; eligible direct canonical storage uses YUV422P |
| Y210 | Packed 4:2:2; 10 significant bits per 16-bit word | 4W × H | Y210 | Not implemented as a raw VPP format |
| RGB565 (RG16) | Packed 5:6:5 RGB, 16-bit word | 2W × H | RGB565 | RGB565, whose V4L2 token is RGBP |
| RGBA | R,G,B,A bytes | 4W × H | ABGR8888 | RGBA32 |
| RGBX | R,G,B,X bytes; X not color data | 4W × H | XBGR8888 | RGBX32 |
| ARGB | A,R,G,B bytes | 4W × H | BGRA8888 | ARGB32 |
| BGRA | B,G,R,A bytes | 4W × H | ARGB8888 | ABGR32 |
| BGRX | B,G,R,X bytes; X not color data | 4W × H | XRGB8888 | XBGR32 |

VA byte-order names and DRM word-order names differ. For example VA RGBA maps to DRM ABGR8888 on little-endian systems. V4L2 RGBP means packed RGB565 here; VA RGBP is a separate planar RGB format and is not implemented. RGBX/BGRX contain 24 significant bits in 32-bit storage. RGB alpha-bearing inputs are usable for VPP only when the input crop is opaque; the bridge does not advertise blending.

Separate DRM layers use R8 for 8-bit Y/U/V, GR88 for interleaved 8-bit chroma, R16 and GR1616 for P010. Composed descriptors use the table's aggregate DRM format. Up to three pixel planes and validated object references are supported; a fourth unused descriptor slot is not a fourth color plane. Object indices may alias the same FD object, which is detected before overlap checks.

## Decoder capture

| Native V4L2 CAPTURE | Actual layout | Public readback | Export behavior |
| --- | --- | --- | --- |
| NV12 | One memory plane, Y + UV; negotiated stride and aligned height | NV12 and implemented 420 transfer conversions | Eligible native EXPBUF; otherwise coherent/GBM copy |
| NV15 | Packed 10-bit 4:2:0, four samples per five little-endian bytes | P010 unpacked to 16-bit words, sample shifted left six | Never relabelled NV12; converted/public storage export |
| NV16 | 8-bit 4:2:2 Y + UV, full-height chroma | YUY2 or supported planar 422 output | Layout-aware conversion/public storage |
| NV20 | Packed 10-bit 4:2:2, four samples per five bytes | Y210 or explicitly requested 8-bit YUY2 readback | Layout-aware conversion/public storage |

The current decoder backend handles one physical CAPTURE plane. NV12M and other multiplanar capture, tiled formats, native P010 capture and new codec-native layouts require explicit capture/readback implementations. Their presence in a kernel menu does not cause an NV12 guess. Unrecognized FourCC, unsupported bit depth/chroma or changed negotiated format returns an error. The application's software decode path may then take over.

SPS-dependent formats are negotiated after sequence controls. Width/height, physical plane count, bytesperline, sizeimage, QUERYBUF lengths and completion bytes are validated. A default CAPTURE menu from a fresh fd is not used to exclude NV15/NV16/NV20 before an appropriate SPS is set.

## VPP and JPEG transport

The 16 VPP public layouts are the table entries except P010 and Y210. Exposure is filtered by actual input/output menus and geometry; support for one direction is not assumed in reverse. A format catalog entry does not imply that all format pairs, dimensions, colorspaces or transforms work. RGB565/RGBA/RGBX-only raw nodes are recognized, as are nodes without rotate controls.

Direct VPP supports eligible contiguous one-object storage and supported NV12M/NV21M two-object arrangements. Every requested job must match exact pitches, dimensions, physical planes and offsets. Planar public YVU order is preserved independently from the canonical queue's physical U/V order. Gaps, arbitrary displaced offsets, row-interleaved planes and incompatible imported topology use validated staging when allowed. The import itself may still be valid.

JPEG public input is NV12/NV21/I420/YV12. Two zero-offset NV12 objects may use NV12M; three zero-offset I420/YV12 objects may use YUV420M with correct physical U/V indices. Canonical single-object NV12 is direct only if the selected encoder accepts it exactly. NV21 conversion, cropped input and incompatible geometry stage. Packed YUYV/UYVY in the encoder's kernel menu is recorded but is not yet a complete public JPEG input path.

## Representations deliberately rejected

Non-LINEAR modifiers including AFBC/tile/compression, auxiliary metadata planes, protected content and unmappable device-only allocations are unsupported. PRIME3 flags must be zero and reserved words must be zero. Modifier-list allocation requests must contain LINEAR for every surface; modifier lists are not silently applied to imported objects. PRIME1 external-buffer descriptors are not advertised.

For nonoverlapping row layouts, another plane may occupy padding between pixel rows. The validator checks actual occupied row intervals, not just broad bounding rectangles; real pixel overlap remains invalid. Plane bounds, overflow, FD identity and lifetime rules still apply. These import shapes do not automatically become direct queue shapes.

Sources: [libva DRM descriptors](https://github.com/intel/libva/blob/2.23.0/va/va_drmcommon.h), [Linux planar YUV formats](https://docs.kernel.org/userspace-api/media/v4l/pixfmt-yuv-planar.html), [DRM format definitions](https://gitlab.freedesktop.org/mesa/drm/-/blob/main/include/drm/drm_fourcc.h).
