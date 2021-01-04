sources = [
  'constants.c',
  'context.c',
  'dictionary.c',
  'platform.c',
  'transform.c',
]

libbrotlicommon = library('brotlicommon', sources,
  include_directories: brotli_incdirs,
  install: true,
)

libbrotlicommon_dep = declare_dependency(link_with: libbrotlicommon)

pkg = import('pkgconfig')
pkg.generate(libbrotlicommon,
  name: 'libbrotlicommon',
  description: 'Brotli common dictionary library',
  url: 'https://github.com/google/brotli',
)
