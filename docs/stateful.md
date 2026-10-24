# Stateful decoding

The stateful backend translates VA decode buffers into complete Annex-B access units for `V4L2_PIX_FMT_H264` and `V4L2_PIX_FMT_HEVC`. The kernel manages codec parsing and its DPB. Device selection checks profile menus, source-change events and decoder STOP support. Discovery selects the coded OUTPUT format before querying its profile menu. The stateless backend uses parsed-frame/slice formats, public controls and media requests.

## Syntax

H.264 supports 8-bit 4:2:0 Constrained Baseline, Main and High frame pictures, including frame-coded MBAFF. SPS/PPS reconstruction preserves the coded slice PPS identity, reference defaults, scaling matrices, POC0/POC2 and frame geometry. POC1 is unsupported because VA buffers lack the original SPS cycle arrays. Separate field pictures, FMO, SP/SI and extension profiles are rejected.

HEVC supports Main 8-bit, a single temporal sublayer, inline slice RPS, scaling lists, tiles and the implemented public PPS fields. VPS/SPS/PPS IDs are reconstructed consistently with the slice PPS ID. Original SPS ST/LT RPS arrays cannot be recovered from standard VA buffers; streams requiring them are rejected before OUTPUT submission. Optional slice output suppression is also rejected because VA surfaces require observable completion. Main10 requires an additional native 10-bit CAPTURE layout and is not advertised by this backend.

Reconstruction retains the entropy-coded slice bytes and emulation prevention. Parameter sets are generated from the active VA metadata. VP8, MPEG-2 and AV1 currently use the stateless translators. VP9 translation is pending.

## Queue lifecycle

1. Subscribe to `V4L2_EVENT_SOURCE_CHANGE`, configure compressed OUTPUT, allocate/map coded buffers and stream OUTPUT.
2. Submit access units with unique timestamps. OUTPUT completion releases that buffer independently of picture completion.
3. On resolution source-change, query parsed CAPTURE, enumerate formats, select implemented LINEAR NV12 and validate plane count, pitch, sizeimage and compose rectangle. Allocate the decoder minimum plus spare slots and queue CAPTURE before streaming it.
4. Route each nonempty CAPTURE result to its submitted VA surface using the copied timestamp, including timestamp zero. Reordered results complete their own surfaces. Missing/duplicate timestamps, short/error buffers and invalid ownership fail the context.
5. Keep decoded slots attached until the surface is reused or destroyed. The same index always refers to the same backing during streaming. Native NV12 export uses EXPBUF; imported VA targets use the existing validated transfer path.

`vaEndPicture` can return with a surface Rendering. Sync waits for that surface; a zero timeout only pumps ready work. Since VA decode has no end-of-stream callback, a blocking sync may drain submitted access units with `V4L2_DEC_CMD_STOP`; after CAPTURE LAST and all submitted OUTPUT completions, `START` resumes decoding without seeking. A timed-out wait retains pending ownership. The legacy untimed sync has a five-second diagnostic limit. No software codec is run inside the driver.

Source changes finish the old CAPTURE sequence through LAST, preserve completed surface snapshots and reconfigure CAPTURE while OUTPUT remains streaming. Changes to the visible dimensions require a matching VA context; incompatible geometry is rejected. Queued surfaces cannot be destroyed or retargeted. A queue/protocol failure closes the decoder and marks pending pictures failed; applications must recreate the context.

## Hardware testing

Run the [hardware suite](testing.md) on the target device to compare H.264/HEVC output with software-decoded frames. Set `VAV4L2_DECODE_TRACE` to a file path for source-change, queue, completion and drain logs. Device results are listed under [tested hardware](hardware/README.md).

Sources: [Linux stateful decoder interface](https://docs.kernel.org/userspace-api/media/v4l/dev-decoder.html), [compressed formats](https://docs.kernel.org/userspace-api/media/v4l/pixfmt-compressed.html), [libva decode ABI](https://github.com/intel/libva/tree/2.23.0/va), [GStreamer V4L2 decoder](https://github.com/GStreamer/gstreamer/blob/79884e3c50a3538dbaf6bed10d4be2885120d35c/subprojects/gst-plugins-good/sys/v4l2/gstv4l2videodec.c).
