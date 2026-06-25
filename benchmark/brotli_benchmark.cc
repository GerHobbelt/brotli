/*
 * Brotli encoder benchmark using Google Benchmark.
 *
 * Loads all .min.js files from benchmark/corpus/ into memory, then
 * benchmarks BrotliEncoderCompress() across the entire corpus.
 *
 * Usage:
 *   brotli_benchmark [--quality=N] [--lgwin=N] [--corpus=DIR] [--digest=FILE]
 *                    [--dictionary=FILE] [--opt] [Google Benchmark flags...]
 *
 * Examples:
 *   brotli_benchmark
 *   brotli_benchmark --quality=9
 *   brotli_benchmark --dictionary=shared.dict   # attach a raw LZ77 dictionary
 *   brotli_benchmark --benchmark_perf_counters=CYCLES,INSTRUCTIONS
 *   brotli_benchmark --benchmark_out=results.json --benchmark_out_format=json
 *   brotli_benchmark --digest=out.txt   # write per-file output hashes, no bench
 */

#include <benchmark/benchmark.h>
#include <brotli/encode.h>
#include <brotli/shared_dictionary.h>

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
static std::string g_digest_path;
static int g_opt_writer = 0;  /* BROTLI_PARAM_OPT_WRITER toggle (--opt) */

/* Optional shared (raw LZ77 prefix) dictionary attached to every encode via the
   streaming API (--dictionary). The one-shot BrotliEncoderCompress cannot take
   a custom dictionary, which is why all encoding goes through EncodeOneFile. */
static std::string g_dictionary_path;
static std::vector<uint8_t> g_dictionary;  /* must outlive g_prepared_dict */
static BrotliEncoderPreparedDictionary* g_prepared_dict = nullptr;

/* Encoder mode shared by the benchmark loop and the --digest mode, so a digest
   reflects exactly the bytes the benchmark would compress. */
static const BrotliEncoderMode g_mode = BROTLI_MODE_TEXT;

static void LoadCorpus(const std::string& dir) {
  g_corpus.clear();
  g_corpus_total_size = 0;

  std::vector<fs::path> paths;
  for (const auto& entry : fs::directory_iterator(dir)) {
    if (entry.is_regular_file()) {
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

/* Read a raw (LZ77 prefix) dictionary file and prepare it once for the encoder.
   Preparation -- building the dictionary's hash table -- happens here, before
   the benchmark's timed loop, mirroring real usage where the dictionary is
   prepared offline; the timed path only attaches the result. Returns false on
   failure. The data buffer must outlive the prepared dictionary, so it is kept
   in the global g_dictionary. */
static bool LoadDictionary(const std::string& path) {
  std::error_code ec;
  auto size = fs::file_size(path, ec);
  if (ec || size == 0) {
    fprintf(stderr, "Error: cannot read dictionary %s\n", path.c_str());
    return false;
  }
  g_dictionary.resize(size);
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) {
    fprintf(stderr, "Error: cannot open dictionary %s\n", path.c_str());
    return false;
  }
  size_t read = fread(g_dictionary.data(), 1, size, f);
  fclose(f);
  if (read != size) {
    fprintf(stderr, "Error: short read on dictionary %s\n", path.c_str());
    return false;
  }
  /* Prepare for the maximum quality so the prepared dictionary is valid at any
     quality the benchmark might run. */
  g_prepared_dict = BrotliEncoderPrepareDictionary(
      BROTLI_SHARED_DICTIONARY_RAW, g_dictionary.size(), g_dictionary.data(),
      BROTLI_MAX_QUALITY, nullptr, nullptr, nullptr);
  if (!g_prepared_dict) {
    fprintf(stderr, "Error: failed to prepare dictionary %s\n", path.c_str());
    return false;
  }
  fprintf(stderr, "Dictionary: %s (%zu bytes)\n", path.c_str(),
          g_dictionary.size());
  return true;
}

/* Compress one buffer via the streaming API. This mirrors
   BrotliEncoderCompress (quality/lgwin/mode/size_hint then a single
   OPERATION_FINISH with all input available) so output is identical, but lets
   us also set BROTLI_PARAM_OPT_WRITER. Returns true on success and updates
   *output_size to the compressed length. */
static bool EncodeOneFile(const uint8_t* input, size_t input_size,
                          uint8_t* output, size_t* output_size) {
  BrotliEncoderState* s = BrotliEncoderCreateInstance(nullptr, nullptr, nullptr);
  if (!s) return false;
  BrotliEncoderSetParameter(s, BROTLI_PARAM_QUALITY, (uint32_t)g_quality);
  BrotliEncoderSetParameter(s, BROTLI_PARAM_LGWIN, (uint32_t)g_lgwin);
  BrotliEncoderSetParameter(s, BROTLI_PARAM_MODE, (uint32_t)g_mode);
  BrotliEncoderSetParameter(s, BROTLI_PARAM_SIZE_HINT, (uint32_t)input_size);
  if (g_opt_writer) {
    BrotliEncoderSetParameter(s, BROTLI_PARAM_OPT_WRITER, 1);
  }
  /* Attach the already-prepared dictionary (cheap: references the precomputed
     hash table built once at startup by LoadDictionary, not re-indexed here). */
  if (g_prepared_dict) {
    if (!BrotliEncoderAttachPreparedDictionary(s, g_prepared_dict)) {
      BrotliEncoderDestroyInstance(s);
      return false;
    }
  }
  size_t avail_in = input_size;
  const uint8_t* next_in = input;
  size_t avail_out = *output_size;
  uint8_t* next_out = output;
  BROTLI_BOOL ok = BrotliEncoderCompressStream(
      s, BROTLI_OPERATION_FINISH, &avail_in, &next_in,
      &avail_out, &next_out, nullptr);
  bool finished = ok && BrotliEncoderIsFinished(s) && avail_in == 0;
  *output_size -= avail_out;
  BrotliEncoderDestroyInstance(s);
  return finished;
}

/* FNV-1a 64-bit hash of a byte buffer. */
static uint64_t Fnv1a64(const uint8_t* data, size_t len) {
  uint64_t h = 0xcbf29ce484222325ULL;
  for (size_t i = 0; i < len; i++) {
    h ^= data[i];
    h *= 0x100000001b3ULL;
  }
  return h;
}

/* Compress every corpus file at the configured quality/lgwin/mode and write a
   per-file digest of the compressed output: "name<TAB>len<TAB>fnv64". Used to
   verify two encoder versions produce bit-identical output. Returns 0 on
   success. */
static int WriteDigest(const std::string& path) {
  FILE* out = fopen(path.c_str(), "wb");
  if (!out) {
    fprintf(stderr, "Error: cannot open digest file %s\n", path.c_str());
    return 1;
  }
  for (const auto& cf : g_corpus) {
    size_t max_size = BrotliEncoderMaxCompressedSize(cf.data.size());
    if (max_size == 0) max_size = cf.data.size() * 2;
    std::vector<uint8_t> compressed(max_size);
    size_t encoded_size = max_size;
    bool ok = EncodeOneFile(cf.data.data(), cf.data.size(),
                            compressed.data(), &encoded_size);
    if (!ok) {
      fprintf(stderr, "Error: compression failed for %s\n", cf.name.c_str());
      fclose(out);
      return 1;
    }
    fprintf(out, "%s\t%zu\t%016llx\n", cf.name.c_str(), encoded_size,
            (unsigned long long)Fnv1a64(compressed.data(), encoded_size));
  }
  fclose(out);
  fprintf(stderr, "Wrote digest for %zu files to %s\n", g_corpus.size(),
          path.c_str());
  return 0;
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
      bool ok = EncodeOneFile(g_corpus[i].data.data(), g_corpus[i].data.size(),
                              output_bufs[i].data(), &encoded_size);
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

  /* Compression ratio as "times smaller": original / compressed (e.g. 4.75
     means the output is 4.75x smaller than the input). */
  double ratio = static_cast<double>(g_corpus_total_size) /
                 static_cast<double>(total_compressed);
  state.counters["CompressionRatio"] = ratio;
  state.counters["Quality"] = g_quality;
  state.counters["Opt"] = g_opt_writer;
  state.counters["DictKB"] =
      static_cast<double>(g_dictionary.size()) / 1024.0;
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
    } else if (strncmp(argv[i], "--digest=", 9) == 0) {
      g_digest_path = argv[i] + 9;
    } else if (strncmp(argv[i], "--dictionary=", 13) == 0) {
      g_dictionary_path = argv[i] + 13;
    } else if (strcmp(argv[i], "--opt") == 0) {
      g_opt_writer = 1;
    } else if (strncmp(argv[i], "--opt=", 6) == 0) {
      g_opt_writer = atoi(argv[i] + 6);
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
  fprintf(stderr, "Quality: %d, lgwin: %d, opt_writer: %d\n",
          g_quality, g_lgwin, g_opt_writer);
  LoadCorpus(g_corpus_dir);

  if (g_corpus.empty()) {
    fprintf(stderr, "Error: No .js files found in %s\n", g_corpus_dir.c_str());
    fprintf(stderr, "Run: python3 benchmark/download_corpus.py\n");
    return 1;
  }

  /* Ingest + prepare the dictionary once, before any timing or digesting. */
  if (!g_dictionary_path.empty()) {
    if (!LoadDictionary(g_dictionary_path)) return 1;
  }

  /* Digest mode: emit per-file compressed-output hashes and exit without
     running the benchmark (used for bit-identity comparison). */
  if (!g_digest_path.empty()) {
    return WriteDigest(g_digest_path);
  }

  benchmark::Initialize(&argc, argv);
  benchmark::RunSpecifiedBenchmarks();
  benchmark::Shutdown();

  if (g_prepared_dict) BrotliEncoderDestroyPreparedDictionary(g_prepared_dict);
  return 0;
}
