#pragma once

#include <stdbool.h>
#include <stddef.h>

/* Writes "sha256:" followed by 64 lowercase hexadecimal digits. */
bool ny_sha256_file(const char *path, char *out, size_t out_len);
