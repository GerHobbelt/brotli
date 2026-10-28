sources = [
  'backward_references.c',
  'backward_references_hq.c',
  'bit_cost.c',
  'block_splitter.c',
  'brotli_bit_stream.c',
  'cluster.c',
  'command.c',
  'compound_dictionary.c',
  'compress_fragment.c',
  'compress_fragment_two_pass.c',
  'dictionary_hash.c',
  'encode.c',
  'encoder_dict.c',
  'entropy_encode.c',
  'fast_log.c',
  'histogram.c',
  'literal_cost.c',
  'memory.c',
  'metablock.c',
  'static_dict.c',
  'utf8_util.c',
]

libbrotlienc = library('brotlienc', sources,
  include_directories: brotli_incdirs,
  dependencies: [libbrotlicommon_dep, m_dep],
  install: true,
)

libbrotlienc_dep = declare_dependency(
  include_directories: brotli_incdirs_public,
  link_with: libbrotlienc,
)

meson.override_dependency('libbrotlienc', libbrotlienc_dep)

pkg = import('pkgconfig')
pkg.generate(libbrotlienc,
  name: 'libbrotlienc',
  description: 'Brotli encoder library',
  url: 'https://github.com/google/brotli',
)
