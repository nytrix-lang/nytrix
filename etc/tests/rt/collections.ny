use std.core

mut xs = [4, 1, 9]
assert(__store_item(xs, 1, 7) == 7, "__store_item returns stored value")
assert(xs == [4, 7, 9], "__store_item mutates list")
xs = __list_reserve(xs, 32)
assert(xs.len == 3, "__list_reserve keeps length")
assert(xs == [4, 7, 9], "__list_reserve keeps values")
assert(__list_sum_int_range(xs, 0, xs.len) == 20, "__list_sum_int_range sums full list")
assert(__list_sum_int_range(xs, -4, 2) == 11, "__list_sum_int_range clamps negative start")
assert(__list_sum_int_range(xs, 2, 99) == 9, "__list_sum_int_range clamps stop")
mut sortable = [9, 4, 7, 1]
assert(__sort_list(sortable) == [1, 4, 7, 9], "__sort_list returns sorted list")
assert(sortable == [1, 4, 7, 9], "__sort_list mutates list")
mut d = dict(1)
d = __dict_reserve(d, 12)
d = __dict_write_fast(d, "a", 10)
d = __dict_write_fast(d, "b", 20)
d = __dict_write_fast(d, "a", 30)
assert(d.get("a", 0) == 30, "__dict_write_fast overwrites existing key")
assert(d.get("b", 0) == 20, "__dict_write_fast inserts key")
assert(load64(d, 0) == 2, "__dict_write_fast keeps dict cardinality")
print("✓ runtime container tests passed")

use std.core
use std.core.error

def KEY_NULL = 0
def KEY_ESCAPE = 256
def KEY_HOME = 257
def KEY_LEFT = 258
def KEY_F1 = 1000

comptime table KeyMap {
   0x30..0x39 -> raw
   0x41..0x5a -> raw
   0x61..0x7a -> raw - 32
   0xff1b -> KEY_ESCAPE
   0xff50 -> KEY_HOME
   0xff51 -> KEY_LEFT
   0xffbe..0xffc9 -> KEY_F1 + (raw - 0xffbe)
}

fn map_key(i32 raw) i32 = comptime match KeyMap(raw, KEY_NULL)
assert(map_key(0x35) == 0x35, "comptime table digit range")
assert(map_key(0x61) == 0x41, "comptime table lowercase fold")
assert(map_key(0xff1b) == KEY_ESCAPE, "comptime table literal")
assert(map_key(0xffc1) == KEY_F1 + 3, "comptime table function range")
assert(map_key(0) == KEY_NULL, "comptime table fallback")
assert(_key_map(0xff50) == KEY_HOME, "comptime table legacy helper literal")
assert(_key_map(0, -123) == -123, "comptime table legacy helper explicit fallback")

comptime table SemanticKind {
   1, 2, 3 -> 10
   10..19 -> raw * 2
   _ -> 99
}

fn semantic_kind(i32 raw) i32 = comptime match SemanticKind(raw, -1)
assert(semantic_kind(2) == 10, "comptime table multi-pattern")
assert(semantic_kind(12) == 24, "comptime table range expression")
assert(semantic_kind(40) == 99, "comptime table wildcard")
assert(_semantic_kind(40) == 99, "comptime table legacy helper wildcard")

module TableModule(
   map_mod_key
){
   comptime table ModKeyMap {
      7 -> raw + 1
      _ -> default
   }
   fn map_mod_key(i32 raw) i32 = comptime match ModKeyMap(raw, -7)
}

use TableModule (map_mod_key)

assert(map_mod_key(7) == 8, "module-local comptime table")
assert(map_mod_key(99) == -7, "module-local comptime table fallback")
print("✓ comptime table tests passed")

use std.core
use std.core.error
use std.core.str
use std.math.crypto.encoding.bytes

mut caught = ""

fn capture(thunk) {
   caught = ""
   try {
      thunk()
   } catch e {
      caught = e
   }
   caught
}

fn did_catch(thunk) {
   try {
      thunk()
      false
   } catch _ {
      true
   }
}

def xs = [10, 20, 30]
assert(xs[0] == 10, "list index read")
assert(xs[-1] == 30, "list negative index read")
def tp = (4, 5, 6)
assert(tp[1] == 5, "tuple index read")
assert(tp[-1] == 6, "tuple negative index read")
def s = "abcd"
assert(s[2] == "c", "string index read")
assert(s[-2] == "c", "string negative index read")
mut bytes bs = bytes(3)
bs = bytes_set(bs, 0, 65)
bs = bytes_set(bs, 1, 66)
bs = bytes_set(bs, 2, 67)
assert(bs[1] == 66, "bytes index read")
assert(bs[-1] == 67, "bytes negative index read")
def rg = range(2, 8, 2)
assert(rg[0] == 2, "range index read")
assert(rg[-1] == 6, "range negative index read")
mut mix = 0
mut i = 0
while i < xs.len {
   mix += xs[i] + tp[i] + rg[i]
   i += 1
}

assert(mix == 87, "mixed indexed loop read")
def raw_probe = [2, 3, 4, 5, 6, 7, 8, 9]
mut int raw_acc = 0
mut int raw_i = 0
while raw_i < 64 {
   def int raw_idx = (((raw_i * 3) + 1) % 8)
   raw_acc += get(raw_probe, raw_idx, 0) + (raw_i % 5)
   raw_i += 1
}

assert(raw_acc == 478, "raw dynamic int-list get")
def raw_repr_probe = [0, 0, 1]
assert(raw_repr_probe == [0, 0, 1], "list representation valid")
assert(repr(raw_repr_probe) == "[0, 0, 1]", "list repr")
assert(to_str(raw_repr_probe) == "[0, 0, 1]", "list to_str")
mut int redecl_i = 0
while redecl_i < 3 {
   redecl_i += 1
}

mut int redecl_i = 0
mut int redecl_sum = 0
while redecl_i < 4 {
   redecl_sum += redecl_i
   redecl_i += 1
}

assert(redecl_sum == 6, "top-level mutable redeclaration resets value")
def nested = [[1, 2], [3, 4], [5, 6]]
mut nested_sum = 0
mut ni = 0
while ni < nested.len {
   nested_sum += nested[ni][0] + nested[ni][1]
   ni += 1
}

assert(nested_sum == 21, "nested indexed loop read")
def d = {"a": 1}
assert(d["a"] == 1, "dict index read")
assert(d["missing"] == 0, "dict index miss default")
assert(str_contains(capture(fn() { xs[9] }), "index_read out of range"), "list out-of-range should panic")
assert(str_contains(capture(fn() { s[9] }), "index_read out of range"), "string out-of-range should panic")
assert(str_contains(capture(fn() { bs[9] }), "index_read out of range"), "bytes out-of-range should panic")
assert(str_contains(capture(fn() { rg[9] }), "index_read out of range"), "range out-of-range should panic")
assert(str_contains(capture(fn() { xs["x"] }), "index_read expects an integer index"), "list non-int index should panic")
assert(str_contains(capture(fn() { tp["x"] }), "index_read expects an integer index"), "tuple non-int index should panic")
assert(str_contains(capture(fn() { s[false] }), "index_read expects an integer index"), "string non-int index should panic")
assert(str_contains(capture(fn() { rg[true] }), "index_read expects an integer index"), "range non-int index should panic")
assert(did_catch(fn() { xs[9] }), "list out-of-range should be catchable")
assert(did_catch(fn() { xs["x"] }), "list non-int index should be catchable")
print("✓ index runtime tests passed")

use std.core
use std.core.error
use std.core.iter as it

assert(it.range(5) == [0, 1, 2, 3, 4], "range stop")
assert(it.range(2, 6) == [2, 3, 4, 5], "range start stop")
assert(it.range(5, 0) == [], "range stop zero two-arg")
assert(it.range(0, 0) == [], "range zero zero")
assert(it.range(6, 2, -2) == [6, 4], "range step")
assert(it.range2(1, 8, 3) == [1, 4, 7], "range2")
assert(is_range(it.range(1)), "range predicate")
assert(__tagof(it.range(1)) == __runtime_tag("range"), "range runtime tag")
assert(it.enumerate(["a", "b"], 10) == [[10, "a"], [11, "b"]], "enumerate")
assert(it.map([1, 2, 3], fn(v) { v * v }) == [1, 4, 9], "map list")
assert(it.map("ab", fn(v) { v + "!" }) == "a!b!", "map string")
assert(it.filter([1, 2, 3, 4, 5, 6], fn(v) { (v % 2) == 0 }) == [2, 4, 6], "filter list")
assert(it.filter("aaba", fn(v) { v >= "a" }) == "aaba", "filter string")
assert(it.repeat("x", 3) == ["x", "x", "x"], "repeat")
assert(it.take([9, 8, 7], 2) == [9, 8], "take list")
assert(it.take("abcd", 2) == "ab", "take string")
assert(it.drop([9, 8, 7], 1) == [8, 7], "drop list")
assert(it.drop("abcd", 2) == "cd", "drop string")
assert(it.take([], 3) == [], "take empty list")
assert(it.take("", 3) == "", "take empty string")
assert(it.take([7], 1) == [7], "take single item list")
assert(it.take("z", 1) == "z", "take single item string")
assert(it.drop([], 1) == [], "drop empty list")
assert(it.drop("", 1) == "", "drop empty string")
assert(it.drop([7], 1) == [], "drop single item list")
assert(it.drop("z", 1) == "", "drop single item string")
assert(it.reverse([1, 2, 3]) == [3, 2, 1], "reverse list")
assert(it.reverse("abc") == "cba", "reverse string")
assert(it.reverse([]) == [], "reverse empty list")
assert(it.reverse("") == "", "reverse empty string")
assert(it.reverse([5]) == [5], "reverse single item list")
assert(it.reverse("q") == "q", "reverse single item string")
assert(it.zip2(["a", "b", "c"], [1, 2]) == [["a", 1], ["b", 2]], "zip2")
assert(it.any([1, 2, 3], fn(v) { v > 2 }), "any true")
assert(!it.any([1, 2, 3], fn(v) { v > 5 }), "any false")
assert(it.all([4, 5, 6], fn(v) { v > 3 }), "all true")
assert(!it.all([4, 5, 6], fn(v) { v > 5 }), "all false")
assert(it.fold([1, 2, 3, 4], 0, fn(a, v) { a + v }) == 10, "fold")
assert(it.reduce([1, 2, 3], 1, fn(a, v) { a * v }) == 6, "reduce")
assert(it.count([1, 2, 3]) == 3, "count")
assert(it.count_if([1, 2, 3, 4], fn(v) { v > 2 }) == 2, "count if")
assert(it.first([10, 20], -1) == 10, "first")
assert(it.first([], -1) == -1, "first default")
assert(it.last([10, 20], -1) == 20, "last")
assert(it.last([], -1) == -1, "last default")
assert(it.find_if([10, 20, 30], fn(v) { v > 15 }) == 20, "find")
assert(it.find_if([10, 20, 30], fn(v) { v > 50 }, -1) == -1, "find missing")
assert(it.find_index_if([10, 20, 30], fn(v) { v > 15 }) == 1, "find index")
assert(it.chain([1, 2], [3, 4]) == [1, 2, 3, 4], "chain list")
assert(it.chain("ab", "cd") == "abcd", "chain string")
assert(it.chain([], []) == [], "chain empty lists")
assert(it.chain("", "") == "", "chain empty strings")
assert(it.chain([], [4, 5]) == [4, 5], "chain left empty list")
assert(it.chain([4, 5], []) == [4, 5], "chain right empty list")
assert(it.flatten([[1, 2], 3, [4, 5]]) == [1, 2, 3, 4, 5], "flatten")
assert(it.flatten([(1, 2), it.range(2), "ab"]) == [1, 2, 0, 1, "a", "b"], "flatten sequences")
assert(it.mapcat(fn(v) { [v, v] }, [1, 2]) == [1, 1, 2, 2], "mapcat")
assert(it.mapcat(fn(v) { v }, [(1, 2), "ab", it.range(2)]) == [1, 2, "a", "b", 0, 1], "mapcat sequences")
assert(it.filter_map([1, 2, 3, 4], fn(v) {
         if v % 2 == 0 { return v * 10 }
         0
}) == [20, 40], "filter map")
assert(it.compact([0, 1, "", "x", nil, 4]) == [1, "x", 4], "compact")
assert(it.zip_with([1, 2], [10, 20], fn(a, b) { a + b }) == [11, 22], "zip with")
assert(it.cycle([1, 2], 3) == [1, 2, 1, 2, 1, 2], "cycle")
assert(it.chunk([1, 2, 3, 4, 5], 2) == [[1, 2], [3, 4], [5]], "chunk list")
assert(it.chunk("abcde", 2) == ["ab", "cd", "e"], "chunk string")
assert(it.windowed([1, 2, 3, 4], 3) == [[1, 2, 3], [2, 3, 4]], "windowed list")
assert(it.windowed("abcd", 2, 2) == ["ab", "cd"], "windowed string")
assert([1, 2, 3].map(fn(v) { v + 1 }) == [2, 3, 4], "list map method")
assert([1, 2, 3, 4].filter(fn(v) { v % 2 == 0 }) == [2, 4], "list filter method")
assert([1, 2, 3].reduce(0, fn(a, v) { a + v }) == 6, "list reduce method")
assert("abcd".chunk(2) == ["ab", "cd"], "str chunk method")
assert((1, 2, 3).first(-1) == 1, "tuple first method")
def pt = it.partition([1, 2, 3, 4, 5], fn(v) { v > 3 })
assert(pt.get(0) == [4, 5], "partition true")
assert(pt.get(1) == [1, 2, 3], "partition false")
assert([1, 2, 3].get(-1, 0) == 3, "get list negative index")
assert("abc".get(-1, "") == "c", "get string negative index")
assert([1, 2, 3].get(9, 77) == 77, "get list out of range default")
assert("abc".get(9, "?") == "?", "get string out of range default")
assert(slice([1, 2, 3], 0, 0) == [], "slice empty list range")
assert(slice("abc", 0, 0) == "", "slice empty string range")
assert(values(it.range(5, 0, -1)) == [5, 4, 3, 2, 1], "values range")
assert(keys(it.range(5, 0, -1)) == [0, 1, 2, 3, 4], "keys range")
assert(items(it.range(5, 0, -1)) == [[0, 5], [1, 4], [2, 3], [3, 2], [4, 1]], "items range")
assert(sort(it.range(5, 0, -1)) == [1, 2, 3, 4, 5], "sort range")
assert(sorted(it.range(5, 0, -1)) == [1, 2, 3, 4, 5], "sorted range")
def fruits = [1, 2, 3, 4]
mut weighted = 0
for fruit, i in fruits {
   weighted += fruit * (i + 1)
}

assert(weighted == 30, "for value,index over list")
mut letters = []
for x, i in "test" {
   letters = letters.append(f"{x}:{i}")
}

assert(letters == ["t:0", "e:1", "s:2", "t:3"], "for value,index over string")
print("✓ std.core.iter runtime tests passed")
