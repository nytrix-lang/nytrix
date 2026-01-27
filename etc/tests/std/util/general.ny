;; std.util.general (Test)
;; Tests general utility functions including UUIDv4.

use std.util.general *
use std.core.dict *
use std.core.set *
use std.core.reflect *
use std.core.test *

print("Testing std.util.general (UUID & Extras)...")

fn test_uuid_properties(){
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
