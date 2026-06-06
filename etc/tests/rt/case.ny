use std.core
use std.core.error
use std.core.reflect
use std.core.dict
use std.core.io
use std.core.str

;; Case / control-flow / edge cases (Test)
print("Testing case...")
mut g = 0

fn set_g(v) {
   g = v
   0
}

fn case_multi(tag) {
   g = 0
   case tag {
      0x4c495354, 0x44494354 -> set_g(1)
      _ -> set_g(2)
   }
   g
}

assert(case_multi(0x4c495354) == 1, "case multi")
assert(case_multi(0x5455504c) == 2, "case default")

fn case_return(tag) {
   case tag {
      0x44494354 -> { print("ciao") print("ciao") return 5 }
      _ -> { return 3 }
   }
   0
}

assert(case_return(0x44494354) == 5, "case return")
assert(case_return(0) == 3, "case return default")

fn case_expr(tag) {
   mut out = 0
   case tag {
      0x44494354 -> { out = 7  5 }
      _ -> { out = 9 }
   }
   out
}

assert(case_expr(0x44494354) == 7, "case expr block")

fn case_load(tag, ptr) {
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

fn case_wild(tag) {
   mut out = 0
   case tag {
      _ -> { out = 11 }
   }
   out
}

assert(case_wild(123) == 11, "case wildcard")

fn case_as_expr(tag) {
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

fn case_as_expr_native(int tag) int {
   def res = case int(tag){
      0 -> 1
      8 -> 256
      _ -> 0
   }
   res
}

assert(case_as_expr_native(0) == 1, "case expr native first arm")
assert(case_as_expr_native(8) == 256, "case expr native second arm")
assert(case_as_expr_native(9) == 0, "case expr native default")

fn map_x11_key(i32 raw) i32 {
   case raw {
      0x30..0x39 -> raw
      0x41..0x5a -> raw
      0x61..0x7a -> raw - 32
      0xff1b -> 256
      0xff50 -> 257
      0xff51 -> 258
      0xffbe..0xffc9 -> 1000 + (raw - 0xffbe)
      _ -> 0
   }
}

assert(map_x11_key(0x35) == 0x35, "case range digit")
assert(map_x11_key(0x61) == 0x41, "case range lowercase fold")
assert(map_x11_key(0xffc1) == 1003, "case range function key")
assert(map_x11_key(0) == 0, "case range default")
def float_case_range = case 1.5 {
   1.0..2.0 -> 7
   _ -> 0
}

def string_case_range = case "m" {
   "a".."z" -> 9
   _ -> 0
}

assert(float_case_range == 7, "case range float")
assert(string_case_range == 9, "case range string")

fn case_range_guard(v) {
   case v {
      1..9 if v % 2 == 0 -> 20
      1..9 -> 10
      _ -> 0
   }
}

assert(case_range_guard(4) == 20, "case range guard")
assert(case_range_guard(5) == 10, "case range fallback")
assert(case_range_guard(12) == 0, "case range miss")

fn case_range_for_sum() {
   mut total = 0
   for v in 1..4 {
      total += v
   }
   total
}

assert(case_range_for_sum() == 10, "for inclusive range expression")

fn case_range_for_typed(i32 hi) int {
   mut total = 0
   for v in 1..hi {
      total += v
   }
   total
}

assert(case_range_for_typed(4) == 10, "for typed inclusive range expression")
print("✓ case tests passed")

;; Edge cases (Test)
def max_small = 4611686018427387903
def min_small = -4611686018427387904
assert(max_small > 0, "max_small")
assert(min_small < 0, "min_small")
def empty = ""
assert(empty.len == 0, "empty string")

#main {
   def s = "   "
   def stripped = strip(s)
   assert(stripped.len == 0, "strip whitespace")
}

mut l = list(0)
assert(l.len == 0, "empty list")
mut i = 0
while(i < 100){
   l = l.append(i)
   i += 1
}

assert(l.len == 100, "list growth")
assert(l.get(99) == 99, "list last")
print("✓ Edge cases passed")
