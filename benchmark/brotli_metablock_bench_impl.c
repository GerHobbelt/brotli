/*
 * C shim for brotli_metablock_benchmark.
 *
 * Pulls in the whole encoder (c/enc/encode.c) so it can reach the encoder's
 * static helpers (EncodeData, WriteMetaBlockInternal, ChooseContextMode,
 * ExtendLastCommand, ...) that are not exported from libbrotlienc.so. It
 * exposes the small opaque MBB_* interface declared in
 * brotli_metablock_bench_impl.h.
 *
 * MBB_Prepare runs the matchfinding pass (BrotliCreateBackwardReferences) over
 * the input, splitting it into metablocks exactly as the real encoder does, and
 * captures each metablock's Command array plus the pre-write encoder state. It
 * also runs the real WriteMetaBlockInternal once per metablock so the captured
 * state transitions (distance cache, flush position, prev bytes) are exact and
 * the produced stream is the encoder's true output (for the digest gate).
 *
 * MBB_Store re-runs WriteMetaBlockInternal for one captured metablock -- this is
 * the unit the benchmark times.
 */

#include <stdio.h>

#include "c/enc/encode.c"

#include "brotli_metablock_bench_impl.h"

/* One captured metablock: everything WriteMetaBlockInternal needs, plus the
   Command array (owned copy). */
typedef struct MBB_Metablock {
  uint32_t num_commands;
  uint32_t num_literals;
  uint64_t last_flush_pos;
  uint32_t bytes;
  int is_last;
  int literal_context_mode;  /* ContextType */
  uint8_t prev_byte;
  uint8_t prev_byte2;
  int dist_cache[4];
  int saved_dist_cache[4];
  Command* commands;
} MBB_Metablock;

struct MBB_Prepared {
  BrotliEncoderState* s;
  MBB_Metablock* mbs;
  int nmb;
  int mbs_cap;
  uint16_t header_last_bytes;
  uint8_t header_last_bytes_bits;
  /* Scratch Command buffer for MBB_Store, sized at prepare time to the largest
     captured metablock so the timed loop never allocates. */
  Command* cmd_scratch;
  size_t cmd_scratch_cap;
};

/* Mirrors EncodeData's matchfinding portion, then captures the metablock and
   runs the real WriteMetaBlockInternal (for exact output + state transitions).
   Returns BROTLI_TRUE if a metablock was completed (captured into *mb and, if
   *out is non-NULL, stored there; *out_size bytes), else BROTLI_FALSE (merged
   with the next input block). */
static BROTLI_BOOL MBB_EncodeData(BrotliEncoderState* s, BROTLI_BOOL is_last,
                                  const uint8_t* input, size_t mask,
                                  MBB_Metablock* mb, uint8_t** storage,
                                  size_t* storage_cap, uint8_t* out,
                                  size_t out_cap, size_t* out_size) {
  const uint64_t delta = UnprocessedInputSize(s);
  uint32_t bytes = (uint32_t)delta;
  uint32_t wrapped_last_processed_pos = WrapPosition(s->last_processed_pos_);
  uint8_t* data = s->ringbuffer_.buffer_;
  uint32_t rmask = s->ringbuffer_.mask_;
  MemoryManager* m = &s->memory_manager_;
  ContextType literal_context_mode;
  ContextLut literal_context_lut;

  if (delta == 0) return BROTLI_FALSE;
  if (s->is_last_block_emitted_) return BROTLI_FALSE;
  if (is_last) s->is_last_block_emitted_ = BROTLI_TRUE;
  if (delta > InputBlockSize(s)) return BROTLI_FALSE;

  {
    size_t newsize = s->num_commands_ + bytes / 2 + 1;
    if (newsize > s->cmd_alloc_size_) {
      Command* new_commands;
      newsize += (bytes / 4) + 16;
      s->cmd_alloc_size_ = newsize;
      new_commands = BROTLI_ALLOC(m, Command, newsize);
      if (BROTLI_IS_OOM(m) || BROTLI_IS_NULL(new_commands)) return BROTLI_FALSE;
      if (s->commands_) {
        memcpy(new_commands, s->commands_, sizeof(Command) * s->num_commands_);
        BROTLI_FREE(m, s->commands_);
      }
      s->commands_ = new_commands;
    }
  }

  InitOrStitchToPreviousBlock(m, &s->hasher_, data, rmask, &s->params,
      wrapped_last_processed_pos, bytes, is_last);
  literal_context_mode = ChooseContextMode(
      &s->params, data, WrapPosition(s->last_flush_pos_), rmask,
      (size_t)(s->input_pos_ - s->last_flush_pos_));
  literal_context_lut = BROTLI_CONTEXT_LUT(literal_context_mode);
  if (BROTLI_IS_OOM(m)) return BROTLI_FALSE;

  if (s->num_commands_ && s->last_insert_len_ == 0) {
    ExtendLastCommand(s, &bytes, &wrapped_last_processed_pos);
  }

  if (s->params.quality == ZOPFLIFICATION_QUALITY) {
    BrotliCreateZopfliBackwardReferences(m, bytes, wrapped_last_processed_pos,
        data, rmask, literal_context_lut, &s->params,
        &s->hasher_, s->dist_cache_,
        &s->last_insert_len_, &s->commands_[s->num_commands_],
        &s->num_commands_, &s->num_literals_);
  } else if (s->params.quality == HQ_ZOPFLIFICATION_QUALITY) {
    BrotliCreateHqZopfliBackwardReferences(m, bytes, wrapped_last_processed_pos,
        data, rmask, literal_context_lut, &s->params,
        &s->hasher_, s->dist_cache_,
        &s->last_insert_len_, &s->commands_[s->num_commands_],
        &s->num_commands_, &s->num_literals_);
  } else {
    BrotliCreateBackwardReferences(bytes, wrapped_last_processed_pos,
        data, rmask, literal_context_lut, &s->params,
        &s->hasher_, s->dist_cache_,
        &s->last_insert_len_, &s->commands_[s->num_commands_],
        &s->num_commands_, &s->num_literals_);
  }
  if (BROTLI_IS_OOM(m)) return BROTLI_FALSE;

  {
    const size_t max_length = MaxMetablockSize(&s->params);
    const size_t max_literals = max_length / 8;
    const size_t max_commands = max_length / 8;
    const size_t processed_bytes = (size_t)(s->input_pos_ - s->last_flush_pos_);
    const BROTLI_BOOL next_input_fits_metablock = TO_BROTLI_BOOL(
        processed_bytes + InputBlockSize(s) <= max_length);
    const BROTLI_BOOL should_flush = TO_BROTLI_BOOL(
        s->params.quality < MIN_QUALITY_FOR_BLOCK_SPLIT &&
        s->num_literals_ + s->num_commands_ >= MAX_NUM_DELAYED_SYMBOLS);
    if (!is_last && !should_flush &&
        next_input_fits_metablock &&
        s->num_literals_ < max_literals &&
        s->num_commands_ < max_commands) {
      if (UpdateLastProcessedPos(s)) HasherReset(&s->hasher_);
      return BROTLI_FALSE;  /* merge with next input block */
    }
  }

  if (s->last_insert_len_ > 0) {
    InitInsertCommand(&s->commands_[s->num_commands_++], s->last_insert_len_);
    s->num_literals_ += s->last_insert_len_;
    s->last_insert_len_ = 0;
  }

  /* Capture the metablock (pre-write state). */
  mb->num_commands = (uint32_t)s->num_commands_;
  mb->num_literals = (uint32_t)s->num_literals_;
  mb->last_flush_pos = s->last_flush_pos_;
  mb->bytes = (uint32_t)(s->input_pos_ - s->last_flush_pos_);
  mb->is_last = (int)is_last;
  mb->literal_context_mode = (int)literal_context_mode;
  mb->prev_byte = s->prev_byte_;
  mb->prev_byte2 = s->prev_byte2_;
  memcpy(mb->dist_cache, s->dist_cache_, 4 * sizeof(int));
  memcpy(mb->saved_dist_cache, s->saved_dist_cache_, 4 * sizeof(int));
  mb->commands = BROTLI_ALLOC(m, Command, s->num_commands_);
  if (BROTLI_IS_OOM(m) || BROTLI_IS_NULL(mb->commands)) return BROTLI_FALSE;
  memcpy(mb->commands, s->commands_, sizeof(Command) * s->num_commands_);

  /* Run the real WriteMetaBlockInternal (exact output + state transitions). */
  {
    size_t need = 2 * (size_t)mb->bytes + 503;
    if (need > *storage_cap) {
      BROTLI_FREE(m, *storage);
      *storage = BROTLI_ALLOC(m, uint8_t, need);
      if (BROTLI_IS_OOM(m) || BROTLI_IS_NULL(*storage)) return BROTLI_FALSE;
      *storage_cap = need;
    }
    size_t storage_ix = s->last_bytes_bits_;
    (*storage)[0] = (uint8_t)s->last_bytes_;
    (*storage)[1] = (uint8_t)(s->last_bytes_ >> 8);
    WriteMetaBlockInternal(m, input, mask, s->last_flush_pos_, mb->bytes,
        is_last, literal_context_mode, &s->params, s->prev_byte_,
        s->prev_byte2_, s->num_literals_, s->num_commands_, s->commands_,
        s->saved_dist_cache_, s->dist_cache_, &storage_ix, *storage);
    if (BROTLI_IS_OOM(m)) return BROTLI_FALSE;
    s->last_bytes_ = (uint16_t)(*storage)[storage_ix >> 3];
    s->last_bytes_bits_ = (uint8_t)(storage_ix & 7u);
    *out_size = storage_ix >> 3;
    if (out && *out_size <= out_cap) {
      memcpy(out, *storage, *out_size);
    } else if (out) {
      return BROTLI_FALSE;
    }
  }

  /* Post-write state update (exactly as EncodeData does). */
  s->last_flush_pos_ = s->input_pos_;
  if (UpdateLastProcessedPos(s)) HasherReset(&s->hasher_);
  if (s->last_flush_pos_ > 0)
    s->prev_byte_ = data[((uint32_t)s->last_flush_pos_ - 1) & rmask];
  if (s->last_flush_pos_ > 1)
    s->prev_byte2_ = data[(uint32_t)(s->last_flush_pos_ - 2) & rmask];
  s->num_commands_ = 0;
  s->num_literals_ = 0;
  memcpy(s->saved_dist_cache_, s->dist_cache_,
         4 * sizeof(s->saved_dist_cache_[0]));
  return BROTLI_TRUE;
}

MBB_Prepared* MBB_Prepare(int quality, int lgwin, int mode, size_t size_hint,
                          int opt_writer, int opt_matchfinder,
                          int opt_block_splitter, const void* dict,
                          const uint8_t* input, size_t input_size, size_t mask,
                          int* nmb_out, uint8_t* output, size_t out_cap,
                          size_t* compressed_size_out) {
  MemoryManager* m;
  BrotliEncoderState* s = BrotliEncoderCreateInstance(NULL, NULL, NULL);
  if (BROTLI_IS_NULL(s)) return NULL;
  BrotliEncoderSetParameter(s, BROTLI_PARAM_QUALITY, (uint32_t)quality);
  BrotliEncoderSetParameter(s, BROTLI_PARAM_LGWIN, (uint32_t)lgwin);
  BrotliEncoderSetParameter(s, BROTLI_PARAM_MODE, (uint32_t)mode);
  BrotliEncoderSetParameter(s, BROTLI_PARAM_SIZE_HINT, (uint32_t)size_hint);
  /* opt_writer / opt_block_splitter are real params on this branch; the
     matchfinder toggle is a raw value that SetParameter rejects (inert) unless
     the branch carries the optimized match-finder copies. */
  BrotliEncoderSetParameter(s, BROTLI_PARAM_OPT_WRITER, (uint32_t)opt_writer);
  BrotliEncoderSetParameter(s, BROTLI_PARAM_OPT_BLOCK_SPLITTER,
                            (uint32_t)opt_block_splitter);
  BrotliEncoderSetParameter(s, (BrotliEncoderParameter)14,
                            (uint32_t)opt_matchfinder);
  if (dict != NULL) {
    BrotliEncoderAttachPreparedDictionary(
        s, (const BrotliEncoderPreparedDictionary*)dict);
  }

  /* Initialize (allocates ring buffer, hash table, encodes window bits). */
  if (!EnsureInitialized(s)) {
    BrotliEncoderDestroyInstance(s);
    return NULL;
  }
  m = &s->memory_manager_;

  MBB_Prepared* p = (MBB_Prepared*)malloc(sizeof(MBB_Prepared));
  if (BROTLI_IS_NULL(p)) {
    BrotliEncoderDestroyInstance(s);
    return NULL;
  }
  p->s = s;
  p->nmb = 0;
  p->mbs_cap = 8;
  p->header_last_bytes = s->last_bytes_;
  p->header_last_bytes_bits = s->last_bytes_bits_;
  p->cmd_scratch = NULL;
  p->cmd_scratch_cap = 0;
  p->mbs = (MBB_Metablock*)BROTLI_ALLOC(m, MBB_Metablock, p->mbs_cap);
  if (BROTLI_IS_NULL(p->mbs)) {
    free(p);
    BrotliEncoderDestroyInstance(s);
    return NULL;
  }

  size_t offset = 0;
  size_t out = 0;
  uint8_t* storage = NULL;
  size_t storage_cap = 0;
  while (offset < input_size) {
    /* Copy up to the unprocessed-input limit (capped at InputBlockSize, not the
       ring-buffer size) before processing, mirroring BrotliEncoderCompressStream.
       When the limit is reached (block == 0) or the input is exhausted, fall
       through to MBB_EncodeData, which frees the unprocessed count. */
    size_t block = RemainingInputBlockSize(s);
    if (block != 0) {
      size_t copy =
          block < (input_size - offset) ? block : (input_size - offset);
      CopyInputToRingBuffer(s, copy, input + offset);
      offset += copy;
      s->total_in_ += copy;
    }
    BROTLI_BOOL is_last = (BROTLI_BOOL)(offset == input_size);

    if (p->nmb >= p->mbs_cap) {
      int new_cap = p->mbs_cap * 2;
      MBB_Metablock* grown =
          (MBB_Metablock*)BROTLI_ALLOC(m, MBB_Metablock, new_cap);
      if (BROTLI_IS_NULL(grown)) break;
      memcpy(grown, p->mbs, sizeof(MBB_Metablock) * p->nmb);
      BROTLI_FREE(m, p->mbs);
      p->mbs = grown;
      p->mbs_cap = new_cap;
    }

    size_t out_size = 0;
    uint8_t* out_ptr = output ? output + out : NULL;
    size_t out_room = output ? out_cap - out : 0;
    if (MBB_EncodeData(s, is_last, input, mask, &p->mbs[p->nmb], &storage,
                       &storage_cap, out_ptr, out_room, &out_size)) {
      out += out_size;
      p->nmb++;
    }
  }
  BROTLI_FREE(m, storage);

  {
    size_t max_cmds = 1;
    int i;
    for (i = 0; i < p->nmb; i++) {
      if (p->mbs[i].num_commands > max_cmds) max_cmds = p->mbs[i].num_commands;
    }
    p->cmd_scratch_cap = max_cmds;
    p->cmd_scratch = BROTLI_ALLOC(m, Command, max_cmds);
    if (BROTLI_IS_OOM(m) || BROTLI_IS_NULL(p->cmd_scratch)) {
      MBB_DestroyPrepared(p);
      return NULL;
    }
  }

  if (nmb_out) *nmb_out = p->nmb;
  if (compressed_size_out) *compressed_size_out = output ? out : 0;
  return p;
}

int MBB_NumMetablocks(const MBB_Prepared* p) {
  return p->nmb;
}

void MBB_GetHeader(const MBB_Prepared* p, uint16_t* last_bytes,
                   uint8_t* last_bytes_bits) {
  *last_bytes = p->header_last_bytes;
  *last_bytes_bits = p->header_last_bytes_bits;
}

size_t MBB_Store(const MBB_Prepared* p, int i, const uint8_t* input,
                 size_t mask, uint8_t* storage, uint16_t* last_bytes,
                 uint8_t* last_bytes_bits) {
  const MBB_Metablock* mb = &p->mbs[i];
  BrotliEncoderState* s = p->s;
  MemoryManager* m = &s->memory_manager_;
  /* Local distance caches so repeated timed iterations start from the exact
     pre-write state (a fallback may rewrite dist_cache). */
  int dist_cache[4];
  int saved_dist_cache[4];
  memcpy(dist_cache, mb->dist_cache, 4 * sizeof(int));
  memcpy(saved_dist_cache, mb->saved_dist_cache, 4 * sizeof(int));
  /* The store (BrotliBuildMetaBlock -> RecomputeDistancePrefixes) rewrites
     dist_prefix_ in place. The real encoder never reuses a command array, but
     the timed loop re-runs the same captured commands each iteration, so copy
     them into the prepare-time scratch to keep the captured state pristine.
     (The copy is timed; it is a small constant overhead on both sides of an
     A/B.) */
  Command* cmd_copy = p->cmd_scratch;
  memcpy(cmd_copy, mb->commands, mb->num_commands * sizeof(Command));

  size_t storage_ix = *last_bytes_bits;
  storage[0] = (uint8_t)*last_bytes;
  storage[1] = (uint8_t)(*last_bytes >> 8);
  WriteMetaBlockInternal(m, input, mask, mb->last_flush_pos, mb->bytes,
                         (BROTLI_BOOL)mb->is_last,
                         (ContextType)mb->literal_context_mode, &s->params,
                         mb->prev_byte, mb->prev_byte2, mb->num_literals,
                         mb->num_commands, cmd_copy, saved_dist_cache,
                         dist_cache, &storage_ix, storage);
  *last_bytes = (uint16_t)(storage[storage_ix >> 3]);
  *last_bytes_bits = (uint8_t)(storage_ix & 7u);
  return storage_ix >> 3;
}

void MBB_DestroyPrepared(MBB_Prepared* p) {
  if (BROTLI_IS_NULL(p)) return;
  BrotliEncoderState* s = p->s;
  MemoryManager* m = &s->memory_manager_;
  for (int i = 0; i < p->nmb; i++) {
    if (p->mbs[i].commands) BROTLI_FREE(m, p->mbs[i].commands);
  }
  if (p->cmd_scratch) BROTLI_FREE(m, p->cmd_scratch);
  BROTLI_FREE(m, p->mbs);
  BrotliEncoderDestroyInstance(s);
  free(p);
}
