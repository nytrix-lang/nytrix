/* File hashing interface that returns a prefixed lowercase SHA-256 digest. */
#pragma once

#include <stdbool.h>
#include <stddef.h>

/* Writes "sha256:" followed by 64 lowercase hexadecimal digits. */
bool ny_sha256_file(const char *path, char *out, size_t out_len);
/* Writes the 64 hexadecimal digits without the prefix. */
bool ny_sha256_file_hex(const char *path, char out[65]);
