# Build and packaging

The driver is built against public libva, DRM/GBM and Linux UAPI headers. Compilation and package creation do not probe video hardware. Only hardware test commands open target devices.

```sh
meson setup build --prefix=/usr --buildtype=release
meson compile -C build
meson test -C build --suite unit --print-errorlogs
```

`tests=true` builds hardware-free regression suites. `hardware_tests=auto` builds VA acceptance clients and adds the EGL/GLES clients when their development packages are available. `enabled` requires those graphics dependencies; `disabled` omits the acceptance clients. None of these choices changes the production driver's codec or DMA-BUF paths. Test clients are not installed in the driver package.

## Debian

Install the dependencies listed in `debian/control`, then:

```sh
dpkg-buildpackage -us -uc -b
```

The binary package is `libva-v4l2`; the source package is `libva-v4l2`. The driver installs into the architecture's multiarch `dri` directory. Native package builds run the hardware-free tests; hardware tests are never part of package creation. `DEB_BUILD_OPTIONS=nocheck` skips the native test run.

## Cross-compilation

Use a target sysroot containing the same development dependencies and a Meson cross file for its compiler and pkg-config setup:

```sh
meson setup build-cross --cross-file target.ini --prefix=/usr
meson compile -C build-cross
DESTDIR="$PWD/stage" meson install -C build-cross
```

Tests may be compiled for the target without being executed on the build machine. Run them on the target, or configure an appropriate executable wrapper before using `meson test`. Debian uses `dpkg-buildpackage -a<architecture> -us -uc -b` with a configured cross toolchain and dependencies. The package recipe skips target test execution during a cross build.
