use std.io
; Test std.collections.mod - Collection operations
use std.collections
use std.core.error

fn test_dict_ops(){
   print("Testing dict ops...")
   def d = dict(8)
   assert(is_dict(d), "dict creation")
   d = dict_set(d, "name", "Nytrix")
   d = dict_set(d, "version", "0.1")
   d = dict_set(d, "year", 2026)
   assert(eq(dict_get(d, "name", 0), "Nytrix"), "get string value")
   assert(dict_get(d, "year", 0) == 2026, "get int value")
   assert(contains(d, "name"), "dict has key")
   assert(!contains(d, "missing"), "dict doesn't have key")
   def ks = keys(d)
   assert(list_len(ks) == 3, "keys returns all keys")
   def vs = values(d)
   assert(list_len(vs) == 3, "values returns all values")
   print("Dict ops passed")
}

fn test_set_ops(){
   print("Testing set ops...")
   def s = set()
   assert(is_set(s), "set creation")
   s = set_add(s, 1)
   s = set_add(s, 2)
   s = set_add(s, 3)
   s = set_add(s, 2)
   assert(set_contains(s, 1), "set contains 1")
   assert(set_contains(s, 2), "set contains 2")
   assert(!set_contains(s, 10), "set doesn't contain 10")
   s = set_remove(s, 2)
   assert(!set_contains(s, 2), "element removed")
   print("Set ops passed")
}

fn test_list_helpers(){
   print("Testing list helpers...")
   def lst = [3, 1, 4, 1, 5, 9, 2, 6]
   assert(list_contains(lst, 4), "list contains element")
   assert(!list_contains(lst, 10), "list doesn't contain element")
   def rev = list_reverse(lst)
   assert(get(rev, 0) == 6, "reversed first element")
   assert(get(rev, -1) == 3, "reversed last element")
   print("List helpers passed")
}

fn test_sorted(){
   print("Testing sorted...")
   def lst = [3, 1, 4, 1, 5, 9, 2, 6]
   def sorted_lst = sorted(lst)
   assert(get(sorted_lst, 0) == 1, "sorted first element")
   assert(get(sorted_lst, -1) == 9, "sorted last element")
   def i = 0
   while(i < list_len(sorted_lst) - 1){
      assert(get(sorted_lst, i) <= get(sorted_lst, i + 1), "ascending order")
      i = i + 1
   }
   print("Sorted passed")
}

fn test_main(){
   test_dict_ops()
   test_set_ops()
   test_list_helpers()
   test_sorted()
   print("✓ std.collections.mod tests passed")
}

test_main()
