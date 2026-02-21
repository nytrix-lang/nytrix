;; Keywords: str bytes
;; Bytes module.

module std.str.bytes (
   bytes, bytes_set, bytes_get, bytes_len
)
use std.core *

fn bytes(n){
   "Allocates a bytes buffer of length `n`."
   if(n < 0){ return 0 }
   def p = malloc(n)
   if(!p){ return 0 }
   store64(p, 122, -8)
   store64(p, n, -16)
   p
}

fn bytes_set(b, i, v){
   "Stores byte `v` at index `i`."
   if(!is_bytes(b)){ return 0 }
   if(!is_int(i)){ return 0 }
   def n = bytes_len(b)
   if(i < 0 || i >= n){ return 0 }
   store8(b, v, i)
   b
}

fn bytes_get(b, i){
   "Returns byte at index `i`."
   if(!is_bytes(b)){ return 0 }
   if(!is_int(i)){ return 0 }
   def n = bytes_len(b)
   if(i < 0 || i >= n){ return 0 }
   load8(b, i)
}

fn bytes_len(b){
   "Returns the number of bytes in a bytes buffer."
   if(!b){ return 0 }
   if(!is_bytes(b)){ return 0 }
   load64(b, -16)
}

if(comptime{__main()}){
    use std.core *
    use std.str.bytes *

    def b = bytes(4)
    assert(bytes_len(b) == 4, "bytes_len")
    bytes_set(b, 0, 65)
    bytes_set(b, 1, 66)
    bytes_set(b, 2, 67)
    bytes_set(b, 3, 68)
    assert(bytes_get(b, 0) == 65, "bytes_get 0")
    assert(bytes_get(b, 3) == 68, "bytes_get 3")
    assert(bytes_set(b, -1, 1) == 0, "bytes_set negative index")
    assert(bytes_set(b, 4, 1) == 0, "bytes_set out of range")
    assert(bytes_get(b, -1) == 0, "bytes_get negative index")
    assert(bytes_get(b, 4) == 0, "bytes_get out of range")

    print("✓ std.str.bytes tests passed")
}
