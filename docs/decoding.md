# Decoding

The decoder backend is selected from the coded formats and queue capabilities of the opened V4L2 node. Both backends use the same VA config, context, surface and buffer validation. Codec support and native CAPTURE layouts are listed in the [support matrix](support.md).

| Backend | Input to the kernel | Decode state | Completion |
| --- | --- | --- | --- |
| Stateless | Parsed-frame/slice formats and controls attached to media requests | Codec translators maintain reference pictures and submit per-picture parameters | The request and both queues complete before `vaEndPicture` returns; a held first field remains Rendering |
| Stateful | H.264/HEVC Annex-B access units with reconstructed parameter sets | The kernel parses the bytestream and maintains its DPB | CAPTURE timestamps identify completed VA surfaces; `vaEndPicture` can return while a picture is Rendering |

## Backend selection

Stateless decoding requires the coded FourCC, mandatory controls, OUTPUT queue Request support and an associated media node. Stateful decoding requires the coded bytestream FourCC, a supported profile menu, source-change events and decoder STOP support. The coded OUTPUT format is selected before querying stateful profile menus. When a node supports both implemented protocols, Request decoding takes precedence.

Selection uses the node's capabilities rather than its path or device model. Unsupported profiles are omitted from VA queries. The application handles software codec fallback.

## Stateless decoding

`src/v4l2/stateless.c` owns Request initialization, submission, capture reconfiguration and recovery. The H.264, HEVC, AV1, VP8 and MPEG-2 translators prepare codec controls and validate retained references. H.264/HEVC use frame-based Annex-B Request submission. AV1 is experimental and opt-in.

### Queue lifecycle

1. Open the associated media node, select coded OUTPUT, allocate/map compressed buffers and configure CAPTURE when sequence controls are available.
2. Set sequence controls before negotiating the raw format. Validate dimensions, physical plane count, pitch and sizeimage. Choose native CAPTURE, eligible direct targets or an internal DMA-BUF pool.
3. Bind coded OUTPUT and per-picture controls to the media request. Queue CAPTURE independently, without attaching it to the request.
4. Submit the request, wait for completion, dequeue both queues and match the completed indices. Reinitialize the request for reuse.
5. Retain decoded CAPTURE slots for reference pictures and export. A failed submission resets the queues; failed ownership recovery closes the device and requires context recreation.

Paired H.264 PAFF fields require `V4L2_BUF_CAP_SUPPORTS_M2M_HOLD_CAPTURE_BUF`. The first field completes its OUTPUT/request while holding CAPTURE. The second uses the same CAPTURE index and timestamp; the surface becomes Ready only after the full pair completes. A format or coded-size change stops the queues and rebuilds the affected buffers before submission resumes.

## Stateful decoding

`src/v4l2/stateful.c` owns bytestream queue initialization, source changes, completion and drain/resume. `src/codecs/bytestream.c` reconstructs parameter sets from VA metadata and preserves the original coded slices.

### Syntax

H.264 supports 8-bit 4:2:0 Constrained Baseline, Main and High frame pictures, including frame-coded MBAFF. SPS/PPS reconstruction preserves the coded slice PPS identity, reference defaults, scaling matrices, POC0/POC2 and frame geometry. POC1 is unsupported because VA buffers lack the original SPS cycle arrays. Separate field pictures, FMO, SP/SI and extension profiles are rejected.

HEVC supports Main 8-bit, a single temporal sublayer, inline slice RPS, scaling lists, tiles and the implemented public PPS fields. VPS/SPS/PPS IDs are reconstructed consistently with the slice PPS ID. Original SPS ST/LT RPS arrays cannot be recovered from standard VA buffers; streams requiring them are rejected before OUTPUT submission. Optional slice output suppression is also rejected because VA surfaces require observable completion. Main10 requires an additional native 10-bit CAPTURE layout and is not advertised by this backend.

Reconstruction retains the entropy-coded slice bytes and emulation prevention. Parameter sets are generated from the active VA metadata. VP8, MPEG-2 and AV1 currently use the stateless translators. VP9 translation is pending.

### Queue lifecycle

1. Subscribe to `V4L2_EVENT_SOURCE_CHANGE`, configure compressed OUTPUT, allocate/map coded buffers and stream OUTPUT.
2. Submit access units with unique timestamps. OUTPUT completion releases that buffer independently of picture completion.
3. On resolution source-change, query parsed CAPTURE, enumerate formats, select implemented LINEAR NV12 and validate plane count, pitch, sizeimage and compose rectangle. Allocate the decoder minimum plus spare slots and queue CAPTURE before streaming it.
4. Route each nonempty CAPTURE result to its submitted VA surface using the copied timestamp, including timestamp zero. Reordered results complete their own surfaces. Missing/duplicate timestamps, short/error buffers and invalid ownership fail the context.
5. Keep decoded slots attached until the surface is reused or destroyed. The same index always refers to the same backing during streaming. Native NV12 export uses EXPBUF; imported VA targets use the existing validated transfer path.

`vaEndPicture` can return with a surface Rendering. Sync waits for that surface; a zero timeout only pumps ready work. Since VA decode has no end-of-stream callback, a blocking sync may drain submitted access units with `V4L2_DEC_CMD_STOP`; after CAPTURE LAST and all submitted OUTPUT completions, `START` resumes decoding without seeking. A timed-out wait retains pending ownership. The legacy untimed sync has a five-second diagnostic limit. No software codec is run inside the driver.

Source changes finish the old CAPTURE sequence through LAST, preserve completed surface snapshots and reconfigure CAPTURE while OUTPUT remains streaming. Changes to the visible dimensions require a matching VA context; incompatible geometry is rejected. Queued surfaces cannot be destroyed or retargeted. A queue/protocol failure closes the decoder and marks pending pictures failed; applications must recreate the context.

## Hardware testing

Run the [hardware suite](testing.md) on the target device to compare decoded frames with software output. Set `VAV4L2_DECODE_TRACE` to a file path for queue, request, source-change, completion and drain logs. Tests must cover the selected backend; results from one protocol do not qualify the other. Device results are listed under [tested hardware](hardware/README.md).

Sources: [Linux stateless decoder interface](https://docs.kernel.org/userspace-api/media/v4l/dev-stateless-decoder.html), [Linux stateful decoder interface](https://docs.kernel.org/userspace-api/media/v4l/dev-decoder.html), [compressed formats](https://docs.kernel.org/userspace-api/media/v4l/pixfmt-compressed.html), [libva decode ABI](https://github.com/intel/libva/tree/2.23.0/va).
