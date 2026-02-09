use std.core *
use std.str.str *
use std.str *

;; std.str (Test)

def s = "hello"
assert(str_len(s) == 5, "str_len")
assert(find(s, "ell") == 1, "find substring")
assert(find(s, "zzz") == -1, "find missing")

assert(_str_eq("a", "a"), "_str_eq true")
assert(!_str_eq("a", "b"), "_str_eq false")

assert(pad_start("7", 3, "0") == "007", "pad_start")
assert(startswith("hello", "he"), "startswith")
assert(endswith("hello", "lo"), "endswith")

assert(atoi("123") == 123, "atoi")
assert(atoi("-7") == -7, "atoi negative")

mut parts = split("a,b,c", ",")
assert(len(parts) == 3, "split count")
assert(get(parts, 0) == "a", "split first")
assert(get(parts, 2) == "c", "split last")

assert(strip("  hi \n") == "hi", "strip")
assert(str_add("he", "llo") == "hello", "str_add")
assert(upper("heLlo") == "HELLO", "upper")
assert(lower("HeLLo") == "hello", "lower")
assert(str_contains("hello", "ell"), "str_contains")

mut items = ["a", "b", "c"]
assert(join(items, ",") == "a,b,c", "join")

assert(str_replace("a-b-a", "a", "x") == "x-b-x", "str_replace")
assert(replace_all("a-b-a", "-", ":") == "a:b:a", "replace_all")

def s = "abcdef"
assert(_str_eq(str_slice(s, 0, 3), "abc"), "slice start")
assert(_str_eq(str_slice(s, 1, 5, 2), "bd"), "slice step")
assert(_str_eq(str_slice(s, -3, -1), "de"), "slice negative")
assert(_str_eq(str_slice(s, 0, 0), ""), "slice empty")

print("✓ std.str tests passed")
