/*
 * Brotli encoder benchmark using Google Benchmark.
 *
 * Loads all .min.js files from benchmark/corpus/ into memory, then
 * benchmarks BrotliEncoderCompress() across the entire corpus.
 *
 * Usage:
 *   brotli_benchmark [--quality=N] [--corpus=DIR] [Google Benchmark flags...]
 *
 * Examples:
 *   brotli_benchmark
 *   brotli_benchmark --quality=9
 *   brotli_benchmark --benchmark_perf_counters=CYCLES,INSTRUCTIONS
 *   brotli_benchmark --benchmark_out=results.json --benchmark_out_format=json
 */

#include <benchmark/benchmark.h>
#include <brotli/encode.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

/* Corpus: each entry is a file's contents loaded into memory. */
struct CorpusFile {
  std::string name;
  std::vector<uint8_t> data;
};

static std::vector<CorpusFile> g_corpus;
static size_t g_corpus_total_size = 0;
static int g_quality = 6;
static int g_lgwin = BROTLI_DEFAULT_WINDOW;
static std::string g_corpus_dir;

static void LoadCorpus(const std::string& dir) {
  g_corpus.clear();
  g_corpus_total_size = 0;

  std::vector<fs::path> paths;
  for (const auto& entry : fs::directory_iterator(dir)) {
    if (entry.is_regular_file() && entry.path().extension() == ".js") {
      paths.push_back(entry.path());
    }
  }
  std::sort(paths.begin(), paths.end());

  for (const auto& path : paths) {
    auto size = fs::file_size(path);
    if (size == 0) continue;

    CorpusFile cf;
    cf.name = path.filename().string();
    cf.data.resize(size);

    FILE* f = fopen(path.c_str(), "rb");
    if (!f) {
      fprintf(stderr, "Warning: cannot open %s\n", path.c_str());
      continue;
    }
    size_t read = fread(cf.data.data(), 1, size, f);
    fclose(f);
    if (read != size) {
      fprintf(stderr, "Warning: short read on %s\n", path.c_str());
      continue;
    }

    g_corpus_total_size += size;
    g_corpus.push_back(std::move(cf));
  }

  fprintf(stderr, "Loaded %zu files, %.1f MB total\n",
          g_corpus.size(), g_corpus_total_size / (1024.0 * 1024.0));
}

static void BM_BrotliEncode(benchmark::State& state) {
  if (g_corpus.empty()) {
    state.SkipWithError("No corpus files loaded");
    return;
  }

  /* Pre-allocate output buffers sized for worst case. */
  std::vector<std::vector<uint8_t>> output_bufs(g_corpus.size());
  std::vector<size_t> max_sizes(g_corpus.size());
  for (size_t i = 0; i < g_corpus.size(); i++) {
    max_sizes[i] = BrotliEncoderMaxCompressedSize(g_corpus[i].data.size());
    if (max_sizes[i] == 0) {
      max_sizes[i] = g_corpus[i].data.size() * 2;
    }
    output_bufs[i].resize(max_sizes[i]);
  }

  size_t total_compressed = 0;

  for (auto _ : state) {
    total_compressed = 0;
    for (size_t i = 0; i < g_corpus.size(); i++) {
      size_t encoded_size = max_sizes[i];
      BROTLI_BOOL ok = BrotliEncoderCompress(
          g_quality, g_lgwin, BROTLI_MODE_TEXT,
          g_corpus[i].data.size(), g_corpus[i].data.data(),
          &encoded_size, output_bufs[i].data());
      if (!ok) {
        state.SkipWithError("BrotliEncoderCompress failed");
        return;
      }
      total_compressed += encoded_size;
    }
  }

  state.SetBytesProcessed(
      static_cast<int64_t>(state.iterations()) *
      static_cast<int64_t>(g_corpus_total_size));

  double ratio = static_cast<double>(total_compressed) /
                 static_cast<double>(g_corpus_total_size);
  state.counters["CompressionRatio"] = ratio;
  state.counters["Quality"] = g_quality;
  state.counters["InputMB"] =
      static_cast<double>(g_corpus_total_size) / (1024.0 * 1024.0);
  state.counters["CompressedMB"] =
      static_cast<double>(total_compressed) / (1024.0 * 1024.0);
}

BENCHMARK(BM_BrotliEncode)
    ->Unit(benchmark::kMillisecond)
    ->Iterations(1)
    ->UseRealTime();

/* Custom flag parsing for --quality, --lgwin, --corpus. */
static void ParseCustomFlags(int* argc, char** argv) {
  int dst = 1;
  for (int i = 1; i < *argc; i++) {
    if (strncmp(argv[i], "--quality=", 10) == 0) {
      g_quality = atoi(argv[i] + 10);
      if (g_quality < BROTLI_MIN_QUALITY || g_quality > BROTLI_MAX_QUALITY) {
        fprintf(stderr, "Error: quality must be %d..%d\n",
                BROTLI_MIN_QUALITY, BROTLI_MAX_QUALITY);
        exit(1);
      }
    } else if (strncmp(argv[i], "--lgwin=", 8) == 0) {
      g_lgwin = atoi(argv[i] + 8);
    } else if (strncmp(argv[i], "--corpus=", 9) == 0) {
      g_corpus_dir = argv[i] + 9;
    } else {
      argv[dst++] = argv[i];
    }
  }
  *argc = dst;
}

int main(int argc, char** argv) {
  ParseCustomFlags(&argc, argv);

  /* Default corpus path: benchmark/corpus/ relative to working directory,
     or relative to the binary location. */
  if (g_corpus_dir.empty()) {
    if (fs::exists("benchmark/corpus")) {
      g_corpus_dir = "benchmark/corpus";
    } else {
      /* Try relative to binary location. */
      fs::path bin_dir = fs::path(argv[0]).parent_path();
      fs::path candidate = bin_dir / ".." / ".." / "benchmark" / "corpus";
      if (fs::exists(candidate)) {
        g_corpus_dir = candidate.string();
      } else {
        fprintf(stderr,
                "Error: Cannot find corpus directory. "
                "Use --corpus=DIR or run from the brotli repo root.\n");
        return 1;
      }
    }
  }

  fprintf(stderr, "Corpus: %s\n", g_corpus_dir.c_str());
  fprintf(stderr, "Quality: %d, lgwin: %d\n", g_quality, g_lgwin);
  LoadCorpus(g_corpus_dir);

  if (g_corpus.empty()) {
    fprintf(stderr, "Error: No .js files found in %s\n", g_corpus_dir.c_str());
    fprintf(stderr, "Run: python3 benchmark/download_corpus.py\n");
    return 1;
  }

  benchmark::Initialize(&argc, argv);
  benchmark::RunSpecifiedBenchmarks();
  benchmark::Shutdown();
  return 0;
}
