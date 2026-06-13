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
import shutil
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
        sys.exit(f"command failed (exit {proc.returncode}): {' '.join(cmd)}")


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
    """Prefix that pins the run and removes layout noise."""
    wrap = []
    if not no_pin:
        wrap += ["taskset", "-c", str(core)]
    if not no_aslr and shutil.which("setarch"):
        # Verify setarch -R works on this box before relying on it.
        if run(["setarch", "-R", "true"], capture=True, check=False).returncode == 0:
            wrap += ["setarch", "-R"]
        else:
            print("WARNING: 'setarch -R' unavailable; ASLR not disabled.",
                  file=sys.stderr)
    if os.geteuid() == 0 and shutil.which("chrt"):
        wrap += ["chrt", "-f", "99"]   # real-time priority (root only)
    return wrap


# --- build ------------------------------------------------------------------


def build_version(repo_root, ref):
    """Resolve ref -> sha, materialize a worktree, build, return info dict."""
    sha = git(["rev-parse", ref], cwd=repo_root)
    cache = repo_root / ".ab-cache"
    wt = cache / "wt" / sha
    build = cache / "build" / sha
    binary = build / "benchmark" / "brotli_benchmark"

    if not wt.exists():
        wt.parent.mkdir(parents=True, exist_ok=True)
        run(["git", "worktree", "add", "--detach", str(wt), sha], cwd=repo_root)

    if not binary.exists():
        run(["cmake", "-B", str(build), "-S", str(wt),
             "-DCMAKE_BUILD_TYPE=Release", "-DBROTLI_BUILD_BENCHMARKS=ON",
             "-DBROTLI_DISABLE_TESTS=ON"])
        run(["cmake", "--build", str(build), "-j", str(os.cpu_count()),
             "--target", "brotli_benchmark"])

    if not binary.exists():
        sys.exit(f"build produced no benchmark binary at {binary}")

    return {
        "ref": ref,
        "sha": sha,
        "worktree": str(wt),
        "build_dir": str(build),
        "binary": str(binary),
        "binary_sha256": sha256_file(binary),
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


def base_args(args, corpus):
    return [f"--corpus={corpus}", f"--quality={args.quality}",
            f"--lgwin={args.lgwin}"]


def find_libprofiler():
    for cand in ("/usr/lib/x86_64-linux-gnu/libprofiler.so",
                 "/usr/lib/libprofiler.so", "/usr/local/lib/libprofiler.so"):
        if Path(cand).exists():
            return cand
    return None


def run_one(args, version, which, record_dir, wrapper):
    """Run one version in the selected mode. Returns artifact paths."""
    env = dict(os.environ)
    env["LD_LIBRARY_PATH"] = version["build_dir"]  # use THIS version's libs
    binary = version["binary"]
    corpus = args.corpus
    cmd = list(wrapper) + [binary] + base_args(args, corpus)
    artifacts = {}

    if args.mode == "digest":
        out = record_dir / f"{which}.digest.txt"
        cmd += [f"--digest={out}"]
        run(cmd, env=env)
        artifacts["digest"] = str(out)

    elif args.mode in ("time", "counters"):
        out = record_dir / f"{which}.json"
        cmd += [f"--benchmark_out={out}", "--benchmark_out_format=json",
                f"--benchmark_repetitions={args.repetitions}",
                "--benchmark_enable_random_interleaving=true"]
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
        env["LD_PRELOAD"] = lib
        env["CPUPROFILE"] = str(out)
        env["CPUPROFILE_FREQUENCY"] = "1000"
        cmd += [f"--benchmark_repetitions={args.repetitions}"]
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


def compare_timing(record_dir, base_json, cand_json):
    b = summarize_json(base_json)
    c = summarize_json(cand_json)
    out = {"baseline": b, "candidate": c}
    if b["median_real_time"] and c["median_real_time"]:
        speedup = b["median_real_time"] / c["median_real_time"]
        out["candidate_speedup"] = speedup
        print(f"\n  baseline : {b['median_real_time']:.2f} {b['time_unit']}"
              f"  ({b['median_MBps'] or 0:.1f} MB/s, ratio "
              f"{b['compression_ratio']:.4f})")
        print(f"  candidate: {c['median_real_time']:.2f} {c['time_unit']}"
              f"  ({c['median_MBps'] or 0:.1f} MB/s, ratio "
              f"{c['compression_ratio']:.4f})")
        print(f"  speedup  : {speedup:.4f}x  "
              f"({(speedup - 1) * 100:+.2f}%)\n")
    # Use google-benchmark's compare.py too, if it's available with deps.
    cmp_py = next(Path(record_dir).glob("../../.ab-cache/build/*/"
                                        "_deps/benchmark-src/tools/compare.py"),
                  None)
    if cmp_py:
        res = run([sys.executable, str(cmp_py), "benchmarks",
                   base_json, cand_json], capture=True, check=False)
        (Path(record_dir) / "compare_py.txt").write_text(res.stdout or "")
    return out


def compare_digests(base_digest, cand_digest):
    def load(p):
        d = {}
        for line in Path(p).read_text().splitlines():
            name, length, h = line.split("\t")
            d[name] = (length, h)
        return d
    b, c = load(base_digest), load(cand_digest)
    diffs = [n for n in b if n in c and b[n] != c[n]]
    only_b = sorted(set(b) - set(c))
    only_c = sorted(set(c) - set(b))
    identical = not diffs and not only_b and not only_c
    if identical:
        print(f"\n  BIT-IDENTICAL: all {len(b)} files match.\n")
    else:
        print(f"\n  NOT bit-identical: {len(diffs)} differing files, "
              f"{len(only_b)} only-baseline, {len(only_c)} only-candidate.")
        for n in diffs[:10]:
            print(f"    {n}: baseline {b[n]}  candidate {c[n]}")
        print()
    return {"identical": identical, "num_differing": len(diffs),
            "differing_sample": diffs[:50],
            "only_baseline": only_b[:50], "only_candidate": only_c[:50]}


# --- main -------------------------------------------------------------------


def main():
    ap = argparse.ArgumentParser(
        description="A/B benchmark the brotli encoder across two git refs.")
    ap.add_argument("--baseline", required=True, help="git ref for version A")
    ap.add_argument("--candidate", required=True, help="git ref for version B")
    ap.add_argument("--corpus", required=True, help="corpus directory")
    ap.add_argument("--quality", type=int, default=6)
    ap.add_argument("--lgwin", type=int, default=22)
    ap.add_argument("--mode", choices=["time", "counters", "pprof", "digest"],
                    default="time")
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
                    help="proceed even if the working tree has uncommitted "
                         "changes (which are NOT benchmarked)")
    args = ap.parse_args()

    repo_root = Path(git(["rev-parse", "--show-toplevel"])).resolve()
    corpus = str(Path(args.corpus).expanduser().resolve())
    args.corpus = corpus
    if not Path(corpus).is_dir():
        sys.exit(f"corpus directory not found: {corpus}")

    # Provenance gate: uncommitted changes to tracked files are not captured by
    # SHA worktrees. Untracked files (editor swaps, __pycache__) don't affect a
    # worktree build, so ignore them here.
    dirty = bool(git(["status", "--porcelain", "--untracked-files=no"],
                     cwd=repo_root))
    diff_sha = None
    if dirty:
        diff_sha = hashlib.sha256(
            git(["diff", "HEAD"], cwd=repo_root).encode()).hexdigest()
        if not args.allow_dirty:
            sys.exit("Working tree has uncommitted changes; they will NOT be "
                     "benchmarked (versions are built from committed SHAs). "
                     "Commit them, or pass --allow-dirty to proceed anyway.")
        print("WARNING: working tree is dirty; uncommitted changes are NOT "
              "included in either version.", file=sys.stderr)

    env_state = read_env_state()
    warn_if_noisy(env_state)

    print(f"Building baseline ({args.baseline}) ...", file=sys.stderr)
    baseline = build_version(repo_root, args.baseline)
    print(f"Building candidate ({args.candidate}) ...", file=sys.stderr)
    candidate = build_version(repo_root, args.candidate)

    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    record_dir = (repo_root / "benchmark" / "results" /
                  f"{stamp}__{baseline['sha'][:12]}_vs_{candidate['sha'][:12]}")
    record_dir.mkdir(parents=True, exist_ok=True)

    wrapper = build_wrapper(args.core, args.no_pin, args.no_aslr_disable)

    with Tuner(args.tune):
        # Re-read env so the manifest reflects the actual (tuned) state.
        env_state = read_env_state()
        print(f"\n== baseline {baseline['sha'][:12]} ==", file=sys.stderr)
        base_art = run_one(args, baseline, "baseline", record_dir, wrapper)
        print(f"== candidate {candidate['sha'][:12]} ==", file=sys.stderr)
        cand_art = run_one(args, candidate, "candidate", record_dir, wrapper)

    comparison = None
    if args.mode in ("time", "counters"):
        comparison = compare_timing(record_dir, base_art["json"],
                                    cand_art["json"])
    elif args.mode == "digest":
        comparison = compare_digests(base_art["digest"], cand_art["digest"])

    compiler = run(["c++", "--version"], capture=True, check=False).stdout
    manifest = {
        "timestamp": stamp,
        "hostname": os.uname().nodename,
        "kernel": os.uname().release,
        "env": env_state,
        "tuned": args.tune,
        "wrapper": wrapper,
        "pinned_core": None if args.no_pin else args.core,
        "mode": args.mode,
        "quality": args.quality,
        "lgwin": args.lgwin,
        "repetitions": args.repetitions,
        "counters": args.counters if args.mode == "counters" else None,
        "corpus": {"path": corpus, **corpus_fingerprint(corpus)},
        "compiler": (compiler or "").splitlines()[0] if compiler else None,
        "working_tree_dirty": dirty,
        "working_tree_diff_sha256": diff_sha,
        "baseline": {**baseline, "artifacts": base_art},
        "candidate": {**candidate, "artifacts": cand_art},
        "comparison": comparison,
    }
    (record_dir / "manifest.json").write_text(json.dumps(manifest, indent=2))

    print(f"\nRun record: {record_dir}")
    for f in sorted(record_dir.iterdir()):
        print(f"  {f.name}")


if __name__ == "__main__":
    main()
