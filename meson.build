project('libva-v4l2', 'c', version: '0.1.0', license: 'GPL-3.0-or-later', meson_version: '>= 0.63', default_options: ['c_std=c11', 'warning_level=2', 'buildtype=release', 'b_ndebug=false'])
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
  'src/v4l2/queue.c',
  'src/v4l2/probe.c',
  'src/v4l2/stateful.c',
  'src/v4l2/stateless.c',
  'src/codecs/h264.c',
  'src/codecs/hevc.c',
  'src/codecs/bytestream.c',
  'src/codecs/h264_bytestream.c',
  'src/codecs/hevc_bytestream.c',
  'src/codecs/av1_bytestream.c',
  'src/codecs/av1.c',
  'src/codecs/vp8.c',
  'src/codecs/vp9.c',
  'src/codecs/vp9_request.c',
  'src/codecs/request_slices.c',
  'src/codecs/h264_request.c',
  'src/codecs/hevc_request.c',
  'src/codecs/mpeg2.c',
  'src/memory/format.c',
  'src/memory/gbm.c',
  'src/memory/allocator.c',
  'src/memory/drm.c',
  'src/memory/prime.c',
  'src/memory/capture_dmabuf.c',
  'src/memory/capture_pool.c',
  'src/memory/capture_layout.c',
  'src/codecs/jpeg.c',
  'src/vpp/vpp.c',
  'src/v4l2/codec.c',
)
subdir('src/compat')
core_sources += compat_sources
core = static_library('va_v4l2_core', core_sources, gnu_symbol_visibility: 'hidden', include_directories: inc, dependencies: core_deps)
driver = shared_library('v4l2_drv_video', [], link_whole: core, dependencies: core_deps, name_prefix: '', install: true, install_dir: driverdir)
python = find_program('python3')
dl_dep = cc.find_library('dl')
if get_option('tests')
  # glibc redirects large-file calls to distinct symbols; musl uses the POSIX
  # names and does not expose/link the transitional LFS64 interfaces.
  test_lfs64 = cc.compiles('''
    #include <fcntl.h>
    #include <unistd.h>
    #include <sys/mman.h>
    #include <dirent.h>
    #if defined(open64) || defined(mmap64) || defined(pread64) || defined(readdir64) || defined(scandir64)
    #error LFS64 interfaces are aliases
    #endif
    int main(void) {
      return sizeof(off64_t) + sizeof(struct dirent64) + sizeof(&open64) +
             sizeof(&mmap64) + sizeof(&pread64) + sizeof(&readdir64) + sizeof(&scandir64);
    }
  ''', args: ['-D_GNU_SOURCE'], name: 'distinct LFS64 test interfaces')
  test_config = configuration_data()
  test_config.set10('VAV4L2_TEST_LFS64', test_lfs64)
  configure_file(output: 'test_config.h', configuration: test_config)
  # Linker wrapping requires calls to remain external to each translation unit.
  test_no_lto = cc.get_supported_arguments('-fno-lto')
  test_core = static_library('va_v4l2_test_core', core_sources, gnu_symbol_visibility: 'hidden', include_directories: inc, dependencies: core_deps, c_args: test_no_lto, override_options: ['b_lto=false'])
  packet_oracle = shared_library('stateful_oracle', 'tests/stateful_oracle.c', link_args: ['-Wl,--wrap=vav4l2_validate_reference'], name_prefix: '', install: false, link_with: test_core, include_directories: inc, dependencies: core_deps + [dl_dep])
  oracle_env = []
  if cc.get_id() == 'gcc' and get_option('b_sanitize').contains('address')
    oracle_env += 'VAV4L2_ORACLE_ASAN=' + run_command(cc.cmd_array() + ['-print-file-name=libasan.so'], check: true).stdout().strip()
  endif
  test('stateful_oracle', python, args: [files('tests/test_stateful_oracle.py'), meson.current_build_dir(), packet_oracle.full_path()], env: oracle_env, depends: [driver, packet_oracle], suite: 'unit', timeout: 120)
  metadata_adapter = shared_library('hevc_slice_interpose', 'tests/hevc_slice_interpose.c', name_prefix: '', install: false, dependencies: [libva_dep, threads_dep, dl_dep])
  units = {
    'allocator': ['open', 'open64', 'vav4l2_list_video_nodes'],
    'api_state': ['vav4l2_vp8_translate'],
    'av1_tiles': ['vav4l2_ensure_capture', 'vav4l2_decode_submit', 'vav4l2_surface_pull_capture'],
    'buffer_info': [],
    'buffers': ['vav4l2_scan_all_cached'],
    'capture_dmabuf': ['poll'],
    'capture_pool': ['vav4l2_alloc_shared_storage', 'poll', 'mmap', 'mmap64', 'munmap'],
    'capture_readback': ['realloc'],
    'codec_references': [],
    'compatibility': ['vav4l2_compat_modules', 'open', 'open64', 'vav4l2_list_video_nodes'],
    'decode_recovery': ['poll'],
    'decode_reference_state': [],
    'decode_trace': ['open', 'open64', 'write'],
    'discovery': ['ioctl', 'scandir', 'scandir64', 'opendir', 'readdir', 'readdir64', 'closedir', 'open', 'open64'],
    'dmabuf_complete_client': ['fork', '_exit', 'pread', 'pread64', 'ioctl', 'mmap', 'mmap64', 'munmap', 'opendir', 'readdir', 'readdir64', 'closedir', 'dirfd'],
    'dmabuf_wait': ['poll', 'clock_gettime'],
    'export_recapture': ['vav4l2_open_device', 'vav4l2_open_media_for_device', 'vav4l2_gbm_surface_ensure_export'],
    'format': [],
    'gbm_coherence': ['gbm_create_device', 'gbm_device_destroy', 'gbm_bo_create', 'gbm_bo_import', 'gbm_bo_map', 'gbm_bo_unmap', 'gbm_bo_get_stride_for_plane', 'gbm_bo_get_modifier', 'gbm_bo_get_fd_for_plane', 'gbm_bo_destroy', 'mmap', 'mmap64', 'vav4l2_alloc_shared_storage', 'vav4l2_alloc_coherent_storage', 'vav4l2_alloc_drm_storage'],
    'h264_fields': ['vav4l2_decode_submit', 'vav4l2_decode_submit_field', 'vav4l2_ensure_capture', 'vav4l2_surface_pull_capture'],
    'hevc_submission': [],
    'image_coherence': ['vav4l2_gbm_surface_sync'],
    'image_transfers': [],
    'jpeg420': [],
    'jpeg_dmabuf': ['poll'],
    'm2m_safety': [],
    'prime_layouts': ['poll', 'mmap', 'mmap64'],
    'reference_headers': [],
    'rga2_allocator': ['open', 'open64', 'vav4l2_list_video_nodes'],
    'stateful_packet': [],
    'packet_validation': [],
    'vp9_packet': [],
    'vp9_request': ['vav4l2_ensure_capture', 'vav4l2_decode_submit', 'vav4l2_surface_pull_capture'],
    'queue_api': [],
    'request_queue': ['poll', 'vav4l2_scan_all_cached'],
    'capture_layout': ['vav4l2_dmabuf_wait'],
    'request_slices': ['vav4l2_decode_submit', 'vav4l2_decode_submit_field', 'vav4l2_surface_pull_capture'],
    'drm_storage': ['drmGetVersion', 'drmFreeVersion', 'drmIoctl', 'drmPrimeHandleToFD'],
    'stateful_queue': ['poll', 'ioctl', 'vav4l2_scan_all_cached', 'gbm_bo_create', 'gbm_bo_map', 'gbm_bo_unmap', 'gbm_bo_get_stride_for_plane', 'gbm_bo_get_modifier', 'gbm_bo_get_fd_for_plane', 'gbm_bo_destroy', 'gbm_device_destroy', 'vav4l2_alloc_shared_storage', 'vav4l2_alloc_coherent_storage', 'vav4l2_alloc_drm_storage'],
    'slice_packetization': ['vav4l2_decode_submit', 'vav4l2_ensure_capture'],
    'surface_ownership': [],
    'video_probe': [],
    'vp8_mpeg2_fill': [],
    'vpp_dmabuf': ['poll'],
    'vpp_dmabuf_probe': [],
    'vpp_extended': ['poll', 'mmap', 'mmap64', 'munmap'],
    'vpp_features': [],
  }
  foreach name, symbols : units
    wrap_args = []
    foreach symbol : symbols
      if test_lfs64 or not symbol.endswith('64')
        wrap_args += '-Wl,--wrap=' + symbol
      endif
    endforeach
    test_sources = ['tests/test_' + name + '.c']
    if name == 'compatibility'
      test_sources += ['tests/compat_fixture.c']
    endif
    exe = executable('test_' + name, test_sources, link_with: test_core, c_args: test_no_lto, link_args: wrap_args + test_no_lto, override_options: ['b_lto=false'], include_directories: inc, dependencies: core_deps + [dl_dep])
    test(name, exe, suite: 'unit')
    if name == 'buffer_info'
      preload = metadata_adapter.full_path()
      if cc.get_id() == 'gcc' and get_option('b_sanitize').contains('address')
        preload = run_command(cc.cmd_array() + ['-print-file-name=libasan.so'], check: true).stdout().strip() + ':' + preload
      endif
      test('hevc_metadata_dispatch', exe, args: ['--adapter'], suite: 'unit', env: ['LD_PRELOAD=' + preload, 'VAV4L2_HEVC_PACKET_MODE=whole', 'VAV4L2_HEVC_PACKET_LOG=' + meson.current_build_dir() / 'hevc-metadata-dispatch.tsv'])
    endif
  endforeach
  test('capture_trace', python, args: files('tests/test_capture_trace.py'), suite: 'unit', timeout: 120)
  test('collect_capabilities', python, args: files('tests/test_collect_capabilities.py'), suite: 'unit', timeout: 120)
  test('compare_framemd5', python, args: files('tests/test_compare_framemd5.py'), suite: 'unit', timeout: 120)
  test('decode_evidence', python, args: files('tests/test_decode_evidence.py'), suite: 'unit', timeout: 120)
  test('dmabuf_allocation', python, args: files('tests/test_dmabuf_allocation.py'), suite: 'unit', timeout: 120)
  test('dmabuf_complete', python, args: files('tests/test_dmabuf_complete.py'), suite: 'unit', timeout: 120)
  test('dmabuf_decode_runner', python, args: files('tests/test_dmabuf_decode_runner.py'), suite: 'unit', timeout: 120)
  test('dmabuf_kernel', python, args: files('tests/test_dmabuf_kernel.py'), suite: 'unit', timeout: 120)
  test('dmabuf_rows', python, args: files('tests/test_dmabuf_rows.py'), suite: 'unit', timeout: 120)
  test('dmabuf_trace', python, args: files('tests/test_dmabuf_trace.py'), suite: 'unit', timeout: 120)
  test('extended_dmabuf', python, args: files('tests/test_extended_dmabuf.py'), suite: 'unit', timeout: 120)
  test('feature_runner', python, args: files('tests/test_feature_runner.py'), suite: 'unit', timeout: 120)
  test('hevc_pps_vector', python, args: files('tests/test_hevc_pps_vector.py'), suite: 'unit', timeout: 120)
  test('hevc_slice_runner', python, args: files('tests/test_hevc_slice_runner.py'), suite: 'unit', timeout: 120)
  test('matrix_results', python, args: files('tests/test_matrix_results.py'), suite: 'unit', timeout: 120)
  test('mpeg2_reference', python, args: files('tests/test_mpeg2_reference.py'), suite: 'unit', timeout: 120)
  test('hardware_runner', python, args: files('tests/test_hardware_runner.py'), suite: 'unit', timeout: 120)
  test('report_runner', python, args: files('tests/test_report_runner.py'), suite: 'unit', timeout: 120)
endif
if not get_option('hardware_tests').disabled()
  egl_dep = dependency('egl', required: get_option('hardware_tests'))
  gles_dep = dependency('glesv2', required: get_option('hardware_tests'))
  if egl_dep.found() and gles_dep.found()
  foreach name : ['va_export_client', 'va_expbuf_hold', 'va_dmabuf_renderer']
    executable(name, 'tests/' + name + '.c', dependencies: core_deps + [egl_dep, gles_dep])
  endforeach
  endif
  executable('va_capabilities', 'tests/va_capabilities.c', dependencies: core_deps)
  foreach name : ['va_feature_client', 'va_dmabuf_extended', 'va_dmabuf_rows', 'va_dmabuf_complete']
    executable(name, 'tests/' + name + '.c', dependencies: core_deps)
  endforeach
  executable('vpp_dmabuf_probe', 'tests/vpp_dmabuf_probe.c', link_with: core, include_directories: inc, dependencies: core_deps)
  shared_library('prime_surface_interpose', 'tests/prime_surface_interpose.c', name_prefix: '', install: false, dependencies: core_deps + [dl_dep])
endif

install_data('COPYING', 'NOTICE', 'AUTHORS', install_dir: get_option('datadir') / 'doc/libva-v4l2')

install_data('LICENSES/LGPL-2.1-or-later.txt', install_dir: get_option('datadir') / 'doc/libva-v4l2/LICENSES')
