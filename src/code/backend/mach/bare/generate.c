/*
 * Maintainer tool for regenerating the embedded freestanding runtime.
 */
#define _POSIX_C_SOURCE 200809L
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif
#include "code/backend/mach/bare/generate.h"

#ifdef _WIN32
#include <stdio.h>

int ny_bare_generate_main(const char *root, int argc, char **argv) {
  (void)root;
  (void)argc;
  (void)argv;
  fputs("bare payload generation currently requires a POSIX host\n", stderr);
  return 1;
}
#else
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static int run_capture(const char *path, char *const argv[], char **output) {
  int fds[2];
  if (pipe(fds) != 0)
    return -1;
  pid_t pid = fork();
  if (pid == 0) {
    close(fds[0]);
    dup2(fds[1], STDOUT_FILENO);
    close(fds[1]);
    execvp(argv[0], argv);
    _exit(127);
  }
  close(fds[1]);
  if (pid < 0) {
    close(fds[0]);
    return -1;
  }
  FILE *f = fopen(path, "wb");
  if (!f) {
    close(fds[0]);
    (void)waitpid(pid, NULL, 0);
    return -1;
  }
  FILE *stream = fdopen(fds[0], "rb");
  if (!stream) {
    fclose(f);
    close(fds[0]);
    (void)waitpid(pid, NULL, 0);
    return -1;
  }
  char buf[8192];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), stream)) != 0)
    (void)fwrite(buf, 1, n, f);
  fclose(stream);
  fclose(f);
  int status = 0;
  if (waitpid(pid, &status, 0) < 0 || !WIFEXITED(status) ||
      WEXITSTATUS(status) != 0)
    return -1;
  if (output) {
    FILE *in = fopen(path, "rb");
    if (!in)
      return -1;
    (void)fseek(in, 0, SEEK_END);
    long len = ftell(in);
    rewind(in);
    *output = len < 0 ? NULL : malloc((size_t)len + 1);
    if (!*output || fread(*output, 1, (size_t)len, in) != (size_t)len) {
      free(*output);
      *output = NULL;
      fclose(in);
      return -1;
    }
    (*output)[len] = '\0';
    fclose(in);
  }
  return 0;
}

static bool contains_any(const char *text, const char *const *items,
                         size_t count) {
  for (size_t i = 0; i < count; ++i)
    if (strstr(text, items[i]))
      return true;
  return false;
}

static int generate_one(const char *root, const char *tmp, const char *arch,
                        bool check) {
  char obj[4096], elf[4096], bin[4096], script[4096], out[4096], path[4096];
  char target[128], *relocs = NULL, *sections = NULL, *symbols = NULL;
  snprintf(obj, sizeof(obj), "%s/%s.o", tmp, arch);
  snprintf(elf, sizeof(elf), "%s/%s.elf", tmp, arch);
  snprintf(bin, sizeof(bin), "%s/%s.bin", tmp, arch);
  snprintf(script, sizeof(script), "%s/payload.ld", tmp);
  snprintf(out, sizeof(out), "%s/%s.inc", tmp, arch);
  const char *arch_dir = strcmp(arch, "aarch64") == 0 ? "a64" : "x86";
  snprintf(path, sizeof(path), "%s/src/code/backend/mach/arch/%s/payload.h",
           root, arch_dir);
  snprintf(target, sizeof(target), "--target=%s-unknown-linux-gnu", arch);

  char runtime[4096];
  snprintf(runtime, sizeof(runtime), "%s/src/code/backend/mach/bare/runtime.c",
           root);
  if (strcmp(arch, "aarch64") == 0) {
    char *args[] = {"clang",
                    target,
                    "-std=c11",
                    "-Oz",
                    "-ffreestanding",
                    "-fno-builtin",
                    "-fno-stack-protector",
                    "-fno-unwind-tables",
                    "-fno-asynchronous-unwind-tables",
                    "-fno-jump-tables",
                    "-fno-vectorize",
                    "-fno-slp-vectorize",
                    "-fPIC",
                    "-nostdlib",
                    "-mcmodel=tiny",
                    "-mno-outline-atomics",
                    "-c",
                    runtime,
                    "-o",
                    obj,
                    NULL};
    if (run_capture(out, args, NULL) != 0)
      goto fail;
  } else {
    char *args[] = {"clang",
                    target,
                    "-std=c11",
                    "-Oz",
                    "-ffreestanding",
                    "-fno-builtin",
                    "-fno-stack-protector",
                    "-fno-unwind-tables",
                    "-fno-asynchronous-unwind-tables",
                    "-fno-jump-tables",
                    "-fno-vectorize",
                    "-fno-slp-vectorize",
                    "-fPIC",
                    "-nostdlib",
                    "-mno-red-zone",
                    "-c",
                    runtime,
                    "-o",
                    obj,
                    NULL};
    if (run_capture(out, args, NULL) != 0)
      goto fail;
  }
  char *readrel[] = {"readelf", "-rW", obj, NULL};
  if (run_capture(out, readrel, &relocs) != 0)
    goto fail;
  const char *allowed_x[] = {"R_X86_64_PC32", "R_X86_64_PLT32"};
  const char *allowed_a[] = {"R_AARCH64_ADR_PREL_LO21",
                             "R_AARCH64_LD_PREL_LO19", "R_AARCH64_CALL26",
                             "R_AARCH64_JUMP26", "R_AARCH64_CONDBR19"};
  const char *const *allowed =
      strcmp(arch, "aarch64") == 0 ? allowed_a : allowed_x;
  size_t allowed_n = strcmp(arch, "aarch64") == 0 ? 5 : 2;
  for (char *line = strtok(relocs, "\n"); line; line = strtok(NULL, "\n"))
    if (strstr(line, "R_") && !contains_any(line, allowed, allowed_n)) {
      fprintf(stderr, "bare generator: non-PIC relocation: %s\n", line);
      goto fail;
    }
  FILE *ld = fopen(script, "wb");
  if (!ld)
    goto fail;
  fputs("SECTIONS { . = 0; .payload : { *(.text*) *(.rodata*) } "
        "/DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) } }\n",
        ld);
  fclose(ld);
  char *link[] = {"ld.lld", "--no-undefined",
                  "-T",     script,
                  "-e",     "ny_bare_entry",
                  obj,      "-o",
                  elf,      NULL};
  if (run_capture(out, link, NULL) != 0)
    goto fail;
  char *readsec[] = {"readelf", "-SW", elf, NULL};
  if (run_capture(out, readsec, &sections) != 0)
    goto fail;
  const char *badsec[] = {".data", ".bss", ".got", ".rela"};
  if (contains_any(sections, badsec, 4)) {
    fputs("bare generator: payload has mutable data or dynamic relocations\n",
          stderr);
    goto fail;
  }
  char *nm[] = {"nm", elf, NULL};
  if (run_capture(out, nm, &symbols) != 0)
    goto fail;
  char *entry_line = strstr(symbols, " ny_bare_entry");
  if (!entry_line)
    goto fail;
  while (entry_line > symbols && entry_line[-1] != '\n')
    --entry_line;
  unsigned long entry = strtoul(entry_line, NULL, 16);
  char *copy[] = {"llvm-objcopy",
                  "-O",
                  "binary",
                  "--only-section=.payload",
                  elf,
                  bin,
                  NULL};
  if (run_capture(out, copy, NULL) != 0)
    goto fail;
  FILE *data = fopen(bin, "rb");
  if (!data)
    goto fail;
  FILE *generated = fopen(out, "wb");
  if (!generated) {
    fclose(data);
    goto fail;
  }
  fprintf(generated,
          "/* Generated by bare/generate.c from runtime.c. Do not edit. */\n"
          "static const size_t ny_bare_%s_entry = %lu;\n"
          "static const unsigned char ny_bare_%s_code[] = {\n",
          arch, entry, arch);
  unsigned char bytes[12];
  size_t n;
  while ((n = fread(bytes, 1, sizeof(bytes), data)) != 0) {
    fputs("  ", generated);
    for (size_t i = 0; i < n; ++i)
      fprintf(generated, "0x%02x%s", bytes[i], i + 1 == n ? "," : ", ");
    fputs("\n", generated);
  }
  fputs("};\n", generated);
  fclose(data);
  fclose(generated);
  FILE *fresh = fopen(out, "rb");
  if (!fresh)
    goto fail;
  /*
   * Only the --check path compares against the existing file; opening it in
   * the regeneration path leaked the handle (and both handles leaked when
   * `fresh` failed to open).
   */
  FILE *old = check ? fopen(path, "rb") : NULL;
  if (check) {
    int a, b;
    bool same = old != NULL;
    while (same && (a = fgetc(fresh)) != EOF) {
      b = fgetc(old);
      if (a != b)
        same = false;
    }
    if (old && fgetc(old) != EOF)
      same = false;
    fclose(fresh);
    if (old)
      fclose(old);
    if (!same) {
      fprintf(stderr, "%s needs regeneration\n", path);
      goto fail;
    }
  } else {
    FILE *dst = fopen(path, "wb");
    if (!dst) {
      fclose(fresh);
      goto fail;
    }
    while ((n = fread(bytes, 1, sizeof(bytes), fresh)) != 0)
      fwrite(bytes, 1, n, dst);
    fclose(fresh);
    fclose(dst);
  }
  free(relocs);
  free(sections);
  free(symbols);
  return 0;
fail:
  free(relocs);
  free(sections);
  free(symbols);
  fprintf(stderr, "bare generator: failed while generating %s\n", arch);
  return 1;
}

int ny_bare_generate_main(const char *root, int argc, char **argv) {
  bool check = argc > 0 && strcmp(argv[0], "--check") == 0;
  if (argc > (check ? 1 : 0)) {
    fputs("usage: ./make bare-payload [--check]\n", stderr);
    return 2;
  }
  char temp[] = "/tmp/ny-bare-XXXXXX";
  char *dir = mkdtemp(temp);
  if (!dir) {
    perror("mkdtemp");
    return 1;
  }
  int rc = generate_one(root, dir, "x86_64", check) ||
           generate_one(root, dir, "aarch64", check);
  const char *files[] = {"x86_64.o",   "x86_64.elf",  "x86_64.bin",
                         "aarch64.o",  "aarch64.elf", "aarch64.bin",
                         "payload.ld", "x86_64.inc",  "aarch64.inc"};
  for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); ++i) {
    char file[4096];
    snprintf(file, sizeof(file), "%s/%s", dir, files[i]);
    (void)unlink(file);
  }
  (void)rmdir(dir);
  return rc;
}
#endif
