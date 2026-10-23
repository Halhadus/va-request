# rockchip-rga

Source: `src/compat/rockchip-rga/`.

| Module | Matching driver | Matching card |
| --- | --- | --- |
| `rockchip-rga2` | `rockchip-rga` | `rga2` or legacy `rockchip-rga` |
| `rockchip-rga3` | `rockchip-rga` | `rga3` |

The shared VPP code handles formats, dimensions, colorimetry, selection and controls. The RGA modules add the following implementation policies:

- RGA2: mirror-axis mapping, STREAMON-before-QBUF, retained DMA streams and exact importer page span. RGB-to-YUV processing is rejected because the tested path did not produce the requested colors.
- RGA3: crop/compose selection is limited to the origin and full output. Its job score prefers eligible color conversion.
- Both: RGB uses full range and YUV uses limited range on the qualified conversion paths. These restrictions apply only to matching nodes.

These policies apply after format and control checks, for both direct and staged jobs.

The allocation providers use CAPTURE buffers without submitting a processing job. RGA2 ranks at 80 and is excluded for a coherent-provider request. RGA3 ranks at 60 for an exact-fit allocation and 20 otherwise. Allocation spans and exported FD properties are checked by the shared allocator.

See the [module catalog](README.md) and [source interface](../compatibility.md).
