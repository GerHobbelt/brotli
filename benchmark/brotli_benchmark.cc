/*
 * Brotli encoder benchmark using Google Benchmark.
 *
 * Loads all .min.js files from benchmark/corpus/ into memory, then
 * benchmarks BrotliEncoderCompress() across the entire corpus.
 *
 * Usage:
 *   brotli_benchmark [--quality=N] [--lgwin=N] [--corpus=DIR]
 *                    [--digest=FILE] [--roundtrip=FILE] [--threads=N]
 *                    [--dictionary=FILE] [--opt-writer] [Google Benchmark flags...]
 *
 * Examples:
 *   brotli_benchmark
 *   brotli_benchmark --quality=9
 *   brotli_benchmark --dictionary=shared.dict   # attach a raw LZ77 dictionary
 *   brotli_benchmark --benchmark_perf_counters=CYCLES,INSTRUCTIONS
 *   brotli_benchmark --benchmark_out=results.json --benchmark_out_format=json
 *   brotli_benchmark --digest=out.txt   # write per-file output hashes, no bench
 *   brotli_benchmark --roundtrip=out.txt --threads=16   # sizes + decode check
 */

/* For sched_setaffinity / CPU_SET (used to pin digest worker threads). Must be
   defined before any system header pulls in <sched.h>. */
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
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <sched.h>
#endif

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
static std::string g_roundtrip_path;
/* Worker threads for the parallel per-file modes --digest / --roundtrip
   (--threads); 1 = serial. These are correctness gates, not timed runs, so they
   parallelize across all cores. Each worker pins itself to a distinct CPU so the
   isolated benchmarking core(s) -- which the scheduler otherwise leaves idle --
   are used too. */
static int g_worker_threads = 1;
static int g_opt_writer = 0;  /* BROTLI_PARAM_OPT_WRITER toggle (--opt-writer) */
/* Optimized match-finder toggle (--opt-matchfinder[=N]). Parameter id 14 is
   reserved for BROTLI_PARAM_OPT_MATCHFINDER in encode.h; it is spelled
   numerically here (like the fuzzer and the metablock benchmark do) so the
   harness builds on trees that do not carry the optimized match finder --
   there, BrotliEncoderSetParameter rejects it, which is the intended
   loud failure. N selects the core (1 = byte-tag, 2 = packed). */
#define OPT_PARAM_MATCHFINDER ((BrotliEncoderParameter)14)
static int g_opt_matchfinder = 0;
/* BROTLI_PARAM_OPT_BLOCK_SPLITTER toggle (--opt-block-splitter) */
static int g_opt_block_splitter = 0;
/* BROTLI_PARAM_SIMD_HASHER (--simd-hasher): BrotliEncoderSimdHasher value;
   1 (ENABLE) raises the SIMD-hasher quality ceiling to 7. 0 = leave default. */
static int g_simd_hasher = 0;

/* Optional shared (raw LZ77 prefix) dictionary attached to every encode via the
   streaming API (--dictionary). The one-shot BrotliEncoderCompress cannot take
   a custom dictionary, which is why all encoding goes through EncodeOneFile. */
static std::string g_dictionary_path;
static std::vector<uint8_t> g_dictionary;  /* must outlive g_prepared_dict */
static BrotliEncoderPreparedDictionary* g_prepared_dict = nullptr;

/* Encoder mode shared by the benchmark loop and the --digest mode, so a digest
   reflects exactly the bytes the benchmark would compress. */
static const BrotliEncoderMode g_mode = BROTLI_MODE_TEXT;

/* Parallel-mode helpers, defined below with the digest/roundtrip modes. */
static int ClampWorkerThreads(size_t n);
static void RunParallelOverCorpus(int nthreads,
                                  const std::function<void(size_t)>& body);

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

  /* Metadata pre-pass (cheap stats): keep non-empty files, in sorted order, and
     size the g_corpus slots so the byte reads below can run index-aligned with
     no locking. */
  std::vector<std::pair<fs::path, size_t>> pending;
  for (const auto& path : paths) {
    std::error_code ec;
    auto size = fs::file_size(path, ec);
    if (ec || size == 0) continue;
    pending.emplace_back(path, static_cast<size_t>(size));
  }
  const size_t n = pending.size();
  g_corpus.resize(n);
  std::vector<char> ok(n, 1);  /* 0 = read failed, dropped after the loop */

  auto load_one = [&](size_t i) {
    CorpusFile& cf = g_corpus[i];
    cf.name = pending[i].first.filename().string();
    cf.data.resize(pending[i].second);
    FILE* f = fopen(pending[i].first.c_str(), "rb");
    if (!f) { ok[i] = 0; return; }
    size_t got = fread(cf.data.data(), 1, pending[i].second, f);
    fclose(f);
    if (got != pending[i].second) ok[i] = 0;
  };

  /* The parallel per-file modes (--digest/--roundtrip) fan the reads across all
     cores; the benchmark path keeps the load serial so it stays on its single
     pinned core and doesn't perturb timing. */
  const bool parallel = !g_digest_path.empty() || !g_roundtrip_path.empty();
  if (parallel && n > 1) {
    RunParallelOverCorpus(ClampWorkerThreads(n), load_one);
  } else {
    for (size_t i = 0; i < n; i++) load_one(i);
  }

  /* Compact out any files that failed to read (order preserved), and total the
     rest. */
  size_t w = 0;
  for (size_t i = 0; i < n; i++) {
    if (!ok[i]) {
      fprintf(stderr, "Warning: failed to read %s\n",
              pending[i].first.c_str());
      continue;
    }
    g_corpus_total_size += g_corpus[i].data.size();
    if (w != i) g_corpus[w] = std::move(g_corpus[i]);
    w++;
  }
  g_corpus.resize(w);

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
  /* F4 (harness audit): a rejected SetParameter used to run the baseline
     while stderr/counters claimed the requested config. */
  bool params_ok = true;
  params_ok &= !!BrotliEncoderSetParameter(s, BROTLI_PARAM_QUALITY,
                                           (uint32_t)g_quality);
  params_ok &= !!BrotliEncoderSetParameter(s, BROTLI_PARAM_LGWIN,
                                           (uint32_t)g_lgwin);
  params_ok &= !!BrotliEncoderSetParameter(s, BROTLI_PARAM_MODE,
                                           (uint32_t)g_mode);
  params_ok &= !!BrotliEncoderSetParameter(s, BROTLI_PARAM_SIZE_HINT,
                                           (uint32_t)input_size);
  if (g_opt_writer) {
    params_ok &= !!BrotliEncoderSetParameter(s, BROTLI_PARAM_OPT_WRITER,
                                             (uint32_t)g_opt_writer);
  }
  if (g_opt_matchfinder) {
    params_ok &= !!BrotliEncoderSetParameter(s, OPT_PARAM_MATCHFINDER,
                                             (uint32_t)g_opt_matchfinder);
  }
  if (g_opt_block_splitter) {
    params_ok &= !!BrotliEncoderSetParameter(
        s, BROTLI_PARAM_OPT_BLOCK_SPLITTER, (uint32_t)g_opt_block_splitter);
  }
  if (g_simd_hasher) {
    params_ok &= !!BrotliEncoderSetParameter(s, BROTLI_PARAM_SIMD_HASHER,
                                             (uint32_t)g_simd_hasher);
  }
  if (!params_ok) {
    fprintf(stderr, "Error: encoder rejected a parameter value "
            "(quality=%d lgwin=%d opt_writer=%d opt_matchfinder=%d "
            "opt_block_splitter=%d simd_hasher=%d)\n",
            g_quality, g_lgwin, g_opt_writer, g_opt_matchfinder,
            g_opt_block_splitter, g_simd_hasher);
    exit(1);
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

/* Pin the calling thread to a single logical CPU. Explicit pinning is what lets
   the digest use the kernel's isolated cores (isolcpus keeps the scheduler off
   them, so unpinned threads never land there). No-op off Linux. */
static void PinCurrentThreadToCpu(int cpu) {
#if defined(__linux__)
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(cpu, &set);
  sched_setaffinity(0, sizeof(set), &set);  /* 0 = calling thread */
#else
  (void)cpu;
#endif
}

/* Worker-thread count clamped to [1, num_files]. */
static int ClampWorkerThreads(size_t n) {
  int t = g_worker_threads;
  if (t < 1) t = 1;
  if (static_cast<size_t>(t) > n) t = n ? static_cast<int>(n) : 1;
  return t;
}

/* Run body(i) for every corpus index across `nthreads` CPU-pinned workers. Each
   worker owns a disjoint stride of indices, so there is no locking on the hot
   path; the body writes only into slots it owns. Explicit per-thread pinning is
   what lets the isolated benchmarking cores be used (isolcpus keeps the
   scheduler off them, so unpinned threads never land there). */
static void RunParallelOverCorpus(int nthreads,
                                  const std::function<void(size_t)>& body) {
  const size_t n = g_corpus.size();
  auto worker = [&](int t) {
    PinCurrentThreadToCpu(t);
    for (size_t i = static_cast<size_t>(t); i < n;
         i += static_cast<size_t>(nthreads)) {
      body(i);
    }
  };
  std::vector<std::thread> pool;
  for (int t = 1; t < nthreads; t++) pool.emplace_back(worker, t);
  worker(0);
  for (auto& th : pool) th.join();
}

/* Write index-aligned per-file `lines` to `path` in corpus order (so the output
   is independent of thread count/scheduling). Returns 0 on success. */
static int WriteLines(const std::string& path, const std::vector<std::string>& lines,
                      const char* what) {
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

/* Compress every corpus file at the configured quality/lgwin/mode and write a
   per-file digest of the compressed output: "name<TAB>len<TAB>fnv64". Used to
   verify two encoder versions produce bit-identical output. Returns 0 on
   success. */
static int WriteDigest(const std::string& path) {
  const size_t n = g_corpus.size();
  const int nthreads = ClampWorkerThreads(n);
  std::vector<std::string> lines(n);  /* lines[i] = digest line for g_corpus[i] */
  std::atomic<bool> ok_all{true};
  std::mutex err_mu;
  std::string first_err;

  RunParallelOverCorpus(nthreads, [&](size_t i) {
    if (!ok_all.load(std::memory_order_relaxed)) return;
    const CorpusFile& cf = g_corpus[i];
    size_t max_size = BrotliEncoderMaxCompressedSize(cf.data.size());
    if (max_size == 0) max_size = cf.data.size() * 2;
    std::vector<uint8_t> compressed(max_size);
    size_t encoded_size = max_size;
    if (!EncodeOneFile(cf.data.data(), cf.data.size(),
                       compressed.data(), &encoded_size)) {
      ok_all.store(false, std::memory_order_relaxed);
      std::lock_guard<std::mutex> lk(err_mu);
      if (first_err.empty()) first_err = cf.name;
      return;
    }
    char tail[64];
    snprintf(tail, sizeof(tail), "\t%zu\t%016llx\n", encoded_size,
             (unsigned long long)Fnv1a64(compressed.data(), encoded_size));
    lines[i] = cf.name + tail;
  });

  if (!ok_all.load()) {
    fprintf(stderr, "Error: compression failed for %s\n", first_err.c_str());
    return 1;
  }
  if (WriteLines(path, lines, "digest") != 0) return 1;
  fprintf(stderr, "Wrote digest for %zu files to %s (%d thread%s)\n",
          n, path.c_str(), nthreads, nthreads == 1 ? "" : "s");
  return 0;
}

/* Compress + decompress every corpus file, verify the roundtrip reproduces the
   input exactly, and record the compressed size. Writes one line per file:
   "name<TAB>input_size<TAB>compressed_size<TAB>ok" (ok = 1 if decompression
   reproduced the input, else 0). Lets a caller compare the compressed-size
   impact of a ratio-changing change while confirming the output still decodes.
   Returns 0 unless compression itself failed; a roundtrip mismatch is reported
   as ok=0 (and counted), not a hard error. */

/* Decode |encoded| and compare against |expect|. Uses the streaming decoder so
   the raw shared dictionary (when configured) can be attached -- the one-shot
   BrotliDecoderDecompress cannot take one, which made every dict-referencing
   file a false roundtrip FAIL. */
static bool DecodeMatches(const uint8_t* encoded, size_t encoded_size,
                          const uint8_t* expect, size_t expect_size) {
  BrotliDecoderState* ds = BrotliDecoderCreateInstance(nullptr, nullptr,
                                                       nullptr);
  if (!ds) return false;
  if (!g_dictionary.empty() &&
      !BrotliDecoderAttachDictionary(ds, BROTLI_SHARED_DICTIONARY_RAW,
                                     g_dictionary.size(),
                                     g_dictionary.data())) {
    BrotliDecoderDestroyInstance(ds);
    return false;
  }
  std::vector<uint8_t> decoded(expect_size ? expect_size : 1);
  const uint8_t* next_in = encoded;
  size_t avail_in = encoded_size;
  uint8_t* next_out = decoded.data();
  size_t avail_out = expect_size;
  size_t total_out = 0;
  BrotliDecoderResult r = BrotliDecoderDecompressStream(
      ds, &avail_in, &next_in, &avail_out, &next_out, &total_out);
  BrotliDecoderDestroyInstance(ds);
  return r == BROTLI_DECODER_RESULT_SUCCESS && total_out == expect_size &&
         (expect_size == 0 || memcmp(decoded.data(), expect, expect_size) == 0);
}

static int WriteRoundtrip(const std::string& path) {
  const size_t n = g_corpus.size();
  const int nthreads = ClampWorkerThreads(n);
  std::vector<std::string> lines(n);
  std::atomic<bool> enc_ok{true};
  std::atomic<size_t> mismatches{0};
  std::mutex err_mu;
  std::string first_err;

  RunParallelOverCorpus(nthreads, [&](size_t i) {
    const CorpusFile& cf = g_corpus[i];
    size_t max_size = BrotliEncoderMaxCompressedSize(cf.data.size());
    if (max_size == 0) max_size = cf.data.size() * 2;
    std::vector<uint8_t> compressed(max_size);
    size_t encoded_size = max_size;
    if (!EncodeOneFile(cf.data.data(), cf.data.size(),
                       compressed.data(), &encoded_size)) {
      enc_ok.store(false, std::memory_order_relaxed);
      std::lock_guard<std::mutex> lk(err_mu);
      if (first_err.empty()) first_err = cf.name;
      return;
    }
    /* Decode back and compare to the original input. */
    bool ok = DecodeMatches(compressed.data(), encoded_size,
                            cf.data.data(), cf.data.size());
    if (!ok) mismatches.fetch_add(1, std::memory_order_relaxed);
    char tail[80];
    snprintf(tail, sizeof(tail), "\t%zu\t%zu\t%d\n",
             cf.data.size(), encoded_size, ok ? 1 : 0);
    lines[i] = cf.name + tail;
  });

  if (!enc_ok.load()) {
    fprintf(stderr, "Error: compression failed for %s\n", first_err.c_str());
    return 1;
  }
  if (WriteLines(path, lines, "roundtrip") != 0) return 1;
  fprintf(stderr, "Roundtripped %zu files to %s (%d thread%s), %zu mismatch%s\n",
          n, path.c_str(), nthreads, nthreads == 1 ? "" : "s",
          mismatches.load(), mismatches.load() == 1 ? "" : "es");
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
  state.counters["OptMF"] = g_opt_matchfinder;
  state.counters["OptBS"] = g_opt_block_splitter;
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
    } else if (strncmp(argv[i], "--roundtrip=", 12) == 0) {
      g_roundtrip_path = argv[i] + 12;
    } else if (strncmp(argv[i], "--threads=", 10) == 0) {
      g_worker_threads = atoi(argv[i] + 10);
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
    } else if (strncmp(argv[i], "--simd-hasher=", 14) == 0) {
      g_simd_hasher = atoi(argv[i] + 14);
    } else {
      argv[dst++] = argv[i];
    }
  }
  *argc = dst;
}


/* F3 (harness audit): a mistyped custom flag used to be passed through to
   google-benchmark and silently ignored in every mode (digest/roundtrip
   return before Initialize), turning A/B runs into A/A. Anything left after
   ParseCustomFlags must be a google-benchmark flag. */
static void RejectUnknownFlags(int argc, char** argv) {
  for (int i = 1; i < argc; i++) {
    const char* a = argv[i];
    if (strncmp(a, "--benchmark_", 12) == 0 || strcmp(a, "--help") == 0 ||
        strncmp(a, "--v=", 4) == 0) {
      continue;
    }
    fprintf(stderr, "Error: unrecognized flag '%s'\n", a);
    exit(1);
  }
}

int main(int argc, char** argv) {
  ParseCustomFlags(&argc, argv);
  RejectUnknownFlags(argc, argv);

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
  fprintf(stderr, "Quality: %d, lgwin: %d, opt_writer: %d, "
          "opt_matchfinder: %d, opt_block_splitter: %d, simd_hasher: %d\n",
          g_quality, g_lgwin, g_opt_writer, g_opt_matchfinder,
          g_opt_block_splitter, g_simd_hasher);
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

  /* Roundtrip mode: emit per-file input/compressed sizes + a decode check and
     exit without benchmarking (used to compare compressed-size gain/loss). */
  if (!g_roundtrip_path.empty()) {
    return WriteRoundtrip(g_roundtrip_path);
  }

  benchmark::Initialize(&argc, argv);
  if (benchmark::ReportUnrecognizedArguments(argc, argv)) return 1;
  benchmark::RunSpecifiedBenchmarks();
  benchmark::Shutdown();

  if (g_prepared_dict) BrotliEncoderDestroyPreparedDictionary(g_prepared_dict);
  return 0;
}
