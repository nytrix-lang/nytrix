;; repl-expect: REPL_INDEXED_FOR_OK
use std.core

fn main(): int {
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
   print("REPL_INDEXED_" + "FOR_OK")
   return 0
}
