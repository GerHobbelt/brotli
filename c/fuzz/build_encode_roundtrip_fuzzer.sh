#!/usr/bin/env bash
#
# Build the roundtrip encoder fuzzer and its standalone replay runner.
#
# This fork's optimized encoder paths must, at minimum, roundtrip losslessly
# whether or not they are bit-identical to the reference. encode_roundtrip_fuzzer
# encodes each input with the opt paths enabled, decodes the result, and aborts
# unless it reproduces the original bytes. Built with a sanitizer it also catches
# the memory-safety bugs (SIMD over-reads, prefetch of invalid addresses,
# uninitialized lanes) that a fixed-corpus check can hide.
#
# All encoder and decoder sources are compiled together with the harness so that
# coverage instrumentation reaches inside them and there is no sanitizer-runtime
# mismatch between a prebuilt library and the driver.
#
# Usage:
#   [SAN=address|memory|undefined|none] [CC=clang] \
#       c/fuzz/build_encode_roundtrip_fuzzer.sh
#
# Produces, under build-encode-fuzz/:
#   encode_roundtrip_fuzzer       coverage-guided libFuzzer binary
#   run_encode_roundtrip_fuzzer   single-input replay runner (triage / corpus)
#
# Examples:
#   # Fuzz, seeded from a real corpus (payload cap must clear header + data):
#   build-encode-fuzz/encode_roundtrip_fuzzer -max_len=70000 seeds/
#   # Just run the check over the seeds once, no mutation:
#   build-encode-fuzz/encode_roundtrip_fuzzer -runs=0 seeds/
#   # Reproduce / triage a flagged input:
#   build-encode-fuzz/run_encode_roundtrip_fuzzer crash-abcd1234
#
# Shared-dictionary campaigns (env-selected; neither set = no dictionary):
#   ENCODE_FUZZ_DICT=corpus.dict build-encode-fuzz/encode_roundtrip_fuzzer seeds/  # fixed real dict
#   ENCODE_FUZZ_DICT_FUZZED=1    build-encode-fuzz/encode_roundtrip_fuzzer seeds/  # fuzz dict content
#
set -euo pipefail

CC=${CC:-clang}
SAN=${SAN:-address}

BROTLI="$( cd "$( dirname "${BASH_SOURCE[0]}" )/../.." && pwd )"
SRC=$BROTLI/c
OUT=$BROTLI/build-encode-fuzz
mkdir -p "$OUT"

SRCS=( "$SRC"/common/*.c "$SRC"/enc/*.c "$SRC"/dec/*.c )
COMMON_FLAGS=( -O2 -g -fno-omit-frame-pointer -std=c99 -I"$SRC/include" )

san=""
if [ "$SAN" != "none" ] && [ -n "$SAN" ]; then
  san="$SAN"
fi

echo "Building encode_roundtrip_fuzzer (sanitizer=${san:-none}) ..."
$CC "${COMMON_FLAGS[@]}" -fsanitize=fuzzer${san:+,$san} \
    "${SRCS[@]}" "$SRC/fuzz/encode_roundtrip_fuzzer.c" \
    -lm -o "$OUT/encode_roundtrip_fuzzer"

echo "Building run_encode_roundtrip_fuzzer (sanitizer=${san:-none}) ..."
$CC "${COMMON_FLAGS[@]}" ${san:+-fsanitize=$san} \
    "${SRCS[@]}" "$SRC/fuzz/encode_roundtrip_fuzzer.c" \
    "$SRC/fuzz/run_encode_roundtrip_fuzzer.c" \
    -lm -o "$OUT/run_encode_roundtrip_fuzzer"

echo "Done. Binaries in ${OUT#$BROTLI/}/"
