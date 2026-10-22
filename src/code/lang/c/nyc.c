#include "nyc_frontend.h"

#include <nyr/nyr.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *read_source(const char *path, size_t *length) {
  FILE *file = fopen(path, "rb");
  if (!file)
    return NULL;
  if (fseek(file, 0, SEEK_END) != 0) {
    fclose(file);
    return NULL;
  }
  long size = ftell(file);
  if (size < 0 || (uintmax_t)size > SIZE_MAX - 1 ||
      fseek(file, 0, SEEK_SET) != 0) {
    fclose(file);
    return NULL;
  }
  size_t bytes = (size_t)size;
  char *source = malloc(bytes + 1);
  if (!source) {
    fclose(file);
    return NULL;
  }
  size_t read = fread(source, 1, bytes, file);
  bool ok = read == bytes && !ferror(file);
  fclose(file);
  if (!ok) {
    free(source);
    return NULL;
  }
  source[bytes] = '\0';
  *length = bytes;
  return source;
}

static nyc_abi_t nyc_abi_for_target(const char *triple) {
  if (!triple || !*triple)
    return NYC_ABI_UNSUPPORTED;
  if ((strncmp(triple, "x86_64", 6) == 0 ||
       strncmp(triple, "amd64", 5) == 0) &&
      (strstr(triple, "windows") || strstr(triple, "mingw") ||
       strstr(triple, "msvc")))
    return NYC_ABI_WIN64;
  if ((strncmp(triple, "aarch64", 7) == 0 ||
       strncmp(triple, "arm64", 5) == 0) &&
      strstr(triple, "linux") && !strstr(triple, "windows") &&
      !strstr(triple, "msvc") && !strstr(triple, "mingw"))
    return NYC_ABI_AAPCS64;
  if ((strncmp(triple, "x86_64", 6) == 0 ||
       strncmp(triple, "amd64", 5) == 0) &&
      !strstr(triple, "windows") && !strstr(triple, "mingw") &&
      !strstr(triple, "msvc"))
    return NYC_ABI_SYSV_X86_64;
  return NYC_ABI_UNSUPPORTED;
}

int main(int argc, char **argv) {
  const char *target = NULL;
  int source_arg = 1;
  if (argc == 5 && strncmp(argv[1], "--target=", 9) == 0 && argv[1][9]) {
    target = argv[1] + 9;
    source_arg = 2;
  }
  if (argc != source_arg + 3) {
    fprintf(stderr,
            "usage: nyc [--target=TRIPLE] SOURCE.c OUTPUT.o SYMBOL\n");
    return 2;
  }
  size_t source_len = 0;
  char *source = read_source(argv[source_arg], &source_len);
  if (!source) {
    fprintf(stderr, "%s: cannot read C source\n", argv[source_arg]);
    return 1;
  }
  char err[512] = {0};
  nyr_func_t ir;
  nyr_func_init(&ir);
  nyr_target_t host;
  const char *triple = target;
  if (!triple) {
    if (!nyr_host_target(&host, err, sizeof(err))) {
      fprintf(stderr, "%s\n", err[0] ? err : "cannot determine host target");
      nyr_func_destroy(&ir);
      free(source);
      return 1;
    }
    triple = host.target;
  }
  nyc_abi_t abi = nyc_abi_for_target(triple);
  bool ok = nyc_frontend_compile(source, source_len, argv[source_arg + 2], abi,
                                 &ir, err, sizeof(err));
  ok = ok &&
            nyr_verify(&ir, err, sizeof(err)) &&
            (getenv("NYC_DISABLE_PASS")
                 ? (nyr_set_pass_controls(getenv("NYC_DISABLE_PASS"), NULL),
                    true)
                 : true) &&
            nyr_optimize(&ir, getenv("NYC_OPT_O0") ? NYR_OPT_O0 : NYR_OPT_O2,
                         err, sizeof(err)) &&
            /*
             * Re-check after optimization: ABI metadata must survive the
             * pipeline before any object file is written.
             */
            nyr_verify(&ir, err, sizeof(err)) &&
            (target ? nyr_emit_object_for_target(&ir, argv[source_arg + 1],
                                                 argv[source_arg + 2], target,
                                                 err, sizeof(err))
                    : nyr_emit_object(&ir, argv[source_arg + 1],
                                      argv[source_arg + 2], err,
                                      sizeof(err)));
  if (!ok)
    fprintf(stderr, "%s: %s\n", argv[source_arg],
            err[0] ? err : "C compilation failed");
  else
    fprintf(stderr, "compiled %s -> %s (%s)\n", argv[source_arg],
            argv[source_arg + 1], argv[source_arg + 2]);
  nyr_func_destroy(&ir);
  free(source);
  return ok ? 0 : 1;
}
