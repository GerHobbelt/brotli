/*
 * Brotli metablock benchmark using Google Benchmark.
 *
 * The full encode is dominated by matchfinding (BrotliCreateBackwardReferences),
 * so benchmarking the whole run gives a poor signal for changes to the metablock
 * build/store step (BrotliBuildMetaBlock / BrotliBuildMetaBlockGreedy /
 * BrotliStoreMetaBlock). This benchmark isolates that step:
 *
 *   1. (untimed, once) run matchfinding over each corpus file, splitting it into
 *      metablocks exactly as the real encoder does, and capture each metablock's
 *      Command array + pre-write encoder state (see brotli_metablock_bench_impl.c,
 *      which compiles the encoder in and reaches its static helpers).
 *   2. (timed, in the loop) turn those Command arrays back into metablocks via the
 *      encoder's real WriteMetaBlockInternal.
 *
 * The --digest / --roundtrip modes use the exact encoder output produced in step 1
 * (bit-identical to brotli_benchmark's), so ab.py's bit-identity gate applies.
 *
 * Usage:
 *   brotli_metablock_benchmark [--quality=N] [--lgwin=N] [--corpus=DIR]
 *       [--dictionary=FILE] [--opt-writer] [--opt-matchfinder]
 *       [--opt-block-splitter] [--files=N] [--digest=FILE] [--roundtrip=FILE]
 *       [Google Benchmark flags...]
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <benchmark/benchmark.h>
#include <brotli/decode.h>
#include <brotli/encode.h>
#include <brotli/shared_dictionary.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#include "brotli_metablock_bench_impl.h"

namespace fs = std::filesystem;

struct CorpusFile {
  std::string name;
  std::vector<uint8_t> data;
};

/* A corpus file after the untimed matchfinding pass: the zero-padded input
   buffer (for literal indexing), the captured metablocks, and the exact encoder
   output (for the digest/roundtrip gates and the ratio counter). */
struct PreparedFile {
  std::string name;
  std::vector<uint8_t> input_buf;  /* zero-padded to a power of two > file_size */
  size_t file_size;
  size_t mask;
  MBB_Prepared* prepared;
  int nmb;
  std::vector<uint8_t> compressed;  /* exact encoder output */
};

static std::vector<CorpusFile> g_corpus;
static std::vector<PreparedFile> g_prepared;
static size_t g_corpus_total_size = 0;
static int g_quality = 6;
static int g_lgwin = BROTLI_DEFAULT_WINDOW;
static std::string g_corpus_dir;
static std::string g_digest_path;
static std::string g_roundtrip_path;
static int g_opt_writer = 0;
static int g_opt_matchfinder = 0;
static int g_opt_block_splitter = 0;
static int g_files = 0;  /* 0 = all */
static std::string g_dictionary_path;
static std::vector<uint8_t> g_dictionary;
static BrotliEncoderPreparedDictionary* g_prepared_dict = nullptr;
static const BrotliEncoderMode g_mode = BROTLI_MODE_TEXT;

static void LoadCorpus(const std::string& dir) {
  g_corpus.clear();
  g_corpus_total_size = 0;

  std::vector<fs::path> paths;
  for (const auto& entry : fs::directory_iterator(dir)) {
    if (entry.is_regular_file()) paths.push_back(entry.path());
  }
  std::sort(paths.begin(), paths.end());

  for (const auto& path : paths) {
    std::error_code ec;
    auto size = fs::file_size(path, ec);
    if (ec || size == 0) continue;
    if (g_files > 0 && (int)g_corpus.size() >= g_files) break;
    CorpusFile cf;
    cf.name = path.filename().string();
    cf.data.resize(static_cast<size_t>(size));
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) continue;
    size_t got = fread(cf.data.data(), 1, cf.data.size(), f);
    fclose(f);
    if (got != cf.data.size()) continue;
    g_corpus_total_size += cf.data.size();
    g_corpus.push_back(std::move(cf));
  }
}

static bool LoadDictionary(const std::string& path) {
  std::error_code ec;
  auto size = fs::file_size(path, ec);
  if (ec || size == 0) {
    fprintf(stderr, "Error: cannot read dictionary %s\n", path.c_str());
    return false;
  }
  g_dictionary.resize(size);
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) return false;
  size_t read = fread(g_dictionary.data(), 1, size, f);
  fclose(f);
  if (read != size) return false;
  g_prepared_dict = BrotliEncoderPrepareDictionary(
      BROTLI_SHARED_DICTIONARY_RAW, g_dictionary.size(), g_dictionary.data(),
      BROTLI_MAX_QUALITY, nullptr, nullptr, nullptr);
  if (!g_prepared_dict) {
    fprintf(stderr, "Error: failed to prepare dictionary %s\n", path.c_str());
    return false;
  }
  fprintf(stderr, "Dictionary: %s (%zu bytes)\n", path.c_str(), g_dictionary.size());
  return true;
}

/* Run the untimed matchfinding pass over every corpus file, capturing the
   per-metablock Command arrays and producing the exact encoder output. */
static bool PrepareAll() {
  g_prepared.clear();
  for (const auto& cf : g_corpus) {
    /* Zero-padded input buffer sized to a power of two strictly greater than
       file_size, so (mask+1) > file_size and the two sentinel bytes (-1, -2)
       land in the zeroed region. */
    size_t k = 0;
    while ((size_t(1) << k) < cf.data.size() + 2) k++;
    const size_t buf_size = size_t(1) << k;
    const size_t mask = buf_size - 1;
    std::vector<uint8_t> input_buf(buf_size, 0);
    memcpy(input_buf.data(), cf.data.data(), cf.data.size());

    const size_t out_cap = 2 * cf.data.size() + 1000;
    std::vector<uint8_t> out(out_cap);
    int nmb = 0;
    size_t csize = 0;
    MBB_Prepared* p = MBB_Prepare(
        g_quality, g_lgwin, (int)g_mode, cf.data.size(),
        g_opt_writer, g_opt_matchfinder, g_opt_block_splitter, g_prepared_dict,
        input_buf.data(), cf.data.size(), mask, &nmb, out.data(), out_cap, &csize);
    if (!p) {
      fprintf(stderr, "Error: MBB_Prepare failed for %s\n", cf.name.c_str());
      return false;
    }
    out.resize(csize);
    g_prepared.push_back(
        {cf.name, std::move(input_buf), cf.data.size(), mask, p, nmb,
         std::move(out)});
  }
  return true;
}

static uint64_t Fnv1a64(const uint8_t* data, size_t len) {
  uint64_t h = 0xcbf29ce484222325ULL;
  for (size_t i = 0; i < len; i++) {
    h ^= data[i];
    h *= 0x100000001b3ULL;
  }
  return h;
}

static int WriteLines(const std::string& path,
                      const std::vector<std::string>& lines, const char* what) {
  FILE* out = fopen(path.c_str(), "wb");
  if (!out) {
    fprintf(stderr, "Error: cannot open %s file %s\n", what, path.c_str());
    return 1;
  }
  for (const auto& line : lines) {
    if (!line.empty()) fwrite(line.data(), 1, line.size(), out);
  }
  fclose(out);
  return 0;
}

/* Per-file hash of the exact encoder output: "name<TAB>len<TAB>fnv64". */
static int WriteDigest(const std::string& path) {
  std::vector<std::string> lines(g_prepared.size());
  for (size_t i = 0; i < g_prepared.size(); i++) {
    const PreparedFile& f = g_prepared[i];
    char tail[64];
    snprintf(tail, sizeof(tail), "\t%zu\t%016llx\n", f.compressed.size(),
             (unsigned long long)Fnv1a64(f.compressed.data(), f.compressed.size()));
    lines[i] = f.name + tail;
  }
  if (WriteLines(path, lines, "digest") != 0) return 1;
  fprintf(stderr, "Wrote digest for %zu files to %s\n", g_prepared.size(),
          path.c_str());
  return 0;
}

/* Decode each file's exact encoder output and verify it reproduces the input:
   "name<TAB>input_size<TAB>compressed_size<TAB>ok". */
static int WriteRoundtrip(const std::string& path) {
  std::vector<std::string> lines(g_prepared.size());
  size_t mismatches = 0;
  for (size_t i = 0; i < g_prepared.size(); i++) {
    const PreparedFile& f = g_prepared[i];
    std::vector<uint8_t> decoded(f.file_size);
    size_t decoded_size = f.file_size;
    BrotliDecoderResult r = BrotliDecoderDecompress(
        f.compressed.size(), f.compressed.data(), &decoded_size, decoded.data());
    bool ok = (r == BROTLI_DECODER_RESULT_SUCCESS) &&
              (decoded_size == f.file_size) &&
              (decoded_size == 0 ||
               memcmp(decoded.data(), f.input_buf.data(), decoded_size) == 0);
    if (!ok) mismatches++;
    char tail[80];
    snprintf(tail, sizeof(tail), "\t%zu\t%zu\t%d\n", f.file_size,
             f.compressed.size(), ok ? 1 : 0);
    lines[i] = f.name + tail;
  }
  if (WriteLines(path, lines, "roundtrip") != 0) return 1;
  fprintf(stderr, "Roundtripped %zu files to %s, %zu mismatch%s\n",
          g_prepared.size(), path.c_str(), mismatches,
          mismatches == 1 ? "" : "es");
  return 0;
}

static void BM_MetablockStore(benchmark::State& state) {
  if (g_prepared.empty()) {
    state.SkipWithError("No corpus files prepared");
    return;
  }

  size_t max_size = 0;
  for (const auto& f : g_prepared) max_size = std::max(max_size, f.file_size);
  std::vector<uint8_t> storage(2 * max_size + 503);

  /* Sink so the store work cannot be optimized away. */
  static volatile uint64_t g_sink;
  uint64_t checksum = 0;

  for (auto _ : state) {
    checksum = 0;
    for (const auto& f : g_prepared) {
      uint16_t lb;
      uint8_t lbb;
      MBB_GetHeader(f.prepared, &lb, &lbb);
      for (int i = 0; i < f.nmb; i++) {
        MBB_Store(f.prepared, i, f.input_buf.data(), f.mask, storage.data(),
                  &lb, &lbb);
      }
      checksum += (uint64_t)lb * 31u + lbb;
    }
  }
  g_sink = checksum;

  state.SetBytesProcessed(
      static_cast<int64_t>(state.iterations()) *
      static_cast<int64_t>(g_corpus_total_size));

  double total_compressed = 0;
  int total_mb = 0;
  for (const auto& f : g_prepared) {
    total_compressed += f.compressed.size();
    total_mb += f.nmb;
  }
  state.counters["CompressionRatio"] =
      static_cast<double>(g_corpus_total_size) / total_compressed;
  state.counters["Quality"] = g_quality;
  state.counters["OptWriter"] = g_opt_writer;
  state.counters["OptMatchfinder"] = g_opt_matchfinder;
  state.counters["OptBS"] = g_opt_block_splitter;
  state.counters["DictKB"] = static_cast<double>(g_dictionary.size()) / 1024.0;
  state.counters["InputMB"] =
      static_cast<double>(g_corpus_total_size) / (1024.0 * 1024.0);
  state.counters["Metablocks"] = static_cast<double>(total_mb);
}

BENCHMARK(BM_MetablockStore)
    ->Unit(benchmark::kMillisecond)
    ->Iterations(1)
    ->UseRealTime();

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
      if (g_quality <= 1) {
        fprintf(stderr,
            "Error: quality %d uses the fast-fragment path "
            "(BrotliCompressFragment*), which has no separate metablock "
            "build/store step. This benchmark covers quality 2..11 only.\n",
            g_quality);
        exit(1);
      }
    } else if (strncmp(argv[i], "--lgwin=", 8) == 0) {
      g_lgwin = atoi(argv[i] + 8);
    } else if (strncmp(argv[i], "--corpus=", 9) == 0) {
      g_corpus_dir = argv[i] + 9;
    } else if (strncmp(argv[i], "--digest=", 9) == 0) {
      g_digest_path = argv[i] + 9;
    } else if (strncmp(argv[i], "--roundtrip=", 12) == 0) {
      g_roundtrip_path = argv[i] + 12;
    } else if (strncmp(argv[i], "--files=", 8) == 0) {
      g_files = atoi(argv[i] + 8);
    } else if (strncmp(argv[i], "--dictionary=", 13) == 0) {
      g_dictionary_path = argv[i] + 13;
    } else if (strcmp(argv[i], "--opt-writer") == 0) {
      g_opt_writer = 1;
    } else if (strncmp(argv[i], "--opt-writer=", 13) == 0) {
      g_opt_writer = atoi(argv[i] + 13);
    } else if (strcmp(argv[i], "--opt-matchfinder") == 0) {
      g_opt_matchfinder = 1;
    } else if (strncmp(argv[i], "--opt-matchfinder=", 18) == 0) {
      g_opt_matchfinder = atoi(argv[i] + 18);
    } else if (strcmp(argv[i], "--opt-block-splitter") == 0) {
      g_opt_block_splitter = 1;
    } else if (strncmp(argv[i], "--opt-block-splitter=", 21) == 0) {
      g_opt_block_splitter = atoi(argv[i] + 21);
    } else {
      argv[dst++] = argv[i];
    }
  }
  *argc = dst;
}

int main(int argc, char** argv) {
  ParseCustomFlags(&argc, argv);

  if (g_corpus_dir.empty()) {
    if (fs::exists("benchmark/corpus")) {
      g_corpus_dir = "benchmark/corpus";
    } else {
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
  fprintf(stderr, "Quality: %d, lgwin: %d, opt_writer: %d, opt_matchfinder: %d, "
          "opt_block_splitter: %d\n",
          g_quality, g_lgwin, g_opt_writer, g_opt_matchfinder,
          g_opt_block_splitter);
  LoadCorpus(g_corpus_dir);

  if (g_corpus.empty()) {
    fprintf(stderr, "Error: No files found in %s\n", g_corpus_dir.c_str());
    return 1;
  }

  if (!g_dictionary_path.empty()) {
    if (!LoadDictionary(g_dictionary_path)) return 1;
  }

  /* Untimed matchfinding pass (run once). */
  if (!PrepareAll()) return 1;
  fprintf(stderr, "Prepared %zu files (%d metablocks total)\n", g_prepared.size(),
          [&] { int t = 0; for (auto& f : g_prepared) t += f.nmb; return t; }());

  if (!g_digest_path.empty()) {
    return WriteDigest(g_digest_path);
  }
  if (!g_roundtrip_path.empty()) {
    return WriteRoundtrip(g_roundtrip_path);
  }

  benchmark::Initialize(&argc, argv);
  benchmark::RunSpecifiedBenchmarks();
  benchmark::Shutdown();

  for (auto& f : g_prepared) MBB_DestroyPrepared(f.prepared);
  if (g_prepared_dict) BrotliEncoderDestroyPreparedDictionary(g_prepared_dict);
  return 0;
}
