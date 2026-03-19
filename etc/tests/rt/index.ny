use std.core
use std.core.error
use std.core.str
use std.math.crypto.encoding.bytes

mut caught = ""

fn capture(thunk){
   caught = ""
   try {
      thunk()
   } catch e {
      caught = e
   }
   caught
}

fn did_catch(thunk){
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
mut bytes: bs = bytes(3)
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
while(i < xs.len){
   mix += xs[i] + tp[i] + rg[i]
   i += 1
}

assert(mix == 87, "mixed indexed loop read")
def raw_probe = [2, 3, 4, 5, 6, 7, 8, 9]
mut int: raw_acc = 0
mut int: raw_i = 0
while(raw_i < 64){
   def int: raw_idx = (((raw_i * 3) + 1) % 8)
   raw_acc += get(raw_probe, raw_idx, 0) + (raw_i % 5)
   raw_i += 1
}

assert(raw_acc == 478, "raw dynamic int-list get")
def raw_repr_probe = [0, 0, 1]
assert(raw_repr_probe == [0, 0, 1], "list representation valid")
assert(repr(raw_repr_probe) == "[0, 0, 1]", "list repr")
assert(to_str(raw_repr_probe) == "[0, 0, 1]", "list to_str")
mut int: redecl_i = 0
while(redecl_i < 3){
   redecl_i += 1
}

mut int: redecl_i = 0
mut int: redecl_sum = 0
while(redecl_i < 4){
   redecl_sum += redecl_i
   redecl_i += 1
}

assert(redecl_sum == 6, "top-level mutable redeclaration resets value")
def nested = [[1, 2], [3, 4], [5, 6]]
mut nested_sum = 0
mut ni = 0
while(ni < nested.len){
   nested_sum += nested[ni][0] + nested[ni][1]
   ni += 1
}

assert(nested_sum == 21, "nested indexed loop read")
def d = {"a": 1}
assert(d["a"] == 1, "dict index read")
assert(d["missing"] == 0, "dict index miss default")
assert(str_contains(capture(fn(){ xs[9] }), "index_read out of range"), "list out-of-range should panic")
assert(str_contains(capture(fn(){ s[9] }), "index_read out of range"), "string out-of-range should panic")
assert(str_contains(capture(fn(){ bs[9] }), "index_read out of range"), "bytes out-of-range should panic")
assert(str_contains(capture(fn(){ rg[9] }), "index_read out of range"), "range out-of-range should panic")
assert(str_contains(capture(fn(){ xs["x"] }), "index_read expects an integer index"), "list non-int index should panic")
assert(str_contains(capture(fn(){ tp["x"] }), "index_read expects an integer index"), "tuple non-int index should panic")
assert(str_contains(capture(fn(){ s[false] }), "index_read expects an integer index"), "string non-int index should panic")
assert(str_contains(capture(fn(){ rg[true] }), "index_read expects an integer index"), "range non-int index should panic")
assert(did_catch(fn(){ xs[9] }), "list out-of-range should be catchable")
assert(did_catch(fn(){ xs["x"] }), "list non-int index should be catchable")
print("✓ index runtime tests passed")
