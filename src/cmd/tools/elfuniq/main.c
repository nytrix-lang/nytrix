/*
 * ny-elf-unique: copies a shared library and rebinds its STB_GNU_UNIQUE
 * symbols as STB_WEAK.
 *
 * glibc binds a GNU-unique symbol process-wide by name alone, ignoring symbol
 * versions. The full compiler links LLVM; a JIT-run program that opens Mesa's
 * llvmpipe brings a second LLVM into the process, and that LLVM's
 * unique template statics (for example FoldingSet<SDNode>'s Info table) were
 * bound to the compiler's copy, so llvmpipe ran the other version's code and
 * crashed in shader compilation. Weak, versioned definitions resolve by
 * version instead (LLVM_21.1 vs LLVM_23.1), so both copies stay separate.
 *
 * usage: ny-elf-unique INPUT OUTDIR
 * Writes OUTDIR/<DT_SONAME of INPUT> (or the input's file name) and prints
 * how many symbols were rebound.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__linux__)
#include <elf.h>

static unsigned char *read_all(const char *path, size_t *len) {
  FILE *f = fopen(path, "rb");
  if (!f)
    return NULL;
  if (fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    return NULL;
  }
  long n = ftell(f);
  if (n <= 0 || fseek(f, 0, SEEK_SET) != 0) {
    fclose(f);
    return NULL;
  }
  unsigned char *buf = malloc((size_t)n);
  if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) {
    free(buf);
    buf = NULL;
  }
  fclose(f);
  *len = (size_t)n;
  return buf;
}

int main(int argc, char **argv) {
  if (argc != 3) {
    fprintf(stderr, "usage: ny-elf-unique INPUT OUTDIR\n");
    return 2;
  }
  size_t len = 0;
  unsigned char *data = read_all(argv[1], &len);
  if (!data || len < sizeof(Elf64_Ehdr) || memcmp(data, ELFMAG, SELFMAG) != 0 ||
      data[EI_CLASS] != ELFCLASS64) {
    fprintf(stderr, "ny-elf-unique: %s is not a 64-bit ELF file\n", argv[1]);
    free(data);
    return 1;
  }
  Elf64_Ehdr *eh = (Elf64_Ehdr *)data;
  if (eh->e_shoff == 0 || eh->e_shoff + (uint64_t)eh->e_shnum * sizeof(Elf64_Shdr) > len) {
    fprintf(stderr, "ny-elf-unique: %s has no section table\n", argv[1]);
    free(data);
    return 1;
  }
  Elf64_Shdr *sh = (Elf64_Shdr *)(data + eh->e_shoff);
  const char *soname = NULL;
  size_t rebound = 0;
  for (unsigned i = 0; i < eh->e_shnum; ++i) {
    if (sh[i].sh_offset + sh[i].sh_size > len)
      continue;
    if ((sh[i].sh_type == SHT_DYNSYM || sh[i].sh_type == SHT_SYMTAB) &&
        sh[i].sh_entsize == sizeof(Elf64_Sym)) {
      Elf64_Sym *sym = (Elf64_Sym *)(data + sh[i].sh_offset);
      size_t count = sh[i].sh_size / sizeof(Elf64_Sym);
      for (size_t j = 0; j < count; ++j) {
        if (ELF64_ST_BIND(sym[j].st_info) == STB_GNU_UNIQUE) {
          sym[j].st_info = ELF64_ST_INFO(STB_WEAK, ELF64_ST_TYPE(sym[j].st_info));
          ++rebound;
        }
      }
    }
    if (sh[i].sh_type == SHT_DYNAMIC && sh[i].sh_link < eh->e_shnum) {
      const Elf64_Shdr *strs = &sh[sh[i].sh_link];
      Elf64_Dyn *dyn = (Elf64_Dyn *)(data + sh[i].sh_offset);
      size_t count = sh[i].sh_size / sizeof(Elf64_Dyn);
      for (size_t j = 0; j < count && dyn[j].d_tag != DT_NULL; ++j)
        if (dyn[j].d_tag == DT_SONAME && strs->sh_offset + dyn[j].d_un.d_val < len)
          soname = (const char *)data + strs->sh_offset + dyn[j].d_un.d_val;
    }
  }
  const char *base = strrchr(argv[1], '/');
  base = base ? base + 1 : argv[1];
  char out[4096];
  snprintf(out, sizeof(out), "%s/%s", argv[2], soname ? soname : base);
  char tmp[4200];
  snprintf(tmp, sizeof(tmp), "%s.tmp", out);
  FILE *f = fopen(tmp, "wb");
  if (!f || fwrite(data, 1, len, f) != len || fclose(f) != 0 ||
      rename(tmp, out) != 0) {
    fprintf(stderr, "ny-elf-unique: cannot write %s\n", out);
    free(data);
    return 1;
  }
  printf("ny-elf-unique: %s: rebound %zu unique symbols\n", out, rebound);
  free(data);
  return 0;
}
#else
int main(void) { return 0; }
#endif
