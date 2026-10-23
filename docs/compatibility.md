# Compatibility modules

The shared driver implements public V4L2 formats, controls, queue handling and VA translation. Compatibility modules handle measured implementation exceptions and allocation providers. They are compiled into the same package and selected automatically for each discovered node.

The shared implementation handles standard submission modes, start codes, pixel layouts, plane topology and negotiated geometry. Modules can restrict or rank codec candidates, prepare codec formats and controls, maintain per-context codec state, supply video processing (VPP) policies and provide shared-storage allocation.

## Interface

The source interface is declared in `src/compat/compat.h`. A `vareq_compat_module` has a unique name, an API version, a pure identity matcher and optional codec, processing and storage callbacks. Matching receives NUL-terminated `driver`, `card` and `bus_info` strings plus the reported version. Use the narrowest reliable identity for the measured behavior. A version number alone does not establish behavior on a kernel with backports.

| Entry | Contract |
| --- | --- |
| `match` | Identify affected kernel implementations; do not open devices or submit work |
| `codec_score` | Reject a codec candidate with a negative result or return a nonnegative preference adjustment after common capability checks; receives decode/encode direction, coded FourCC and a VA profile when known |
| `codec_setup`, `codec_finish` | Paired callbacks for per-context module state; setup runs before codec queues are configured, and finish unwinds in reverse order before the device closes, including partial setup failures |
| `codec_format` | Prepare plane storage and colorimetry before codec S_FMT; queue type, coded/raw format, picture dimensions, field and physical plane count remain unchanged |
| `codec_controls` | Prepare values and payload contents before global/request control submission; batch count, control IDs/sizes/pointers and request association remain unchanged |
| `behaviors` | Independent mirror-axis mapping, STREAMON-before-QBUF, exact importer span and retained-stream requirements |
| `vpp_score` | Reject an affected job with a negative result, or return a nonnegative preference adjustment after common format/control eligibility checks; the job includes the requested input/output quantization |
| `storage_score` | Reject an ineligible storage request or rank this provider; it must be paired with `allocate` |
| `allocate` | Receives the required row-byte span, row count and coherent-provider request; returns an owned CLOEXEC FD or -1 without leaving a submitted job or temporary resources |

Multiple matching policies compose. Behavior requirements are combined and every VPP restriction must accept the job. A preference adjustment cannot restore a missing format or control. Codec restrictions compose in the same way; equal codec scores retain common profile/geometry ranking and discovery order. Duplicate names reject registry resolution. For a matching module, unknown behavior bits, incompatible API versions or incomplete provider or codec-lifecycle registration reject resolution without publishing partial policy; at most eight modules may match. An unrelated module with a version mismatch does not disable another device.

The shared storage allocator tries eligible providers by descending score, retaining registration/discovery order for ties. A provider failure proceeds to the next candidate, including another matching device. Returned FDs must cover the requested span and be CLOEXEC. Callers handle allocation failure through their supported backing paths.

`ops->export_capture` supplies the validated MMAP/EXPBUF allocation sequence. It checks returned format, physical plane count, sizeimage, queue/buffer properties, allocation size and FD ownership. Its `exact` argument additionally bounds allocation to the requested page span. Providers describe allocation geometry without changing the public surface layout. They must not decode or process dummy frames to allocate memory.

## Codec integration

Decoder discovery checks the standard coded format, required controls and Request API before applying module preference. Encoder discovery checks the coded CAPTURE and raw OUTPUT contract first. A restriction can remove an affected candidate or VA profile; it cannot advertise an absent kernel capability. Cached selections are checked against current module restrictions.

A codec context resolves modules from its opened node's identity. Its source API version is `VAREQ_COMPAT_API_VERSION` (currently 2). Each matching module has separate context state; there is no process-wide device-state override. A failed setup calls finish for that module and all earlier setups. The shared backend owns the video/media/request FDs, queue lifetime and buffer mappings.

Format callbacks run for compressed OUTPUT, native/direct CAPTURE and staged/direct JPEG input and output negotiation. Standard kernel-returned format, geometry and buffer checks still apply. A callback cannot turn an unsupported format into another advertised layout. Controls pass through the same preparation path for H.264, HEVC, AV1, VP8, MPEG-2 and JPEG. Single controls retain S_CTRL submission; batches retain their global or request-scoped S_EXT_CTRLS submission.

Callbacks run in registry order. They must set errno on failure, keep payload storage alive through submission and leave queue/FD ownership with the shared backend. Preparation failure prevents the ioctl. Shared backend error and recovery handling remains in effect. Payload contents can be modified in place, so modules must not assume rollback of pointed-to data after a later callback rejects a batch. An initialized context retains its matching policy until teardown. Modules without codec callbacks preserve the common codec path.

Source modules handle measured implementation exceptions. Additional standard codec translators, submission modes and pixel layouts belong in the shared backend and require their own negotiation and validation.

## Add a source module

1. Add a directory below `src/compat/` containing the module source and `meson.build`.
2. Define one or more `const struct vareq_compat_module` descriptors using `VAREQ_COMPAT_API_VERSION`.
3. Append the source and descriptor symbols to `compat_sources` and `compat_symbols` in that directory's `meson.build`.
4. Add its `subdir()` entry to `src/compat/meson.build` before registry declarations and entries are generated.
5. Add regressions for matching, unaffected devices, codec/profile rejection, context state, format/control preparation, affected jobs, allocation failure and cleanup. Record hardware evidence in a device report.

For example, a module exporting `vareq_compat_example` uses:

```meson
compat_sources += files('module.c')
compat_symbols += ['vareq_compat_example']
```

Meson generates registry declarations and entries. A source module consists of its directory, registration change and tests. Installed standalone `.so` modules are not supported; the source API is internal and its version must match the driver being built.

## Module catalog

The [module catalog](compat/README.md) lists the bundled implementations, their matching rules and the additional behavior or storage path each provides. Codec, format and control support is still probed independently.

## Regression coverage

`tests/compat_fixture.c` is an independently compiled extension used by `test_compatibility`. Test-only registry substitution connects it to the actual codec, VPP and allocator consumers. Checks cover narrow matching, composed policies, atomic rejection of invalid registrations, required formats/controls, a mirror-only policy without unrelated DMA behavior, successful allocation, rejection/closure of invalid provider FDs, coherent-request propagation and fallback to a second device. Codec checks cover decoder selection and encoder sessions, profile restrictions, common OUTPUT setup, control values, forbidden format/request/control-identity changes, setup failure cleanup, device closure and retained kernel errors. The fixture is never linked into the installed driver.

Sources: [stateless decoder interface](https://docs.kernel.org/userspace-api/media/v4l/dev-stateless-decoder.html), [stateless controls](https://docs.kernel.org/userspace-api/media/v4l/ext-ctrls-codec-stateless.html), [device identity](https://docs.kernel.org/userspace-api/media/v4l/vidioc-querycap.html).

Color range is negotiated through the standard V4L2 colorimetry fields. Returned colorimetry must match the request before processing succeeds; implementation-specific restrictions stay in the relevant module.
