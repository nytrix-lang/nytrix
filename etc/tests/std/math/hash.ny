use std.math.hash as hash
use std.str *

;; std.math.hash (Test)
;; Tests hash functions.

def s = "123456789"

;; CRC32: 0xCBF43926 -> 3421780262
;; CRC32: 0xCBF43926 -> 3421780262
def c = hash.crc32(s, 0, 0)
assert(c == 3421780262, "crc32")

;; Adler32: 152961502
def a = hash.adler32(s, 0, 0)
assert(a == 152961502, "adler32")

;; XXH32: 2474356071
def x = hash.xxh32(s, 0, 0, 0)
assert(x == 2474356071, "xxh32")

;; MD5('123456789'): 25f9e794323b453885f5181f1b624d0b
def m = hash.md5(s, 0, 0)
assert(eq(m, "25f9e794323b453885f5181f1b624d0b"), "md5")

;; SHA1('123456789'): d2032181892c6c0a4597019109faaaf6224f771d
def s1 = hash.sha1(s, 0, 0)
assert(eq(s1, "d2032181892c6c0a4597019109faaaf6224f771d"), "sha1")

print("✓ std.math.hash tests passed")


