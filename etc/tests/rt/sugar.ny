use std.core
use std.math.crypto.encoding.bytes as bytes_mod

fn double(x) {
   "Doubles an integer for pipeline syntax tests."
   x * 2
}

fn cast_float_param(v) {
   float(v)
}

def x = 10 |> double() |> double()
assert(x == 40, "pipeline double")
def t = true ? 11 : 22
def f = false ? 11 : 22
assert(t == 11, "ternary true")
assert(f == 22, "ternary false")
def list = [100, 200, 300]
def first = list |> [0]
assert(first == 100, "pipeline index")
def obj = {"val": 500}
def v = obj |> .val
assert(v == 500, "pipeline member")
mut method_dict = {"a": 1}
method_dict.set("b", 2)
assert(method_dict["b"] == 2, "dict method set")
mut mut_a, mut_b, mut_c = 1, 2, 3
mut_a = mut_a + mut_b + mut_c
assert(mut_a == 6, "comma mut declaration")
mut assign_a, assign_b = 0, 0
assign_a, assign_b = 7, 9
assert(assign_a == 7 && assign_b == 9, "comma assignment")
assign_a, assign_b = assign_b, assign_a
assert(assign_a == 9 && assign_b == 7, "comma assignment evaluates rhs first")
def assign_pair = [11, 13]
assign_a, assign_b = assign_pair.get(0), assign_pair.get(1)
assert(assign_a == 11 && assign_b == 13, "comma assignment from pair")
def dict: typed_merge_dict = {"a": 1}.merge({"b": 2})
assert(typed_merge_dict["b"] == 2, "typed dict method merge")
mut method_list = [4, 5, 6]
method_list.set(1, 50)
assert(method_list[1] == 50, "list method set")
method_list[1] = 51
method_list[-1] = 60
assert(method_list[1] == 51 && method_list[2] == 60, "list indexed assignment")
def method_dict_added = {"b": 3, "c": 4}
assert(method_dict_added["b"] == 3 && method_dict_added.get("c", 0) == 4, "dict lookup after literal construction")
assert(method_dict.keys().contains("a"), "dict method keys/contains")
mut method_set = set()
method_set = method_set.add("x")
assert(method_set.contains("x"), "set constructor/add/contains methods")
mut bytes: method_bytes = bytes_mod.bytes(3)
method_bytes = method_bytes.set(0, 65).set(1, 66).set(2, 67)
assert(method_bytes.len == 3, "bytes method len")
assert(method_bytes.get(1, 0) == 66, "bytes method get/set")
def method_range = range(2, 8, 2)
assert(method_range.len == 3, "range method len")
assert(method_range.get(2, 0) == 6, "range method get")
assert(method_range.contains(4), "range method contains")
assert(method_range.values() == [2, 4, 6], "range method values")
assert(str(123) == "123", "str cast")
assert(int(1.9) == 1, "int cast")
assert(float(2) == 2.0, "float cast")
assert(float(2) == 2, "boxed float equals int")
def cast_src = 128
assert(float(cast_src) == 128.0, "float cast variable")
assert(cast_float_param(256) == 256.0, "float cast parameter")
def n = nil
assert((n ?? 123) == 123, "nil ?? fallback")
assert((456 ?? 123) == 456, "value ?? fallback")
def o = {"a": 777}
def o_nil = nil
assert(o?.a == 777, "optional chaining value")
assert(o_nil?.a == nil, "optional chaining nil")
assert((o_nil?.a ?? 999) == 999, "optional chaining fallback")

if def a = 100 a == 100 {
   assert(a == 100, "if-def scope")
} else {
   assert(false, "if-def failed")
}

mut j = 0
mut seen = false
while def k = 10 j < k {
   seen = true
   assert(k == 10, "while-def binding")
   j = j + 5
}

assert(seen == true, "while-def entered")
assert(j == 10, "while-def result")
mut acc = 0
while mut i = 0 i < 5 ++i {
   acc += i
}

assert(acc == 10, "while header sugar")
print("✓ all pipeline/option tests passed")
