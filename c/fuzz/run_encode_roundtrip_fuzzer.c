/* Copyright 2026 The Brotli Authors.

   Distributed under MIT license.
   See file LICENSE for detail or copy at https://opensource.org/licenses/MIT
*/

/* Simple runner for encode_roundtrip_fuzzer.c: replays a single input file
   through the roundtrip check. Build it with a sanitizer (but no fuzzing
   engine) to reproduce and triage an input libFuzzer flagged, or to run the
   check over a directory of files without the fuzzer. */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

void LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);

int main(int argc, char** argv) {
  if (argc != 2) {
    fprintf(stderr, "Exactly one argument is expected.\n");
    exit(EXIT_FAILURE);
  }

  FILE* f = fopen(argv[1], "r");
  if (!f) {
    fprintf(stderr, "Failed to open input file.");
    exit(EXIT_FAILURE);
  }

  size_t max_len = 1 << 20;
  unsigned char* tmp = (unsigned char*)malloc(max_len);
  size_t len = fread(tmp, 1, max_len, f);
  if (ferror(f)) {
    fclose(f);
    fprintf(stderr, "Failed read input file.");
    exit(EXIT_FAILURE);
  }
  fclose(f);
  /* Make data after the end "inaccessible". */
  unsigned char* data = (unsigned char*)malloc(len);
  memcpy(data, tmp, len);
  free(tmp);

  LLVMFuzzerTestOneInput(data, len);
  free(data);
  exit(EXIT_SUCCESS);
}
