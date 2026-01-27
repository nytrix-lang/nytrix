use std.io
use std.strings.bytes
use std.strings.str
use std.core.error

;; std.strings.bytes (Test)
;; Tests bytes creation, mutation, comparison, slicing, and hex encode/decode.

print("Testing Strings Bytes...")

def b = bytes_from_str("hello")
assert(is_bytes(b), "is_bytes")
assert(is_str(b) == false, "not str")
assert(bytes_len(b) == 5, "bytes_len")
assert(bytes_get(b, 0) == 104, "get h")

bytes_set(b, 0, 97)
assert(bytes_get(b, 0) == 97, "set a")
assert(bytes_eq(b, bytes_from_str("aello")), "bytes_eq")

def sub = bytes_slice(b, 1, 3)
assert(bytes_eq(sub, bytes_from_str("el")), "bytes_slice")

def h = hex_encode(bytes_from_str("abc"))
assert(bytes_eq(h, bytes_from_str("616263")), "hex_encode")

def decoded = hex_decode("616263")
assert(bytes_eq(decoded, bytes_from_str("abc")), "hex_decode")

def c = bytes_concat(bytes_from_str("hi"), bytes_from_str("!"))
assert(bytes_len(c) == 3, "bytes_concat len")
assert(bytes_get(c, 0) == 104, "bytes_concat h")
assert(bytes_get(c, 2) == 33, "bytes_concat !")

print("✓ std.strings.bytes tests passed")
