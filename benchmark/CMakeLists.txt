cmake_minimum_required(VERSION 3.15)

include(FetchContent)

# Google Benchmark
set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_LIBPFM ON CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
  googlebenchmark
  GIT_REPOSITORY https://github.com/google/benchmark.git
  GIT_TAG v1.9.5
)
FetchContent_MakeAvailable(googlebenchmark)

add_executable(brotli_benchmark brotli_benchmark.cc)
target_link_libraries(brotli_benchmark
  PRIVATE
    benchmark::benchmark
    brotlienc
    brotlidec  # BrotliDecoderDecompress, for --roundtrip
    brotlicommon
)
target_include_directories(brotli_benchmark
  PRIVATE ${CMAKE_SOURCE_DIR}/c/include
)
set_target_properties(brotli_benchmark PROPERTIES
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED ON
)

# Metablock benchmark: isolates the Command-array -> metablock build/store step.
# The C shim (brotli_metablock_bench_impl.c) #includes c/enc/encode.c to reach
# the encoder's static helpers (EncodeData, WriteMetaBlockInternal, ...), which
# are hidden-visibility and not exported from libbrotlienc.so. So this target
# compiles the encoder source directly (all c/enc/*.c except encode.c, which
# lives in the shim, plus c/common/*.c) instead of linking brotlienc. brotlidec
# is linked only for the --roundtrip decode check.
file(GLOB METABLOCK_ENC_SOURCES ${CMAKE_SOURCE_DIR}/c/enc/*.c)
list(REMOVE_ITEM METABLOCK_ENC_SOURCES ${CMAKE_SOURCE_DIR}/c/enc/encode.c)
file(GLOB METABLOCK_COMMON_SOURCES ${CMAKE_SOURCE_DIR}/c/common/*.c)

add_executable(brotli_metablock_benchmark
  brotli_metablock_benchmark.cc
  brotli_metablock_bench_impl.c
  ${METABLOCK_ENC_SOURCES}
  ${METABLOCK_COMMON_SOURCES}
)
target_link_libraries(brotli_metablock_benchmark
  PRIVATE
    benchmark::benchmark
    brotlidec  # BrotliDecoderDecompress, for --roundtrip
)
# ${CMAKE_SOURCE_DIR} so the shim's #include "c/enc/encode.c" resolves; c/include
# for the public <brotli/*.h> headers.
target_include_directories(brotli_metablock_benchmark
  PRIVATE ${CMAKE_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/c/include
)
set_target_properties(brotli_metablock_benchmark PROPERTIES
  C_STANDARD 99
  C_STANDARD_REQUIRED ON
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED ON
)
