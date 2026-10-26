# Supported operations and interface boundaries

Available operations depend on the running kernel interface, implemented translation, buffer layout and application requirements.

## Bridge support

| Operation | Implemented scope | Limits |
| --- | --- | --- |
| H.264 VLD | Both backends: Constrained Baseline, Main, High and frame-coded MBAFF. Stateless also supports High10/High422 and compatible paired PAFF fields when the node permits them. | Stateful: 8-bit 4:2:0 NV12, no separate fields. Stateful POC1 is normalized to POC0, with IDR/wrap/MMCO5 bounds described in [decoding](decoding.md). Stateless POC1, FMO, SP/SI, 4:4:4 and MVC/SVC are unsupported. |
| HEVC VLD | Both backends: Main/Main10, slice arrays, fragmented packets and PPS identities. Stateful adds picture-local RPS/list normalization and temporal sublayers. | Stateful: LINEAR NV12/P010; normalized headers require complete VA slice offsets. No 12-bit, range extensions, multilayer or 4:4:4 support. |
| VP8 VLD | Frame metadata, probabilities, fragment assembly and reference lifetime | Stateless: VP8F, required controls and Request API. |
| MPEG-2 VLD | Simple/Main, multiple slices, fragment assembly and P/B references | Hardware IDCT can differ from FFmpeg software; qualification includes a same-kernel GStreamer reference. |
| AV1 VLD | Profile0 8/10-bit; both backends; opt-in with `VAV4L2_ADVERTISE_AV1=1` | Experimental. Stateful reconstructs OBUs and reference slots; resolved film grain; no large-scale tiles. Request needs original OBU metadata for nonimplicit refresh masks. Up to 32 tiles; broader syntax and hardware qualification remain pending. |
| VP9 VLD | Both backends: Profile 0/2 with complete frames. Request translation reads both headers and retains loop-filter/segmentation state. | Requires VP90 or VP9F, the matching profile menu and matching-depth CAPTURE storage (NV12/NV12M, P010 or packed NV15 as appropriate). Show-existing-only VA submissions are unsupported. |
| Other image/video encode | Not implemented | An encoder needs its own VA parameter translation, V4L2 negotiation and tests. |
| JPEG encode | Baseline; NV12/NV21/I420/YV12 source layouts; native quality; staged and eligible direct input | No arbitrary DQT/DHT programming, restart controls, lossless/progressive encode or completed packed-422 source path. |
| VPP | Scale, supported crop/compose, rotation/mirror and BT.601/BT.709 RGB/YUV conversion | Per-direction formats, TRY_FMT geometry, controls and compatibility quirks apply. No denoise, sharpen, deinterlace, HDR/tone mapping, arbitrary transfer functions, chroma-phase conversion or nonopaque alpha blending. |
| Surface/Image | 22 layouts; allocation, GetImage/PutImage, supported direct derivation, offsets and padding | Direct derivation requires writable surface backing. |
| DRM PRIME | LINEAR PRIME1 import and PRIME2/3, composed/separate layers, validated independent pitches/offsets and retained FDs | No legacy export descriptor, protected content, nonlinear/compressed modifiers, opaque unmappable imports or explicit-fence VA extension. |
| Display | Export to a DRM/EGL consumer | PutSurface, subpictures, palettes and display attributes return typed unsupported-operation errors. |
| Scheduling | Serialized frontend state; bounded native Request jobs and asynchronous stateful pictures | Completion is matched by timestamp. Held fields, imported Request CAPTURE and previously exported Request targets retain synchronous submission; jobs are bounded by the coded buffer pool. |

Decode and JPEG encode currently have a 4096-by-4096 frontend limit. Lower kernel limits and exact per-picture negotiation still apply. VPP bounds come from discovered nodes. Both single-planar and multi-planar queue APIs are supported. Implemented planar CAPTURE layouts retain two or three physical planes; [format contracts](formats.md) list their readback and export paths. Other native layouts are rejected.

Stateless H.264 PAFF stages an owned first field, keeps the target Rendering, and submits two requests with the same timestamp to one held CAPTURE buffer. HOLD capability is checked using CREATE_BUFS(count=0). Only a compatible, completed field pair becomes Ready. Standalone or incompatible pairs fail without publishing completed pixels.

## Runtime availability

Codec, memory-transport and processing capabilities are probed independently. VPP checks formats, controls and geometry for each job and can select a different compatible node. Buffer setup can fall back before submission; failures after a job starts are reported rather than retried over potentially modified data.

Unsupported profiles are omitted from VA queries. Software codec fallback is handled by the application.

Stateless H.264/HEVC negotiate frame-based Annex-B or slice-based submission. Slice-based decoding requires slice controls and HOLD_CAPTURE support; start codes follow the selected menu. Stateful H.264/HEVC use reconstructed Annex-B packets; VP9 uses complete frames and AV1 uses reconstructed OBUs. Independent queues complete surfaces by timestamp. Native CAPTURE supports LINEAR NV12/P010 and implemented planar YUV layouts. Implemented NV12/P010 tile layouts are detiled to LINEAR surface storage; their native buffers are not exported as LINEAR. Separate H.264 field pictures remain unsupported by the stateful backend. See [decoding](decoding.md) for exact bounds. Slice-based H.264 currently accepts frame pictures; paired PAFF uses the frame-based path.

Binary packages follow the CPU architecture and distribution ABI. Device capabilities are negotiated at runtime.

Device-specific measurements and kernel inventories are collected in the [hardware reports](hardware/README.md).

## Remaining bridge work

Broader VP9 and AV1 hardware qualification, additional native capture layouts, other VA raw formats and additional codec syntax, JPEG packed-422 inputs, and broader syntax vectors remain implementation or qualification work. P010/Y210 raw VPP requires matching kernel input/output menus and exact storage geometry. Unknown formats and unavailable controls are rejected instead of guessed.

## Features requiring changes outside this bridge

| Feature | Missing contract / required external change |
| --- | --- |
| Exact H.264 POC1 SPS reconstruction | Standard VA decode payload lacks original cycle length/offset arrays; a client/libva metadata contract must transport them. The stateful normalizer instead uses equivalent POC0 syntax; it does not recover the original arrays. |
| Arbitrary original HEVC SPS ST/LT RPS and sublayer metadata | Clients must preserve and transmit original parameter sets through an agreed API. The stateful normalizer instead expresses each picture's reference graph as inline RPS; it does not recover the original tables or presentation metadata. |
| Unsupported frontend profile accepted by FFmpeg | FFmpeg's own format/profile checks may reject before the driver is called; bridge-only changes cannot bypass that decision. |
| Missing encoder or decoder interface | The kernel driver must expose the required formats, controls and queue interface. |
| Custom JPEG quantization/Huffman/restart programming | Selected encoder must expose the corresponding controls; accepted VA buffers alone cannot program absent controls. |
| HDR, denoise, deinterlace, nonopaque blending | The current raw-M2M backend needs matching kernel operations. Another processing backend would require its own implementation, capability negotiation and validation. |
| Protected/nonlinear buffers and explicit-fence interop | Producer, kernel, libva contract and consumer must agree on protection/modifier/synchronization semantics. |

## Sources

- [Linux stateful decoder contract](https://docs.kernel.org/userspace-api/media/v4l/dev-decoder.html).
- [Linux stateless decoder contract](https://docs.kernel.org/userspace-api/media/v4l/dev-stateless-decoder.html) and [stateless controls](https://docs.kernel.org/userspace-api/media/v4l/ext-ctrls-codec-stateless.html).
- [libva 2.23.0 public headers](https://github.com/intel/libva/tree/2.23.0/va), especially va.h, va_dec_hevc.h and va_drmcommon.h.
