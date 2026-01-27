use std.core
use std.io
use std.core.error

;; comptime (Test)

def v1 = comptime { return 1 + 2 + 3 }
assert(v1 == 6, "comptime basic")

def v2 = comptime { def x = 10 }
assert(v2 == 0, "comptime fallthrough")

def v3 = comptime {
   def sum = 0
   def i = 0
   while(i < 5){
      sum = sum + i
      i = i + 1
   }
   if(sum == 10){ return sum }
   return 0
}
assert(v3 == 10, "comptime control flow")

def v4 = comptime {
   def inner = comptime { return 5 }
   return inner * 2
}
assert(v4 == 10, "comptime nested")

print("✓ comptime tests passed")
