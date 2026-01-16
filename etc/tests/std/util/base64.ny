use std.io
use std.util.base64
use std.core
use std.core.reflect
use std.strings.str

fn test_b64(){
   def s = "hello"
   def enc = b64_encode(s)
   print("Encoded:", enc)
   assert(_str_eq(enc, "aGVsbG8="), "b64_encode 'hello'")
   def dec = b64_decode(enc)
   print("Decoded:", dec)
   assert(_str_eq(dec, s), "b64_decode 'hello'")
   def s2 = "Nytrix is awesome!"
   def enc2 = b64_encode(s2)
   def dec2 = b64_decode(enc2)
   assert(_str_eq(dec2, s2), "roundtrip longer string")
}

test_b64()
print("✓ std.util.base64 tests passed")
