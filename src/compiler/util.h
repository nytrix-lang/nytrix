#ifndef NT_COMPILER_UTIL_H
#define NT_COMPILER_UTIL_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

char *nt_read_file(const char *path);
bool nt_copy_file(const char *src, const char *dst);
void nt_ensure_dir(const char *path);
void nt_write_text_file(const char *path, const char *contents);
uint64_t nt_fnv1a64(const void *data, size_t len, uint64_t seed);
const char *nt_src_root(void);

#endif
