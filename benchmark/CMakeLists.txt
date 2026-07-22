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
