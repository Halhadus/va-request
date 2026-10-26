# Supplied RK3588 kernel inventory

This is a capability snapshot of the supplied 23 October 2026 hardware report, kernel `7.3+unreleased-arm64`. Device numbers are intentionally omitted: identities describe observed nodes, not hardcoded assignments. Default CAPTURE enumeration is sequence-dependent and does not exhaust post-SPS formats. A node/menu entry is not proof of every supported combination or of pixel correctness. Recollect with `python3 tools/collect_capabilities.py` after kernel changes.

## Nodes and queue menus

| Observed card | Kernel driver | OUTPUT FourCCs | CAPTURE FourCCs at default state |
| --- | --- | --- | --- |
| rga2 | rockchip-rga | `BA24`, `AR24`, `XR24`, `RGB3`, `BGR3`, `AR12`, `AR15`, `RGBP`, `NV21`, `NV61`, `NV12`, `NM12`, `NV16`, `YU12`, `422P`, `YV12` | `BA24`, `AR24`, `XR24`, `RGB3`, `BGR3`, `AR12`, `AR15`, `RGBP`, `NV21`, `NV61`, `NV12`, `NM12`, `NV16`, `YU12`, `422P`, `YV12` |
| snps_hdmirx | snps_hdmirx |  | `BGR3` |
| rga3 | rockchip-rga | `RGB3`, `BGR3`, `AR24`, `AB24`, `XR24`, `XB24`, `RGBP`, `NM12`, `NV12`, `NM21`, `NV21`, `NM16`, `NV16`, `NM61`, `NV61`, `YUYV`, `YVYU`, `UYVY`, `VYUY`, `BA24`, `RA24`, `BX24`, `RX24` | `RGB3`, `BGR3`, `AR24`, `AB24`, `XR24`, `XB24`, `RGBP`, `NM12`, `NV12`, `NM21`, `NV21`, `NM16`, `NV16`, `NM61`, `NV61`, `YUYV`, `YVYU`, `UYVY`, `VYUY` |
| rkvdec | rkvdec | `S265`, `S264`, `VP9F` | `NV12` |
| rockchip,rk3568-vpu-dec | hantro-vpu | `S264`, `MG2S`, `VP8F` | `NV12` |
| rockchip,rk3588-vepu121-enc | hantro-vpu | `YM12`, `NM12`, `YUYV`, `UYVY` | `JPEG` |
| rockchip,rk3588-av1-vpu-dec | hantro-vpu | `AV1F` | `VT12`, `NV12` |

The HDMI receiver is a capture interface, not a memory-to-memory codec/processor, and is excluded from bridge discovery. The two RGA nodes offer different direction-specific format sets. RGA2 exposes rotate/horizontal-flip/vertical-flip; the supplied RGA3 control set does not expose those transforms. New format or control removal is handled through current discovery and job negotiation rather than a static board inventory.

## Every enumerated FourCC

| Kernel FourCC | Kernel description | Bridge interpretation |
| --- | --- | --- |
| `422P` | Planar YUV 4:2:2 | Planar 422 transport for 422H/YV16 |
| `AB24` | 32-bit RGBA 8-8-8-8 | VA RGBA; opaque VPP input |
| `AR12` | 16-bit ARGB 4-4-4-4 | Kernel ARGB4444 only; public bridge layout pending |
| `AR15` | 16-bit ARGB 1-5-5-5 | Kernel ARGB1555 only; public bridge layout pending |
| `AR24` | 32-bit BGRA 8-8-8-8 | VA BGRA; opaque VPP input |
| `AV1F` | AV1 Frame, compressed | Experimental opt-in AV1; not normal acceptance |
| `BA24` | 32-bit ARGB 8-8-8-8 | VA ARGB where each queue direction supports it; opaque VPP input |
| `BGR3` | 24-bit BGR 8-8-8 | Kernel BGR24 only; public bridge layout pending |
| `BX24` | 32-bit XRGB 8-8-8-8 | Kernel XRGB byte layout only; no corresponding public bridge layout |
| `JPEG` | JFIF JPEG, compressed | JPEG encoded CAPTURE; no generic JPEG decoder |
| `MG2S` | MPEG-2 Parsed Slice Data, compressed | MPEG-2 translator |
| `NM12` | Y/UV 4:2:0 (N-C | NV12M physical two-plane storage; implemented for eligible VPP/JPEG transport and decoder CAPTURE |
| `NM16` | Y/UV 4:2:2 (N-C | NV16M present in kernel; no completed direct bridge transport |
| `NM21` | Y/VU 4:2:0 (N-C | NV21M eligible two-plane VPP transport |
| `NM61` | Y/VU 4:2:2 (N-C | NV61M present in kernel; no completed direct bridge transport |
| `NV12` | Y/UV 4:2:0 | Implemented 420 layout; native capture, eligible VPP/JPEG and PRIME |
| `NV16` | Y/UV 4:2:2 | Native 422 capture / staged planar-422 VPP transport |
| `NV21` | Y/VU 4:2:0 | Implemented image/PRIME/VPP; JPEG may convert/stage |
| `NV61` | Y/VU 4:2:2 | Staged YV16 VPP transport; not an additional public VA layout |
| `RA24` | 32-bit ABGR 8-8-8-8 | Kernel ABGR byte layout only; no corresponding public bridge layout |
| `RGB3` | 24-bit RGB 8-8-8 | Kernel RGB24 only; public bridge layout pending |
| `RGBP` | 16-bit RGB 5-6-5 | RGB565 Image/PRIME/VPP; distinct from VA planar RGBP |
| `RX24` | 32-bit XBGR 8-8-8-8 | Kernel XBGR byte layout only; no corresponding public bridge layout |
| `S264` | H.264 Parsed Slice Data, compressed | H.264 translator; exact profile/controls/stream negotiation required |
| `S265` | HEVC Parsed Slice Data, compressed | HEVC Main/Main10 translator |
| `UYVY` | UYVY 4:2:2 | Image/PRIME/VPP; JPEG packed-input implementation pending |
| `VP8F` | VP8 Frame, compressed | VP8 translator |
| `VP9F` | VP9 Frame, compressed | Request Profile 0/2 passed the recorded eight-frame fixtures |
| `VT12` | Y/UV 4:2:0 (4x4 Linear | Recorded interface format; no matching public bridge path is claimed |
| `VYUY` | VYUY 4:2:2 | Image/PRIME/VPP |
| `XB24` | 32-bit RGBX 8-8-8-8 | VA RGBX |
| `XR24` | 32-bit BGRX 8-8-8-8 | VA BGRX |
| `YM12` | Planar YUV 4:2:0 (N-C | YUV420M eligible JPEG three-object input |
| `YU12` | Planar YUV 4:2:0 | I420 image/PRIME/VPP; eligible staged transport |
| `YUYV` | YUYV 4:2:2 | YUY2 Image/PRIME/VPP; JPEG packed-input implementation pending |
| `YV12` | Planar YVU 4:2:0 | YV12 logical image/PRIME; canonicalized queue transport |
| `YVYU` | YVYU 4:2:2 | Image/PRIME/VPP |

NV15/NV16/NV20 in the decoder implementation must be considered after compatible SPS controls, even if absent from a fresh-fd CAPTURE list. For the complete public VA/DRM mapping, plane topology, bit packing and staging restrictions see [formats](../../formats.md). Extra RGB/alpha and multiplanar kernel menu entries remain visible in inventory without being silently added as unsupported VA layouts.

VP9 results apply to the recorded fixtures on this custom kernel; they do not establish support in every upstream/distribution RK3588 kernel. Multicore kernel/interface integration and bridge scheduling remain pending.
