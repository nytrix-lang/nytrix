use std.core
use std.core.error
use std.core.reflect
use std.core.dict
use std.core.io
use std.core.str

;; Loop strict syntax (Test)
mut i = 0
while i < 3 {
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
for x in nums {
   if seen.contains(2020 - x) {
      pair_sum = x * (2020 - x)
      break
   }
   add(seen, x)
}

assert(pair_sum == 514579, "indexed for loop with mutating add")
mut loop = 0
while mut i = 0 i < 10 ++i {
   loop = i
}

assert(loop == 9, "loop sequence failed")
print("✓ Loop strict syntax tests passed")

;; REPL migration: indexed for
fn main() {
   mut list_seen = []
   def fruits = [1, 2, 3, 4]
   for fruit, i in fruits {
      list_seen = list_seen.append(f"{fruit}:{i}")
   }
   assert(list_seen == ["1:0", "2:1", "3:2", "4:3"], "REPL indexed list for")
   mut str_seen = []
   for x, i in "test" {
      str_seen = str_seen.append(f"{x} iter is {i}")
   }
   assert(str_seen == ["t iter is 0", "e iter is 1", "s iter is 2", "t iter is 3"], "REPL indexed string for")
}
main()
