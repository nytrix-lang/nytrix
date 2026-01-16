use std.io
use std.strings.bytes
use std.core.test

fn test_bytes_extra(){
   print("Testing std.strings.bytes (extra)...")
   def b = bytes_from_str("abc")
   assert(bytes_len(b) == 3, "bytes_len")
   assert(bytes_get(b, 0) == 97, "bytes_get 0")
   bytes_set(b, 1, 120) ; 'x'
   assert(bytes_get(b, 0) == 97, "bytes_set a")
   assert(bytes_get(b, 1) == 120, "bytes_set x")
   assert(bytes_get(b, 2) == 99, "bytes_set c")
   def s = bytes_slice(b, 1, 3)
   assert(bytes_len(s) == 2, "bytes_slice len")
   assert(bytes_get(s, 0) == 120, "bytes_slice x")
   assert(bytes_get(s, 1) == 99, "bytes_slice c")
   def c = bytes_concat(bytes_from_str("hi"), bytes_from_str("!"))
   assert(bytes_len(c) == 3, "bytes_concat len")
   assert(bytes_get(c, 0) == 104, "bytes_concat h")
   assert(bytes_get(c, 1) == 105, "bytes_concat i")
   assert(bytes_get(c, 2) == 33, "bytes_concat !")
   def hx = hex_encode(bytes_from_str("hi"))
   assert(bytes_len(hx) == 4, "hex encode len")
   assert(bytes_get(hx, 0) == 54, "hex encode '6'")
   assert(bytes_get(hx, 1) == 56, "hex encode '8'")
   assert(bytes_get(hx, 2) == 54, "hex encode '6'")
   assert(bytes_get(hx, 3) == 57, "hex encode '9'")
   def back = hex_decode("6869")
   assert(bytes_len(back) == 2, "hex decode len")
   assert(bytes_get(back, 0) == 104, "hex decode h")
   assert(bytes_get(back, 1) == 105, "hex decode i")
   print("✓ std.strings.bytes extra passed")
}

test_bytes_extra()
