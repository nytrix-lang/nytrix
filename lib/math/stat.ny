;; Keywords: math stat
;; Math Stat module.

module std.math.stat (
   sum, mean, median
)
use std.core *
use std.core *

fn sum(xs){
   "Returns the sum of all elements in list `xs`."
   mut acc = 0  mut i = 0  mut n = len(xs)
   while(i < n){ acc = acc + get(xs, i)  i += 1  }
   return acc
}

fn mean(xs){
   "Returns the arithmetic mean (average) of the elements in list `xs`. Returns 0 if the list is empty."
   mut n = len(xs)
   if(n == 0){ return 0 }
   return sum(xs) / n
}

fn median(xs){
   "Returns the median value of list `xs`. For even-sized lists, returns the average of the two middle elements."
   mut n = len(xs)
   if(n == 0){ return 0 }
   def tmp = list_clone(xs)
   sort(tmp)
   def mid = n / 2
   if((n % 2) == 1){ return get(tmp, mid) }
   return (get(tmp, mid - 1) + get(tmp, mid)) / 2
}

if(comptime{__main()}){
   use std.math.stat *
   use std.core.error *

   print("Testing Math Stat...")

   def xs = [1, 2, 3, 4, 5]
   assert(mean(xs) == 3, "mean odd")
   assert(sum(xs) == 15, "sum odd")
   assert(median(xs) == 3, "median odd")

   def ys = [1, 2, 3, 4]
   assert(mean(ys) == 2, "mean even int")
   assert(median(ys) == 2, "median even")

   print("✓ std.math.stat tests passed")
}
