/*
 * Corpus roundtrip test: loads all .js files from a directory, then for each
 * quality level (5-9) compresses and decompresses every file in parallel
 * threads, verifying each roundtrips to the original bytes.
 *
 * Uses Google Test with one parameterized test per quality level. File-level
 * parallelism within each quality uses std::thread.
 *
 * Usage:
 *   corpus_roundtrip_test --corpus=DIR [--gtest_filter=...]
 */

#include <gtest/gtest.h>
#include <brotli/encode.h>
#include <brotli/decode.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

struct CorpusFile {
  std::string name;
  std::vector<uint8_t> data;
};

static std::vector<CorpusFile> g_corpus;
static size_t g_corpus_total_size = 0;

static void LoadCorpus(const std::string& dir) {
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
    if (!f) continue;
    size_t nread = fread(cf.data.data(), 1, size, f);
    fclose(f);
    if (nread != size) continue;

    g_corpus_total_size += size;
    g_corpus.push_back(std::move(cf));
  }
}

class CorpusRoundtrip : public testing::TestWithParam<int> {};

TEST_P(CorpusRoundtrip, AllFiles) {
  const int quality = GetParam();
  ASSERT_FALSE(g_corpus.empty()) << "No corpus files loaded";

  const unsigned nthreads = std::thread::hardware_concurrency();
  const size_t nfiles = g_corpus.size();

  /* Per-file result: empty string = pass, non-empty = error message. */
  std::vector<std::string> errors(nfiles);
  std::vector<size_t> compressed_sizes(nfiles, 0);
  std::atomic<size_t> next_file{0};

  auto worker = [&]() {
    for (;;) {
      size_t i = next_file.fetch_add(1, std::memory_order_relaxed);
      if (i >= nfiles) break;

      const auto& cf = g_corpus[i];

      /* Compress */
      size_t max_compressed = BrotliEncoderMaxCompressedSize(cf.data.size());
      if (max_compressed == 0) max_compressed = cf.data.size() * 2;
      std::vector<uint8_t> compressed(max_compressed);
      size_t compressed_size = max_compressed;

      BROTLI_BOOL ok = BrotliEncoderCompress(
          quality, BROTLI_DEFAULT_WINDOW, BROTLI_MODE_GENERIC,
          cf.data.size(), cf.data.data(),
          &compressed_size, compressed.data());
      if (!ok) {
        errors[i] = "compression failed";
        continue;
      }

      /* Decompress */
      std::vector<uint8_t> decompressed(cf.data.size());
      size_t decompressed_size = cf.data.size();

      BrotliDecoderResult res = BrotliDecoderDecompress(
          compressed_size, compressed.data(),
          &decompressed_size, decompressed.data());
      if (res != BROTLI_DECODER_RESULT_SUCCESS) {
        errors[i] = "decompression failed (result=" +
                    std::to_string((int)res) + ")";
        continue;
      }

      /* Verify */
      if (decompressed_size != cf.data.size() ||
          memcmp(decompressed.data(), cf.data.data(), cf.data.size()) != 0) {
        errors[i] = "data mismatch";
        continue;
      }

      compressed_sizes[i] = compressed_size;
    }
  };

  std::vector<std::thread> threads;
  for (unsigned t = 0; t < nthreads; t++) {
    threads.emplace_back(worker);
  }
  for (auto& t : threads) t.join();

  /* Report failures */
  size_t total_compressed = 0;
  for (size_t i = 0; i < nfiles; i++) {
    EXPECT_TRUE(errors[i].empty())
        << "File " << g_corpus[i].name << ": " << errors[i];
    total_compressed += compressed_sizes[i];
  }

  double ratio = (double)total_compressed / (double)g_corpus_total_size;
  printf("Quality %d: %zu files, %.1f MB, ratio %.1f%%, %u threads\n",
         quality, nfiles, g_corpus_total_size / (1024.0 * 1024.0),
         ratio * 100.0, nthreads);
}

INSTANTIATE_TEST_SUITE_P(
    Qualities, CorpusRoundtrip,
    testing::Values(5, 6, 7, 8, 9),
    [](const testing::TestParamInfo<int>& info) {
      return "q" + std::to_string(info.param);
    });

int main(int argc, char** argv) {
  std::string corpus_dir;

  /* Strip --corpus= before passing to GTest. */
  int dst = 1;
  for (int i = 1; i < argc; i++) {
    if (strncmp(argv[i], "--corpus=", 9) == 0) {
      corpus_dir = argv[i] + 9;
    } else {
      argv[dst++] = argv[i];
    }
  }
  argc = dst;

  if (corpus_dir.empty()) {
    fprintf(stderr, "Usage: %s --corpus=DIR [--gtest_filter=...]\n", argv[0]);
    return 1;
  }

  LoadCorpus(corpus_dir);
  if (g_corpus.empty()) {
    fprintf(stderr, "No .js files found in %s\n", corpus_dir.c_str());
    return 1;
  }
  fprintf(stderr, "Loaded %zu files, %.1f MB from %s\n",
          g_corpus.size(), g_corpus_total_size / (1024.0 * 1024.0),
          corpus_dir.c_str());

  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
