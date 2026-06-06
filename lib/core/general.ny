;; Keywords: general utility core
;; General utility for Nytrix
;; References:
;; - std.core
module std.core.general(uuid)
use std.core
use std.math.random as random

def _HEX_LOWER = "0123456789abcdef"

fn _hex_digit(int n) int { load8(_HEX_LOWER, n) }

fn uuid() str {
   "Generates a random Version 4 UUID string(e.g., 'f47ac10b-58cc-4372-a567-0e02b2c3d479')."
   def out = malloc(37)
   if(!out){ return "" }
   init_str(out, 36)
   mut i, o = 0, 0
   mut b=0
   while(i<16){
      b = random.rand() & 255
      if(i == 6){ b = (b & 15) | 64  }
      if(i == 8){ b = (b & 63) | 128  }
      store8(out, _hex_digit((b / 16) % 16), o)
      store8(out, _hex_digit(b % 16), o + 1)
      o += 2
      if(i == 3 || i == 5 || i == 7 || i == 9){
         store8(out, 45, o)
         o += 1
      }
      i += 1
   }
   store8(out, 0, o)
   out
}

#main {
   def u = uuid()
   assert(u.len == 36, "uuid len")
   assert(load8(u, 8) == 45, "uuid hyphen 1")
   assert(load8(u, 13) == 45, "uuid hyphen 2")
   assert(load8(u, 18) == 45, "uuid hyphen 3")
   assert(load8(u, 23) == 45, "uuid hyphen 4")
   assert(load8(u, 14) == 52, "uuid version")
   def variant = load8(u, 19)
   assert(variant == 56 || variant == 57 || variant == 97 || variant == 98, "uuid variant")
   mut int: snapshot_acc = 1
   def int: snapshot_row = snapshot_acc
   snapshot_acc -= snapshot_row
   snapshot_acc -= snapshot_row
   assert(snapshot_acc == -1, "immutable int def snapshots mutable source")
   mut seen, i = set(), 0
   while(i < 16){
      def next = uuid()
      assert(!seen.contains(next), "uuid duplicate in smoke run")
      seen = seen.add(next)
      i += 1
   }
   print("✓ std.core.general self-test passed")
}
