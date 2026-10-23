# rockchip-hantro

Source: `src/compat/rockchip-hantro/`.

The `rockchip-hantro-jpeg` descriptor matches driver `hantro-vpu` and card `rockchip,rk3588-vepu121-enc`.

This allocation provider uses the matched implementation's variable-size JPEG CAPTURE allocation to obtain an exported buffer with the required page span. The descriptor ranks at 40, sets a 128-by-128 allocation geometry and requests `sizeimage = bytes × rows`. The common export helper checks the returned layout and exact page extent. Allocation uses S_FMT, REQBUFS, QUERYBUF and EXPBUF.

See the [module catalog](README.md) and [source interface](../compatibility.md).
