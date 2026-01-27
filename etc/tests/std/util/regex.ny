use std.io
use std.util.regex
use std.core.error

;; std.util.regex (Test)
;; Tests regex matching and finding.

print("Testing Regex...")

assert(regex_match("abc", "abc"), "exact match")
assert(regex_match("abc", "xabcy"), "substring match")
assert(regex_match("^abc", "abc"), "anchor match")
assert(regex_match("^abc", "xabc") == 0, "anchor no match")
assert(regex_match("a.c", "abc"), "dot match")
assert(regex_match("a*b", "aaab"), "star match")
assert(regex_match(".*", "anything"), "dot star")
assert(regex_match("a$", "ba"), "end anchor")
assert(regex_match("^a*b$", "aaab"), "complex match")

assert(regex_find("b", "abc") == 1, "find index")
assert(regex_find("z", "abc") == -1, "find miss")

assert(regex_match("^ab.*", "abcd") == 1, "prefix")
assert(regex_match(".*cd$", "abcd") == 1, "suffix")
assert(regex_match("^a.c$", "abc") == 1, "dot ok")
assert(regex_match("^a.c$", "abb") == 0, "dot fail")

assert(regex_find("b.*z", "zzabczz") == 3, "find middle")
assert(regex_find("^z", "abc") == -1, "find start miss")

print("✓ std.util.regex tests passed")
