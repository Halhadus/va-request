# RK3588 hardware report

The table describes the supplied RK3588 inventory and qualification evidence, not every distribution kernel. Its kernel release is `7.3+unreleased-arm64`; custom VP9 support is present. RGA2 and RGA3 appear as separate processing nodes with different formats and controls.

| Feature | Silicon / vendor stack | Supplied V4L2 interface | Bridge status |
| --- | --- | --- | --- |
| H.264 / HEVC decode | Rockchip MPP lists RK3588's high-resolution decoder and 10-bit-capable decode blocks | S264/S265; profiles/geometry and NV12/NV15/NV16/NV20 depend on selected SPS/node | Qualified in the recorded run, including reference pictures, packet layouts and paired fields |
| VP8 / MPEG-2 decode | Vendor legacy VPU paths | Hantro stateless VP8F/MG2S | Implemented |
| AV1 decode | Separate AV1 block/vendor path | Separate decoder node | Experimental, pending further bridge qualification |
| VP9 decode | Vendor high-resolution decoder path | VP9F is in this custom-kernel inventory; do not infer this from generic upstream version numbers | Profile 0/2 passed the eight-frame decode fixtures in the recorded run |
| H.264 / HEVC / VP8 encode | Vendor codec blocks exist; availability is chip/backend dependent | No corresponding encoder nodes in the supplied inventory | Kernel interface missing for these paths; no VA encoder advertised |
| JPEG encode | Vendor VPU/JPEG encoder paths | JPEG CAPTURE with YUV420M/NV12M and packed-422 raw menu entries | Four 420 public inputs implemented; packed-422 input remains bridge work |
| JPEG decode | Vendor JPEG decoder | Not exposed as a supported decoder in the supplied inventory | Requires a kernel-facing decoder backend before a bridge translator can use it |
| RGA2 / RGA3 | Vendor librga exposes richer 2D operations and backend-dependent formats | Raw M2M nodes; RGA2 has rotate/flip controls, supplied RGA3 does not | Both discovered; selection is per job with isolated compatibility quirks |
| Multicore decode/encode and RGA scheduling | Vendor stack contains multicore facilities | No bridge-usable aggregate scheduling contract established by this inventory | Pending kernel/interface work and bridge scheduling; no multicore claim |
| AFBC / protected / opaque buffers | Vendor/backend-dependent facilities | No validated end-to-end contract for this bridge | Unsupported; never relabelled LINEAR |
| Blend / HDR / advanced postprocessing | Vendor operations vary by IP/backend | Required controls absent from the collected RGA interface | Cannot enable by setting VA capability bits |

The pinned vendor SoC table also assigns legacy H.263/MPEG-4/AVS, AVS+/AVS2 and JPEG blocks to RK3588. These are not bridge capabilities: the supplied inventory does not establish usable matching interfaces/translators for those codecs. The vendor VDPU381 record includes 8K/10-bit capability, raster/AFBC16x16 storage, downscaling and two cores; the separate AV1 record is narrower (4K, 8-bit in this pinned table). Vendor configuration flags are neither a throughput benchmark nor a guarantee that each codec/profile shares every limit. MPP's richer encode and postprocessing facilities are outside the current V4L2 backend.

See the [complete kernel menu inventory](inventory.md) for every enumerated raw/coded FourCC and each bridge interpretation. VP9 results apply to the recorded fixtures and custom kernel; multicore support remains pending.

The vendor comparison records silicon capabilities. Features without a corresponding V4L2 interface remain unavailable to this driver.

## Test results

The latest hardware report, `run-cyyqnnyk`, ran on an Orange Pi 5 Plus with aarch64 kernel `7.3+unreleased-arm64`. The source was commit `2743db3c36c961a7df40cf10150ba161ca591506` with local changes. Driver SHA256: `eebed318d965b2d657b93d3996e940f7faac16d16c9d1bff885526f2e4a51069`. GPU copy was disabled with `VAV4L2_GPU_COPY=0`.

| Checks | Result |
| --- | --- |
| Main unit suite | 69/69 passed |
| H.264, HEVC Main/Main10, VP9 Profile 0/2, VP8 and MPEG-2 | Basic decode checks passed |
| Native and GBM matrix | Selected decode, JPEG, VPP and unit checks passed |
| Image and PRIME transfers | Selected checks passed, including all 22 formats in the PRIME3 completion group |
| Imported targets, direct CAPTURE and renderer | Selected groups passed |
| Overall | 1469 PASS / 0 FAIL / 72 SKIP; complete report, exit 0 |

Counts include group gates and component checks across native-export and GBM modes. Skips include experimental AV1 cases and unavailable renderer/transport combinations; they are not qualified features. The tests used the Request backend; stateful decoding and GPU copying were not exercised.

## Extended regression profile

```sh
python3 tools/test.py --suite acceptance --render-node /dev/dri/renderD128
```

This profile covers the recorded interface set: public decode pixels and references, native/GBM export, imported and direct CAPTURE, VPP, JPEG encode, FD lifetime, 4K pools and EGL rendering. It includes implementation-specific expectations for this system.

Dependencies include FFmpeg with libx264/libx265/libvpx and the encoders used by the fixtures, `vainfo`, `v4l-utils`, `gstreamer1.0-tools`, the GStreamer bad plugins providing `v4l2slmpeg2dec`, and EGL/GLES development packages. Missing mandatory tools or features fail this profile. Experimental AV1 cases are reported separately as SKIP.

The outer report contains `hardware.tar.gz` with the detailed pixel checks, traces, generated JPEGs, source/binary identity and layout diagnostics. The [general hardware profile](../../testing.md) selects acceptance groups from the available interfaces and also checks decode, JPEG and VPP independently.

Sources: [Rockchip MPP capability table](https://github.com/rockchip-linux/mpp/blob/20959ec3dabb313b8c33ca0b7b09c06ffd2a9caa/osal/mpp_soc.c), [librga](https://github.com/airockchip/librga/tree/2b32edcb97b601b25683e2941d888c8515da6d55).
