use std.io
use std.util.msgpack
use std.core
use std.core.test
use std.strings.str

print("Testing Msgpack with Manual Heap Strings...")

fn test_heap_string_encoding(){
   ; Manually construct a string on the heap to ensure it doesn't rely on compiler string literals
   def src_s = "this is a long string that should be encoded with str 8"
   def len = str_len(src_s)
   def s2 = __malloc(len + 1)
   __init_str(s2, len)
   def k=0
   while(k < len){
      __store8_idx(s2, k, load8(src_s, k))
      k = k + 1
   }
   __store8_idx(s2, k, 0)
   def encoded = msgpack_encode(s2)
   ; print("Encoded heap bytes: ", encoded)
   def decoded = msgpack_decode(encoded)
   ; print("Decoded heap str: '", decoded, "'")
   assert(eq(decoded, s2), "Decoded heap string matches original")
   __free(s2)
}

test_heap_string_encoding()

print("✓ std.util.msgpack_heap tests passed")
