use std.io
use std.core.mem
use std.core

fn test_memcpy(){
   def s = "hello world"
   def n = str_len(s)
   def d = malloc(n + 1)
   __init_str(d, n)
   memcpy(d, s, n)
   store8(d, 0, n)
   assert(_str_eq(d, s), "memcpy works")
   free(d)
}

fn test_memset(){
   def p = malloc(16)
   memset(p, 0, 16)
   assert(load64(p) == 0, "memset works 0-8")
   assert(load64(p, 8) == 0, "memset works 8-16")
   memset(p, 65, 4) ; 'A'
   assert(load8(p) == 65, "memset 8-bit")
   assert(load8(p, 3) == 65, "memset 8-bit end")
   free(p)
}

fn test_memchr(){
   def s = "abcdef"
   def p = memchr(s, 100, 6) ; 'd'
   assert(p == s + 3, "memchr found char")
   def p2 = memchr(s, 122, 6) ; 'z'
   assert(p2 == 0, "memchr not found")
}

fn test_memcmp(){
   assert(memcmp("abc", "abc", 3) == 0, "memcmp equal")
   assert(memcmp("abc", "abd", 3) != 0, "memcmp unequal")
}

test_memcpy()
test_memset()
test_memchr()
test_memcmp()

print("✓ std.core.mem tests passed")
