use std.core *
use std.core.error *
use std.core.reflect *
use std.core.list *
use std.core.dict *
use std.str.io *
use std.str *

;; comptime (Test)

mut v1 = comptime { return 1 + 2 + 3 }
assert(v1 == 6, "comptime basic")

mut v2 = comptime { def x = 10 }
assert(v2 == 0, "comptime fallthrough")

mut v3 = comptime {
   mut sum = 0
   mut i = 0
   while(i < 5){
      sum = sum + i
      i = i + 1
   }
   if(sum == 10){ return sum }
   return 0
}
assert(v3 == 10, "comptime control flow")

mut v4 = comptime {
   def inner = comptime { return 5 }
   return inner * 2
}
assert(v4 == 10, "comptime nested")

print("✓ comptime tests passed")

