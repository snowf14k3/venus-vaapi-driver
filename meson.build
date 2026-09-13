project(
  'venus-vaapi-driver',
  'c',
  version: '0.1.0',
  license: 'MIT',
  meson_version: '>=1.2.0',
  default_options: [
    'c_std=c11',
    'warning_level=3',
    'werror=true',
  ],
)

cc = meson.get_compiler('c')
libva_dep = dependency('libva', version: '>=1.20')
threads_dep = dependency('threads')

add_project_arguments(
  '-D_GNU_SOURCE',
  '-DVENUS_VAAPI_PROBE_LIMIT=@0@'.format(get_option('probe_limit')),
  language: 'c',
)

project_inc = include_directories('include', 'src')

common_sources = files(
  'src/capabilities.c',
  'src/v4l2_probe.c',
)

codec_sources = files(
  'src/bit_writer.c',
  'src/h264_annexb.c',
)

stateful_sources = files(
  'src/annexb_split.c',
  'src/v4l2_decoder.c',
)

driver_sources = common_sources + codec_sources + files(
  'src/driver.c',
  'src/va_stubs.c',
)

venus_driver = shared_module(
  'venus_drv_video',
  driver_sources,
  include_directories: project_inc,
  dependencies: [libva_dep, threads_dep],
  name_prefix: '',
  gnu_symbol_visibility: 'hidden',
  install: true,
  install_dir: join_paths(get_option('libdir'), 'dri'),
)

executable(
  'venus-vaapi-info',
  common_sources + files('tools/venus-vaapi-info.c'),
  include_directories: project_inc,
  dependencies: [threads_dep],
  install: true,
)

executable(
  'venus-v4l2-decode',
  common_sources + stateful_sources + files('tools/venus-v4l2-decode.c'),
  include_directories: project_inc,
  dependencies: [threads_dep],
  install: false,
)

subdir('tests')

summary(
  {
    'VA driver': venus_driver.full_path(),
    'Install directory': join_paths(get_option('libdir'), 'dri'),
    'libva': libva_dep.version(),
  },
  section: 'venus-vaapi-driver',
)
