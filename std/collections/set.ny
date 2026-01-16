;; Keywords: collections set
;; Collections Set module.

use std.core
use std.core.reflect
use std.collections.dict
module std.collections.set (
   set, set_add, set_contains, set_remove, set_clear, set_copy, set_union, set_intersection,
   set_difference
)

fn set(){
   "Creates a new empty **set**."
   def d = dict(16)
   store64(d, 102, -8)
   d
}

fn set_add(s, v){
   "Adds value `v` to set `s`."
   dict_set(s, v, 1)
}

fn set_contains(s, v){
   "Returns **true** if value `v` is present in set `s`."
   !!dict_contains(s, v)
}

fn set_remove(s, v){
   "Removes value `v` from set `s`."
   dict_del(s, v)
}

fn set_clear(s){
   "Removes all elements from set `s`."
   dict_clear(s)
}

fn set_copy(s){
   "Returns a **shallow copy** of set `s`."
   def out = set()
   def its = dict_items(s)
   def i = 0  def n = list_len(its)
   while(i < n){
      set_add(out, get(get(its, i), 0))
      i += 1
   }
   out
}

fn set_union(a, b){
   "Returns a **new set** representing the union of sets `a` and `b`."
   def out = set_copy(a)
   def its = dict_items(b)
   def i = 0  def n = list_len(its)
   while(i < n){
      set_add(out, get(get(its, i), 0))
      i += 1
   }
   out
}

fn set_intersection(a, b){
   "Returns a **new set** representing the intersection of sets `a` and `b`."
   def out = set()
   def its = dict_items(a)
   def i = 0  def n = list_len(its)
   while(i < n){
      def v = get(get(its, i), 0)
      if(set_contains(b, v)){ set_add(out, v) }
      i += 1
   }
   out
}

fn set_difference(a, b){
   "Returns a **new set** representing the difference of sets `a` and `b` (`a - b`)."
   def out = set()
   def its = dict_items(a)
   def i = 0  def n = list_len(its)
   while(i < n){
      def v = get(get(its, i), 0)
      if(set_contains(b, v) == false){ set_add(out, v) }
      i += 1
   }
   out
}