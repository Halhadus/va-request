# Compatibility modules

Modules are included in the driver and selected per node. The [source interface](../compatibility.md) describes matching, registration and callback contracts.

| Implementation | Source | Modules | Documentation |
| --- | --- | --- | --- |
| Rockchip RGA | `src/compat/rockchip-rga/` | `rockchip-rga2`, `rockchip-rga3` | [Processing policies and allocation](rockchip-rga.md) |
| Rockchip Hantro | `src/compat/rockchip-hantro/` | `rockchip-hantro-jpeg` | [CAPTURE allocation provider](rockchip-hantro.md) |

Kernel inventories and measured results are in the [hardware reports](../hardware/README.md).
