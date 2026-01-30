use std.core
use std.core.mem *

;; Core Mem (Test)
;; Tests low-level memory operations like memcpy, memset, memchr, and memcmp.

print("Testing memcpy...")
def s = "hello world"
def n = str_len(s)
def d = malloc(n + 1)
store64(d, 241, -8)
store64(d, n, -16)
memcpy(d, s, n)
store8(d, 0, n)
assert(_str_eq(d, s), "memcpy works")
free(d)

print("Testing memset...")
def p = malloc(16)
memset(p, 0, 16)
assert(load64(p) == 0, "memset works 0-8")
assert(load64(p, 8) == 0, "memset works 8-16")
memset(p, 65, 4) ; 'A'
assert(load8(p) == 65, "memset 8-bit")
assert(load8(p, 3) == 65, "memset 8-bit end")
free(p)

print("Testing memchr...")
def s2 = "abcdef"
mut p2 = memchr(s2, 100, 6) ; 'd'
assert(p2 == s2 + 3, "memchr found char")
mut p3 = memchr(s2, 122, 6) ; 'z'
assert(p3 == 0, "memchr not found")

print("Testing memcmp...")
assert(memcmp("abc", "abc", 3) == 0, "memcmp equal")
assert(memcmp("abc", "abd", 3) != 0, "memcmp unequal")

print("✓ std.core.mem tests passed")

