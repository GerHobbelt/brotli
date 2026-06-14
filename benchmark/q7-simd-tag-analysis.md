# q7 match-finder analysis: scalar H5 vs SIMD H58

Date: 2026-06-13. Corpus: `srp_desktop` (1000 files, ~633 MB HTML/JS), quality 7,
lgwin 22. Host: AMD Ryzen 7 5700X3D (Zen3), single thread pinned to an isolated
core. All variants below produce bit-identical output (verified via
`ab.py --mode digest`).

## Setup

At q7/lgwin22 with sub-1 MB files, brotli uses the H5 hasher (`quality.h`):
`block_bits = 6` (chains up to 64 deep), `bucket_bits = 15` (32768 buckets),
`num_last_distances_to_check = 10`. The bucket array is `32768 × 64 × 4 B = 8 MB`
(L3-resident; per-core L2 is 512 KB).

H58 (`hash_longest_match_simd_inc.h`) is the SIMD variant, selected at q≤6
(`BROTLI_MAX_SIMD_QUALITY = 6` on gcc/clang; 7 on MSVC x64). It keeps a parallel
`tags_` array of one 8-bit tag per chain slot (low bits of the bucket hash),
SIMD-compares 16 tags at a time into a 64-bit mask, and runs the 4-byte compare
+ `FindMatchLength` only on tag-matching slots. H58 produces the same output as
H5.

## Result 1 — H58 vs H5 timing at q7

H58 forced on at q7 (`BROTLI_MAX_SIMD_QUALITY` 6 → 7); output bit-identical.

Wall time, median of 7 reps:

| | scalar H5 | SIMD H58 |
|---|---:|---:|
| time | 15326 ms | 17239 ms (+11.1%) |

pprof flat time, 2-rep profile:

| function | scalar H5 | SIMD H58 |
|---|---:|---:|
| `FindLongestMatch` (cumulative) | 18.4 s | 20.3 s |
| `Store` | 3.96 s | 4.83 s |
| `StoreRange` (cumulative) | 5.74 s | 7.42 s |
| `FindMatchLengthWithLimit` | 1.41 s | 1.22 s |
| `GetMatchingTagMask` + `_mm_loadu_si128` | — | ~1.24 s |

Relative to H5, H58 reduced `FindMatchLength` by 0.19 s and added ~1.2 s of
tag-compare and ~2.5 s in the store path (the `tags_` writes).

## Result 2 — tag pass rate

One full pass, instrumented:

```
calls      = 107,712,659
examined    = 1,934,835,919   (avg 18.0 candidates/call)
tag_passed = 1,422,794,172   (73.5%)
```

The tag is 8 bits of the same 4-byte hash that selects the bucket, so
same-bucket candidates (same 4 bytes) usually share the tag. Measured
separately: 0.1% of tag survivors have a true match length < 4 (hash
collisions).

## Result 3 — match-length distribution

True match length of tag-surviving candidates (the divergence point), and length
of the winning match per call (107.7 M calls; 47% find no match):

| true match len | tag-surviving candidates | winning matches |
|---|---:|---:|
| none (0) | 0.1% | (47% of calls find no match) |
| 4 | 21.4% | 10.5% |
| 5 | 13.4% | 11.3% |
| 6 | 11.2% | 8.8% |
| 7 | 9.2% | 6.4% |
| 8–15 | 21.6% | 26.1% |
| 16–31 | 13.8% | 16.6% |
| 32–63 | 8.5% | 14.4% |
| 64+ | 0.7% | 3.5% |
| 1–3 | ~0% | 2.4% |

Winner percentages are over the 56.8 M calls that found a match. 55% of tag
survivors diverge before byte 8; 37% of winning matches are length 4–7.

## Result 4 — short-winner distance and chain rank

For each length-4–7 chain winner (18.8 M; a further ~2.3 M length-4–7 winners
come from the last-distance loop, which is always checked): distance, and chain
rank (position in the closest-first walk).

| chain rank of winner | share | cumulative |
|---|---:|---:|
| 1 | 75.5% | 75.5% |
| 2 | 8.3% | 83.8% |
| 3 | 4.0% | 87.7% |
| 4 | 2.4% | 90.1% |
| 5–8 | 4.5% | 94.6% |
| 9–16 | 3.0% | 97.6% |
| 17–32 | 1.7% | 99.2% |
| 33–64 | 0.8% | 100% |

| winner distance | share |
|---|---:|
| 1–15 | 4.1% |
| 16–63 | 17.2% |
| 64–255 | 12.1% |
| 256–1K | 10.6% |
| 1K–4K | 13.0% |
| 4K–16K | 16.0% |
| 16K–64K | 13.7% |
| 64K+ | 13.4% |

Length-4–7 chain winners occur at every chain rank up to 64 and in every
distance bucket. Closest-K coverage: K=4 → 90.1%, K=8 → 94.6%, K=16 → 97.6%,
K=32 → 99.2%, K=64 → 100%.

## Implications

- A sharper tag (more input bytes hashed into the still-8-bit tag) would filter
  ~55% of tag survivors (Result 3). The candidates it removes — length-4–7
  matches — are the winner in 37% of matched positions (Result 3) and occur at
  all chain ranks and distances (Result 4). A closest-K or distance-threshold
  carve-out therefore needs K = 64 (the full chain) to stay bit-identical,
  leaving nothing filtered.
- Bitstream-preserving levers not yet measured: the store path (`Store` +
  `StoreRange`, ~17% of H5 time) and per-file allocation (`__brk` + `memset`,
  ~6%) via encoder-state reuse.

## Reproduction

- A/B (bit-identical check + timing): the SIMD-at-q7 toggle flips
  `BROTLI_MAX_SIMD_QUALITY` 6 → 7 in `c/common/platform.h` on a branch.
  ```
  python3 benchmark/ab.py --baseline benchmarking --candidate <simd-branch> \
      --corpus ~/Development/testdata/srp_desktop --quality 7 --mode digest
  python3 benchmark/ab.py ... --mode time --repetitions 7   # add --tune (root) for lowest noise
  python3 benchmark/ab.py ... --mode pprof
  ```
- Counters (tag pass rate, length histograms, rank/distance): throwaway
  instrumentation in the H5/H58 inc files plus a `__attribute__((destructor))`
  printer in `backward_references.c`, built with `-DCMAKE_C_FLAGS=-g` and run
  directly on the corpus. Not committed.
