use std.io
use std.util.regex
use std.core.test

print("Testing Regex...")

assert(regex_match("abc", "abc"), "exact match")
assert(regex_match("abc", "xabcy"), "substring match")
assert(regex_match("^abc", "abc"), "anchor match")
assert(regex_match("^abc", "xabc") == 0, "anchor no match")
assert(regex_match("a.c", "abc"), "dot match")
assert(regex_match("a*b", "aaab"), "star match")
assert(regex_match(".*", "anything"), "dot star match")
assert(regex_match("a$", "ba"), "end anchor match")
assert(regex_match("^a*b$", "aaab"), "complex match")

assert(regex_find("b", "abc") == 1, "find index")
assert(regex_find("z", "abc") == -1, "find not found")

print("✓ std.util.regex passed")
