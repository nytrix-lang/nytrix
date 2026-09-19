;; repl-expect: REPL-NATIVE-LIST-OK
use std.core

fn op(a, b) { a + 1 / b }
for a in 1..5 {
   for b in 1..5 {
      def ignored = op(a, b)
   }
}

assert([1, 2, 3][0] == 1 && [1, 2, 3][1] == 2 && [1, 2, 3][2] == 3,
   "interactive native list indexing")
assert(to_str([1, 2, 3]) == "[1, 2, 3]", "interactive list to_str")
assert(to_str([1,2,3,4,5].map(fn(x) { x * 2 })) == "[2, 4, 6, 8, 10]",
   "interactive numeric map")
assert(to_str([1,2,3,4,5].map(fn(x) { to_str(x) })) == "[1, 2, 3, 4, 5]",
   "interactive string map")
print("REPL-NATIVE-LIST-OK")
