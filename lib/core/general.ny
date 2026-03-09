;; Keywords: general utility
;; General utility for Nytrix
module std.core.general(uuid)
use std.core
use std.math.random

def _HEX_LOWER = "0123456789abcdef"

fn _hex_digit(int: n): int { load8(_HEX_LOWER, n) }

fn uuid(): str {
   "Generates a random Version 4 UUID string(e.g., 'f47ac10b-58cc-4372-a567-0e02b2c3d479')."
   def out = malloc(37)
   if(!out){ return "" }
   init_str(out, 36)
   mut i, o = 0, 0
   mut b=0
   while(i<16){
      b = rand() & 255
      if(i == 6){ b = (b & 15) | 64  }
      if(i == 8){ b = (b & 63) | 128  }
      store8(out, _hex_digit((b / 16) % 16), o)
      store8(out, _hex_digit(b % 16), o + 1)
      o += 2
      match i {
         3 -> { store8(out, 45, o) o += 1 }
         5 -> { store8(out, 45, o) o += 1 }
         7 -> { store8(out, 45, o) o += 1 }
         9 -> { store8(out, 45, o) o += 1 }
         _ -> {}
      }
      i += 1
   }
   store8(out, 0, o)
   ; length set at init
   out
}
