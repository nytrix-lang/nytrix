#ifndef NY_COMPILER_UTIL_H
#define NY_COMPILER_UTIL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

// File I/O
char *ny_read_file(const char *path);
int ny_write_file(const char *path, const char *content, size_t len);
void ny_write_text_file(const char *path, const char *contents);
int ny_copy_file(const char *src, const char *dst);
int ny_ensure_dir(const char *path);

// String utilities
char *ny_strdup(const char *s);
void ny_str_list_append(char ***list, size_t *len, size_t *cap,
                        const char *str);
void ny_str_list_free(char **list, size_t count);

// Hash
uint64_t ny_fnv1a64(const void *data, size_t len, uint64_t seed);
int ny_levenshtein(const char *s1, const char *s2);

// Paths
const char *ny_src_root(void);
char *ny_get_executable_dir(void);

#endif
