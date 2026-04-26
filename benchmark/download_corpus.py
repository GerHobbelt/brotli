#!/usr/bin/env python3
"""Download a corpus of minified JS files for brotli benchmarking.

Fetches the top 1000 most popular npm packages from jsdelivr, finds all
.min.js files >= 10KB, and downloads them into benchmark/corpus/.

Usage:
    python3 benchmark/download_corpus.py [--corpus-dir DIR] [--jobs N]
"""

import argparse
import hashlib
import json
import os
import sys
import time
import urllib.request
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

JSDELIVR_STATS = "https://data.jsdelivr.com/v1/stats/packages"
JSDELIVR_PKG = "https://data.jsdelivr.com/v1/packages/npm"
JSDELIVR_CDN = "https://cdn.jsdelivr.net/npm"
MIN_FILE_SIZE = 10240  # 10 KB
MAX_RETRIES = 3
TOP_N_PACKAGES = 1000
PACKAGES_PER_PAGE = 100
MANIFEST_NAME = "corpus_manifest.json"
UA = "brotli-benchmark-corpus-builder"


def url_encode_package(pkg):
    return pkg.replace("@", "%40").replace("/", "%2F")


def fetch_json(url, timeout=15):
    req = urllib.request.Request(url, headers={"User-Agent": UA})
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return json.loads(resp.read())


def get_top_packages(n=TOP_N_PACKAGES):
    """Fetch the top N packages by jsdelivr monthly downloads."""
    packages = []
    pages = (n + PACKAGES_PER_PAGE - 1) // PACKAGES_PER_PAGE
    for page in range(1, pages + 1):
        url = (
            f"{JSDELIVR_STATS}?type=npm&period=month"
            f"&limit={PACKAGES_PER_PAGE}&page={page}"
        )
        data = fetch_json(url)
        packages.extend(entry["name"] for entry in data)
        print(f"  Fetched package list page {page}/{pages}", flush=True)
    return packages[:n]


def scan_package(pkg):
    """Find all .min.js files >= 10KB in a package. Returns list of tuples."""
    url_pkg = url_encode_package(pkg)
    resolved = fetch_json(f"{JSDELIVR_PKG}/{url_pkg}/resolved?specifier=latest")
    version = resolved.get("version", "latest")

    listing = fetch_json(
        f"{JSDELIVR_PKG}/{url_pkg}@{version}?structure=flat"
    )
    results = []
    for f in listing.get("files", []):
        if f["name"].endswith(".min.js") and f.get("size", 0) >= MIN_FILE_SIZE:
            cdn_url = f"{JSDELIVR_CDN}/{pkg}@{version}{f['name']}"
            # Use package name + path as a safe filename
            safe_name = f"{pkg}@{version}{f['name']}".replace("/", "__")
            results.append({
                "package": pkg,
                "version": version,
                "path": f["name"],
                "expected_size": f["size"],
                "url": cdn_url,
                "filename": safe_name,
            })
    return results


def scan_all_packages(packages, jobs=40):
    """Scan all packages in parallel for .min.js files."""
    manifest = []
    errors = 0
    done = 0
    total = len(packages)
    with ThreadPoolExecutor(max_workers=jobs) as pool:
        futures = {pool.submit(scan_package, pkg): pkg for pkg in packages}
        for future in as_completed(futures):
            done += 1
            try:
                manifest.extend(future.result())
            except Exception:
                errors += 1
            if done % 100 == 0:
                print(f"  Scanned {done}/{total} packages...", flush=True)
    print(
        f"  Scan complete: {len(manifest)} files found, "
        f"{errors} package errors"
    )
    return manifest


def download_file(entry, corpus_dir):
    """Download a single file with retries and size verification."""
    dest = corpus_dir / entry["filename"]

    # Skip if already downloaded and correct size
    if dest.exists() and dest.stat().st_size == entry["expected_size"]:
        return True, entry["filename"], "already exists"

    for attempt in range(1, MAX_RETRIES + 1):
        try:
            req = urllib.request.Request(
                entry["url"], headers={"User-Agent": UA}
            )
            with urllib.request.urlopen(req, timeout=30) as resp:
                data = resp.read()

            if len(data) != entry["expected_size"]:
                raise ValueError(
                    f"Size mismatch: got {len(data)}, "
                    f"expected {entry['expected_size']}"
                )

            # Verify non-empty
            if not data:
                raise ValueError("Empty file")

            dest.write_bytes(data)
            return True, entry["filename"], "ok"
        except Exception as e:
            if attempt == MAX_RETRIES:
                return False, entry["filename"], str(e)
            time.sleep(0.5 * attempt)

    return False, entry["filename"], "max retries exceeded"


def download_all(manifest, corpus_dir, jobs=40):
    """Download all files in parallel."""
    corpus_dir.mkdir(parents=True, exist_ok=True)
    ok_count = 0
    fail_count = 0
    failures = []
    done = 0
    total = len(manifest)

    with ThreadPoolExecutor(max_workers=jobs) as pool:
        futures = {
            pool.submit(download_file, entry, corpus_dir): entry
            for entry in manifest
        }
        for future in as_completed(futures):
            done += 1
            success, filename, msg = future.result()
            if success:
                ok_count += 1
            else:
                fail_count += 1
                failures.append((filename, msg))
            if done % 200 == 0:
                print(
                    f"  Downloaded {done}/{total} "
                    f"({ok_count} ok, {fail_count} failed)...",
                    flush=True,
                )

    return ok_count, fail_count, failures


def verify_corpus(manifest, corpus_dir):
    """Final verification pass: check all files exist with correct sizes."""
    missing = []
    bad_size = []
    total_size = 0
    for entry in manifest:
        dest = corpus_dir / entry["filename"]
        if not dest.exists():
            missing.append(entry["filename"])
        elif dest.stat().st_size != entry["expected_size"]:
            bad_size.append(
                (
                    entry["filename"],
                    dest.stat().st_size,
                    entry["expected_size"],
                )
            )
        else:
            total_size += dest.stat().st_size
    return missing, bad_size, total_size


def main():
    parser = argparse.ArgumentParser(
        description="Download minified JS corpus for brotli benchmarking"
    )
    parser.add_argument(
        "--corpus-dir",
        type=Path,
        default=Path(__file__).parent / "corpus",
        help="Directory to download files into (default: benchmark/corpus/)",
    )
    parser.add_argument(
        "--jobs", type=int, default=40, help="Number of parallel workers"
    )
    parser.add_argument(
        "--manifest-only",
        action="store_true",
        help="Only generate manifest, don't download",
    )
    args = parser.parse_args()

    manifest_path = args.corpus_dir / MANIFEST_NAME

    # Check for existing manifest
    if manifest_path.exists():
        print(f"Loading existing manifest from {manifest_path}")
        with open(manifest_path) as f:
            manifest = json.load(f)
        print(f"  {len(manifest)} files in manifest")
    else:
        print(f"Fetching top {TOP_N_PACKAGES} packages from jsdelivr...")
        packages = get_top_packages(TOP_N_PACKAGES)

        print(f"Scanning {len(packages)} packages for .min.js files >= 10KB...")
        manifest = scan_all_packages(packages, jobs=args.jobs)

        # Sort by size descending for readability
        manifest.sort(key=lambda e: e["expected_size"], reverse=True)

        # Save manifest
        args.corpus_dir.mkdir(parents=True, exist_ok=True)
        with open(manifest_path, "w") as f:
            json.dump(manifest, f, indent=2)
        print(f"  Manifest saved to {manifest_path}")

    if args.manifest_only:
        total = sum(e["expected_size"] for e in manifest)
        print(f"\n{len(manifest)} files, {total / 1024 / 1024:.1f} MB total")
        return

    # Download
    print(f"\nDownloading {len(manifest)} files to {args.corpus_dir}...")
    ok, failed, failures = download_all(manifest, args.corpus_dir, args.jobs)
    print(f"  Downloads: {ok} ok, {failed} failed")

    if failures:
        print("\n  Failed downloads:")
        for filename, msg in failures[:20]:
            print(f"    {filename}: {msg}")
        if len(failures) > 20:
            print(f"    ... and {len(failures) - 20} more")

    # Final verification
    print("\nVerifying corpus...")
    missing, bad_size, total_size = verify_corpus(manifest, args.corpus_dir)

    if missing:
        print(f"  MISSING: {len(missing)} files")
        for f in missing[:10]:
            print(f"    {f}")
    if bad_size:
        print(f"  BAD SIZE: {len(bad_size)} files")
        for f, got, expected in bad_size[:10]:
            print(f"    {f}: got {got}, expected {expected}")

    file_count = len(manifest) - len(missing) - len(bad_size)
    print(
        f"\nCorpus: {file_count}/{len(manifest)} files, "
        f"{total_size / 1024 / 1024:.1f} MB"
    )

    if missing or bad_size:
        print("\nERROR: Corpus is incomplete!")
        sys.exit(1)
    else:
        print("\nCorpus verified OK.")


if __name__ == "__main__":
    main()
