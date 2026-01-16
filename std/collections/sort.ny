;; Keywords: collections sort
;; Collections Sort module.

use std.core
module std.collections.sort (
   _partition, _quicksort, sort, sorted
)

fn _partition(xs, low, high){
   "Internal: partition list for quicksort, return pivot index."
   def pivot = get(xs, high)
   def i = low - 1
   def j = low
   while(j < high){
      if(get(xs, j) <= pivot){
         i = i + 1
         def tmp = get(xs, i)
         store_item(xs, i, get(xs, j))
         store_item(xs, j, tmp)
      }
      j = j + 1
   }
   def tmp2 = get(xs, i + 1)
   store_item(xs, i + 1, get(xs, high))
   store_item(xs, high, tmp2)
   return i + 1
}

fn _quicksort(xs, low, high){
   "Internal: in-place quicksort over [low, high]."
   if(low < high){
      def p = _partition(xs, low, high)
      _quicksort(xs, low, p - 1)
      _quicksort(xs, p + 1, high)
   }
   return xs
}

fn sort(xs){
   "Sorts the list `xs` in-place using QuickSort."
   def n = list_len(xs)
   if(n < 2){ return xs }
   return _quicksort(xs, 0, n - 1)
}

fn sorted(xs){
   "Return a new sorted list containing the elements of `xs`."
   def out = list_clone(xs)
   return sort(out)
}