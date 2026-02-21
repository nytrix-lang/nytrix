;; Keywords: util general
;; General utility module.

module std.util.general (
   uuid
)
use std.core *
use std.math.random *
use std.str.io *

def _HEX_LOWER = "0123456789abcdef"

fn _hex_digit(n){
   "Internal: returns lowercase hex character code for nibble `n`."
   return load8(_HEX_LOWER, n)
}

fn uuid(){
   "Generates a random Version 4 UUID string (e.g., 'f47ac10b-58cc-4372-a567-0e02b2c3d479')."
   def out = malloc(64)
   init_str(out, 36)
   mut i=0  mut o=0
   mut b=0
   while(i<16){
      b = rand() % 256
      if(i==6){ b = (b % 16) + 64  }
      if(i==8){ b = (b % 64) + 128  }
      store8(out, _hex_digit((b / 16) % 16), o)
      store8(out, _hex_digit(b % 16), o + 1)
      o = o + 2
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
   return out
}

if(comptime{__main()}){
    use std.util.general *
    use std.core *
    use std.core.dict *
    use std.core.set *
    use std.core.reflect *
    use std.core.test *
    use std.str *

    print("Testing std.util.general (UUID & Extras)...")

    fn test_uuid_properties(){
       "Runs test_uuid_properties test."
       print("Checking UUIDv4 format...")
       def u = uuid()
       assert(str_len(u) == 36, "uuid length")
       ; Format: xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx
       assert(load8(u, 8) == 45, "hyphen 1")
       assert(load8(u, 13) == 45, "hyphen 2")
       assert(load8(u, 18) == 45, "hyphen 3")
       assert(load8(u, 23) == 45, "hyphen 4")
       assert(load8(u, 14) == 52, "version 4 digit")
       mut y = load8(u, 19)
       def ok = (y == 56 || y == 57 || y == 97 || y == 98)
       assert(ok, "variant 10xx digit")
       print("UUID format passed")
    }

    fn test_uuid_uniqueness(){
       "Runs test_uuid_uniqueness test."
       print("Checking UUID uniqueness (50 samples)...")
       mut s = set()
       mut i = 0
       while(i < 50){
          def u = uuid()
          if(set_contains(s, u)){
             print("Collision on: ", u)
             panic("UUID collision! (Probability is tiny, check RNG)")
          }
          s = set_add(s, u)
          i = i + 1
       }
       print("UUID uniqueness passed")
    }

    test_uuid_properties()
    test_uuid_uniqueness()

    print("✓ std.util.general tests passed")
}
