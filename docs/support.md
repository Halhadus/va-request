# Supported operations and interface boundaries

Available operations depend on the running kernel interface, implemented translation, buffer layout and application requirements.

## Bridge support

| Operation | Implemented scope | Limits |
| --- | --- | --- |
| H.264 VLD | Both backends: Constrained Baseline, Main, High and frame-coded MBAFF. Stateless also supports High10/High422 and compatible paired PAFF fields when the node permits them. | Stateful: 8-bit 4:2:0 NV12, no separate fields. POC1, FMO, SP/SI, 4:4:4 and MVC/SVC are unsupported. |
| HEVC VLD | Both backends: Main, slice arrays, fragmented packets and PPS identities. Stateless also supports Main10. | Stateful: 8-bit NV12 and one temporal sublayer. Original SPS RPS arrays are unavailable in VA buffers. No 12-bit, range extensions, multilayer or 4:4:4 support. |
| VP8 VLD | Frame metadata, probabilities, fragment assembly and reference lifetime | Stateless: VP8F, required controls and Request API. |
| MPEG-2 VLD | Simple/Main, multiple slices, fragment assembly and P/B references | Hardware IDCT can differ from FFmpeg software; qualification includes a same-kernel GStreamer reference. |
| AV1 VLD | Profile0 8-bit; opt-in with `VAV4L2_ADVERTISE_AV1=1` | Experimental; disabled by default. Multi-tile, 4K, super-resolution, refresh/reference reconstruction and uncommon features need further qualification. Not part of normal acceptance. |
| VP9 VLD | Pending | Kernel inventory may include VP9F/VP90; VA translation is not implemented. |
| Other image/video encode | Not implemented | An encoder needs its own VA parameter translation, V4L2 negotiation and tests. |
| JPEG encode | Baseline; NV12/NV21/I420/YV12 source layouts; native quality; staged and eligible direct input | No arbitrary DQT/DHT programming, restart controls, lossless/progressive encode or completed packed-422 source path. |
| VPP | Scale, supported crop/compose, rotation/mirror and BT.601/BT.709 RGB/YUV conversion | Per-direction formats, TRY_FMT geometry, controls and compatibility quirks apply. No denoise, sharpen, deinterlace, HDR/tone mapping, arbitrary transfer functions, chroma-phase conversion or nonopaque alpha blending. |
| Surface/Image | 18 layouts; allocation, GetImage/PutImage, supported direct derivation, offsets and padding | Direct derivation requires writable surface backing. |
| DRM PRIME | LINEAR PRIME2/3, composed/separate layers, validated independent pitches/offsets and retained FDs | No PRIME1, protected content, nonlinear/compressed modifiers, opaque unmappable imports or explicit-fence VA extension. |
| Display | Export to a DRM/EGL consumer | PutSurface, subpictures, palettes and display attributes return typed unsupported-operation errors. |
| Scheduling | Serialized frontend state; synchronous Request jobs and asynchronous stateful pictures | Stateful completion is matched by timestamp; unrestricted parallel Request execution is not implemented. |

Decode and JPEG encode currently have a 4096-by-4096 frontend limit. Lower kernel limits and exact per-picture negotiation still apply. VPP bounds come from discovered nodes. Single-planar queue API and unsupported capture layouts are not emulated.

Stateless H.264 PAFF stages an owned first field, keeps the target Rendering, and submits two requests with the same timestamp to one held CAPTURE buffer. HOLD capability is checked using CREATE_BUFS(count=0). Only a compatible, completed field pair becomes Ready. Standalone or incompatible pairs fail without publishing completed pixels.

## Runtime availability

Codec, memory-transport and processing capabilities are probed independently. VPP checks formats, controls and geometry for each job and can select a different compatible node. Buffer setup can fall back before submission; failures after a job starts are reported rather than retried over potentially modified data.

Unsupported profiles are omitted from VA queries. Software codec fallback is handled by the application.

Stateless H.264/HEVC require frame-based Annex-B submission. Stateful H.264 and HEVC use reconstructed Annex-B parameter sets and coded slices, independent queues and timestamp-matched completion. The stateful backend currently negotiates single-plane LINEAR NV12 for 8-bit 4:2:0; H.264 field pictures/POC1 and HEVC SPS reference-set arrays or temporal sublayers are outside its reconstructible syntax. See [stateful decoding](stateful.md) for exact bounds. Slice-only Request submission and additional native layouts require backend support.

Binary packages follow the CPU architecture and distribution ABI. Device capabilities are negotiated at runtime.

Device-specific measurements and kernel inventories are collected in the [hardware reports](hardware/README.md).

## Remaining bridge work

VP9 translation, broader AV1 validation, additional native capture layouts, other VA raw formats (including RGB24/BGR24 and packed 16-bit alpha layouts), JPEG packed-422 inputs, and broader syntax vectors remain implementation or qualification work. Existing Image/import support for P010/Y210 is not a 10-bit VPP implementation. Unknown formats and unavailable controls are rejected instead of guessed.

## Features requiring changes outside this bridge

| Feature | Missing contract / required external change |
| --- | --- |
| Exact H.264 POC1 SPS reconstruction | Standard VA decode payload lacks original cycle length/offset arrays; a client/libva metadata contract must transport them. Absolute POC values are insufficient. |
| Arbitrary original HEVC SPS ST/LT RPS and sublayer metadata | Clients must preserve and transmit original parameter sets through an agreed API. Current-picture DPB membership does not recover those tables. |
| Unsupported frontend profile accepted by FFmpeg | FFmpeg's own format/profile checks may reject before the driver is called; bridge-only changes cannot bypass that decision. |
| Missing encoder or decoder interface | The kernel driver must expose the required formats, controls and queue interface. |
| Custom JPEG quantization/Huffman/restart programming | Selected encoder must expose the corresponding controls; accepted VA buffers alone cannot program absent controls. |
| HDR, denoise, deinterlace, nonopaque blending | The kernel processing interface must expose the operation. |
| Protected/nonlinear buffers and explicit-fence interop | Producer, kernel, libva contract and consumer must agree on protection/modifier/synchronization semantics. |

## Sources

- [Linux stateful decoder contract](https://docs.kernel.org/userspace-api/media/v4l/dev-decoder.html).
- [Linux stateless decoder contract](https://docs.kernel.org/userspace-api/media/v4l/dev-stateless-decoder.html) and [stateless controls](https://docs.kernel.org/userspace-api/media/v4l/ext-ctrls-codec-stateless.html).
- [libva 2.23.0 public headers](https://github.com/intel/libva/tree/2.23.0/va), especially va.h, va_dec_hevc.h and va_drmcommon.h.
