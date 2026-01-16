use std.io
use std.util.regex
use std.core.test

print("Testing std.util.regex (extra)...")

assert(regex_match("^ab.*", "abcd") == 1, "match prefix")
assert(regex_match(".*cd$", "abcd") == 1, "match suffix")
assert(regex_match("^a.c$", "abc") == 1, "dot match")
assert(regex_match("^a.c$", "abb") == 0, "dot mismatch")

assert(regex_find("b.*z", "zzabczz") == 3, "find middle")
assert(regex_find("^z", "abc") == -1, "find start miss")

print("✓ std.util.regex extra passed")
