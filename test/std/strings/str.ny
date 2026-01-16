use std.io
; Test std.strings.str - String operations comprehensive
use std.strings.str
use std.core

fn test_basics(){
	print("Testing string basics...")
	def s = "hello"
	assert(str_len(s) == 5, "len")
	assert(eq(s, "hello"), "eq")
	assert(!eq(s, "Hello"), "case sensitive")
	def s2 = str_clone(s)
	assert(eq(s, s2), "clone eq")
	; s == s2 is true (value equality), so we skip pointer check or use unsafe cast if needed.
	; For now just verify value equality preserved.
}

fn test_slice(){
	print("Testing slice...")
	def s = "0123456789"
	assert(eq(slice(s, 0, 5, 1), "01234"), "slice prefix")
	assert(eq(slice(s, 5, 10, 1), "56789"), "slice suffix")
	assert(eq(slice(s, 2, 8, 2), "246"), "slice step 2")
	assert(eq(slice(s, -4, -1, 1), "678"), "negative slice")
	; slice(s, 10, -1, -1) should include index 0.
	; slice stops BEFORE stop. To include 0 with step -1, stop must be -1 AFTER normalization.
	; Normalization: if stop < 0, stop = len + stop.
	; We want len + stop = -1 => stop = -1 - len = -1 - 10 = -11.
	assert(eq(slice(s, 10, -11, -1), "9876543210"), "reverse slice full")
	assert(eq(slice(s, 10, 0, -1), "987654321"), "reverse slice stop at 0")
}

fn test_search(){
	print("Testing search...")
	def s = "banana"
	print("s ptr check:")
	if(is_ptr(s)){ print("s is ptr (aligned)") } else { print("s is NOT ptr (unaligned?)") }
	; print("s addr approx: ", itoa(s)) ; itoa might fail/box. s is ptr.
	; Debug count
	def sub = "a"
	def n = str_len(s)
	def m = str_len(sub)
	print("banana len:", itoa(n))
	print("a len:", itoa(m))
	assert(find(s, "nan") == 2, "find nan")
	assert(find(s, "z") == -1, "find missing")
	assert(find(s, "ana") >= 0, "contains")
	def c1 = count(s, "a")
	print("count(banana, a) =", itoa(c1))
	assert(c1 == 3, "count a")
	assert(count(s, "an") == 2, "count an")
}

fn test_split_join_partition(){
	print("Testing split/join/partition...")
	def s = "a,b,c"
	def parts = split(s, ",")
	assert(len(parts) == 3, "split len")
	assert(eq(join(parts, "-"), "a-b-c"), "join")
	def p = partition("a=b=c", "=")
	assert(eq(get(p, 0), "a"), "part before")
	assert(eq(get(p, 1), "="), "part sep")
	assert(eq(get(p, 2), "b=c"), "part after")
	def lines = splitlines("a\nb\nc")
	assert(len(lines) == 3, "splitlines")
}

fn test_pad_fill(){
	print("Testing pad/fill...")
	assert(eq(pad_start("foo", 5), "  foo"), "pad_start")
	assert(eq(pad_end("foo", 5, "."), "foo.."), "pad_end")
	assert(eq(zfill("42", 5), "00042"), "zfill")
	assert(eq(zfill("-42", 5), "-0042"), "zfill negative")
	assert(eq(repeat("ab", 3), "ababab"), "repeat")
}

fn test_ascii(){
	print("Testing chr/ord...")
	assert(ord("A") == 65, "ord A")
	assert(eq(chr(65), "A"), "chr 65")
}

fn test_conversion(){
	print("Testing atoi/itoa...")
	assert(atoi("123") == 123, "atoi 123")
	assert(atoi("-456") == -456, "atoi -456")
	assert(atoi("   789") == 789, "atoi whitespace")
	assert(eq(itoa(123), "123"), "itoa 123")
}

fn test_case_trim(){
	print("Testing case/trim...")
	assert(eq(upper("Hello"), "HELLO"), "upper")
	assert(eq(lower("HeLLo"), "hello"), "lower")
	assert(eq(strip("  hi  "), "hi"), "strip")
	assert(eq(lstrip("  hi  "), "hi  "), "lstrip")
	assert(eq(rstrip("  hi  "), "  hi"), "rstrip")
}

fn test_main(){
	test_basics()
	test_slice()
	test_search()
	test_split_join_partition()
	test_pad_fill()
	test_ascii()
	test_conversion()
	test_case_trim()
	print("✓ std.strings.str comprehensive tests passed")
}

test_main()
