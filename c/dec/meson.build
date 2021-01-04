sources = [
  'bit_reader.c',
  'decode.c',
  'huffman.c',
  'state.c',
]

libbrotlidec = library('brotlidec', sources,
  include_directories: brotli_incdirs,
  dependencies: libbrotlicommon_dep,
  install: true,
)

libbrotlidec_dep = declare_dependency(link_with: libbrotlidec)

pkg = import('pkgconfig')
pkg.generate(libbrotlidec,
  name: 'libbrotlidec',
  description: 'Brotli decoder library',
  url: 'https://github.com/google/brotli',
)
