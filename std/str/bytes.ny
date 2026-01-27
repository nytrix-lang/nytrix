;; Keywords: str bytes
;; Bytes module.

use std.core *
module std.str.bytes (
   bytes, bytes_set, bytes_get, bytes_len
)

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
   if(!is_ptr(b)){ return 0 }
   store8(b, v, i)
   b
}

fn bytes_get(b, i){
   "Returns byte at index `i`."
   if(!is_ptr(b)){ return 0 }
   load8(b, i)
}

fn bytes_len(b){
   "Returns the number of bytes in a bytes buffer."
   if(!b){ return 0 }
   if(!is_ptr(b)){ return 0 }
   load64(b, -16)
}
