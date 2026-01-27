use std.io
use std.util.uuid
use std.strings.str
use std.core.error
use std.collections.set

;; std.util.uuid (Test)
;; Tests UUIDv4 structure and contents.

print("Testing uuid...")

def j = 0
while(j < 10){
   def u = uuid4()
   def i = 0
   while(i < 36){
      if(load8(u, i) == 0){ panic("zero byte in uuid") }
      i = i + 1
   }
   assert(str_len(u) == 36, "len 36")
   assert(load8(u, 8) == 45, "dash 1")
   assert(load8(u, 13) == 45, "dash 2")
   assert(load8(u, 18) == 45, "dash 3")
   assert(load8(u, 23) == 45, "dash 4")
   assert(load8(u, 14) == 52, "version 4")
   def v = load8(u, 19)
   assert(v == 56 || v == 57 || v == 97 || v == 98, "variant")
   j = j + 1
}

def s = set()
def k = 0
while(k < 50){
   def x = uuid4()
   if(set_contains(s, x)){
      panic("UUID collision")
   }
   set_add(s, x)
   k = k + 1
}

print("✓ std.util.uuid tests passed")
