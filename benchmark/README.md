# Brotli Encoder Benchmark

Measures brotli encoding throughput, compression ratio, and CPU cycles against a ~200 MB corpus of minified JavaScript from popular npm packages.

## Prerequisites

- CMake >= 3.15, C++17 compiler, Python 3
- libpfm4-dev for hardware performance counters: `sudo apt install libpfm4-dev`

## Setup

```bash
python3 benchmark/download_corpus.py
cmake -B build-bench -DCMAKE_BUILD_TYPE=Release -DBROTLI_BUILD_BENCHMARKS=ON
cmake --build build-bench -j$(nproc)
```

The download script fetches ~1,200 `.min.js` files into `benchmark/corpus/` with size verification and automatic retries.

## Run

```bash
export LD_LIBRARY_PATH=build-bench
build-bench/benchmark/brotli_benchmark --benchmark_perf_counters=CYCLES
```

### Options

- `--quality=N` — compression quality 0-11 (default: 6)
- `--lgwin=N` — log2 window size (default: 22)
- `--corpus=DIR` — path to corpus directory
- `--benchmark_out=results.json --benchmark_out_format=json` — save results for comparison

All [Google Benchmark flags](https://github.com/google/benchmark/blob/main/docs/user_guide.md) are also supported (e.g. `--benchmark_repetitions=N`, `--benchmark_perf_counters=CYCLES,INSTRUCTIONS,CACHE-MISSES`).

## CPU Profiling

Capture a profile using [gperftools](https://github.com/gperftools/gperftools) and view it with [pprof](https://github.com/google/pprof):

```bash
# Prerequisites
sudo apt install google-perftools
go install github.com/google/pprof@latest

# Capture a profile (1000 Hz sampling, 5 repetitions)
CPUPROFILE=/tmp/brotli.prof CPUPROFILE_FREQUENCY=1000 \
  LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libprofiler.so \
  build-bench/benchmark/brotli_benchmark --benchmark_repetitions=5

# Interactive web UI (flamegraph, call graph, source annotation)
pprof -http=:8080 build-bench/benchmark/brotli_benchmark /tmp/brotli.prof
```
