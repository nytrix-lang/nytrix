use std.core

def src = "hello"
def bound = __zlib_bound(src.len)
assert(bound > src.len, "__zlib_bound returns usable capacity")

def dest = malloc(bound)
def dest_len = malloc(8)
store64_i(dest_len, bound, 0)
assert(__zlib_compress(dest, dest_len, src, src.len, 6) == 0, "__zlib_compress succeeds")
assert(load64_i(dest_len, 0) > 0, "__zlib_compress writes output length")
assert(bound > load64_i(dest_len, 0) - 1, "__zlib_compress respects bound")

free(dest, dest_len)

print("✓ runtime compression tests passed")
