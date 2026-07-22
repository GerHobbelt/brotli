#!/usr/bin/env python3
"""A/B benchmark driver for the brotli encoder.

Builds two versions of the code (resolved from git refs), runs them
head-to-head under variance-reducing conditions, and writes a fully
self-describing run record so it is never ambiguous which code version was
benchmarked in which configuration on which corpus.

Each version is materialized as a detached git worktree keyed by commit SHA
(cached under .ab-cache/), built in Release with benchmarks enabled, and run
with its own libbrotlienc.so via an explicit LD_LIBRARY_PATH -- so the encoder
under test is unambiguously that version's code.

Modes:
  time      timing only (median real_time + throughput), the default
  counters  + hardware perf counters (--counters CYCLES,INSTRUCTIONS,...)
  pprof     capture a CPU profile per version (needs libprofiler.so)
  digest    per-file compressed-output hashes; diffed for bit-identity

Example:
  benchmark/ab.py --baseline master --candidate HEAD \\
      --corpus ~/Development/testdata/srp_desktop --quality 9 --core 3

Tuning (governor/turbo) is record-only by default; pass --tune (needs root) to
set the performance governor and disable boost for the run, restored on exit.
"""

import argparse
import datetime
import hashlib
import json
import os
import re
import shutil
import signal
import statistics
import subprocess
import sys
from pathlib import Path

# --- small process helpers --------------------------------------------------


def run(cmd, *, capture=False, env=None, check=True, cwd=None):
    """Run a command (list of args). Returns CompletedProcess."""
    return subprocess.run(
        cmd, check=check, cwd=cwd, env=env,
        stdout=subprocess.PIPE if capture else None,
        stderr=subprocess.STDOUT if capture else None,
        text=True)


def describe_exit(returncode):
    """Human-readable description of a process return code. A negative code
    means the process was killed by a signal; name it (e.g. SIGSEGV) instead
    of printing the raw negative number."""
    if returncode < 0:
        try:
            name = signal.Signals(-returncode).name
        except ValueError:
            name = f"signal {-returncode}"
        return f"killed by {name} (signal {-returncode})"
    return f"exited with code {returncode}"


def git(args, cwd=None):
    return run(["git", *args], capture=True, cwd=cwd).stdout.strip()


def run_tee(cmd, *, env, log_path):
    """Run a command, streaming its output to both the terminal and a log file
    so the Google Benchmark report is visible live and persisted in the run
    record."""
    with open(log_path, "w") as log:
        proc = subprocess.Popen(
            cmd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True, bufsize=1)
        for line in proc.stdout:
            sys.stdout.write(line)
            log.write(line)
        proc.wait()
    sys.stdout.flush()
    if proc.returncode != 0:
        sys.exit(f"command {describe_exit(proc.returncode)}: {' '.join(cmd)}")


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


# --- environment / variance control -----------------------------------------

BOOST_PATH = Path("/sys/devices/system/cpu/cpufreq/boost")          # AMD/acpi
NO_TURBO_PATH = Path("/sys/devices/system/cpu/intel_pstate/no_turbo")  # Intel
GOV_PATH = Path("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor")
ASLR_PATH = Path("/proc/sys/kernel/randomize_va_space")
PARANOID_PATH = Path("/proc/sys/kernel/perf_event_paranoid")


def _read(path):
    try:
        return path.read_text().strip()
    except OSError:
        return None


def read_env_state():
    """Snapshot the knobs that affect benchmark variance."""
    state = {
        "governor": _read(GOV_PATH),
        "aslr": _read(ASLR_PATH),
        "perf_event_paranoid": _read(PARANOID_PATH),
        "cpu_model": None,
        "nproc": os.cpu_count(),
    }
    for line in (_read(Path("/proc/cpuinfo")) or "").splitlines():
        if line.startswith("model name"):
            state["cpu_model"] = line.split(":", 1)[1].strip()
            break
    if BOOST_PATH.exists():
        # 1 = boost on, 0 = off
        state["turbo"] = {"mechanism": "cpufreq/boost", "boost": _read(BOOST_PATH)}
    elif NO_TURBO_PATH.exists():
        state["turbo"] = {"mechanism": "intel_pstate/no_turbo",
                          "no_turbo": _read(NO_TURBO_PATH)}
    else:
        state["turbo"] = {"mechanism": "unknown"}
    return state


def warn_if_noisy(state):
    if state.get("governor") not in (None, "performance"):
        print(f"WARNING: cpu governor is '{state['governor']}', not "
              f"'performance' -- pass --tune (as root) to fix.", file=sys.stderr)
    turbo = state.get("turbo", {})
    if turbo.get("boost") == "1" or turbo.get("no_turbo") == "0":
        print("WARNING: CPU boost/turbo is ENABLED -- a big source of "
              "run-to-run variance. Pass --tune (as root) to disable it.",
              file=sys.stderr)


def parse_cpu_list(s):
    """'3,11' or '3-4,11' -> {3, 11} / {3, 4, 11}."""
    cpus = set()
    for part in (s or "").split(","):
        part = part.strip()
        if not part:
            continue
        if "-" in part:
            a, b = part.split("-")
            cpus.update(range(int(a), int(b) + 1))
        else:
            cpus.add(int(part))
    return cpus


def isolated_cpus():
    return parse_cpu_list(_read(Path("/sys/devices/system/cpu/isolated")))


def core_sibling(core):
    """The SMT sibling logical CPU of `core`, or None."""
    sibs = parse_cpu_list(_read(Path(
        f"/sys/devices/system/cpu/cpu{core}/topology/thread_siblings_list")))
    sibs.discard(core)
    return min(sibs) if sibs else None


def read_irq_counts():
    """Per-CPU interrupt counts from /proc/interrupts: {irq: [per-cpu...]}."""
    lines = (_read(Path("/proc/interrupts")) or "").splitlines()
    if not lines:
        return {}
    ncpu = len(lines[0].split())
    counts = {}
    for line in lines[1:]:
        parts = line.split()
        if len(parts) < 1 + ncpu or not parts[0].endswith(":"):
            continue
        try:
            counts[parts[0].rstrip(":")] = [int(x) for x in parts[1:1 + ncpu]]
        except ValueError:
            continue
    return counts


def irq_delta_on_cpu(before, after, cpu):
    """Nonzero per-IRQ deltas fired on one CPU column, plus the total."""
    delta = {}
    for irq, after_vals in after.items():
        if cpu >= len(after_vals):
            continue
        b = before.get(irq, [])
        prev = b[cpu] if cpu < len(b) else 0
        d = after_vals[cpu] - prev
        if d:
            delta[irq] = d
    return {"per_irq": delta, "total": sum(delta.values())}


class Tuner:
    """Optionally set performance governor + disable boost, restore on exit."""

    def __init__(self, enabled):
        self.enabled = enabled
        self._orig_gov = None
        self._orig_boost = None

    def __enter__(self):
        if not self.enabled:
            return self
        if os.geteuid() != 0:
            sys.exit("--tune requires root (to set governor / disable boost).")
        self._orig_gov = _read(GOV_PATH)
        if shutil.which("cpupower"):
            run(["cpupower", "frequency-set", "-g", "performance"], capture=True)
        if BOOST_PATH.exists():
            self._orig_boost = _read(BOOST_PATH)
            BOOST_PATH.write_text("0\n")
        elif NO_TURBO_PATH.exists():
            self._orig_boost = _read(NO_TURBO_PATH)
            NO_TURBO_PATH.write_text("1\n")
        print("Tuned: governor=performance, boost=off", file=sys.stderr)
        return self

    def __exit__(self, *exc):
        if not self.enabled:
            return
        if self._orig_gov and shutil.which("cpupower"):
            run(["cpupower", "frequency-set", "-g", self._orig_gov],
                capture=True, check=False)
        if self._orig_boost is not None:
            target = BOOST_PATH if BOOST_PATH.exists() else NO_TURBO_PATH
            try:
                target.write_text(self._orig_boost + "\n")
            except OSError:
                pass
        print("Restored governor/boost to original state.", file=sys.stderr)


def build_wrapper(core, no_pin, no_aslr):
    """Prefix that pins the run and removes scheduling/layout noise.

    Deliberately NOT real-time (SCHED_FIFO): a long CPU-bound run pinned to one
    core at RT priority starves that core's kernel threads (IRQ/softirq) --
    which can drop networking -- and also hits RT bandwidth throttling, adding
    jitter. Core isolation (isolcpus) is what yields a near-dedicated core;
    `nice` just gives it priority over any stray normal task, safely.
    """
    wrap = []
    if not no_pin:
        wrap += ["taskset", "-c", str(core)]
        if core not in isolated_cpus():
            print(f"WARNING: core {core} is NOT in the kernel's isolated set "
                  f"({_read(Path('/sys/devices/system/cpu/isolated')) or 'none'})"
                  f"; it shares the core with other work and results will be "
                  f"noisier. Boot with isolcpus or pick an isolated --core.",
                  file=sys.stderr)
    if not no_aslr and shutil.which("setarch"):
        # Verify setarch -R works on this box before relying on it.
        if run(["setarch", "-R", "true"], capture=True, check=False).returncode == 0:
            wrap += ["setarch", "-R"]
        else:
            print("WARNING: 'setarch -R' unavailable; ASLR not disabled.",
                  file=sys.stderr)
    if os.geteuid() == 0 and shutil.which("nice"):
        wrap += ["nice", "-n", "-20"]
    return wrap


# --- build ------------------------------------------------------------------


# Friendly compiler families -> (CC, CXX). Anything else is passed as an
# explicit 'CC:CXX' pair (see resolve_compiler).
_COMPILER_FAMILIES = {
    "gcc": ("gcc", "g++"),
    "clang": ("clang", "clang++"),
}


def resolve_compiler(spec):
    """Map a --compiler spec to (CC, CXX, label).

    None -> the system default (whatever CMake auto-detects); label 'default'.
    A family name ('gcc', 'clang') -> its (CC, CXX) pair.
    An explicit 'CC:CXX' pair (e.g. 'clang-18:clang++-18') -> used verbatim.
    The label is a short filename/table-safe tag for the build cache dir and
    run record."""
    if spec is None:
        return (None, None, "default")
    if ":" in spec:
        cc, cxx = spec.split(":", 1)
        if not cc or not cxx:
            sys.exit(f"--compiler '{spec}': expected 'CC:CXX'")
        return (cc, cxx, _safe_label(spec))
    if spec in _COMPILER_FAMILIES:
        cc, cxx = _COMPILER_FAMILIES[spec]
        return (cc, cxx, spec)
    sys.exit(f"unknown --compiler '{spec}'; use "
             f"{', '.join(sorted(_COMPILER_FAMILIES))}, or an explicit 'CC:CXX'")


def _safe_label(s):
    """Sanitize a compiler spec into a path/table-safe label."""
    return re.sub(r"[^A-Za-z0-9._+-]", "-", s)


def _cmake_build(src, build, ref, tag, cc=None, cxx=None):
    """Configure + build brotli_benchmark from source dir `src` into `build`.

    cc/cxx, when given, pin CMAKE_C_COMPILER/CMAKE_CXX_COMPILER; otherwise
    CMake auto-detects the system default."""
    build.mkdir(parents=True, exist_ok=True)
    log_path = build / "ab-build.log"
    print(f"  building {ref} ({tag}); output -> {log_path}", file=sys.stderr)
    configure = [
        # Release optimization plus -g: debug info lets pprof show per-source-
        # line samples. -g does not change generated code, so timing runs are
        # unaffected -- it only adds DWARF sections. Re-running configure also
        # re-globs sources (picks up added/removed files in a working tree).
        "cmake", "-B", str(build), "-S", str(src),
        "-DCMAKE_BUILD_TYPE=Release", "-DBROTLI_BUILD_BENCHMARKS=ON",
        "-DBROTLI_DISABLE_TESTS=ON",
        "-DCMAKE_C_FLAGS=-g", "-DCMAKE_CXX_FLAGS=-g"]
    if cc:
        configure.append(f"-DCMAKE_C_COMPILER={cc}")
    if cxx:
        configure.append(f"-DCMAKE_CXX_COMPILER={cxx}")
    steps = [
        configure,
        ["cmake", "--build", str(build), "-j", str(os.cpu_count()),
         "--target", "brotli_benchmark"],
    ]
    chunks = []
    for step in steps:
        r = run(step, capture=True, check=False)
        chunks.append(f"$ {' '.join(step)}\n{r.stdout}")
        if r.returncode != 0:
            log_path.write_text("".join(chunks))
            sys.stderr.write("".join(chunks))
            sys.exit(f"build failed for {ref} ({tag}); see {log_path}")
    log_path.write_text("".join(chunks))


def build_version(repo_root, ref, compiler=(None, None, "default")):
    """Build a benchmarkable version and return an info dict.

    A normal ref is resolved to a SHA and materialized as a detached worktree,
    cached by SHA. The pseudo-ref WORKTREE builds the live (possibly
    uncommitted) working tree in place; since that can change between runs it is
    always rebuilt (cmake is incremental, so this is cheap), and its provenance
    is recorded as HEAD plus a hash of `git diff HEAD`.

    `compiler` is a (CC, CXX, label) tuple. Since CMake bakes the compiler into
    a build dir and refuses to change it, a non-default compiler gets its own
    build dir (suffixed with the label); the SHA worktree checkout is shared
    because the source is compiler-independent."""
    cc, cxx, clabel = compiler
    suffix = "" if clabel == "default" else f"__{clabel}"
    cache = repo_root / ".ab-cache"
    if ref == "WORKTREE":
        head = git(["rev-parse", "HEAD"], cwd=repo_root)
        diff_sha = hashlib.sha256(
            git(["diff", "HEAD"], cwd=repo_root).encode()).hexdigest()
        build = cache / "build" / f"worktree{suffix}"
        binary = build / "benchmark" / "brotli_benchmark"
        _cmake_build(repo_root, build, ref, f"{head[:12]}+wt", cc, cxx)
        if not binary.exists():
            sys.exit(f"build produced no benchmark binary at {binary}")
        return {
            "ref": ref, "sha": head, "worktree_diff_sha256": diff_sha,
            "worktree": str(repo_root), "build_dir": str(build),
            "binary": str(binary), "binary_sha256": sha256_file(binary),
            "compiler": clabel,
        }

    sha = git(["rev-parse", ref], cwd=repo_root)
    wt = cache / "wt" / sha
    build = cache / "build" / f"{sha}{suffix}"
    binary = build / "benchmark" / "brotli_benchmark"

    if not wt.exists():
        wt.parent.mkdir(parents=True, exist_ok=True)
        run(["git", "worktree", "add", "--detach", str(wt), sha],
            cwd=repo_root, capture=True)

    if not binary.exists():
        _cmake_build(wt, build, ref, sha[:12], cc, cxx)

    if not binary.exists():
        sys.exit(f"build produced no benchmark binary at {binary}")

    return {
        "ref": ref,
        "sha": sha,
        "worktree": str(wt),
        "build_dir": str(build),
        "binary": str(binary),
        "binary_sha256": sha256_file(binary),
        "compiler": clabel,
    }


# --- corpus -----------------------------------------------------------------


def corpus_fingerprint(corpus):
    """Hash the sorted (name, size) list of regular files in the corpus."""
    entries = sorted(
        (p.name, p.stat().st_size)
        for p in Path(corpus).iterdir() if p.is_file())
    h = hashlib.sha256()
    total = 0
    for name, size in entries:
        h.update(f"{name}\t{size}\n".encode())
        total += size
    return {"num_files": len(entries), "total_bytes": total,
            "fingerprint_sha256": h.hexdigest()}


# --- running a version ------------------------------------------------------


def base_args(args, corpus, opt, opt_mf, opt_bs):
    a = [f"--corpus={corpus}", f"--quality={args.quality}",
         f"--lgwin={args.lgwin}"]
    if opt:
        a.append("--opt")  # BROTLI_PARAM_OPT_WRITER=1
    if opt_mf:
        a.append("--opt-matchfinder")  # BROTLI_PARAM_OPT_MATCHFINDER=1
    if opt_bs:
        a.append("--opt-block-splitter")  # BROTLI_PARAM_OPT_BLOCK_SPLITTER=1
    if args.dictionary:
        a.append(f"--dictionary={args.dictionary}")
    if args.bench_args:
        a += args.bench_args.split()
    return a


def find_libprofiler():
    for cand in ("/usr/lib/x86_64-linux-gnu/libprofiler.so",
                 "/usr/lib/libprofiler.so", "/usr/local/lib/libprofiler.so"):
        if Path(cand).exists():
            return cand
    return None


def find_tcmalloc():
    for cand in ("/usr/lib/x86_64-linux-gnu/libtcmalloc_minimal.so",
                 "/usr/lib/x86_64-linux-gnu/libtcmalloc_minimal.so.4",
                 "/usr/lib/libtcmalloc_minimal.so",
                 "/usr/local/lib/libtcmalloc_minimal.so"):
        if Path(cand).exists():
            return cand
    return None


def resolve_malloc(spec):
    """Resolve --malloc to an LD_PRELOAD path (or None for the system allocator).

    Default 'tcmalloc' avoids glibc's per-allocation trim/mmap churn, which on
    this workload (a fresh encoder, hence a fresh multi-MB hash table, per file)
    makes timings allocation-bound and non-monotonic in quality (a notorious q7
    spike). tcmalloc pools and reuses the freed table, so timings reflect the
    encoder, not the allocator -- and match a typical production allocator."""
    if spec == "system":
        return None
    if spec == "tcmalloc":
        p = find_tcmalloc()
        if not p:
            print("WARNING: --malloc tcmalloc, but libtcmalloc_minimal.so not "
                  "found -- using the system allocator (timings may be "
                  "allocation-bound; see q7). Install libtcmalloc-minimal4, or "
                  "pass --malloc system to silence.", file=sys.stderr)
        return p
    if not Path(spec).exists():
        sys.exit(f"--malloc preload library not found: {spec}")
    return spec


def run_one(args, version, which, record_dir, wrapper, opt, opt_mf,
            opt_bs):
    """Run one variant in the selected mode. Returns artifact paths."""
    env = dict(os.environ)
    env["LD_LIBRARY_PATH"] = version["build_dir"]  # use THIS version's libs
    # Preload the chosen allocator (default tcmalloc). For pprof the preload is
    # folded into the env-shim below instead, so it isn't lost.
    if args.malloc_preload and args.mode != "pprof":
        prev = env.get("LD_PRELOAD")
        env["LD_PRELOAD"] = args.malloc_preload + (":" + prev if prev else "")
    binary = version["binary"]
    corpus = args.corpus
    cmd = (list(wrapper) + [binary]
           + base_args(args, corpus, opt, opt_mf, opt_bs))
    artifacts = {}

    if args.mode == "digest":
        out = record_dir / f"{which}.digest.txt"
        # Digest is a correctness gate, not a timed run: it runs multi-threaded
        # across all cores rather than pinned to the single isolated benchmark
        # core. Drop the taskset/setarch/nice wrapper (the binary pins its own
        # worker threads, one per CPU); keep this version's LD_LIBRARY_PATH +
        # allocator preload via env.
        cmd = ([binary] + base_args(args, corpus, opt, opt_mf, opt_bs)
               + [f"--digest={out}", f"--digest-threads={args.digest_jobs}"])
        try:
            run(cmd, env=env)
        except subprocess.CalledProcessError as exc:
            sys.exit(
                f"\n{which} run ({version['ref']} @ {version['sha'][:12]}) "
                f"{describe_exit(exc.returncode)}.\n"
                f"  binary:  {binary}\n"
                f"  command: {' '.join(cmd)}\n"
                f"The benchmark binary faulted before writing its digest, so "
                f"no comparison is possible. A SIGSEGV usually means this "
                f"build has a bug (e.g. an out-of-bounds read from a bad hash "
                f"key); try running the command above under gdb.")
        artifacts["digest"] = str(out)

    elif args.mode in ("time", "counters"):
        out = record_dir / f"{which}.json"
        cmd += [f"--benchmark_out={out}", "--benchmark_out_format=json",
                f"--benchmark_repetitions={args.repetitions}",
                "--benchmark_enable_random_interleaving=true",
                # Console shows only aggregate rows; the JSON keeps every rep.
                "--benchmark_display_aggregates_only=true"]
        if args.mode == "counters":
            cmd += [f"--benchmark_perf_counters={args.counters}"]
        console = record_dir / f"{which}.console.txt"
        run_tee(cmd, env=env, log_path=console)
        artifacts["json"] = str(out)
        artifacts["console"] = str(console)

    elif args.mode == "pprof":
        lib = find_libprofiler()
        if not lib:
            sys.exit("pprof mode needs libprofiler.so "
                     "(apt install libgoogle-perftools-dev)")
        out = record_dir / f"{which}.prof"
        # Scope the profiler vars to the benchmark binary ONLY, via an `env`
        # shim placed after the taskset/setarch wrappers. If LD_PRELOAD/
        # CPUPROFILE were in the wrappers' environment, libprofiler would arm
        # ITIMER_PROF inside the wrapper; that timer survives execve() into the
        # benchmark and fires before the benchmark's own SIGPROF handler is
        # installed, killing it with "Profiling timer expired".
        preload = f"{args.malloc_preload}:{lib}" if args.malloc_preload else lib
        shim = ["env", f"LD_PRELOAD={preload}", f"CPUPROFILE={out}",
                "CPUPROFILE_FREQUENCY=1000"]
        cmd = (list(wrapper) + shim + [binary] +
               base_args(args, corpus, opt, opt_mf, opt_bs) +
               [f"--benchmark_repetitions={args.repetitions}"])
        console = record_dir / f"{which}.console.txt"
        run_tee(cmd, env=env, log_path=console)
        artifacts["pprof"] = str(out)
        artifacts["console"] = str(console)

    artifacts["command"] = cmd
    return artifacts


# --- comparison -------------------------------------------------------------


def summarize_json(path):
    """Median real_time (ms) + throughput from benchmark iteration entries."""
    with open(path) as f:
        data = json.load(f)
    rts, mbps, ratio = [], [], None
    for b in data.get("benchmarks", []):
        if b.get("run_type") == "aggregate":
            continue
        rts.append(b["real_time"])
        if b.get("bytes_per_second"):
            mbps.append(b["bytes_per_second"] / 1e6)
        if "CompressionRatio" in b:
            ratio = b["CompressionRatio"]
    unit = data["benchmarks"][0].get("time_unit", "ms")
    return {
        "median_real_time": statistics.median(rts) if rts else None,
        "time_unit": unit,
        "median_MBps": statistics.median(mbps) if mbps else None,
        "compression_ratio": ratio,
        "n": len(rts),
    }


def variant_tag(v):
    """Short human label for a variant: ref@sha plus opt/compiler markers."""
    clabel = v["version"].get("compiler", "default")
    return (f"{v['ref']}@{v['version']['sha'][:9]}"
            + ("  opt" if v["opt"] else "")
            + ("  optmf" if v["opt_mf"] else "")
            + ("  optbs" if v["opt_bs"] else "")
            + (f"  cc:{clabel}" if clabel != "default" else ""))


def compare_timing(record_dir, variants):
    """Print a timing table across N variants (delta vs first and, for a chain,
    vs the previous variant). Speedup > 0 means faster than the comparator."""
    rows = [(v, summarize_json(v["artifacts"]["json"])) for v in variants]
    base_v, base = rows[0]
    print()
    out = []
    prev_rt = None
    for i, (v, s) in enumerate(rows):
        rt = s["median_real_time"]
        line = (f"  {v['label']:<8}{variant_tag(v):<24} "
                f"{rt:9.2f} {s['time_unit']}  "
                f"({s['median_MBps'] or 0:6.1f} MB/s, ratio "
                f"{s['compression_ratio']:.4f}x)")
        rec = {"label": v["label"], "ref": v["ref"], "opt": v["opt"],
               "opt_mf": v["opt_mf"], "opt_bs": v["opt_bs"],
               "sha": v["version"]["sha"], **s}
        if i > 0 and base["median_real_time"] and rt:
            vs_first = base["median_real_time"] / rt
            line += f"   {(vs_first - 1) * 100:+.2f}% vs {base_v['label']}"
            rec["speedup_vs_first"] = vs_first
            if len(rows) > 2 and prev_rt:
                vs_prev = prev_rt / rt
                line += f"  ({(vs_prev - 1) * 100:+.2f}% vs prev)"
                rec["speedup_vs_prev"] = vs_prev
        print(line)
        out.append(rec)
        prev_rt = rt
    print()
    # google-benchmark's compare.py for the simple 2-variant case, if available.
    if len(rows) == 2:
        cmp_py = next(Path(record_dir).glob(
            "../../.ab-cache/build/*/_deps/benchmark-src/tools/compare.py"),
            None)
        if cmp_py:
            res = run([sys.executable, str(cmp_py), "benchmarks",
                       rows[0][0]["artifacts"]["json"],
                       rows[1][0]["artifacts"]["json"]],
                      capture=True, check=False)
            (Path(record_dir) / "compare_py.txt").write_text(res.stdout or "")
    return {"variants": out}


def compare_digests(variants):
    """Check every variant's per-file output against the first; report identity.
    For a perf-only change (or the opt toggle) all variants must match."""
    def load(p):
        d = {}
        for line in Path(p).read_text().splitlines():
            name, length, h = line.split("\t")
            d[name] = (length, h)
        return d
    rows = [(v, load(v["artifacts"]["digest"])) for v in variants]
    base_v, base = rows[0]
    print()
    all_identical = True
    pairs = []
    for v, d in rows[1:]:
        diffs = [n for n in base if n in d and base[n] != d[n]]
        only_b = sorted(set(base) - set(d))
        only_c = sorted(set(d) - set(base))
        identical = not diffs and not only_b and not only_c
        all_identical = all_identical and identical
        if identical:
            print(f"  {v['label']} ({variant_tag(v)}) vs {base_v['label']}: "
                  f"BIT-IDENTICAL ({len(base)} files)")
        else:
            print(f"  {v['label']} ({variant_tag(v)}) vs {base_v['label']}: "
                  f"NOT identical -- {len(diffs)} differ, {len(only_b)} "
                  f"only-{base_v['label']}, {len(only_c)} only-{v['label']}")
            for n in diffs[:10]:
                print(f"    {n}: {base[n]} vs {d[n]}")
        pairs.append({"label": v["label"], "vs": base_v["label"],
                      "identical": identical, "num_differing": len(diffs),
                      "differing_sample": diffs[:50]})
    print()
    return {"all_identical": all_identical, "pairs": pairs}


# --- main -------------------------------------------------------------------


def main():
    ap = argparse.ArgumentParser(
        description="Benchmark/compare the brotli encoder across git refs and "
                    "the BROTLI_PARAM_OPT_WRITER / BROTLI_PARAM_OPT_MATCHFINDER "
                    "/ BROTLI_PARAM_OPT_BLOCK_SPLITTER toggles.")
    ap.add_argument("refs", nargs="*", default=[],
                    help="git refs to benchmark: none=HEAD, one=just measure, "
                         "two=A/B, N=chain (table of deltas). The pseudo-ref "
                         "WORKTREE builds the current (uncommitted) tree.")
    ap.add_argument("--baseline", help="alias for the first positional ref")
    ap.add_argument("--candidate", help="alias for the second positional ref")
    ap.add_argument("--opt", action="store_true",
                    help="run every ref with the optimized writer on "
                         "(BROTLI_PARAM_OPT_WRITER=1)")
    ap.add_argument("--opt-ab", dest="opt_ab", action="store_true",
                    help="one build, compare writer-opt OFF vs ON at a single "
                         "ref (default HEAD) -- the production toggle A/B")
    ap.add_argument("--opt-mf", dest="opt_mf", action="store_true",
                    help="run every ref with the optimized match finder on "
                         "(BROTLI_PARAM_OPT_MATCHFINDER=1)")
    ap.add_argument("--opt-mf-ab", dest="opt_mf_ab", action="store_true",
                    help="one build, compare matchfinder-opt OFF vs ON at a "
                         "single ref; combine with --opt to hold the writer "
                         "on in both variants")
    ap.add_argument("--opt-bs", dest="opt_bs", action="store_true",
                    help="run every ref with the optimized HQ block splitter "
                         "on (BROTLI_PARAM_OPT_BLOCK_SPLITTER=1)")
    ap.add_argument("--opt-bs-ab", dest="opt_bs_ab", action="store_true",
                    help="one build, compare block-splitter-opt OFF vs ON at "
                         "a single ref; combine with --opt/--opt-mf to hold "
                         "the other toggles on in both variants")
    ap.add_argument("--compiler",
                    help="compiler to build every ref with: a family name "
                         "('gcc', 'clang') or an explicit 'CC:CXX' pair "
                         "(e.g. 'clang-18:clang++-18'). Default: whatever CMake "
                         "auto-detects (the system default).")
    ap.add_argument("--compiler-ab", dest="compiler_ab",
                    help="build one ref with two compilers and compare them; "
                         "value is 'A,B' (two specs) or just 'B' (compare the "
                         "system default vs B). Each spec is a family name or "
                         "'CC:CXX'. Combine with --opt/--opt-mf/--opt-bs to hold "
                         "the runtime toggles on in both variants.")
    ap.add_argument("--corpus", required=True, help="corpus directory")
    ap.add_argument("--dictionary", help="raw (LZ77 prefix) shared-dictionary "
                    "file to attach to every encode; prepared once, outside the "
                    "timed loop")
    ap.add_argument("--bench-args", dest="bench_args", default="",
                    help="extra args appended verbatim to the benchmark binary "
                         "for every variant (e.g. '--dict-tree'); recorded in "
                         "each variant's command in the manifest")
    ap.add_argument("--malloc", default="tcmalloc",
                    help="allocator to LD_PRELOAD: 'tcmalloc' (default), "
                         "'system' (no preload), or a path to a preload .so. "
                         "tcmalloc avoids glibc per-file alloc churn (see q7).")
    ap.add_argument("--quality", type=int, default=6)
    ap.add_argument("--lgwin", type=int, default=22)
    ap.add_argument("--mode", choices=["time", "counters", "pprof", "digest"],
                    default="time")
    ap.add_argument("--digest-jobs", dest="digest_jobs", type=int,
                    default=os.cpu_count() or 1,
                    help="worker threads for --mode digest (default: all "
                         "logical CPUs). Digest is a correctness gate, not a "
                         "timed run, so it uses every core -- including the "
                         "isolated benchmarking core(s) the scheduler otherwise "
                         "leaves idle. Set 1 for the old serial behavior.")
    ap.add_argument("--repetitions", type=int, default=5)
    ap.add_argument("--counters", default="CYCLES,INSTRUCTIONS",
                    help="perf counters for --mode counters")
    ap.add_argument("--core", type=int, default=3, help="CPU to pin to")
    ap.add_argument("--no-pin", action="store_true", help="don't taskset")
    ap.add_argument("--no-aslr-disable", action="store_true",
                    help="don't wrap in setarch -R")
    ap.add_argument("--tune", action="store_true",
                    help="set performance governor + disable boost (root)")
    ap.add_argument("--allow-dirty", action="store_true",
                    help="benchmark committed SHAs even with a dirty tree "
                         "(uncommitted changes excluded; use WORKTREE to "
                         "include them)")
    args = ap.parse_args()
    args.malloc_preload = resolve_malloc(args.malloc)

    repo_root = Path(git(["rev-parse", "--show-toplevel"])).resolve()
    corpus = str(Path(args.corpus).expanduser().resolve())
    args.corpus = corpus
    if not Path(corpus).is_dir():
        sys.exit(f"corpus directory not found: {corpus}")
    if args.dictionary:
        dictionary = str(Path(args.dictionary).expanduser().resolve())
        args.dictionary = dictionary
        if not Path(dictionary).is_file():
            sys.exit(f"dictionary file not found: {dictionary}")

    # Resolve refs. --baseline/--candidate are aliases for two positional refs.
    refs = list(args.refs)
    if args.baseline or args.candidate:
        if refs:
            sys.exit("pass refs positionally OR via --baseline/--candidate")
        if not (args.baseline and args.candidate):
            sys.exit("--baseline and --candidate must be given together")
        refs = [args.baseline, args.candidate]
    if not refs:
        refs = ["HEAD"]

    # The compiler is a build-time axis (unlike the opt-* runtime toggles): the
    # default compiler builds every ref, while --compiler-ab is its own A/B that
    # builds one ref with two compilers. The two are mutually exclusive.
    if args.compiler and args.compiler_ab:
        sys.exit("--compiler sets one compiler for all builds; --compiler-ab "
                 "A/Bs two compilers. Pass one or the other, not both.")
    default_compiler = resolve_compiler(args.compiler)

    # Expand to variants: (label, ref, opt_writer, opt_mf, opt_bs, compiler).
    # Each *-ab flag A/Bs its own axis at one ref; the other toggles can be held
    # on for both variants via --opt / --opt-mf / --opt-bs.
    ab_flags = [args.opt_ab, args.opt_mf_ab, args.opt_bs_ab,
                bool(args.compiler_ab)]
    if sum(ab_flags) > 1:
        sys.exit("--opt-ab, --opt-mf-ab, --opt-bs-ab and --compiler-ab are "
                 "mutually exclusive; A/B one axis at a time (hold the other "
                 "toggles on via --opt/--opt-mf/--opt-bs)")
    cc = default_compiler
    if args.opt_ab:
        if len(refs) > 1:
            sys.exit("--opt-ab compares one ref with writer-opt off vs on; "
                     "pass at most one ref")
        specs = [("orig", refs[0], False, args.opt_mf, args.opt_bs, cc),
                 ("opt", refs[0], True, args.opt_mf, args.opt_bs, cc)]
    elif args.opt_mf_ab:
        if len(refs) > 1:
            sys.exit("--opt-mf-ab compares one ref with matchfinder-opt off "
                     "vs on; pass at most one ref")
        specs = [("orig", refs[0], args.opt, False, args.opt_bs, cc),
                 ("optmf", refs[0], args.opt, True, args.opt_bs, cc)]
    elif args.opt_bs_ab:
        if len(refs) > 1:
            sys.exit("--opt-bs-ab compares one ref with block-splitter-opt "
                     "off vs on; pass at most one ref")
        specs = [("orig", refs[0], args.opt, args.opt_mf, False, cc),
                 ("optbs", refs[0], args.opt, args.opt_mf, True, cc)]
    elif args.compiler_ab:
        if len(refs) > 1:
            sys.exit("--compiler-ab compares one ref built two ways; "
                     "pass at most one ref")
        parts = args.compiler_ab.split(",")
        if len(parts) == 1:
            a_spec, b_spec = None, parts[0]  # default vs B
        elif len(parts) == 2:
            a_spec, b_spec = (parts[0] or None), (parts[1] or None)
        else:
            sys.exit("--compiler-ab takes 'B' or 'A,B' (two compiler specs)")
        ca, cb = resolve_compiler(a_spec), resolve_compiler(b_spec)
        if ca[2] == cb[2]:
            sys.exit(f"--compiler-ab: both sides resolve to '{ca[2]}'; "
                     f"pass two different compilers")
        specs = [(ca[2], refs[0], args.opt, args.opt_mf, args.opt_bs, ca),
                 (cb[2], refs[0], args.opt, args.opt_mf, args.opt_bs, cb)]
    else:
        specs = [(f"v{i}", r, args.opt, args.opt_mf, args.opt_bs, cc)
                 for i, r in enumerate(refs)]
    using_worktree = any(r == "WORKTREE" for _, r, _, _, _, _ in specs)

    # Provenance gate: uncommitted changes to tracked files are not captured by
    # SHA worktrees. A WORKTREE variant is the deliberate way to benchmark them,
    # so don't gate when one is requested.
    dirty = bool(git(["status", "--porcelain", "--untracked-files=no"],
                     cwd=repo_root))
    diff_sha = None
    if dirty:
        diff_sha = hashlib.sha256(
            git(["diff", "HEAD"], cwd=repo_root).encode()).hexdigest()
        if not using_worktree and not args.allow_dirty:
            sys.exit("Working tree has uncommitted changes that SHA builds will "
                     "NOT include. Use the WORKTREE ref to benchmark the working "
                     "tree, commit the changes, or pass --allow-dirty.")
        if not using_worktree:
            print("WARNING: working tree is dirty; SHA builds do NOT include "
                  "uncommitted changes.", file=sys.stderr)

    env_state = read_env_state()
    warn_if_noisy(env_state)

    # Preflight: counters mode needs perf_event_open access. Root/CAP_PERFMON
    # bypasses perf_event_paranoid; otherwise paranoid > 2 blocks it. Fail fast
    # with the fix rather than building, then emitting cryptic per-counter
    # "Failed to get a file descriptor" warnings and recording no counters.
    if args.mode == "counters" and os.geteuid() != 0:
        try:
            paranoid = int(env_state.get("perf_event_paranoid"))
        except (TypeError, ValueError):
            paranoid = None
        if paranoid is not None and paranoid > 2:
            sys.exit(
                f"counters mode needs perf access, but "
                f"kernel.perf_event_paranoid={paranoid} blocks it for non-root."
                f"\nFix one of:\n"
                f"  - run as root:  sudo python3 {sys.argv[0]} ... --mode counters\n"
                f"  - lower it:     sudo sysctl -w kernel.perf_event_paranoid=1"
                f"  (persist in /etc/sysctl.d/)")

    # Build each unique (ref, compiler) once. A ref can back two variants that
    # share a build (e.g. --opt-ab toggles at runtime), or two builds that
    # differ only in compiler (--compiler-ab); the compiler label keys them
    # apart.
    builds = {}
    for _, ref, _, _, _, compiler in specs:
        key = (ref, compiler[2])
        if key not in builds:
            desc = ref if compiler[2] == "default" else f"{ref} [{compiler[2]}]"
            print(f"Building {desc} ...", file=sys.stderr)
            builds[key] = build_version(repo_root, ref, compiler)
    variants = [{"label": label, "ref": ref, "opt": opt, "opt_mf": opt_mf,
                 "opt_bs": opt_bs, "version": builds[(ref, compiler[2])]}
                for (label, ref, opt, opt_mf, opt_bs, compiler) in specs]

    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    tags = "_".join(v["version"]["sha"][:8] + ("o" if v["opt"] else "")
                    + ("m" if v["opt_mf"] else "")
                    + ("b" if v["opt_bs"] else "")
                    + ("-" + v["version"]["compiler"]
                       if v["version"].get("compiler", "default") != "default"
                       else "")
                    for v in variants)
    record_dir = repo_root / "benchmark" / "results" / f"{stamp}__{tags}"
    record_dir.mkdir(parents=True, exist_ok=True)

    wrapper = build_wrapper(args.core, args.no_pin, args.no_aslr_disable)

    with Tuner(args.tune):
        # Re-read env so the manifest reflects the actual (tuned) state.
        env_state = read_env_state()
        irq_before = read_irq_counts()
        for v in variants:
            clabel = v["version"].get("compiler", "default")
            print(f"\n== {v['label']}: {v['ref']} "
                  f"({v['version']['sha'][:12]}) opt={int(v['opt'])} "
                  f"optmf={int(v['opt_mf'])} optbs={int(v['opt_bs'])}"
                  f"{'' if clabel == 'default' else '  cc=' + clabel} ==",
                  file=sys.stderr)
            v["artifacts"] = run_one(args, v["version"], v["label"],
                                     record_dir, wrapper, v["opt"],
                                     v["opt_mf"], v["opt_bs"])
        irq_after = read_irq_counts()

    # Isolation provenance: was the pinned core actually isolated, and did any
    # interrupts fire on it (or its SMT sibling) during the runs?
    iso = isolated_cpus()
    sibling = None if args.no_pin else core_sibling(args.core)
    isolation = {
        "isolated_cpus": _read(Path("/sys/devices/system/cpu/isolated")) or "",
        "pinned_core_isolated": (not args.no_pin) and args.core in iso,
        "pinned_core_sibling": sibling,
    }
    if not args.no_pin:
        irqs = {f"cpu{args.core}":
                irq_delta_on_cpu(irq_before, irq_after, args.core)}
        if sibling is not None:
            irqs[f"cpu{sibling}"] = irq_delta_on_cpu(irq_before, irq_after,
                                                     sibling)
        isolation["interrupts_during_run"] = irqs

    comparison = None
    if args.mode in ("time", "counters"):
        comparison = compare_timing(record_dir, variants)
    elif args.mode == "digest" and len(variants) >= 2:
        comparison = compare_digests(variants)

    # Probe the version of each distinct compiler actually used to build a
    # variant (keyed by label); 'default' resolves to the system c++.
    def _cxx_version(cxx):
        out = run([cxx or "c++", "--version"], capture=True, check=False).stdout
        return (out or "").splitlines()[0] if out else None
    compilers = {}
    for _, _, _, _, _, comp in specs:
        cc_, cxx_, clabel_ = comp
        if clabel_ not in compilers:
            compilers[clabel_] = _cxx_version(cxx_)
    manifest = {
        "timestamp": stamp,
        "hostname": os.uname().nodename,
        "kernel": os.uname().release,
        "env": env_state,
        "tuned": args.tune,
        "wrapper": wrapper,
        "pinned_core": None if args.no_pin else args.core,
        "isolation": isolation,
        "mode": args.mode,
        "digest_jobs": args.digest_jobs if args.mode == "digest" else None,
        "opt_ab": args.opt_ab,
        "opt_mf_ab": args.opt_mf_ab,
        "opt_bs_ab": args.opt_bs_ab,
        "compiler_arg": args.compiler,
        "compiler_ab": args.compiler_ab,
        "malloc": {"spec": args.malloc, "preload": args.malloc_preload},
        "quality": args.quality,
        "lgwin": args.lgwin,
        "repetitions": args.repetitions,
        "counters": args.counters if args.mode == "counters" else None,
        "corpus": {"path": corpus, **corpus_fingerprint(corpus)},
        "dictionary": ({"path": args.dictionary,
                        "size_bytes": Path(args.dictionary).stat().st_size,
                        "sha256": sha256_file(args.dictionary)}
                       if args.dictionary else None),
        "compiler": compilers,
        "working_tree_dirty": dirty,
        "working_tree_diff_sha256": diff_sha,
        "variants": [{"label": v["label"], "ref": v["ref"], "opt": v["opt"],
                      "opt_mf": v["opt_mf"], "opt_bs": v["opt_bs"],
                      **v["version"], "artifacts": v["artifacts"]}
                     for v in variants],
        "comparison": comparison,
    }
    (record_dir / "manifest.json").write_text(json.dumps(manifest, indent=2))

    # Print each artifact as a path relative to the current directory (so it can
    # be pasted straight into a pprof/compare command), falling back to the
    # absolute path when the cwd isn't an ancestor of the record dir.
    cwd = Path.cwd()

    def rel_to_cwd(p):
        p = Path(p)
        try:
            return str(p.relative_to(cwd))
        except ValueError:
            return str(p)

    print(f"\nRun record: {rel_to_cwd(record_dir)}")
    for f in sorted(record_dir.iterdir()):
        print(f"  {rel_to_cwd(f)}")


if __name__ == "__main__":
    main()
