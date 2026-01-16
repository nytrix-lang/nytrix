use std.io
use std.strings.str

; Test string operations and parsing

; Basic string operations
def s1 = "hello"
def s2 = "world"
assert(len(s1) == 5, "string length")
assert(len(s2) == 5, "string length 2")

; String comparison
assert(eq(s1, "hello"), "string equality")

; Empty string
def empty = ""
assert(len(empty) == 0, "empty string length")

; String concatenation
def concat = str_cat(s1, s2)
assert(len(concat) == 10, "concatenated length")

; String with numbers
def num_str = "42"
assert(len(num_str) == 2, "numeric string")

; Case operations
def upper_test = to_upper("hello")
assert(eq(upper_test, "HELLO"), "to_upper")

def lower_test = to_lower("WORLD")
assert(eq(lower_test, "world"), "to_lower")

; String search
def search_str = "hello world"
assert(starts_with(search_str, "hello"), "starts_with")
assert(ends_with(search_str, "world"), "ends_with")
assert(contains(search_str, "world"), "contains")

; String trimming
def trim_test = "  hello  "
def trimmed = trim(trim_test)
assert(eq(trimmed, "hello"), "trim whitespace")

; String splitting
def split_test = "a,b,c,d"
def parts = split(split_test, ",")
assert(len(parts) == 4, "split length")
assert(eq(parts[0], "a"), "split part 0")
assert(eq(parts[3], "d"), "split part 3")

; String joining
def join_test = join(["x", "y", "z"], "-")
assert(eq(join_test, "x-y-z"), "join strings")

; String replacement
def replace_test = replace("hello world", "world", "universe")
assert(eq(replace_test, "hello universe"), "replace string")

print("✓ String tests passed")

