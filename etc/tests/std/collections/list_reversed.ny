use std.io
use std.collections
use std.core.error

fn test_list_reversed(){
   print("Testing list_reverse...")
   def l = [1, 2, 3, 4, 5]
   def rev = list_reverse(l)
   assert(list_len(rev) == 5, "Reversed len 5")
   assert(get(rev, 0) == 5, "rev[0] == 5")
   assert(get(rev, 4) == 1, "rev[4] == 1")
   ; Test empty
   assert(list_len(list_reverse(list())) == 0, "Empty list reversed")
   ; Test single
   l = [99]
   rev = list_reverse(l)
   assert(list_len(rev) == 1, "Single len 1")
   assert(get(rev, 0) == 99, "Single val")
   ; Test strings
   l = ["a", "b"]
   rev = list_reverse(l)
   assert(get(rev, 0) == "b", "Strings rev[0]")
   assert(get(rev, 1) == "a", "Strings rev[1]")
   print("list_reverse passed")
}

fn test_main(){
   test_list_reversed()
   print("✓ std.collections.list_reverse passed")
}

test_main()
