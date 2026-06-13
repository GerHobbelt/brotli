# Brotli Encoder Benchmark

Measures brotli encoding throughput, compression ratio, and CPU cycles, and
provides an A/B driver (`ab.py`) for comparing two versions of the encoder
head-to-head under variance-reducing conditions.

## Prerequisites

- CMake >= 3.15, C++17 compiler, Python 3
- `libpfm4-dev` for hardware performance counters
- `libgoogle-perftools-dev` for pprof capture (optional, only for `--mode pprof`)

```bash
sudo apt install -y build-essential cmake libpfm4-dev libgoogle-perftools-dev
```

## Corpus

The benchmark and the corpus roundtrip tests load **every regular file** in a
given directory (the corpora are arbitrary files, not necessarily `.js`). Point
them at any directory of files via `--corpus=DIR`. The corpora used locally
live under `~/Development/testdata/` (e.g. `srp_desktop/`).

## Build

```bash
cmake -B build-bench -DCMAKE_BUILD_TYPE=Release -DBROTLI_BUILD_BENCHMARKS=ON
cmake --build build-bench -j$(nproc) --target brotli_benchmark
```

## Run the benchmark directly

```bash
export LD_LIBRARY_PATH=build-bench
build-bench/benchmark/brotli_benchmark \
  --corpus=$HOME/Development/testdata/srp_desktop \
  --benchmark_perf_counters=CYCLES
```

### Options

- `--quality=N` — compression quality 0-11 (default: 6)
- `--lgwin=N` — log2 window size (default: 22)
- `--corpus=DIR` — corpus directory (loads all regular files in it)
- `--digest=FILE` — write per-file compressed-output hashes
  (`name<TAB>len<TAB>fnv64`) and exit without benchmarking; used to check that
  two encoder versions produce **bit-identical** output
- `--benchmark_out=results.json --benchmark_out_format=json` — save results

All [Google Benchmark flags](https://github.com/google/benchmark/blob/main/docs/user_guide.md)
are also supported (e.g. `--benchmark_repetitions=N`,
`--benchmark_perf_counters=CYCLES,INSTRUCTIONS,CACHE-MISSES`,
`--benchmark_enable_random_interleaving=true`).

## A/B comparison: `ab.py`

`ab.py` builds two git refs, runs them head-to-head, and writes a
self-describing **run record** so it is never ambiguous which code version was
benchmarked in which configuration on which corpus.

Each version is checked out as a detached **git worktree keyed by commit SHA**
(cached under `.ab-cache/`, managed entirely by the script — you never touch
it), built in Release, and run with its own `libbrotlienc.so` via an explicit
`LD_LIBRARY_PATH`, so the encoder under test is unambiguously that version's
code.

```bash
benchmark/ab.py --baseline master --candidate HEAD \
  --corpus ~/Development/testdata/srp_desktop --quality 9 --core 3
```

### Modes (`--mode`)

- `time` (default) — median real_time + throughput, with repetitions and
  random interleaving; prints a speedup summary
- `counters` — adds `--benchmark_perf_counters` (set with `--counters`)
- `pprof` — captures a CPU profile per version (needs `libprofiler.so`)
- `digest` — per-file output hashes, diffed to report **bit-identity**
  (the correct gate for a perf-only change that shouldn't alter the bitstream)

### Variance reduction

By default the run is pinned to a core (`--core`, default 3), wrapped in
`setarch -R` (ASLR off), and — when run as root — given high niceness
(`nice -n -20`). It deliberately does **not** use real-time (`SCHED_FIFO`)
scheduling: a long CPU-bound run pinned at RT priority starves the pinned
core's kernel threads (which can drop networking) and trips RT bandwidth
throttling. The CPU governor and turbo/boost are **recorded but not changed** —
pass `--tune` (requires root) to set the `performance` governor and disable
boost for the run, restoring them on exit. Without `--tune` the script warns if
the governor isn't `performance` or boost is on.

For a near-dedicated core, isolate it (and its SMT sibling) at the kernel
level. On this 8C/16T Ryzen, physical core 3 is logical CPUs 3 and 11; add to
`GRUB_CMDLINE_LINUX_DEFAULT` and `update-grub` + reboot:

```
isolcpus=domain,managed_irq,3,11 nohz_full=3,11 rcu_nocbs=3,11 irqaffinity=0-2,4-10,12-15
```

This removes 3/11 from the scheduler, stops their tick, offloads RCU, and
steers movable IRQs (incl. wifi) off them; `managed_irq` keeps managed device
IRQs (NVMe, etc.) away too where possible. `ab.py` warns if `--core` isn't in
`/sys/devices/system/cpu/isolated`, and records the isolation state plus
per-CPU `/proc/interrupts` deltas for the pinned core and its sibling in the
manifest. With this set up, A/B runs here see <0.1% run-to-run variance.

Other flags: `--lgwin`, `--repetitions`, `--no-pin`, `--no-aslr-disable`,
`--allow-dirty` (proceed despite uncommitted tracked changes, which are *not*
included since builds come from committed SHAs).

### Run records

Each run writes `benchmark/results/<timestamp>__<baseSHA>_vs_<candSHA>/`
containing the per-version artifacts (JSON / profile / digest) and a
`manifest.json` with full provenance: both commit SHAs, binary hashes, compiler
version, corpus path + fingerprint, CPU model, governor/turbo/ASLR state,
pinned core, isolation state (isolated CPUs, whether the pinned core is
isolated, SMT sibling, interrupt deltas), exact command lines, and the
comparison result.

## Corpus roundtrip tests

Configuring with tests enabled registers one `corpus-roundtrip/<name>` ctest
per corpus in `BROTLI_TEST_CORPORA` that exists. Each loads its corpus once and
roundtrips every file through compress/decompress at qualities 5-9 in parallel.

```bash
ctest --test-dir build-bench -R corpus-roundtrip --output-on-failure
```

Override the corpus list with
`-DBROTLI_TEST_CORPORA="dir1;dir2"`. Use `--gtest_filter='*qN'` (via the test
binary) to run a single quality.

## CPU Profiling (pprof)

```bash
benchmark/ab.py --baseline master --candidate HEAD \
  --corpus ~/Development/testdata/srp_desktop --mode pprof
```

View a captured profile with Go pprof, passing the matching version's binary
(for symbols) and the profile:

```bash
# one-time: sudo apt install -y golang-go && go install github.com/google/pprof@latest
~/go/bin/pprof -http=:8080 \
  .ab-cache/build/<sha>/benchmark/brotli_benchmark \
  benchmark/results/<run>/candidate.prof
```

The binary is always `.ab-cache/build/<sha>/benchmark/brotli_benchmark`, where
`<sha>` is taken from the run-record directory name (and recorded in
`manifest.json`). Builds include `-g` (without changing optimization or
generated code), so pprof's per-source-line view and `-list <func>` work; the
hot encoder code lives in `libbrotlienc.so` in that same build dir, so don't
move/delete it or symbols won't resolve.
