use std.core
use std.core.error
use std.core.reflect
use std.core.dict
use std.core.io
use std.core.str

;; Loop strict syntax (Test)
mut i = 0
while(i < 3){
   i += 1
}

assert(i == 3, "while strict")
mut sum = 0
def list_vals = [1, 2, 3]
for x in list_vals {
   sum = sum + x
}

assert(sum == 6, "for strict")
mut pair_sum = 0
def nums = [1721, 979, 366, 299, 675, 1456]
mut seen = set()
for(i in nums){
   def x = nums[i]
   if(seen.contains(2020 - x)){
      pair_sum = x * (2020 - x)
      break
   }
   add(seen, x)
}

assert(pair_sum == 514579, "indexed for loop with mutating add")
mut loop=0
while(mut i=0 i<10 ++i){
   loop = i
}

assert(loop == 9, "loop sequence failed")
print("✓ Loop strict syntax tests passed")
