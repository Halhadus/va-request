# libva-v4l2

libva-v4l2 bridges VA-API and Linux V4L2 for hardware decoding, encoding, video processing and DMA-BUF sharing. Decoding uses stateful V4L2 memory-to-memory or stateless Request API interfaces; encoding and processing use memory-to-memory queues. Available operations are discovered at runtime.

See the [support matrix](docs/support.md) for codecs, formats and limitations, and the [hardware reports](docs/hardware/README.md) for results on tested devices.

## Install

Build dependencies: a C11 compiler, Meson 0.63 or newer, Ninja, pkg-config, libva 2.23 or newer, GBM, libdrm headers with `DRM_FORMAT_NV20`, and Linux headers with stateless AV1 controls.

```sh
meson setup build --prefix=/usr --buildtype=release
meson compile -C build
meson test -C build --suite unit --print-errorlogs
sudo meson install -C build
```

Select the driver in the application environment:

```sh
export LIBVA_DRIVER_NAME=v4l2
vainfo --display drm --device /dev/dri/renderD128
```

Use your DRM node in place of the example path. The module installs to `libdir/dri`; `-Ddriverdir` overrides that location. [Application settings](docs/applications.md) cover browser and player configuration.

Debian packaging is included. Builds and hardware-free tests can run on a separate machine; no video device is needed to produce the package. [Build and packaging](docs/building.md) includes cross-compilation.

## Tests

```sh
python3 tools/test.py --suite unit
python3 tools/test.py --suite hardware --render-node /dev/dri/renderD128
```

The hardware suite checks available decode profiles, JPEG encoding, video processing and buffer sharing, with unavailable coverage recorded as SKIP. Each run prints a `REPORT=...tar.gz` path containing JSON, JUnit, TSV and logs. [Testing](docs/testing.md) describes test coverage, dependencies and report contents.

## Reference

- [Supported operations and limitations](docs/support.md)
- [Formats](docs/formats.md)
- [Memory and DMA-BUF](docs/memory.md)
- [Environment variables](docs/environment.md)
- [Architecture](docs/architecture.md)
- [Decoding](docs/decoding.md)
- [Compatibility modules](docs/compatibility.md)
- [Tested hardware](docs/hardware/README.md)
- [Contributing](CONTRIBUTING.md)

## Acknowledgements

- [vaapi-v4l2-bridge](https://github.com/pty819/vaapi-v4l2-bridge), by liyifan and contributors, provided the initial VA frontend.
- [Radxa libva-v4l2](https://github.com/radxa-pkg/libva-v4l2) provided the H.264/HEVC/AV1 normalization and VP9 packet approach.
- [Omacom libva-v4l2_request](https://github.com/omacom/libva-v4l2_request) provided the VP9 Request parser and H.264/HEVC slice parsing and controls.

Maintained by Halhadus. The combined driver is licensed under GPL-3.0-or-later. Individual MIT and LGPL source notices are retained; see [COPYING](COPYING), [AUTHORS](AUTHORS) and [third-party notices](NOTICE).
