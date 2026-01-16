use std.io
use std.collections.set
use std.collections.dict
use std.core.error

fn test_basic(){
   print("Testing set basic operations...")
   def s = set()
   s = set_add(s, 10)
   s = set_add(s, 20)
   assert(set_contains(s, 10), "Contains 10")
   assert(set_contains(s, 20), "Contains 20")
   assert(!set_contains(s, 30), "Does not contain 30")
   s = set_remove(s, 10)
   assert(!set_contains(s, 10), "Removed 10")
   assert(set_contains(s, 20), "Still has 20")
   print("Basic operations passed")
}

fn test_ops(){
   print("Testing set union/intersection/difference...")
   def s1 = set()
   set_add(s1, 1)
   set_add(s1, 2)
   def s2 = set()
   set_add(s2, 2)
   set_add(s2, 3)
   def u = set_union(s1, s2)
   assert(set_contains(u, 1), "Union has 1")
   assert(set_contains(u, 2), "Union has 2")
   assert(set_contains(u, 3), "Union has 3")
   def i = set_intersection(s1, s2)
   assert(!set_contains(i, 1), "Intersection no 1")
   assert(set_contains(i, 2), "Intersection has 2")
   assert(!set_contains(i, 3), "Intersection no 3")
   def d = set_difference(s1, s2)
   assert(set_contains(d, 1), "Diff has 1")
   assert(!set_contains(d, 2), "Diff no 2")
   assert(!set_contains(d, 3), "Diff no 3")
   print("Set ops passed")
}

fn test_mixed_types(){
   print("Testing mixed types in set...")
   def s = set()
   set_add(s, 100)
   set_add(s, "str")
   assert(set_contains(s, 100), "Contains int")
   assert(set_contains(s, "str"), "Contains str")
   assert(!set_contains(s, 101), "No int")
   assert(!set_contains(s, "other"), "No str")
   print("Mixed types passed")
}

fn test_stress_cycle(){
   print("Testing set stress cycle...")
   def s = set()
   def i = 0
   while(i < 50){
      s = set_add(s, i)
      i = i + 1
   }
   i = 0
   while(i < 50){
      assert(set_contains(s, i), "Contains value")
      i = i + 1
   }
   i = 0
   while(i < 50){
      s = set_remove(s, i)
      i = i + 1
   }
   i = 0
   while(i < 50){
      assert(!set_contains(s, i), "Removed value")
      i = i + 1
   }
   print("Stress cycle passed")
}

fn test_clear(){
   print("Testing set clear...")
   def s = set()
   set_add(s, 1)
   set_add(s, 2)
   set_clear(s)
   assert(!set_contains(s, 1), "Cleared 1")
   assert(!set_contains(s, 2), "Cleared 2")
   set_add(s, 3)
   assert(set_contains(s, 3), "Re-used after clear")
   print("Clear passed")
}

fn test_methods(){
   print("Testing set method aliases...")
   def s = set()
   set_add(s, 10)
   assert(set_contains(s, 10), "add")
   set_remove(s, 10)
   assert(!set_contains(s, 10), "remove")
   set_add(s, 20)
   def s2 = set_copy(s)
   assert(set_contains(s2, 20), "alias copy")
   set_clear(s)
   assert(!set_contains(s, 20), "alias clear")
   print("Method aliases passed")
}

fn run_set_tests(){
   test_basic()
   test_ops()
   test_mixed_types()
   test_stress_cycle()
   test_clear()
   test_methods()
   print("✓ std.collections.set passed")
}

run_set_tests()
