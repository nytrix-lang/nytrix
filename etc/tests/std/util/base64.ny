use std.io
use std.util.base64
use std.strings.str
use std.core.error

;; std.util.base64 (Test)
;; Tests base64 encode/decode roundtrip.

print("Testing base64...")

def s = "hello"
def enc = b64_encode(s)
assert(_str_eq(enc, "aGVsbG8="), "b64 encode hello")
def dec = b64_decode(enc)
assert(_str_eq(dec, s), "b64 decode hello")

def s2 = "Nytrix is awesome!"
def enc2 = b64_encode(s2)
def dec2 = b64_decode(enc2)
assert(_str_eq(dec2, s2), "b64 roundtrip")

print("✓ std.util.base64 tests passed")
