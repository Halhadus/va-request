# Tests and reports

Run these commands from the repository root:

```sh
python3 tools/test.py --suite unit
python3 tools/test.py --suite unit --build-dir build-asan --sanitize
python3 tools/test.py --suite hardware --render-node /dev/dri/renderD128
```

Each run creates a directory below `verify/` and prints `REPORT=...tar.gz`. Send that archive when reporting a failure; it includes the results and diagnostic logs.

## Test profiles

| Profile | Coverage | Requirements |
| --- | --- | --- |
| `unit` | Lifecycle, fault injection, buffer ownership, queue recovery, formats, codec parameters, packet assembly, compatibility modules and report generation | Build dependencies and Python; no hardware required |
| `hardware` | Unit tests, discovery, decode pixels, JPEG encode/readback, VPP and available acceptance groups in native/GBM export modes | A usable VA display, FFmpeg and the dependencies of each selected group |
| `smoke` | Unit tests, discovery and eight-frame decode comparisons | A usable VA display and FFmpeg |
| `acceptance` | All acceptance groups with mandatory fixture prerequisites | The complete interface set and dependencies described below |
| `conformance` | Unit tests and hash-pinned external decode vectors, with optional comparison against previous results | A corpus manifest, a usable VA display and FFmpeg |

The hardware profile selects codecs and acceptance groups from VA queries and the current kernel inventory. No board identity is required. Missing devices or profiles are SKIP. A failed generation, decode, encode, processing operation or pixel comparison is FAIL. A selected acceptance group fails if a required test dependency is missing.

FFmpeg needs libx264, libx265, libvpx and the MPEG-2 encoder to cover all implemented nonexperimental decoders. The runner generates eight changing 320-by-192 frames per available profile and compares the complete NV12 or P010 output with software decoding. H.264, HEVC, VP8 and VP9 require identical pixels. Explicit AV1 advertisement also enables 8/10-bit decode checks using libaom-av1 fixtures; these require identical pixels and remain experimental. MPEG-2 allows a maximum sample error of 4 and mean error of 0.5 for IDCT rounding. The VA invocation requires hardware frames followed by `hwdownload`.

The `hardware` profile also runs JPEG encode and software readback, identity VPP pixels, and applicable acceptance groups. The `smoke` profile stops after the small decode checks.

## Acceptance groups

The report's `acceptance-plan.json` records selected groups and prerequisites. A skipped fixed fixture does not mean that every operation in that group is unavailable; independent decode, JPEG and VPP checks still run. The `acceptance` profile runs every group without filtering. `rk3588` remains an alias for that strict profile.

| Groups | Coverage and prerequisites |
| --- | --- |
| `features`, `extended`, `rows`, `complete` | Image transfers and PRIME1/2 layouts across the 22 public formats, VPP transforms, layout conversion, DMA-BUF completion, JPEG pixels and FD lifetime; require JPEG/VPP, the fixture's raw format menus and rotate/flip controls |
| `matrix` | 720p/1080p/4K decode, high bit depth, 4:2:2, reference pictures, packet layouts, JPEG and VPP; requires the complete codec and processing fixture set |
| `imported`, `direct-capture` | Imported targets, direct CAPTURE and 58 packet checks; require H.264 Main, HEVC Main/Main10, VP8 and MPEG-2 Main |
| `renderer` | Export lifetime, EGL import, GPU pixels and synchronization; requires the EGL/GLES client, with per-format runtime queries |
| `transport` | Queue memory types, layouts, allocator attachment and completion; requires a raw M2M node |

Packed-format VPP fixtures use 128x128 images; format enumeration does not imply that a node accepts a 64x64 job. PRIME3 completion checks cover all 22 public formats once per composed/separate representation, with 32 import/lifetime cycles each.

Selected groups run in native-export and GBM modes. Failures remain failures when a later diagnostic retry passes. The report includes case records from all acceptance TSV files, including renderer and transport checks, and `hardware.tar.gz` with pixel evidence and traces. Counts include both group gates and component checks; they are not counts of independent video streams.

The full fixture set needs FFmpeg with libx264/libx265/libvpx, `vainfo`, `v4l-utils`, GStreamer tools and bad plugins providing the stateless reference decoders, and EGL/GLES development packages. Device reports under [tested hardware](hardware/README.md) record results for particular kernels.

With EGL/GLES development packages available, the default build includes the optional GPU copy worker. Run the existing hardware profile with `VAV4L2_GPU_COPY=1` to select this path. In the imported-target acceptance group, the H.264 case requires eight matching pictures and at least eight distinct successful `capture-gpu-copy` trace records. Missing GPU completion evidence fails the case. Direct CAPTURE DMA-BUF tests retain their own transport checks because they do not need a copy. Other native formats may use their supported transfer paths; this H.264 check does not qualify GPU copying of P010 or NV12M.

## Manual builds

```sh
meson setup build --buildtype=debug -Db_ndebug=false -Dhardware_tests=enabled
meson compile -C build
meson test -C build --suite unit --print-errorlogs
```

`--sanitize` enables AddressSanitizer and UndefinedBehaviorSanitizer for C tests and compiled test adapters. `hardware_tests=enabled` also builds the VA and EGL/GLES acceptance clients; see [build options](building.md).

## Report contents

| File | Contents |
| --- | --- |
| `report.json` | Versioned results, source and binary identity, profile, platform, case status and completeness |
| `junit.xml` | JUnit results for CI |
| `results.tsv` | NAME, STATUS, EXIT_CODE and DURATION_S columns |
| `summary.txt` | PASS/FAIL/SKIP counts, COMPLETE and EXIT |
| `configure.log`, `build.log`, `unit.log` | Build and test output |
| `testlog.json`, `testlog.txt`, `testlog.junit.xml` | Meson's individual test diagnostics |
| `<codec>-decode.tsv` | Basic hardware decode queue, source-change, timestamp and completion evidence |
| `hardware.tar.gz` | Extended-profile pixel results, traces, generated images and layout diagnostics, when applicable |

The JSON format is defined by [report.schema.json](../tools/report.schema.json), with identifier `libva-v4l2.test-report/v1`. Source identity includes the commit, tracked tree, dirty state and driver SHA256 when available. Source archives without `.git` have null Git fields. Shared Meson logs omit the inherited environment dump; their JSON environment retains only driver, libva and test instrumentation settings. Test output and failure diagnostics remain included.

`complete=false` indicates an interrupted run. A nonzero exit code indicates failure even if a report was created. SKIP records unavailable coverage. Successful diagnostic retries retain earlier failures in the report.

## Regression checks

The optional `stateful_oracle` test checks bytestream reconstruction and Request-header parsing without hardware. A test-only VA shim writes the reconstructed packets. FFmpeg decodes the original and reconstructed streams, and the test compares all eight frames of each stream. Cases cover H.264 Baseline/Main/High, POC1 CABAC/CAVLC, scaling matrices, multiple slices, MBAFF, HEVC Main/Main10, temporal layers, PPS identities and VP9 Profile 0/2 with shown and hidden key pictures when libvpx-vp9 is available. AV1 Main 8/10-bit, multiple-tile and resolved film-grain reconstruction are compared when libaom-av1 is available. The shim also parses the original H.264/HEVC slice controls and VP9 Request headers; reference availability is supplied by the shim rather than kernel CAPTURE storage. The test uses `-fps_mode passthrough` and requires FFmpeg with libx264/libx265, which CI installs. Request queue ownership, malformed later slices, native planar objects, rectangular tile addressing, AV1 timing metadata and GPU worker failure are covered by separate unit tests. These checks do not qualify hardware decoding or successful GPU transfers.

Stateless queue tests cover request completion, held CAPTURE, capture reconfiguration and recovery after submission failures. Stateful queue tests cover source changes, timestamps, drain/resume, pending surfaces and failure cleanup. VP9 hidden-picture cases check separate OUTPUT submissions, a one-buffer OUTPUT grant and failure of the second submission. Drain interrupted by source change, native reader-fence failures, stable stateful exports, malformed Request completions, P010-only menus and optional AV1 controls have fault regressions. Both backends use mocked ioctls. Run the hardware profile to check actual H.264/HEVC decoding and DMA-BUF behavior on a device.

Codec checks cover frame order, reference lifetime, fragmented buffers, slice arrays and field pairing. PAFF fixtures check 16 field submissions for eight output pictures, held CAPTURE ownership and reference pixels. MPEG-2 extended checks also use a same-kernel IDCT reference.

Memory and processing checks cover changing frames, poisoned destinations, cold/warm sessions, reverse transfers, early FD/surface destruction and public readback. Direct-transport checks inspect queue memory types, plane geometry and completion traces. Negative cases cover overlapping planes, short objects, synchronization failures, missing controls, removed formats and uncertain queue cleanup.

Test adapters repackage VA buffers to exercise different submission shapes. They are built only for testing and are not installed.

## Package builds

Debian builds run the hardware-free tests. `DEB_BUILD_OPTIONS=nocheck` skips them; cross builds skip target execution. See [build and packaging](building.md).

## Conformance vectors

The `conformance` profile runs an external corpus from a JSON manifest. Each vector declares `name`, `path`, `sha256`, `profile`, `width`, `height`, `frames` and `format` (`nv12` or `p010le`). The manifest uses `schema: "libva-v4l2.conformance/v1"`, a corpus `origin` and a `vectors` array. Paths are relative to the manifest. Keep corpus source, version and redistribution terms with the manifest; media files are not bundled.

```sh
python3 tools/test.py --suite conformance --manifest /path/to/corpus.json --build-dir build --render-node /dev/dri/renderD128
python3 tools/test.py --suite conformance --manifest /path/to/corpus.json --build-dir build --previous /path/to/previous/report.json
```

The standard report archive records source/binary identity, corpus origin, compressed hashes, exact frame counts, pixel equality and driver completion evidence. `hwdownload` requires VA output frames, so software fallback cannot pass as hardware decode. A previous pass becoming FAIL, SKIP, absent or a different input is reported as a regression even if total pass counts stay equal. These detailed reports stay with the run; hardware documentation keeps one current device summary.

`test_packet_validation [seed] [iterations]` replays bounded H.264/HEVC/AV1 header mutations. Rejected inputs must leave OUTPUT bytes, packet size and committed codec state unchanged and must not issue an ioctl. It complements the pixel oracle and reference/queue fault tests.

Conformance streams can be repeated inside one FFmpeg decoder session with `python3 tools/test.py --suite conformance --manifest corpus.json --repeat 1000`. Decode completion traces are read incrementally and pixel hashes use bounded reads. On Linux the existing runner samples the FFmpeg process and records peak RSS, peak open FDs and sample count separately for hardware and software; these observations do not establish that a stream is leak-free. The manifest still identifies the original compressed file and its frame count; the report records iterations and verifies every repeated output frame and driver completion. A shorter repeat count cannot preserve a previous longer qualification through `--previous`. Test media and their licenses remain external to the source package.

`build/test_packet_validation SEED ITERATIONS` extends the deterministic malformed-packet checks without a separate runner. The default unit invocation remains bounded. Request queue regressions cover independent completion, timestamp routing, retained references, zero-timeout sync and queue cancellation.
