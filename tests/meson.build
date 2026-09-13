capabilities_test = executable(
  'test-capabilities',
  common_sources + files('test_capabilities.c'),
  include_directories: project_inc,
  dependencies: [threads_dep],
)

annexb_split_test = executable(
  'test-annexb-split',
  files('../src/annexb_split.c', 'test_annexb_split.c'),
  include_directories: project_inc,
)

v4l2_decoder_test = executable(
  'test-v4l2-decoder',
  files('../src/v4l2_decoder.c', 'test_v4l2_decoder.c'),
  include_directories: project_inc,
)

h264_annexb_test = executable(
  'test-h264-annexb',
  codec_sources + files('test_h264_annexb.c'),
  include_directories: project_inc,
  dependencies: [libva_dep],
)

backend_objects_test = executable(
  'test-backend-objects',
  driver_sources + files('test_backend_objects.c'),
  include_directories: project_inc,
  dependencies: [libva_dep, threads_dep],
)

driver_init_test = executable(
  'test-driver-init',
  driver_sources + files('test_driver_init.c'),
  include_directories: project_inc,
  dependencies: [libva_dep, threads_dep],
)

dl_dep = cc.find_library('dl')
driver_load_test = executable(
  'test-driver-load',
  files('test_driver_load.c'),
  dependencies: [libva_dep, dl_dep],
)

test('capabilities', capabilities_test)
test('annexb-split', annexb_split_test)
test('v4l2-decoder', v4l2_decoder_test)
test('h264-annexb', h264_annexb_test)
test('backend-objects', backend_objects_test)
test('driver-init', driver_init_test)
test('driver-load', driver_load_test, args: [venus_driver])

nm = find_program('nm')
symbol_test = find_program('check-driver-symbol.sh')
test('driver-symbol', symbol_test, args: [nm.full_path(), venus_driver])
