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
python = find_program('python3')
dl_dep = cc.find_library('dl')
if get_option('tests')
  # Linker wrapping requires calls to remain external to each translation unit.
  test_no_lto = cc.get_supported_arguments('-fno-lto')
  test_core = static_library('va_request_test_core', core_sources, gnu_symbol_visibility: 'hidden', include_directories: inc, dependencies: core_deps, c_args: test_no_lto, override_options: ['b_lto=false'])
  metadata_adapter = shared_library('hevc_slice_interpose', 'tests/hevc_slice_interpose.c', name_prefix: '', install: false, dependencies: [libva_dep, threads_dep, dl_dep])
  units = {
    'allocator': ['open', 'open64', 'vareq_list_video_nodes'],
    'api_state': ['vareq_vp8_translate'],
    'av1_tiles': [],
    'buffer_info': [],
    'buffers': [],
    'capture_dmabuf': ['poll'],
    'capture_pool': ['vareq_alloc_shared_storage', 'poll', 'mmap', 'mmap64', 'munmap'],
    'capture_readback': ['realloc'],
    'codec_references': [],
    'compatibility': ['vareq_compat_modules', 'open', 'open64', 'vareq_list_video_nodes'],
    'decode_recovery': ['poll'],
    'decode_reference_state': [],
    'decode_trace': ['open', 'open64', 'write'],
    'discovery': ['ioctl', 'scandir', 'scandir64', 'opendir', 'readdir', 'readdir64', 'closedir', 'open', 'open64'],
    'dmabuf_complete_client': ['fork', '_exit', 'pread', 'pread64', 'ioctl', 'mmap', 'mmap64', 'munmap', 'opendir', 'readdir', 'readdir64', 'closedir', 'dirfd'],
    'dmabuf_wait': ['poll', 'clock_gettime'],
    'export_recapture': ['vareq_open_device', 'vareq_open_media_for_device'],
    'format': [],
    'gbm_coherence': ['gbm_create_device', 'gbm_device_destroy', 'gbm_bo_create', 'gbm_bo_import', 'gbm_bo_map', 'gbm_bo_unmap', 'gbm_bo_get_stride_for_plane', 'gbm_bo_get_modifier', 'gbm_bo_get_fd_for_plane', 'gbm_bo_destroy', 'mmap', 'mmap64', 'vareq_alloc_shared_storage', 'vareq_alloc_coherent_storage'],
    'h264_fields': ['vareq_decode_submit', 'vareq_decode_submit_field', 'vareq_ensure_capture', 'vareq_surface_pull_capture'],
    'hevc_submission': [],
    'image_coherence': ['vareq_gbm_surface_sync'],
    'image_transfers': [],
    'jpeg420': [],
    'jpeg_dmabuf': ['poll'],
    'm2m_safety': [],
    'prime_layouts': ['poll', 'mmap', 'mmap64'],
    'reference_headers': [],
    'rga2_allocator': ['open', 'open64', 'vareq_list_video_nodes'],
    'slice_packetization': ['vareq_decode_submit', 'vareq_ensure_capture'],
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
      wrap_args += '-Wl,--wrap=' + symbol
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
      test('hevc_metadata_dispatch', exe, args: ['--adapter'], suite: 'unit', env: ['LD_PRELOAD=' + preload, 'VAREQ_HEVC_PACKET_MODE=whole', 'VAREQ_HEVC_PACKET_LOG=' + meson.current_build_dir() / 'hevc-metadata-dispatch.tsv'])
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
