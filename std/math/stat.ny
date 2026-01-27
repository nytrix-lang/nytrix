;; Keywords: math stat
;; Math Stat module.

use std.core *
use std.core *
module std.math.stat (
   sum, mean, median
)

fn sum(xs){
   "Returns the sum of all elements in list `xs`."
   mut acc = 0  mut i = 0  mut n = list_len(xs)
   while(i < n){ acc = acc + get(xs, i)  i = i + 1  }
   return acc
}

fn mean(xs){
   "Mean of list of numbers."
   mut n = list_len(xs)
   if(n == 0){ return 0 }
   return sum(xs) / n
}

fn median(xs){
   "Median (simple sort copy)."
   mut n = list_len(xs)
   if(n == 0){ return 0 }
   def tmp = list_clone(xs)
   sort(tmp)
   def mid = n / 2
   if((n % 2) == 1){ return get(tmp, mid) }
   return (get(tmp, mid - 1) + get(tmp, mid)) / 2
}

