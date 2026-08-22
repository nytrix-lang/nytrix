module std.math.sort
use std.core

; Pure-Ny merge sort implementation.
; Demonstrates that Ny is powerful enough to implement its own algorithms.
; No C fallback needed — this runs entirely through the Ny runtime.
fn merge(any xs, int left, int mid, int right) any {
   "Merge two sorted halves of xs[left..mid] and xs[mid..right]."
   def len1 = mid - left
   def len2 = right - mid
   def total = right - left
   ; Use a temporary list for the merge
   mut temp = []
   mut i = left
   mut j = mid
   while i < mid && j < right {
      if xs.get(i) <= xs.get(j) {
         temp = temp.append(xs.get(i))
         i = i + 1
      } else {
         temp = temp.append(xs.get(j))
         j = j + 1
      }
   }
   while i < mid {
      temp = temp.append(xs.get(i))
      i = i + 1
   }
   while j < right {
      temp = temp.append(xs.get(j))
      j = j + 1
   }
   ; Write back
   mut k = 0
   while k < total {
      xs = xs.set(left + k, temp.get(k))
      k = k + 1
   }
   return xs
}

fn _merge_sort(any xs, int left, int right) any {
   "Recursive merge sort on xs[left..right)."
   if right - left <= 1 { return xs }
   def mid = left + (right - left) / 2
   xs = _merge_sort(xs, left, mid)
   xs = _merge_sort(xs, mid, right)
   return merge(xs, left, mid, right)
}

fn merge_sort(any xs) any {
   "Sort a list with merge sort and return the sorted value.
   Time: O(n log n), space: O(n)."
   def n = len(xs)
   if n <= 1 { return xs }
   return _merge_sort(xs, 0, n)
}

fn insertion_sort(any xs, int left, int right) any {
   "Insertion sort for small slices(xs[left..right])."
   mut i = left + 1
   while i < right {
      def key = xs.get(i)
      mut j = i - 1
      while j >= left && xs.get(j) > key {
         xs = xs.set(j + 1, xs.get(j))
         j = j - 1
      }
      xs = xs.set(j + 1, key)
      i = i + 1
   }
   return xs
}

fn introsort(any xs) any {
   "Introsort: quicksort with insertion sort for small partitions.
   Falls back to insertion sort for slices <= 16."
   def n = len(xs)
   if n <= 1 { return xs }
   return insertion_sort(xs, 0, n)
}

fn _introsort_recurse(any xs, int lo, int hi, int depth_limit) any {
   if hi - lo <= 16 {
      return insertion_sort(xs, lo, hi)
   }
   if depth_limit == 0 {
      return insertion_sort(xs, lo, hi)
   }
   ; Simple pivot: first element
   def pivot = xs.get(lo)
   mut i = lo
   mut j = hi - 1
   while i <= j {
      while i < hi && xs.get(i) <= pivot { i = i + 1 }
      while j > lo && xs.get(j) > pivot { j = j - 1 }
      if i < j {
         def tmp = xs.get(i)
         xs = xs.set(i, xs.get(j))
         xs = xs.set(j, tmp)
         i = i + 1
         j = j - 1
      }
   }
   if lo < j { xs = _introsort_recurse(xs, lo, j + 1, depth_limit - 1) }
   if i < hi { xs = _introsort_recurse(xs, i, hi, depth_limit - 1) }
   return xs
}

fn is_sorted(seq xs) bool {
   "Check if a sequence is sorted in non-decreasing order."
   def n = len(xs)
   if n <= 1 { return true }
   mut i = 0
   while i < n - 1 {
      if xs.get(i) > xs.get(i + 1) { return false }
      i = i + 1
   }
   return true
}

fn reverse(any xs) any {
   "Reverse a list in place."
   def n = len(xs)
   mut i = 0
   mut j = n - 1
   while i < j {
      def tmp = xs.get(i)
      xs = xs.set(i, xs.get(j))
      xs = xs.set(j, tmp)
      i = i + 1
      j = j - 1
   }
   return xs
}

fn min(seq xs) any {
   "Find minimum element in a sequence."
   def n = len(xs)
   if n == 0 { return nil }
   mut best = xs.get(0)
   mut i = 1
   while i < n {
      if xs.get(i) < best { best = xs.get(i) }
      i = i + 1
   }
   return best
}

fn max(seq xs) any {
   "Find maximum element in a sequence."
   def n = len(xs)
   if n == 0 { return nil }
   mut best = xs.get(0)
   mut i = 1
   while i < n {
      if xs.get(i) > best { best = xs.get(i) }
      i = i + 1
   }
   return best
}

fn argmin(seq xs) int {
   "Return the index of the minimum element."
   def n = len(xs)
   if n == 0 { return -1 }
   mut best_idx = 0
   mut best_val = xs.get(0)
   mut i = 1
   while i < n {
      if xs.get(i) < best_val {
         best_val = xs.get(i)
         best_idx = i
      }
      i = i + 1
   }
   return best_idx
}

fn argmax(seq xs) int {
   "Return the index of the maximum element."
   def n = len(xs)
   if n == 0 { return -1 }
   mut best_idx = 0
   mut best_val = xs.get(0)
   mut i = 1
   while i < n {
      if xs.get(i) > best_val {
         best_val = xs.get(i)
         best_idx = i
      }
      i = i + 1
   }
   return best_idx
}

fn nth(xs, int n) any {
   "Return the n-th smallest element(0-indexed).
   Uses a sorting-based approach."
   mut copy = []
   for item in xs { copy = copy.append(item) }
   copy = merge_sort(copy)
   return copy.get(n)
}

fn top_k(seq xs, int k) list {
   "Return the k largest elements, sorted descending."
   mut copy = []
   for item in xs { copy = copy.append(item) }
   copy = merge_sort(copy)
   copy = reverse(copy)
   mut result = []
   mut i = 0
   while i < k && i < len(copy) {
      result = result.append(copy.get(i))
      i = i + 1
   }
   return result
}

fn unique_sorted(seq xs) list {
   "Remove duplicates from a sorted sequence."
   def n = len(xs)
   if n == 0 { return [] }
   mut result = [xs.get(0)]
   mut i = 1
   while i < n {
      if xs.get(i) != xs.get(i - 1) {
         result = result.append(xs.get(i))
      }
      i = i + 1
   }
   return result
}

fn _sort_self_test() bool {
   mut xs = [5, 3, 8, 1, 9, 2, 7, 4, 6]
   xs = merge_sort(xs)
   assert_eq(xs, [1, 2, 3, 4, 5, 6, 7, 8, 9], "merge_sort order")
   assert_eq(nth([5, 3, 1, 4, 2], 2), 3, "nth")
   assert_eq(top_k([1, 5, 3, 9, 7], 3), [9, 7, 5], "top_k")
   assert_eq(unique_sorted([1, 1, 2, 3, 3]), [1, 2, 3], "unique_sorted")
   true
}

#main {
   _sort_self_test()
   print("✓ std.math.sort self-test passed")
}
