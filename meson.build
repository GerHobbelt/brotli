project('brotli', 'c',
  version: '1.0.9',
)

host_os = host_machine.system()
if host_os == 'android'
  host_os_family = 'linux'
else
  host_os_family = host_os
endif

cc = meson.get_compiler('c')

have_msvc = cc.get_id() == 'msvc'

cdata = configuration_data()

m_dep = cc.find_library('m', required: false)
cdata.set10('BROTLI_HAVE_LOG2', cc.has_function('log2', dependencies: m_dep))

if host_os_family == 'darwin'
  cdata.set('OS_MACOSX', true)
else
  cdata.set('OS_' + host_os_family.to_upper(), true)
endif

configure_file(
  output: 'config.h',
  configuration: cdata,
)
add_project_arguments(have_msvc ? '/FI' : '-include', meson.current_build_dir() / 'config.h', language: 'c')

brotli_incdirs = include_directories(
  '.',
  'c' / 'include',
)
brotli_incdirs_public = include_directories(
  'c' / 'include',
)

subdir('c')
