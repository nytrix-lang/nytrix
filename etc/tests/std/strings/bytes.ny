use std.io
use std.strings.bytes
use std.core.test
use std.core
use std.strings.str

fn test_bytes(){
   print("Testing Strings Bytes...")
   def b = bytes_from_str("hello")
   assert(is_bytes(b), "is_bytes")
   assert(is_str(b) == false, "bytes not str")
   assert(bytes_len(b) == 5, "bytes_len")
   assert(bytes_get(b, 0) == 104, "bytes_get 'h'")
   bytes_set(b, 0, 97)
   assert(bytes_get(b, 0) == 97, "bytes_set 'a'")
   assert(bytes_eq(b, bytes_from_str("aello")), "cmp bytes")
   def sub = bytes_slice(b, 1, 3)
   assert(bytes_eq(sub, bytes_from_str("el")), "bytes_slice")
   def h = hex_encode(bytes_from_str("abc"))
   assert(bytes_eq(h, bytes_from_str("616263")), "hex_encode")
   def decoded = hex_decode("616263")
   assert(bytes_eq(decoded, bytes_from_str("abc")), "hex_decode")
   print("✓ std.strings.bytes passed")
}

test_bytes()
