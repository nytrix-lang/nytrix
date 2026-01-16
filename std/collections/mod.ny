;; Keywords: collections mod
;; Collections Mod module.

use std.core
use std.core.reflect
use std.collections.dict
use std.collections.set
use std.collections.queue
use std.collections.heap
module std.collections (
   list, list_len, list_extend, list_clear,
   list_clone, list_reverse, list_sort, list_sorted, list_contains,
   dict, dict_len, dict_get, dict_set, dict_contains, dict_del, dict_items,
   dict_keys, dict_values, dict_clear, dict_copy, dict_update,
   set, set_len, set_contains, set_add, set_remove, set_clear, set_copy, set_union,
   set_intersection, set_difference,
   queue, queue_push, queue_pop, queue_len,
   heap, heap_push, heap_pop, heap_peek,
   get, set_idx, contains, clear, clone, items, keys, values, append, pop
)

fn list_extend(lst, other){
   "Appends all elements from `other` to the list `lst`."
   extend(lst, other)
}

fn list_clear(lst){
   "Removes all elements from the list `lst` in-place."
   if(is_ptr(lst)){
      store64(lst, 0, 0)
   }
   lst
}

fn list_clone(lst){
   "Returns a **shallow copy** of the list `lst`."
   if(lst == 0){ return 0 }
   if(is_list(lst) == false){ return 0 }
   def n = list_len(lst)
   def out = list(n)
   def i = 0
   while(i < n){
       out = append(out, get(lst, i))
      i += 1
   }
   out
}

fn list_reverse(lst){
   "Returns a **new** list containing the elements of `lst` in reverse order."
   def n = list_len(lst)
   def out = list(8)
   def i = n - 1
   while(i >= 0){
       out = append(out, get(lst, i))
      i -= 1
   }
   out
}

fn _list_partition(xs, low, high){
   "Internal: Partition function for the **QuickSort** algorithm."
   def pivot = get(xs, high)
   def i = low - 1
   def j = low
   while(j < high){
      if(get(xs, j) <= pivot){
         i += 1
         def tmp = get(xs, i)
         store_item(xs, i, get(xs, j))
         store_item(xs, j, tmp)
      }
      j += 1
   }
   def tmp2 = get(xs, i + 1)
   store_item(xs, i + 1, get(xs, high))
   store_item(xs, high, tmp2)
   i + 1
}

fn _list_quicksort(xs, low, high){
   "Internal: Recursive **QuickSort** implementation."
   if(low < high){
      def p = _list_partition(xs, low, high)
      _list_quicksort(xs, low, p - 1)
      _list_quicksort(xs, p + 1, high)
   }
   xs
}

fn list_sort(lst){
   "Sorts the list `lst` **in-place** using the QuickSort algorithm."
   def n = list_len(lst)
   if(n < 2){ return lst }
   _list_quicksort(lst, 0, n - 1)
}

fn list_sorted(lst){
   "Returns a **new sorted list** containing the elements of `lst`."
   def out = list_clone(lst)
   list_sort(out)
   out
}

fn list_contains(lst, x){
   "Returns **true** if the element `x` is present in list `lst`."
   def i = 0  def n = list_len(lst)
   while(i < n){
      if(eq(get(lst, i), x)){ return true }
      i += 1
   }
   false
}

fn dict_len(d){
  "Returns the number of entries in dictionary `d`."
  len(d)
}

fn dict_get(d, key, default=0){
   "Returns the value for `key` in `d`, or `default` if missing."
   dict_get(d, key, default)
}

fn dict_set(d, key, val){
   "Sets `d[key]` to `val`."
   dict_set(d, key, val)
}

fn dict_contains(d, key){
   "Returns **true** if `key` is present in dictionary `d`."
   dict_contains(d, key)
}

fn dict_del(d, key){
   "Removes `key` from dictionary `d`."
   dict_del(d, key)
}

fn dict_items(d){
   "Returns a list of `[key, value]` pairs from dictionary `d`."
   dict_items(d)
}

fn dict_keys(d){
   "Returns a list of all keys in dictionary `d`."
   dict_keys(d)
}

fn dict_values(d){
   "Returns a list of all values in dictionary `d`."
   dict_values(d)
}

fn dict_clear(d){
   "Removes all entries from dictionary `d` in-place."
   dict_clear(d)
}

fn dict_copy(d){
   "Returns a **shallow copy** of dictionary `d`."
   dict_copy(d)
}

fn dict_update(d, other){
   "Updates dictionary `d` with entries from `other`."
   dict_update(d, other)
}

fn set_len(s){
  "Returns the number of elements in set `s`."
  len(s)
}

fn set_contains(s, v){
   "Returns **true** if value `v` is present in set `s`."
   set_contains(s, v)
}

fn set_add(s, v){
   "Adds value `v` to set `s`."
   set_add(s, v)
}

fn set_remove(s, v){
   "Removes value `v` from set `s`."
   set_remove(s, v)
}

fn set_clear(s){
   "Removes all elements from set `s` in-place."
   set_clear(s)
}

fn set_copy(s){
   "Returns a **shallow copy** of set `s`."
   set_copy(s)
}

fn set_union(a, b){
   "Returns a **new set** representing the union of sets `a` and `b`."
   set_union(a, b)
}

fn set_intersection(a, b){
   "Returns a **new set** representing the intersection of sets `a` and `b`."
   set_intersection(a, b)
}

fn set_difference(a, b){
   "Returns a **new set** representing the difference of sets `a` and `b` (`a - b`)."
   set_difference(a, b)
}

fn queue_len(q){
   "Returns the number of elements in `q`."
   queue_len(q)
}

fn heap_push(h, v){
   "Pushes value `v` into min-heap `h`."
   heap_push(h, v)
}

fn heap_pop(h){
   "Removes and returns the smallest value from min-heap `h`."
   heap_pop(h)
}

fn heap_peek(h){
   "Returns the smallest value from min-heap `h` without removing it."
   heap_peek(h)
}

; Generic collection helpers

fn contains(x, item){
   "Generic membership test. Supported for sets, dicts, lists, and strings."
   if(!x){ return false }
   case type(x) {
      "set"  -> set_contains(x, item)
      "dict" -> dict_contains(x, item)
      "list" -> list_contains(x, item)
      "str"  -> {
         use std.strings.str
         find(x, item) >= 0
      }
      _      -> false
   }
}

fn clear(x){
   "Clears collection `x` (list, dict, or set) in-place."
   case type(x) {
     "list" -> list_clear(x)
     "dict" -> dict_clear(x)
     "set"  -> set_clear(x)
     _      -> x
   }
}

fn clone(x){
   "Returns a **shallow clone** of collection `x`."
   case type(x) {
     "list" -> list_clone(x)
     "dict" -> dict_copy(x)
     "set"  -> set_copy(x)
     _      -> x
   }
}

fn items(x){
   "Generic item iterator. Returns a list of `[index/key, value]` pairs."
   case type(x) {
      "dict" -> dict_items(x)
      "set"  -> {
         def its = dict_items(x)
         def out = list(8)
         def i = 0  def n = list_len(its)
         while(i < n){
             out = append(out, get(get(its, i), 0))
            i += 1
         }
         out
      }
      "list", "tuple", "str" -> {
         def out = list(8)
         def n = len(x)
         def i = 0
         while(i < n){
             out = append(out, [i, get(x, i)])
            i += 1
         }
         out
      }
      _ -> list(0)
   }
}

fn keys(x){
   "Generic key iterator. Returns keys or indices for the given collection."
   case type(x) {
      "dict" -> dict_keys(x)
      "set"  -> items(x)
      "list", "tuple", "str" -> {
         def out = list(8)
         def n = len(x)
         def i = 0
         while(i < n){  out = append(out, i)  i += 1 }
         out
      }
      _ -> list(0)
   }
}

fn values(x){
   "Generic value iterator for all collection types."
   case type(x) {
      "dict" -> dict_values(x)
      "set"  -> items(x)
      "list", "tuple", "str" -> {
         def out = list(8)
         def n = len(x)
         def i = 0
         while(i < n){  out = append(out, get(x, i))  i += 1 }
         out
      }
      _ -> list(0)
   }
}