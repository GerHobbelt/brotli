# Working in this repo

Fork of google/brotli used for **encoder performance work**: making compression
faster while producing a **bit-identical** bitstream. Perf changes are
A/B-benchmarked and gated on output identity.

## Benchmarking & A/B — read before touching perf code

`benchmark/ab.py` is the A/B harness. **`benchmark/README.md` is the
authoritative operational doc — read it first** rather than re-deriving how the
script works (it documents every mode, variance controls, and the run-record
layout).

Essentials that are easy to forget:

- **Refs are positional, 0–N** (`ab.py`→HEAD, `ab.py A B`→A/B, `ab.py c1 c2 c3`
  →chain table). Don't hand-roll `taskset`/digest runs — drive ab.py.
- Benchmark the **uncommitted working copy** with the `WORKTREE` pseudo-ref
  (`ab.py WORKTREE ...`) — no commit needed; it builds the live tree and records
  HEAD+diff-hash. (Plain SHA refs still exclude uncommitted changes and the
  dirty gate still fires unless you pass `WORKTREE` or `--allow-dirty`.)
- **Bit-identity gate** for a perf-only change (roundtrip is not enough):
  `ab.py <refs...> --corpus <dir> --mode digest` — diffs per-file output hashes;
  valid only if **bit-identical**.
- The encoder has a runtime **opt-writer toggle** (`BROTLI_PARAM_OPT_WRITER`,
  the benchmark's `--opt`). `ab.py --opt-ab` does a single-build opt-OFF-vs-ON
  A/B at one ref (e.g. `ab.py WORKTREE --opt-ab --mode digest`); `--opt` runs all
  refs on the optimized path. The optimized path lives in `c/enc/*_opt.c`.
- Other modes: `--mode time` (default), `--mode counters`
  (`--counters CYCLES,...`), `--mode pprof`. Runs pin to isolated core 3;
  provenance lands in `benchmark/results/<timestamp>__.../manifest.json`.

## Builds

- `build-bench/` is the in-place Release (`-O3`) build:
  `cmake --build build-bench --target brotli_benchmark -j$(nproc)`.
- Hot encoder code is in `libbrotlienc.so`. Default corpus is
  `~/Development/testdata/srp_desktop/` (corpora are not in the repo).

## Profiling

`perf` needs root here (`kernel.perf_event_paranoid=4`); gperftools capture via
`--mode pprof` does not. View a captured `.prof` with
`go tool pprof .ab-cache/build/<sha>/benchmark/brotli_benchmark <prof>`
(`-top -cum`, `-list <fn>`, `-focus=<fn>`); ab.py builds carry `-g` for
line-level views without changing generated code.
