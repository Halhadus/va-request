# Build and packaging

The driver is built against public libva, DRM/GBM and Linux UAPI headers. Its entry point matches the libva ABI used for the build. EGL/GLES libraries are loaded only when the copy worker starts, keeping graphics dependencies out of VA driver initialization. Compilation, package creation and regression tests do not require video hardware. Hardware test commands run against target devices.

```sh
meson setup build --prefix=/usr --buildtype=release
meson compile -C build
meson test -C build --suite unit --print-errorlogs
```

`tests=true` builds hardware-free regression suites. `hardware_tests=auto` builds VA acceptance clients and adds the EGL/GLES clients when their development packages are available. `enabled` requires those graphics dependencies; `disabled` omits the acceptance clients. `gpu_copy=auto` also builds the production copy worker when EGL/GLES development packages are available; `enabled` requires them and `disabled` omits the worker. Runtime copying remains off unless `VAV4L2_GPU_COPY=1` is set. Test clients are not installed in the driver package.

## Debian

Install the dependencies listed in `debian/control`, then:

```sh
dpkg-buildpackage -us -uc -b
```

The source and binary package are `libva-v4l2`. The driver installs into the architecture's multiarch `dri` directory. Native package builds run the hardware-free tests; hardware tests are never part of package creation. `DEB_BUILD_OPTIONS=nocheck` skips the native test run.

## musl and Alpine

The driver and tests build with glibc or musl. The shell test runners require Bash and support BusyBox utilities.

On Alpine, install the build dependencies with:

```sh
apk add build-base meson ninja python3 pkgconf linux-headers libdrm-dev mesa-dev libva-dev bash
```

libva 2.23 or newer is required. If the distribution package is older, build libva from upstream first. Use the Meson commands above to build and run the unit tests. APK packaging is not included.

Install `ffmpeg` for generated test streams and `v4l-utils` for capability collection. FFmpeg needs libx264/libx265 for the H.264/HEVC fixtures. See [testing](testing.md) for the full hardware test dependencies.

## Cross-compilation

Use a target sysroot containing the same development dependencies and a Meson cross file for its compiler and pkg-config setup:

```sh
meson setup build-cross --cross-file target.ini --prefix=/usr
meson compile -C build-cross
DESTDIR="$PWD/stage" meson install -C build-cross
```

Tests may be compiled for the target without being executed on the build machine. Run them on the target, or configure an appropriate executable wrapper before using `meson test`. Debian uses `dpkg-buildpackage -a<architecture> -us -uc -b` with a configured cross toolchain and dependencies. The package recipe skips target test execution during a cross build.
