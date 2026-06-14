use std.core
use std.core.error
use std.core.reflect
use std.core.dict
use std.core.iter as it
use std.core.io
use std.core.str

def s1 = "hello"
def s2 = "world"
assert(s1.len == 5, "len s1")
assert(s2.len == 5, "len s2")
def str: typed_len_source = "hello"
assert(typed_len_source.len == 5, "typed str property len")
assert("abc".len == 3, "literal str property len")
assert(["x", "y"].len == 2, "literal list property len")

fn property_len(x) { x.len }
assert(property_len("abcd") == 4, "dynamic str property len")
assert(property_len(["a", "b", "c"]) == 3, "dynamic list property len")

fn _len_return_source() { [7, 8, 9] }

fn _len_modulo_index(xs) {
   mut i = 0
   mut total = 0
   while i < 10 {
      total += xs[i % xs.len]
      i += 1
   }
   total
}

def shifts = _len_return_source()
assert(shifts.len == 3, "function-return local list .len")
assert(_len_modulo_index(shifts) == 79, "list .len inside modulo index")
assert(eq(s1, "hello"), "eq")
def empty = ""
assert(empty.len == 0, "empty len")
def concat = s1 + s2
assert(concat.len == 10, "concat len")
def plus = s1 + s2
assert(eq(plus, concat), "plus concat")
def cstr_buf = malloc(6)
store8(cstr_buf, 104, 0)
store8(cstr_buf, 101, 1)
store8(cstr_buf, 108, 2)
store8(cstr_buf, 108, 3)
store8(cstr_buf, 111, 4)
store8(cstr_buf, 0, 5)
assert(__cstr_to_str(cstr_buf) == "hello", "__cstr_to_str copies C string")
free(cstr_buf)
assert(__cstr_to_str(0) == nil, "__cstr_to_str rejects tagged zero")
def str_builder = __str_builder_new(1)
assert(str_builder != 0, "__str_builder_new allocates")
assert(__str_builder_append(str_builder, "ab") == str_builder, "__str_builder_append string")
assert(__str_builder_append(str_builder, 42) == str_builder, "__str_builder_append int")
assert(__str_builder_to_str(str_builder) == "ab42", "__str_builder_to_str returns content")
assert(__str_builder_free(str_builder) == nil, "__str_builder_free releases builder")
assert(__str_builder_append(0, "x") == 0, "__str_builder_append rejects tagged zero")
assert(__str_builder_to_str(0) == "", "__str_builder_to_str rejects tagged zero")
assert(__str_builder_free(0) == nil, "__str_builder_free rejects tagged zero")
def num_str = "42"
assert(num_str.len == 2, "num str len")
assert(eq(upper("hello"), "HELLO"), "upper")
assert(eq(lower("WORLD"), "world"), "lower")
def search_str = "hello world"
assert(startswith(search_str, "hello"), "startswith")
assert(endswith(search_str, "world"), "endswith")
assert(search_str.contains("world"), "contains")
def trimmed = strip("  hello  ")
assert(eq(trimmed, "hello"), "strip")
def parts = split("a,b,c,d", ",")
assert(parts.len == 4, "split len")
assert(eq(parts.get(0), "a"), "split 0")
assert(eq(parts.get(3), "d"), "split 3")
assert(eq(join(["x","y","z"], "-"), "x-y-z"), "join")
assert(eq(join(["x","y","z"]), "xyz"), "join default separator")
assert(eq(replace("hello world", "world", "universe"), "hello universe"), "replace")
assert(eq("  Ab9 ".strip().upper(), "AB9"), "str method chain strip/upper")
assert(eq("ha" * 3, "hahaha"), "str repeat operator")
assert(eq("a,b".split(",").get(1), "b"), "str split method")
assert("ff".parse_int(16) == 255, "str parse_int method")
def fstring_debug_value = 42
assert(eq(f"{fstring_debug_value=}", "fstring_debug_value=42"), "f-string debug name")
assert(eq(f"{fstring_debug_value + 1=}", "fstring_debug_value + 1=43"), "f-string debug expression")
assert(eq(f"{fstring_debug_value = }", "fstring_debug_value = 42"), "f-string debug spacing")
assert(eq(f"{fstring_debug_value == 42}", "true"), "f-string comparison is not debug syntax")
def fstring_list_value = [1, 9, 25]
def fstring_dict_value = {"xs": fstring_list_value, "ok": true}
assert(eq(f"{fstring_list_value}", "[1, 9, 25]"), "f-string list value")
assert(eq(f"{fstring_list_value=}", "fstring_list_value=[1, 9, 25]"), "f-string debug list value")
assert(eq(f"{[1, 2]=}", "[1, 2]=[1, 2]"), "f-string debug list literal value")
assert(eq(f"{fstring_dict_value}", "{xs: [1, 9, 25], ok: true}"), "f-string dict value")
assert("Ab9".ascii_alnum(), "str ascii alnum")
assert("123".ascii_digit(), "str ascii digit")
assert("   ".ascii_space(), "str ascii space")
assert(ord("a").ascii_upper() == 65, "int ascii upper method")
assert("abc".byte_at(1) == 98, "str byte_at method")
assert(sort("baba") == "aabb", "sort string")
assert(sorted("dcba") == "abcd", "sorted string")
assert(swapped("abcd", 0, 3) == "dbca", "swapped string")
assert(it.map("ab", fn(v) { v + "!" }) == "a!b!", "map string")
assert(it.filter("aaba", fn(v) { v >= "a" }) == "aaba", "filter string")
assert(it.take("abcd", 2) == "ab", "take string")
assert(it.drop("abcd", 2) == "cd", "drop string")
assert(it.reverse("abcd") == "dcba", "reverse string")
assert(it.chain("ab", "cd") == "abcd", "chain string")
print("✓ std.core.str basic tests passed")
