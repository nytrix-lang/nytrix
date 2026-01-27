use std.io
use std.strings.str
use std.core.error

;; std.strings.str (Test)
;; Comprehensive string operations tests.

print("Testing string basics...")
def s = "hello"
assert(str_len(s) == 5, "len")
assert(eq(s, "hello"), "eq")
assert(!eq(s, "Hello"), "case sensitive")
def s2 = str_clone(s)
assert(eq(s, s2), "clone eq")

print("Testing slice...")
def t = "0123456789"
assert(eq(slice(t, 0, 5, 1), "01234"), "slice prefix")
assert(eq(slice(t, 5, 10, 1), "56789"), "slice suffix")
assert(eq(slice(t, 2, 8, 2), "246"), "slice step")
assert(eq(slice(t, -4, -1, 1), "678"), "slice negative")
assert(eq(slice(t, 10, -11, -1), "9876543210"), "slice reverse full")
assert(eq(slice(t, 10, 0, -1), "987654321"), "slice reverse stop")

print("Testing search...")
def s3 = "banana"
assert(find(s3, "nan") == 2, "find nan")
assert(find(s3, "z") == -1, "find missing")
assert(find(s3, "ana") >= 0, "contains")
assert(count(s3, "a") == 3, "count a")
assert(count(s3, "an") == 2, "count an")

print("Testing split/join/partition...")
def parts = split("a,b,c", ",")
assert(len(parts) == 3, "split len")
assert(eq(join(parts, "-"), "a-b-c"), "join")
def p = partition("a=b=c", "=")
assert(eq(get(p, 0), "a"), "part before")
assert(eq(get(p, 1), "="), "part sep")
assert(eq(get(p, 2), "b=c"), "part after")
assert(len(splitlines("a\nb\nc")) == 3, "splitlines")

print("Testing pad/fill...")
assert(eq(pad_start("foo", 5), "  foo"), "pad_start")
assert(eq(pad_end("foo", 5, "."), "foo.."), "pad_end")
assert(eq(zfill("42", 5), "00042"), "zfill")
assert(eq(zfill("-42", 5), "-0042"), "zfill neg")
assert(eq(repeat("ab", 3), "ababab"), "repeat")

print("Testing chr/ord...")
assert(ord("A") == 65, "ord")
assert(eq(chr(65), "A"), "chr")

print("Testing atoi/itoa...")
assert(atoi("123") == 123, "atoi")
assert(atoi("-456") == -456, "atoi neg")
assert(atoi("   789") == 789, "atoi ws")
assert(eq(to_str(123), "123"), "itoa")

print("Testing case/trim...")
assert(eq(upper("Hello"), "HELLO"), "upper")
assert(eq(lower("HeLLo"), "hello"), "lower")
assert(eq(strip("  hi  "), "hi"), "strip")
assert(eq(lstrip("  hi  "), "hi  "), "lstrip")
assert(eq(rstrip("  hi  "), "  hi"), "rstrip")

print("✓ std.strings.str tests passed")
