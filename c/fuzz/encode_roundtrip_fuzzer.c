/* Copyright 2026 The Brotli Authors.

   Distributed under MIT license.
   See file LICENSE for detail or copy at https://opensource.org/licenses/MIT
*/

/* Roundtrip encoder fuzzer.

   This fork carries optimized encoder paths (matchfinder / writer / block
   splitter), enabled via BROTLI_PARAM_OPT_*. Some are bit-identical to the
   reference encoder and some deliberately are not, so byte-for-byte identity
   is not a property that holds across all of them. Lossless roundtrip is: no
   matter which optimizations are engaged, decoding the encoder's output must
   reproduce the original input exactly.

   This harness drives that check under a coverage-guided fuzzer. Each input
   supplies a small control header (encoder parameters + a streaming/flush
   schedule) followed by a payload; we encode the payload with the opt paths
   enabled, decode the result, and abort() unless it reproduces the original
   bytes, so libFuzzer records the input. Built with a sanitizer (see
   build_encode_roundtrip_fuzzer.sh) it simultaneously catches the memory-safety
   bugs -- SIMD over-reads, prefetch of invalid addresses, uninitialized lanes
   -- that a fixed-corpus check can hide.

   Shared (raw prefix) dictionaries -- the compound-dictionary matcher and the
   prepared-dictionary construction/prefetch paths -- are exercised in three
   env-selected modes:

     * No dictionary (default): the whole post-header input is the payload.
     * Fixed dictionary (ENCODE_FUZZ_DICT=<file>): one real dictionary is loaded
       and prepared once at startup and every payload runs against it. Best for
       realistic, full-size production dictionaries.
     * Fuzzed dictionary (ENCODE_FUZZ_DICT_FUZZED=1): the input is
       [header][dict][payload] with a length-prefixed dictionary region,
       re-prepared and attached each iteration. This drives dictionary
       *construction* across arbitrary content, and libFuzzer's cross-over
       mutators splice bytes between the two regions to manufacture matches.

   In every mode the same raw dictionary is attached on decode, so the roundtrip
   stays valid.

   Byte-for-byte "optimized == reference" identity, where it is expected, is a
   separate and stronger property checked out of band by the A/B harness
   (ab.py --mode digest), not here. */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <brotli/decode.h>
#include <brotli/encode.h>
#include <brotli/shared_dictionary.h>

/* Payload and dictionary regions are capped so per-iteration encode + dictionary
   preparation stay fast. lgwin is capped in DeriveConfig for the same reason
   (the ring buffer is allocated at 2^(lgwin+1) regardless of input size). */
#define MAX_PAYLOAD (1u << 16)  /* 64 KiB */
#define MAX_DICT (1u << 16)     /* 64 KiB (fuzzed-dictionary mode) */
#define HEADER_BYTES 8          /* h[0..5] config, h[6..7] fuzzed dict length */

/* ------------------------------------------------------------------ */
/* Growable output collector.                                         */
/* ------------------------------------------------------------------ */

typedef struct {
  uint8_t* data;
  size_t size;
  size_t capacity;
} DynBuf;

static void DynBufInit(DynBuf* b) {
  b->data = NULL;
  b->size = 0;
  b->capacity = 0;
}

static void DynBufFree(DynBuf* b) {
  free(b->data);
  b->data = NULL;
  b->size = 0;
  b->capacity = 0;
}

/* Returns 0 on allocation failure (out of scope for the fuzzer). */
static int DynBufAppend(DynBuf* b, const uint8_t* src, size_t n) {
  if (n == 0) return 1;
  if (b->size + n > b->capacity) {
    size_t ncap = b->capacity ? b->capacity : 4096;
    while (ncap < b->size + n) ncap *= 2;
    uint8_t* nd = (uint8_t*)realloc(b->data, ncap);
    if (!nd) return 0;
    b->data = nd;
    b->capacity = ncap;
  }
  memcpy(b->data + b->size, src, n);
  b->size += n;
  return 1;
}

/* ------------------------------------------------------------------ */
/* Encoder configuration, derived from the control header.            */
/* ------------------------------------------------------------------ */

typedef struct {
  int quality;
  int lgwin;
  int lgblock;      /* 0 = let the encoder choose. */
  int mode;
  int disable_ctx;  /* BROTLI_PARAM_DISABLE_LITERAL_CONTEXT_MODELING */
  size_t chunk_size;    /* 0 = feed the whole payload in one call. */
  size_t flush_period;  /* 0 = never FLUSH; else FLUSH every N chunks. */
} EncConfig;

static void DeriveConfig(const uint8_t* h, EncConfig* c) {
  c->quality = h[0] % (BROTLI_MAX_QUALITY - BROTLI_MIN_QUALITY + 1);

  /* lgwin capped at 20: the window is allocated in full (2^(lgwin+1) bytes)
     independent of payload size. 10..20 keeps allocations <= ~2 MiB while
     still spanning the interesting range -- small windows (<= 15) let a
     64 KiB payload wrap the ring buffer, large ones exercise the wide
     distance codes. Keeping lgwin <= BROTLI_MAX_WINDOW_BITS also lets the
     stock decoder verify the roundtrip without large-window setup. */
  c->lgwin = BROTLI_MIN_WINDOW_BITS + (h[1] % 11);  /* 10..20 */

  c->mode = h[2] % 3;                 /* GENERIC / TEXT / FONT */
  c->disable_ctx = (h[2] >> 4) & 1;

  {
    int lb = h[3] % 10;  /* 0 => auto, otherwise 16..24 */
    c->lgblock = lb ? (BROTLI_MIN_INPUT_BLOCK_BITS +
                       (lb - 1) % (BROTLI_MAX_INPUT_BLOCK_BITS -
                                   BROTLI_MIN_INPUT_BLOCK_BITS + 1))
                    : 0;
  }

  {
    unsigned e = h[4] % 12;                      /* 0..11 */
    c->chunk_size = e ? ((size_t)1 << e) : 0;    /* one-shot, or 2..2048 */
  }
  c->flush_period = h[5] % 8;                    /* 0..7 */
}

/* Optimized-subsystem toggles, referenced by numeric id rather than by enum
   name on purpose: this one harness is developed on `benchmarking` and rebased
   through every experiment branch, and each branch implements a different
   subset (writer / matchfinder / block splitter). Naming a constant a branch
   lacks would break the build there. Instead we enable all three by id and
   ignore the result -- BrotliEncoderSetParameter returns false for a parameter
   the current build doesn't implement, which simply leaves that toggle off.
   Every branch descends from `benchmarking`, which defines
   BROTLI_PARAM_OPT_WRITER = 13, so ids 13..15 are always inside the parameter
   enum's representable range. (10..12 are upstream's Base64/SIMD-hasher
   params -- never set them blindly here.) */
#define OPT_PARAM_WRITER 13
#define OPT_PARAM_MATCHFINDER 14
#define OPT_PARAM_BLOCK_SPLITTER 15

static void EnableOptToggles(BrotliEncoderState* s) {
  BrotliEncoderSetParameter(s, (BrotliEncoderParameter)OPT_PARAM_WRITER, 1u);
  BrotliEncoderSetParameter(s, (BrotliEncoderParameter)OPT_PARAM_MATCHFINDER, 1u);
  BrotliEncoderSetParameter(
      s, (BrotliEncoderParameter)OPT_PARAM_BLOCK_SPLITTER, 1u);
}

/* ------------------------------------------------------------------ */
/* Streaming encode with every available opt path enabled, optionally */
/* attaching a prepared dictionary.                                   */
/*   returns  1 = success (out populated)                             */
/*            0 = encoder reported an error / invalid config          */
/*           -1 = allocation failure (out of scope)                   */
/* ------------------------------------------------------------------ */

static int EncodeStream(const EncConfig* c, const uint8_t* input,
                        size_t input_size,
                        const BrotliEncoderPreparedDictionary* dict,
                        DynBuf* out) {
  BrotliEncoderState* s = BrotliEncoderCreateInstance(NULL, NULL, NULL);
  if (!s) return -1;

  int ok = 1;
  ok &= BrotliEncoderSetParameter(s, BROTLI_PARAM_QUALITY, (uint32_t)c->quality);
  ok &= BrotliEncoderSetParameter(s, BROTLI_PARAM_LGWIN, (uint32_t)c->lgwin);
  if (c->lgblock)
    ok &= BrotliEncoderSetParameter(s, BROTLI_PARAM_LGBLOCK, (uint32_t)c->lgblock);
  ok &= BrotliEncoderSetParameter(s, BROTLI_PARAM_MODE, (uint32_t)c->mode);
  if (c->disable_ctx)
    ok &= BrotliEncoderSetParameter(
        s, BROTLI_PARAM_DISABLE_LITERAL_CONTEXT_MODELING, 1u);
  ok &= BrotliEncoderSetParameter(s, BROTLI_PARAM_SIZE_HINT,
                                  (uint32_t)input_size);
  if (!ok) {
    BrotliEncoderDestroyInstance(s);
    return 0;
  }
  EnableOptToggles(s);
  if (dict && !BrotliEncoderAttachPreparedDictionary(s, dict)) {
    BrotliEncoderDestroyInstance(s);
    return 0;  /* couldn't attach: can't test this input, skip */
  }

  size_t chunk = c->chunk_size ? c->chunk_size : (input_size ? input_size : 1);
  uint8_t obuf[4096];
  size_t pos = 0;
  size_t chunk_index = 0;
  int result = 1;

  for (;;) {
    /* Pick this step's input window and operation. Varying the chunk size and
       flush cadence exercises the ring-buffer wrap and cross-flush encoder
       state that a single one-shot encode never reaches. */
    size_t take = 0;
    BrotliEncoderOperation op = BROTLI_OPERATION_FINISH;
    if (pos < input_size) {
      take = input_size - pos;
      if (take > chunk) take = chunk;
      chunk_index++;
      if (pos + take == input_size) {
        op = BROTLI_OPERATION_FINISH;
      } else if (c->flush_period && (chunk_index % c->flush_period) == 0) {
        op = BROTLI_OPERATION_FLUSH;
      } else {
        op = BROTLI_OPERATION_PROCESS;
      }
    }

    const uint8_t* next_in = input + pos;
    size_t avail_in = take;

    /* Consume the whole window and drain any pending output. */
    for (;;) {
      size_t avail_out = sizeof(obuf);
      uint8_t* next_out = obuf;
      if (!BrotliEncoderCompressStream(s, op, &avail_in, &next_in, &avail_out,
                                       &next_out, NULL)) {
        result = 0;
        goto cleanup;
      }
      size_t produced = sizeof(obuf) - avail_out;
      if (produced && !DynBufAppend(out, obuf, produced)) {
        result = -1;
        goto cleanup;
      }
      if (avail_in == 0 && !BrotliEncoderHasMoreOutput(s)) break;
    }

    pos += take;
    if (BrotliEncoderIsFinished(s)) break;
  }

cleanup:
  BrotliEncoderDestroyInstance(s);
  return result;
}

/* ------------------------------------------------------------------ */

static void ReportFailure(const EncConfig* c, const char* what, size_t dict_size,
                          size_t encoded_size, size_t input_size,
                          size_t decoded_size, size_t diff_offset) {
  fprintf(stderr,
          "roundtrip failure (%s)\n"
          "  quality=%d lgwin=%d lgblock=%d mode=%d disable_ctx=%d\n"
          "  chunk_size=%zu flush_period=%zu dict_size=%zu\n"
          "  input_size=%zu encoded_size=%zu decoded_size=%zu"
          " first_diff_offset=%zu\n",
          what, c->quality, c->lgwin, c->lgblock, c->mode, c->disable_ctx,
          c->chunk_size, c->flush_period, dict_size, input_size, encoded_size,
          decoded_size, diff_offset);
}

/* Decode `encoded` (attaching the same raw dictionary the encoder used) and
   abort() unless it reproduces `payload` exactly. */
static void DecodeAndCheck(const EncConfig* c, const uint8_t* dict_data,
                           size_t dict_size, const uint8_t* encoded,
                           size_t encoded_size, const uint8_t* payload,
                           size_t payload_size) {
  BrotliDecoderState* ds = BrotliDecoderCreateInstance(NULL, NULL, NULL);
  if (!ds) return;  /* allocation failure: out of scope */
  if (dict_size &&
      !BrotliDecoderAttachDictionary(ds, BROTLI_SHARED_DICTIONARY_RAW,
                                     dict_size, dict_data)) {
    BrotliDecoderDestroyInstance(ds);
    return;  /* couldn't set up the check: skip */
  }

  uint8_t* decoded = (uint8_t*)malloc(payload_size ? payload_size : 1);
  if (!decoded) {
    BrotliDecoderDestroyInstance(ds);
    return;
  }

  const uint8_t* next_in = encoded;
  size_t avail_in = encoded_size;
  uint8_t* next_out = decoded;
  size_t avail_out = payload_size;
  size_t total_out = 0;
  /* All input and an exactly-sized output buffer are provided up front, so a
     correct stream resolves in a single call. */
  BrotliDecoderResult r = BrotliDecoderDecompressStream(
      ds, &avail_in, &next_in, &avail_out, &next_out, &total_out);

  const char* fail = NULL;
  if (r == BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT) {
    fail = "decoder produced more output than the input";  /* over-production */
  } else if (r != BROTLI_DECODER_RESULT_SUCCESS) {
    fail = "decoder rejected the encoder's output";  /* truncated or corrupt */
  } else if (total_out != payload_size) {
    fail = "decoded length differs from input";
  }
  size_t off = 0;
  if (!fail && payload_size && memcmp(decoded, payload, payload_size) != 0) {
    while (off < payload_size && decoded[off] == payload[off]) off++;
    fail = "decoded bytes differ from input";
  }
  if (fail) {
    ReportFailure(c, fail, dict_size, encoded_size, payload_size, total_out,
                  off);
    free(decoded);
    abort();
  }
  free(decoded);
  BrotliDecoderDestroyInstance(ds);
}

/* ------------------------------------------------------------------ */
/* Fixed-dictionary mode: ENCODE_FUZZ_DICT names a real dictionary that is    */
/* loaded and prepared once for the whole run. Lazily initialized so the      */
/* standalone runner (which never calls LLVMFuzzerInitialize) honors it too.  */
/* ------------------------------------------------------------------ */

typedef enum { DICT_NONE, DICT_FIXED, DICT_FUZZED } DictMode;
static DictMode g_dict_mode = DICT_NONE;

static int g_init_done = 0;
static uint8_t* g_fixed_dict = NULL;
static size_t g_fixed_dict_size = 0;
static BrotliEncoderPreparedDictionary* g_fixed_prepared = NULL;

static void MaybeInitDict(void) {
  if (g_init_done) return;
  g_init_done = 1;

  const char* path = getenv("ENCODE_FUZZ_DICT");
  if (path && path[0]) {
    FILE* f = fopen(path, "rb");
    if (!f) {
      fprintf(stderr, "ENCODE_FUZZ_DICT: cannot open %s\n", path);
      exit(1);
    }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); exit(1); }
    long n = ftell(f);
    if (n < 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); exit(1); }
    uint8_t* buf = (uint8_t*)malloc((size_t)n ? (size_t)n : 1);
    if (!buf) { fclose(f); exit(1); }
    size_t rd = fread(buf, 1, (size_t)n, f);
    fclose(f);
    if (rd != (size_t)n) {
      fprintf(stderr, "ENCODE_FUZZ_DICT: short read on %s\n", path);
      exit(1);
    }
    /* Prepare for the maximum quality so it is valid at any fuzzed quality. */
    g_fixed_prepared = BrotliEncoderPrepareDictionary(
        BROTLI_SHARED_DICTIONARY_RAW, (size_t)n, buf, BROTLI_MAX_QUALITY, NULL,
        NULL, NULL);
    if (!g_fixed_prepared) {
      fprintf(stderr, "ENCODE_FUZZ_DICT: failed to prepare %s\n", path);
      exit(1);
    }
    g_fixed_dict = buf;
    g_fixed_dict_size = (size_t)n;
    g_dict_mode = DICT_FIXED;
    fprintf(stderr, "ENCODE_FUZZ_DICT: %s (%zu bytes)\n", path,
            g_fixed_dict_size);
    return;
  }

  const char* fz = getenv("ENCODE_FUZZ_DICT_FUZZED");
  if (fz && fz[0] && fz[0] != '0') {
    g_dict_mode = DICT_FUZZED;
    fprintf(stderr, "ENCODE_FUZZ_DICT_FUZZED: fuzzing dictionary content"
                    " (<= %u bytes)\n", (unsigned)MAX_DICT);
  }
  /* else: DICT_NONE (default) */
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  MaybeInitDict();
  if (size < HEADER_BYTES) return 0;  /* need the control header */

  EncConfig c;
  DeriveConfig(data, &c);

  const uint8_t* rest = data + HEADER_BYTES;
  size_t rest_size = size - HEADER_BYTES;

  const uint8_t* dict_data = NULL;
  size_t dict_size = 0;
  const uint8_t* payload = rest;
  size_t payload_size = rest_size;
  const BrotliEncoderPreparedDictionary* prepared = NULL;
  BrotliEncoderPreparedDictionary* owned = NULL;  /* fuzzed mode: we destroy it */

  if (g_dict_mode == DICT_FIXED) {
    /* The whole post-header input is payload; run it against the fixed dict. */
    dict_data = g_fixed_dict;
    dict_size = g_fixed_dict_size;
    prepared = g_fixed_prepared;
  } else if (g_dict_mode == DICT_FUZZED) {
    /* A length-prefixed dictionary region, then the payload. */
    size_t want = (size_t)data[6] | ((size_t)data[7] << 8);
    if (want > MAX_DICT) want = MAX_DICT;
    if (want > rest_size) want = rest_size;
    dict_data = rest;
    dict_size = want;
    payload = rest + dict_size;
    payload_size = rest_size - dict_size;
    if (dict_size) {
      owned = BrotliEncoderPrepareDictionary(BROTLI_SHARED_DICTIONARY_RAW,
                                             dict_size, dict_data,
                                             BROTLI_MAX_QUALITY, NULL, NULL,
                                             NULL);
      if (!owned) return 0;  /* couldn't prepare: skip */
      prepared = owned;
    }
  }
  /* else DICT_NONE: dict_size stays 0, payload is the whole post-header input. */
  if (payload_size > MAX_PAYLOAD) payload_size = MAX_PAYLOAD;

  DynBuf encoded;
  DynBufInit(&encoded);
  if (EncodeStream(&c, payload, payload_size, prepared, &encoded) == 1) {
    DecodeAndCheck(&c, dict_data, dict_size, encoded.data, encoded.size, payload,
                   payload_size);
  }
  DynBufFree(&encoded);
  if (owned) BrotliEncoderDestroyPreparedDictionary(owned);
  return 0;
}
