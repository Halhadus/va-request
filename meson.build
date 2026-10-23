project('va-request', 'c', version: '0.1.0', license: 'LGPL-2.1-or-later', meson_version: '>= 0.63', default_options: ['c_std=c11', 'warning_level=2', 'buildtype=release', 'b_ndebug=false'])
cc = meson.get_compiler('c')
libva_dep = dependency('libva', version: '>= 1.23.0')
libva_drm_dep = dependency('libva-drm', version: '>= 1.23.0')
libdrm_dep = dependency('libdrm', version: '>= 2.4.0')
if not cc.has_header_symbol('drm_fourcc.h', 'DRM_FORMAT_NV20', dependencies: libdrm_dep)
  error('libdrm headers defining DRM_FORMAT_NV20 are required')
endif
gbm_dep = dependency('gbm')
threads_dep = dependency('threads')
core_deps = [libva_dep, libva_drm_dep, libdrm_dep, gbm_dep, threads_dep]
inc = include_directories('src')
add_project_arguments('-D_GNU_SOURCE', language: 'c')
driverdir = get_option('driverdir')
if driverdir == ''
  driverdir = get_option('libdir') / 'dri'
endif
core_sources = files(
  'src/api/driver.c', 'src/api/images.c', 'src/api/unsupported.c',
  'src/v4l2/device.c',
  'src/v4l2/probe.c',
  'src/codecs/h264.c',
  'src/codecs/hevc.c',
  'src/codecs/av1.c',
  'src/codecs/vp8.c',
  'src/codecs/mpeg2.c',
  'src/memory/format.c',
  'src/memory/gbm.c',
  'src/memory/allocator.c',
  'src/memory/prime.c',
  'src/memory/capture_dmabuf.c',
  'src/memory/capture_pool.c',
  'src/codecs/jpeg.c',
  'src/vpp/vpp.c',
  'src/v4l2/codec.c',
)
subdir('src/compat')
core_sources += compat_sources
core = static_library('va_request_core', core_sources, gnu_symbol_visibility: 'hidden', include_directories: inc, dependencies: core_deps)
shared_library('v4l2request_drv_video', [], link_whole: core, dependencies: core_deps, name_prefix: '', install: true, install_dir: driverdir)
