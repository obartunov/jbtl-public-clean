jsonb_toaster_lite_sources = files(
  'jsonb_toaster_lite.c',
  'jsonb_toaster_lite_chain.c',
  'jsonb_toaster_lite_pointers.c',
  'jsonb_toaster_lite_object_field.c',
)

if host_system == 'windows'
  jsonb_toaster_lite_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'jsonb_toaster_lite',
    '--FILEDESC', 'jsonb_toaster_lite - TOAST/storage layer for jsonb',])
endif

jsonb_toaster_lite = shared_module('jsonb_toaster_lite',
  jsonb_toaster_lite_sources,
  include_directories: include_directories('../toastapi'),
  kwargs: contrib_mod_args,
)
contrib_targets += jsonb_toaster_lite

install_data(
  'jsonb_toaster_lite.control',
  'jsonb_toaster_lite--1.0.sql',
  kwargs: contrib_data_args,
)

tests += {
  'name': 'jsonb_toaster_lite',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'jsonb_toaster_lite',
      'jsonb_toaster_lite_roundtrip',
      'jsonb_toaster_lite_slice_plain',
      'jsonb_toaster_lite_slice_compressed',
      'jsonb_toaster_lite_object_field',
    ],
  },
}
