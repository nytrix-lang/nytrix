;; Keywords: stat statistics probability
;; Math Stat for Nytrix
module std.math.crypto.stat(sum, mean, median)
use std.core

fn sum(list: xs): any {
   "Returns the sum of all elements in list `xs`."
   mut acc, i, n = 0, 0, xs.len
   while(i < n){ acc = acc + xs.get(i)  i += 1  }
   return acc
}

fn mean(list: xs): any {
   "Returns the arithmetic mean(average) of the elements in list `xs`. Returns 0 if the list is empty."
   mut n = xs.len
   if(n == 0){ return 0 }
   return sum(xs) / n
}

fn median(list: xs): any {
   "Returns the median value of list `xs`. For even-sized lists, returns the average of the two middle elements."
   mut n = xs.len
   if(n == 0){ return 0 }
   def tmp = clone(xs)
   sort(tmp)
   def mid = n / 2
   if((n % 2) == 1){ return tmp.get(mid) }
   return(tmp.get(mid - 1) + tmp.get(mid)) / 2
}
