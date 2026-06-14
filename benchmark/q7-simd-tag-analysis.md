# q7 match-finder analysis: scalar H5 vs SIMD H58, and the tag prefilter

*Date: 2026-06-13. Corpus: `srp_desktop` (1000 files, ~633 MB HTML/JS), quality 7,
lgwin 22. Host: AMD Ryzen 7 5700X3D (Zen3), single-thread on an isolated core.
Goal: speed up the encoder **without changing the bitstream** (verified
bit-identical via `ab.py --mode digest`).*

## Background

At q7/lgwin22 with sub-1 MB files, brotli uses the **H5** hasher
(`quality.h`): `block_bits = 6` (chains up to **64** deep), `bucket_bits = 15`
(32768 buckets), `num_last_distances_to_check = 10`. The bucket array is
`32768 × 64 × 4 B = 8 MB` — L3-resident on this box (96 MB L3), L2-missing.

Brotli also ships a SIMD variant, **H58** (`hash_longest_match_simd_inc.h`),
used at q≤6 (`BROTLI_MAX_SIMD_QUALITY = 6` on gcc/clang; MSVC x64 uses 7). H58
keeps a parallel `tags_` array: one 8-bit tag per chain slot (low bits of the
same hash), SIMD-compares 16 tags at a time into a 64-bit "candidate" mask, and
only runs the 4-byte compare + `FindMatchLength` on slots whose tag matches.

The question: would enabling H58 at q7 (it's bit-identical to H5) be faster?

## Result 1 — H58 is 11% *slower* at q7

Forcing q7 → H58 (flip `BROTLI_MAX_SIMD_QUALITY` to 7), bit-identical confirmed:

| | scalar H5 | SIMD H58 | Δ |
|---|---:|---:|---:|
| wall time (median, 7 reps) | 15326 ms | 17239 ms | **+11.1%** |

Per-function flat time from `pprof` (2-rep profile, so absolute ms differ):

| function | scalar H5 | SIMD H58 | Δ |
|---|---:|---:|---:|
| `FindLongestMatch` (cumulative) | 18.4 s | 20.3 s | +1.9 s |
| `Store` | 3.96 s | 4.83 s | +0.87 s |
| `StoreRange` (cumulative) | 5.74 s | 7.42 s | +1.68 s |
| `FindMatchLengthWithLimit` | 1.41 s | 1.22 s | **−0.19 s** |
| `GetMatchingTagMask` + `_mm_loadu_si128` | — | ~1.24 s | new |

The tag prefilter saved only **0.19 s** of `FindMatchLength` while adding ~1.2 s
of SIMD/tag-compare and ~2.5 s of extra store cost (writing the parallel
`tags_` array). Net −11%. The existing cutoff at q6 is correct for this corpus.

## Result 2 — the tag filters almost nothing here

Instrumented counters over one full pass:

```
calls = 107,712,659     examined = 1,934,835,919     tag_passed = 1,422,794,172 (73.5%)
avg candidates examined per call = 18.0
```

**73.5%** of candidates pass the tag. The tag is 8 bits of the *same* 4-byte
hash that chose the bucket, so same-bucket candidates (same 4 bytes) almost
always share the tag. The 26.5% it filters are hash-collision candidates the
scalar already rejects cheaply with a single compare. The tag prefilter only
pays off when a bucket holds **many distinct 4-byte prefixes**; this corpus is
dominated by repeats of the same prefix (whitespace, common JS tokens).

## Result 3 — match-length distribution

For every candidate that survives the tag (1.42 B of them), its **true match
length** (where it diverges from the query); and the length of the **winning**
match per call (107.7 M calls; 47% find no match):

| true match len | tag-surviving candidates | winning matches |
|---|---:|---:|
| none (0) | 0.1% | — (47% of *calls* find no match) |
| **4** | **21.4%** | 10.5% |
| **5** | **13.4%** | 11.3% |
| **6** | **11.2%** | 8.8% |
| **7** | **9.2%** | 6.4% |
| 8–15 | 21.6% | 26.1% |
| 16–31 | 13.8% | 16.6% |
| 32–63 | 8.5% | 14.4% |
| 64+ | 0.7% | 3.5% |
| (1–3) | ~0% | 2.4% |

(Winner percentages are of the 56.8 M calls that found a match.)

## Interpretation

- **Opportunity:** **55%** of tag-surviving candidates diverge before byte 8.
  A tag that hashed *more* input bytes (still 8 bits, just over 8 bytes instead
  of reusing the 4-byte bucket hash) could filter more than half the candidates
  that currently reach the history-load — without growing the table.
- **Risk:** **37%** of *winning* matches are length 4–7. A tag that filtered
  `<8`-byte candidates would drop the actual winner in ~a third of matched
  positions. The `if (no_tag_matches) goto exhaustive` fallback does **not**
  cover this — it only fires when *all* candidates are filtered, which won't
  happen whenever a longer candidate coexists. So a naive longer tag is **not**
  bitstream-safe; it would change output frequently.

## A bitstream-safe way to use a sharper tag

Short matches only win at **close** distances (a length-4 match can't overcome
the distance penalty unless it's near). The chain is stored **closest-first**.
So:

> Check the closest **K** candidates exhaustively (no tag filter) — this catches
> every short close winner. Apply the sharper (more-bytes) tag only to the
> **far tail**, where only long matches can win.

`K` is bounded by the score math and is **empirically tunable against the
digest gate**: if output diverges, `K` is too small. The sharper tag is
storage-neutral (still one byte), so it doesn't worsen the store path — but the
store path (`tags_` writes, ~+2.5 s) is a separate anchor. Realistically
**both** a sharper tag (idea 2) **and** a cheaper store/StoreRange (idea 1, e.g.
a plumbed-through hash cache) are needed for H58 to beat the already-tight
scalar H5 at q7.

## Open question / next measurement

The number that decides whether the carve-out is cheap: the **distance
distribution of the len-4–7 winners**. If they cluster in the first few chain
slots, `K` is tiny (1–2) and the carve-out is nearly free; if short winners
reach deep into the chain, `K` is large and the idea is not worth it.

## Reproduction

- A/B (bit-identical check + timing): the SIMD-at-q7 toggle lives on a branch
  that flips `BROTLI_MAX_SIMD_QUALITY` 6 → 7 in `c/common/platform.h`.
  ```
  python3 benchmark/ab.py --baseline benchmarking --candidate <simd-branch> \
      --corpus ~/Development/testdata/srp_desktop --quality 7 --mode digest
  python3 benchmark/ab.py ... --mode time --repetitions 7   # add --tune (root) for lowest noise
  python3 benchmark/ab.py ... --mode pprof
  ```
- Tag / length counters: throwaway instrumentation in
  `hash_longest_match_simd_inc.h` (per-candidate `FindMatchLength` into a length
  histogram) + a `__attribute__((destructor))` printer in
  `backward_references.c`, built with `-DCMAKE_C_FLAGS=-g` and run directly on
  the corpus. Not committed.
