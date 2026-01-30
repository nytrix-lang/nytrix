use std.core.error *
use std.core.reflect *
use std.core.list *
use std.core.dict *
use std.str.io *
use std.str *

;; Case / control-flow / edge cases (Test)

print("Testing case...")

mut g = 0
fn set_g(v){
   g = v
   0
}

fn case_multi(tag){
   g = 0
   case tag {
      0x4c495354, 0x44494354 -> set_g(1)
      _ -> set_g(2)
   }
   g
}

assert(case_multi(0x4c495354) == 1, "case multi")
assert(case_multi(0x5455504c) == 2, "case default")

fn case_return(tag){
   case tag {
      0x44494354 -> { print("ciao") print("ciao") return 5 }
      _ -> { return 3 }
   }
   0
}

assert(case_return(0x44494354) == 5, "case return")
assert(case_return(0) == 3, "case return default")

fn case_expr(tag){
   mut out = 0
   case tag {
      0x44494354 -> { out = 7  5 }
      _ -> { out = 9 }
   }
   out
}

assert(case_expr(0x44494354) == 7, "case expr block")

fn case_load(tag, ptr){
   mut out = 0
   case tag {
      0x4c495354, 0x5345545f, 0x5455504c -> { out = load64(ptr_add(ptr, 8)) }
      _ -> { out = 0 }
   }
   out
}

def mem = malloc(24)
store64(mem, 0x44494354, 8)
assert(case_load(0x4c495354, mem) == 0x44494354, "case load64")

fn case_wild(tag){
   mut out = 0
   case tag {
      _ -> { out = 11 }
   }
   out
}

assert(case_wild(123) == 11, "case wildcard")

fn case_as_expr(tag){
   def res = case tag {
      "hello" -> 1
      "world" -> 2
      _ -> 3
   }
   res
}

assert(case_as_expr("hello") == 1, "case expr 1")
assert(case_as_expr("world") == 2, "case expr 2")
assert(case_as_expr("anything") == 3, "case expr 3")

print("✓ case tests passed")

;; Edge cases (Test)

def max_small = 4611686018427387903
def min_small = -4611686018427387904
assert(max_small > 0, "max_small")
assert(min_small < 0, "min_small")

def empty = ""
assert(len(empty) == 0, "empty string")

def s = "   "
assert(len(strip(s)) == 0, "strip whitespace")

mut l = list(0)
assert(len(l) == 0, "empty list")

mut i = 0
while(i < 100){
   l = append(l, i)
   i = i + 1
}
assert(len(l) == 100, "list growth")
assert(get(l, 99) == 99, "list last")

print("✓ Edge cases passed")

