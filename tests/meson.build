capabilities_test = executable(
  'test-capabilities',
  common_sources + files('test_capabilities.c'),
  include_directories: project_inc,
  dependencies: [threads_dep],
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
test('driver-init', driver_init_test)
test('driver-load', driver_load_test, args: [venus_driver])

nm = find_program('nm')
symbol_test = find_program('check-driver-symbol.sh')
test('driver-symbol', symbol_test, args: [nm.full_path(), venus_driver])
