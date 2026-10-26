# Decoding

The decoder backend is selected from the coded formats and queue capabilities of the opened V4L2 node. Both backends use the same VA config, context, surface and buffer validation. Codec support and native CAPTURE layouts are listed in the [support matrix](support.md).

| Backend | Input to the kernel | Decode state | Completion |
| --- | --- | --- | --- |
| Stateless | Parsed-frame/slice formats and controls attached to media requests | Codec translators maintain reference pictures and submit per-picture parameters | Native unexported jobs may remain Rendering after `vaEndPicture`; held fields, imported CAPTURE and previously exported targets use synchronous submission |
| Stateful | H.264/HEVC Annex-B access units, complete VP9 frames or AV1 temporal units | The kernel parses the bytestream and maintains its DPB | CAPTURE timestamps identify completed VA surfaces; `vaEndPicture` can return while a picture is Rendering |

## Backend selection

Stateless decoding requires the coded FourCC, mandatory controls, OUTPUT queue Request support and an associated media node. Stateful decoding requires the coded bytestream FourCC, a supported profile menu, source-change events and decoder STOP support. The coded OUTPUT format is selected before querying codec controls, Request queue capabilities or stateful profile menus. When a node supports both implemented protocols, Request decoding takes precedence.

Selection uses the node's capabilities rather than its path or device model. Unsupported profiles are omitted from VA queries. The application handles software codec fallback.

## Stateless decoding

`src/v4l2/stateless.c` owns Request initialization, submission, capture reconfiguration and recovery. The H.264, HEVC, VP9, AV1, VP8 and MPEG-2 translators prepare codec controls and validate retained references. H.264/HEVC prefer frame-based Annex-B submission when available; slice-only nodes use their supported start-code mode and per-slice controls. AV1 is experimental and opt-in. Request decoding recovers refresh flags from complete sequence/frame OBU metadata, including frame IDs, decoder-model timing and temporal/spatial operating-point membership. Header fields are checked against VA parameters before committing metadata. Raw shown key and switch pictures have a specification-defined refresh mask. Other raw-tile pictures without this metadata return unsupported rather than guessing an encoder/GOP policy. Film-grain controls are queried and sent only when available; requested grain without that control is unsupported.

### Queue lifecycle

1. Open the associated media node, select coded OUTPUT, allocate/map compressed buffers and configure CAPTURE when sequence controls are available.
2. Set sequence controls before negotiating the raw format. Validate dimensions, physical plane count, pitch and sizeimage. Choose native CAPTURE, eligible direct targets or an internal DMA-BUF pool.
3. Bind coded OUTPUT and per-picture controls to the media request. Queue CAPTURE independently, without attaching it to the request.
4. Submit requests up to the coded OUTPUT pool capacity. Match native CAPTURE by timestamp and validate the completed slot, memory type, plane count, payload bounds and offsets. A surface becomes Ready only after its CAPTURE, OUTPUT and request have completed. Reinitialize completed requests for reuse.
5. Retain decoded CAPTURE slots for reference pictures and export. Pending jobs pin their targets and references against reuse or destruction. Sync waits for completion; zero-timeout sync only pumps ready work. Sequence/queue changes and held fields drain outstanding jobs first. Failed submission resets the queues; failed ownership recovery closes the device and requires context recreation.

Slice-based H.264/HEVC build an owned plan for the complete VA picture before setting sequence controls or queuing OUTPUT. Slice mode carries one logical slice per request. Intermediate requests retain the same CAPTURE index with HOLD_CAPTURE; only the final request completes the surface. HEVC frame mode can carry all slices in one OUTPUT buffer with a dynamic slice-parameter array and concatenated entry-point offsets, bounded by the advertised control dimensions. HEVC entry-point offsets are parsed from the stream and checked against the advertised control capacity. A malformed later slice cannot leave an earlier batch queued. Slice-based H.264 currently supports frame pictures.

VP9 reads the uncompressed header for quantizer values, loop-filter deltas and segmentation features, and the boolean-coded header for probability updates and reference mode. The optional compressed-header control is sent when advertised. Persistent values are committed only after successful decode and storage attachment. Reference validation checks decoded storage ownership; timestamp zero is valid.

Paired H.264 PAFF fields require `V4L2_BUF_CAP_SUPPORTS_M2M_HOLD_CAPTURE_BUF`. The first field completes its OUTPUT/request while holding CAPTURE. The second uses the same CAPTURE index and timestamp; the surface becomes Ready only after the full pair completes. A format or coded-size change stops the queues and rebuilds the affected buffers before submission resumes.

## Stateful decoding

`src/v4l2/stateful.c` owns bytestream queue initialization, source changes, completion and drain/resume. `src/codecs/bytestream.c` assembles packets; the H.264/HEVC/AV1 normalizers and VP9 translator own codec syntax. Packets and candidate state are prepared before OUTPUT is changed. Codec state is committed only after QBUF succeeds.

### Syntax

H.264 supports 8-bit 4:2:0 Constrained Baseline, Main and High frame pictures, including frame-coded MBAFF. SPS/PPS reconstruction preserves the coded slice PPS identity, reference defaults, scaling matrices, POC0/POC2 and frame geometry. POC1 uses equivalent POC0 syntax derived from resolved VA picture order counts. A sequence must start at IDR; ambiguous 16-bit POC wraps and POC1 MMCO5 are rejected. CABAC alignment and CAVLC trailing bits are regenerated without changing entropy-coded data. Separate field pictures, FMO, SP/SI and extension profiles are rejected.

HEVC supports Main and Main10 4:2:0, temporal sublayers, scaling lists, tiles and public PPS fields. The VA DPB is represented by picture-local short/long-term RPS and explicit reference-list mappings, so the original SPS RPS arrays are not required. Slice data offsets must identify the CABAC payload when normalization is needed. Invalid POCs, missing active references and unrepresentable long-term MSB cycles are rejected. Every VA picture requests output; a context starts at an IRAP and rejects unavailable RASL pictures after random access. VPS/SPS/PPS identities remain consistent with the slices.

Reconstruction retains entropy-coded data and regenerates emulation prevention when headers change. VP9 Profile 0 (8-bit) and Profile 2 (10-bit) require complete raw frames, including both headers. Invisible reference pictures use separate OUTPUT buffers for the hidden frame and a show-existing frame for a refreshed slot. Both carry the VA picture's timestamp and complete one surface. A show-existing-only VA submission is unsupported. VP8 and MPEG-2 use the stateless translators.

AV1 Main reconstructs sequence/frame OBUs for 8/10-bit 4:2:0 pictures and preserves tile payloads. It maps VA references to eight decoder slots. When all slots remain live, a picture can be decoded without a refresh and installed later by hidden replay once the next VA map identifies a retired slot. The replay and displayed picture share one temporal unit and timestamp. Film grain is serialized from resolved VA coefficients with complete updates and validated point counts/order. Large-scale tiles, more than 32 tiles and unrepresentable syntax are rejected. AV1 remains experimental and opt-in.

### Queue lifecycle

1. Subscribe to `V4L2_EVENT_SOURCE_CHANGE`, configure compressed OUTPUT, allocate/map coded buffers and stream OUTPUT.
2. Submit access units with unique timestamps. OUTPUT completion releases that buffer independently of picture completion.
3. On resolution source-change, query parsed CAPTURE, enumerate formats, prefer LINEAR NV12, NV12M or P010 for the submitted bit depth; implemented planar NV12 layouts and tiled NV12/P010 layouts can use converted surface storage and validate plane count, pitch, sizeimage and compose rectangle. Allocate the decoder minimum plus spare slots and queue CAPTURE before streaming it.
4. Route each nonempty CAPTURE result to its submitted VA surface using the copied timestamp, including timestamp zero. Reordered results complete their own surfaces. Missing/duplicate timestamps, short/error buffers and invalid ownership fail the context.
5. Keep decoded slots attached until the surface is reused or destroyed. The same index always refers to the same backing during streaming. Stateful VA exports use persistent surface backing because a later picture can complete in another CAPTURE slot. Existing imported/exported backing receives every update through the validated transfer path. Kernel slots retain their fixed allocations while streaming.

When both standard decoder display-delay controls accept delay zero and enable one, the backend sets them before allocating OUTPUT buffers. Sync then waits for timestamp-matched CAPTURE without starting a drain. Nodes without this control pair retain the drain/resume path.

`vaEndPicture` can return with a surface Rendering. Sync waits for that surface; a zero timeout only pumps ready work. Since VA decode has no end-of-stream callback, a blocking sync may drain submitted access units with `V4L2_DEC_CMD_STOP`; after CAPTURE LAST and all submitted OUTPUT completions, `START` resumes decoding without seeking. A timed-out wait retains pending ownership. The legacy untimed sync has a five-second diagnostic limit. No software codec is run inside the driver.

Source changes finish the old CAPTURE sequence through LAST, preserve completed surface snapshots and reconfigure CAPTURE while OUTPUT remains streaming. A CAPTURE restart also clears an explicit drain interrupted by the source change. Changes to the visible dimensions require a matching VA context; incompatible geometry is rejected. Queued surfaces cannot be destroyed. Reusing a completed surface waits for queued readers before releasing its native storage. A surface still being decoded or owned by an active VA picture remains busy. A queue/protocol failure closes the decoder and marks pending pictures failed; applications must recreate the context.

## Hardware testing

Run the [hardware suite](testing.md) on the target device to compare decoded frames with software output. Set `VAV4L2_DECODE_TRACE` to a file path for queue, request, source-change, completion and drain logs. Tests must cover the selected backend; results from one protocol do not qualify the other. Device results are listed under [tested hardware](hardware/README.md).

Sources: [Linux stateless decoder interface](https://docs.kernel.org/userspace-api/media/v4l/dev-stateless-decoder.html), [Linux stateful decoder interface](https://docs.kernel.org/userspace-api/media/v4l/dev-decoder.html), [compressed formats](https://docs.kernel.org/userspace-api/media/v4l/pixfmt-compressed.html), [libva decode ABI](https://github.com/intel/libva/tree/2.23.0/va).
