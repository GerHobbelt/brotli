/*
 * Internal API for brotli_metablock_benchmark.
 *
 * The metablock build/store functions (BrotliBuildMetaBlock /
 * BrotliBuildMetaBlockGreedy / BrotliStoreMetaBlock) and the encoder's static
 * helpers (EncodeData, WriteMetaBlockInternal, ...) are not exported from
 * libbrotlienc.so (BROTLI_INTERNAL = hidden visibility), so the benchmark
 * compiles the encoder source directly into a small C shim
 * (brotli_metablock_bench_impl.c, which #includes c/enc/encode.c) and drives
 * it through this opaque interface.
 *
 * The point of the split: run the (expensive) matchfinding pass ONCE to turn
 * the input into per-metablock Command arrays, then benchmark *only* the
 * Command-array -> metablock build+store step in the timing loop. See
 * brotli_metablock_benchmark.cc.
 */

#ifndef BROTLI_BENCHMARK_METABLOCK_IMPL_H_
#define BROTLI_BENCHMARK_METABLOCK_IMPL_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* An opaque "prepared input": an encoder state plus the Command arrays
   (metablocks) produced by a single matchfinding pass over some input. */
typedef struct MBB_Prepared MBB_Prepared;

/* Run matchfinding over `input` (length `input_size`) at the given
   quality/lgwin/mode/size_hint and opt toggles, capturing every metablock's
   Command array and the pre-write encoder state needed to rebuild it.

   `mask` is the power-of-2-minus-1 index mask used to read literal bytes from
   `input` when (re)building a metablock; it must satisfy (mask + 1) >
   input_size so that positions never wrap. The caller keeps `input` alive for
   the lifetime of the returned handle (MBB_Store re-reads it).

   If `output` is non-NULL (capacity `out_cap` >= 2 * input_size + 1000), the
   complete compressed stream is also produced there by running the real
   metablock store once (untimed) -- this is the exact encoder output, used for
   the --digest / --roundtrip gates. The number of compressed bytes written is
   stored in *compressed_size_out (0 if `output` is NULL).

   `dict` is an opaque prepared dictionary (a BrotliEncoderPreparedDictionary*,
   or NULL) attached before matchfinding so the captured commands match a real
   dictionary encode.

   Returns the handle, or NULL on allocation failure. */
MBB_Prepared* MBB_Prepare(int quality, int lgwin, int mode, size_t size_hint,
                          int opt_writer, int opt_matchfinder,
                          int opt_block_splitter, const void* dict,
                          const uint8_t* input, size_t input_size, size_t mask,
                          int* nmb_out, uint8_t* output, size_t out_cap,
                          size_t* compressed_size_out);

/* Number of metablocks captured for this input. */
int MBB_NumMetablocks(const MBB_Prepared* p);

/* The stream header bits (window bits) that precede the first metablock;
   seed last_bytes / last_bytes_bits with these before storing metablock 0. */
void MBB_GetHeader(const MBB_Prepared* p, uint16_t* last_bytes,
                   uint8_t* last_bytes_bits);

/* Rebuild metablock `i` (Command array -> metablock) via the encoder's real
   WriteMetaBlockInternal, reading literals from `input` indexed by `mask`, and
   writing the compressed bytes into `storage` (capacity >= 2 * input_size +
   503). `last_bytes` / `last_bytes_bits` carry the bit-level stream state in
   and out (chain them across metablocks). Returns the number of output bytes
   written to storage. */
size_t MBB_Store(const MBB_Prepared* p, int i, const uint8_t* input,
                 size_t mask, uint8_t* storage, uint16_t* last_bytes,
                 uint8_t* last_bytes_bits);

/* Free the handle and all captured Command arrays. */
void MBB_DestroyPrepared(MBB_Prepared* p);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif  /* BROTLI_BENCHMARK_METABLOCK_IMPL_H_ */
