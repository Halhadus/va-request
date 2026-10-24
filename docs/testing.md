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

The hardware profile selects codecs and acceptance groups from VA queries and the current kernel inventory. No board identity is required. Missing devices or profiles are SKIP. A failed generation, decode, encode, processing operation or pixel comparison is FAIL. A selected acceptance group fails if a required test dependency is missing.

FFmpeg needs libx264, libx265, libvpx and the MPEG-2 encoder to cover all implemented nonexperimental decoders. The runner generates eight changing 320-by-192 frames per available profile and compares the complete NV12 output with software decoding. H.264, HEVC and VP8 require identical pixels. MPEG-2 allows a maximum sample error of 4 and mean error of 0.5 for IDCT rounding. The VA invocation requires hardware frames followed by `hwdownload`.

The `hardware` profile also runs JPEG encode and software readback, identity VPP pixels, and applicable acceptance groups. The `smoke` profile stops after the small decode checks.

## Acceptance groups

The report's `acceptance-plan.json` records selected groups and prerequisites. A skipped fixed fixture does not mean that every operation in that group is unavailable; independent decode, JPEG and VPP checks still run. The `acceptance` profile runs every group without filtering. `rk3588` remains an alias for that strict profile.

| Groups | Coverage and prerequisites |
| --- | --- |
| `features`, `extended`, `rows`, `complete` | Image transfers, VPP transforms, layout conversion, DMA-BUF completion, JPEG pixels and FD lifetime; require JPEG/VPP, the fixture's raw format menus and rotate/flip controls |
| `matrix` | 720p/1080p/4K decode, high bit depth, 4:2:2, reference pictures, packet layouts, JPEG and VPP; requires the complete codec and processing fixture set |
| `imported`, `direct-capture` | Imported targets, direct CAPTURE and 58 packet checks; require H.264 Main, HEVC Main/Main10, VP8 and MPEG-2 Main |
| `renderer` | Export lifetime, EGL import, GPU pixels and synchronization; requires the EGL/GLES client, with per-format runtime queries |
| `transport` | Queue memory types, layouts, allocator attachment and completion; requires a raw M2M node |

Selected groups run in native-export and GBM modes. Failures remain failures when a later diagnostic retry passes. The report includes case records from all acceptance TSV files, including renderer and transport checks, and `hardware.tar.gz` with pixel evidence and traces. Counts include both group gates and component checks; they are not counts of independent video streams.

The full fixture set needs FFmpeg with libx264/libx265/libvpx, `vainfo`, `v4l-utils`, GStreamer tools and bad plugins providing the stateless reference decoders, and EGL/GLES development packages. Device reports under [tested hardware](hardware/README.md) record results for particular kernels.

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

The optional `stateful_oracle` test checks Annex-B reconstruction without hardware. A test-only VA shim writes the reconstructed packets. FFmpeg decodes the original and reconstructed streams, and the test compares all eight frames of each stream. Cases cover H.264 Baseline/Main/High, scaling matrices, multiple slices, MBAFF and HEVC PPS identities. It requires FFmpeg with libx264/libx265, which CI installs.

Stateful queue tests use mocked ioctls to check source changes, timestamps, drain/resume, pending surfaces and failure cleanup. Run the hardware profile to check actual H.264/HEVC decoding and DMA-BUF behavior on a device.

Codec checks cover frame order, reference lifetime, fragmented buffers, slice arrays and field pairing. PAFF fixtures check 16 field submissions for eight output pictures, held CAPTURE ownership and reference pixels. MPEG-2 extended checks also use a same-kernel IDCT reference.

Memory and processing checks cover changing frames, poisoned destinations, cold/warm sessions, reverse transfers, early FD/surface destruction and public readback. Direct-transport checks inspect queue memory types, plane geometry and completion traces. Negative cases cover overlapping planes, short objects, synchronization failures, missing controls, removed formats and uncertain queue cleanup.

Test adapters repackage VA buffers to exercise different submission shapes. They are built only for testing and are not installed.

## Package builds

Debian builds run the hardware-free tests. `DEB_BUILD_OPTIONS=nocheck` skips them; cross builds skip target execution. See [build and packaging](building.md).
