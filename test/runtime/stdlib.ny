use std.core
use std.core.error
use std.core.reflect
use std.io
use std.io.fs
use std.io.path
use std.strings.str
use std.collections as col
use std.collections.dict as dictmod
use std.collections.set as setmod
use std.util.json as json
use std.util.regex as regex
use std.util.url as url
use std.util.base64 as base64
use std.math
use std.iter
use std.os
use std.os.time
use std.core
use std.core.error
use std.core.reflect
use std.strings.str
use std.strings.bytes
use std.util.base64
use std.util.regex
use std.util.url
use std.io.path

fn test_strings(){
	print("Testing strings...")
	def s = "  Hello,World  "
	def t = strip(s)
	assert(t == "Hello,World", "strip")
	def parts = split(t, ",")
	assert(len(parts) == 2, "split")
	def joined = join(parts, "-")
	assert(joined == "Hello-World", "join")
	assert(find(joined, "World") >= 0, "find")
	assert(upper("a") == "A", "upper")
	assert(lower("B") == "b", "lower")
	def r = replace_all("a-b-a", "a", "x")
	assert(r == "x-b-x", "replace_all")
	def lines = splitlines("a\nb\n")
	assert(len(lines) == 2, "splitlines")
}

fn test_collections(){
	print("Testing collections...")
	def lst = list(4)
	lst = append(lst, 1)
	lst = append(lst, 2)
	assert(len(lst) == 2, "list len")
	assert(get(lst, 1) == 2, "list get")
	lst = col.list_reverse(lst)
	assert(get(lst, 0) == 2, "list_reverse")
	def d = dictmod.dict(8)
	setitem(d, "k", "v")
	assert(getitem(d, "k", 0) == "v", "dict set/get")
	assert(dictmod.has(d, "k") == 1, "dict has")
	def s = setmod.set()
	setmod.add(s, "x")
	assert(setmod.set_contains(s, "x") == 1, "set contains")
}

fn test_io_fs(){
	print("Testing io/fs/path...")
	def base = path_join(cwd(), "build/.tmp/nytrix_std_test")
	fs.mkdirs(base)
	def f = path_join(base, "file.txt")
	def w = "hello nytrix"
	file_write(f, w)
	def r = file_read(f)
	assert(r == w, "file_read/write")
	assert(fs.is_file(f) == 1, "is_file")
	assert(fs.is_dir(base) == 1, "is_dir")
	def names = fs.listdir(base)
	assert(col.list_has(names, "file.txt") == 1, "listdir contains")
}

fn test_json(){
	print("Testing json...")
	def d = dictmod.dict(8)
	setitem(d, "a", "1")
	setitem(d, "b", "2")
	def s = json.json_encode(d)
	def out = json.json_decode(s)
	assert(type(out) == "dict", "json decode type")
	assert(getitem(out, "a", 0) == "1", "json decode value")
}

fn test_regex(){
	print("Testing regex...")
	assert(regex.regex_match("^ab.*", "abcd") == 1, "regex match")
	assert(regex.regex_find("b.c", "zabc") == 1, "regex find")
}

fn test_url(){
	print("Testing url...")
	def p = url.url_parse("http://example.com:8080/path?q=1#frag")
	assert(len(p) == 7, "url_parse len")
	assert(get(p, 0) == "http", "url_parse scheme")
	assert(get(p, 2) == "example.com", "url_parse host")
	assert(get(p, 3) == 8080, "url_parse port")
	def q = url.parse_query("a=1&b=2")
	assert(getitem(q, "a", 0) == "1", "parse_query a")
}

fn test_base64(){
	print("Testing base64...")
	def s = "hello"
	def enc = base64.b64_encode(s)
	def dec = base64.b64_decode(enc)
	assert(dec == s, "base64 encode/decode")
}

fn test_math_iter(){
	print("Testing math/iter...")
	assert(math.abs(-5) == 5, "abs")
	assert(math.gcd(48, 18) == 6, "gcd")
	def xs = iter.range(5)
	assert(len(xs) == 5, "range len")
	assert(get(xs, 4) == 4, "range last")
}

fn test_os(){
	print("Testing os/time...")
	def p = os.pid()
	assert(p > 0, "pid")
	def c = os.getcwd()
	assert(str_len(c) > 0, "getcwd")
	def t = time.time()
	assert(t > 0, "time")
}

fn test_main(){
	test_strings()
	test_collections()
	test_io_fs()
	test_json()
	test_regex()
	test_url()
	test_base64()
	test_math_iter()
	test_os()
	print("✓ Runtime stdlib usage tests passed")
}

test_main()

fn test_bytes_and_base64(){
	print("Runtime edge: bytes/base64...")
	def b = bytes_from_str("hello")
	def hx = hex_encode(b)
	assert(bytes_len(hx) == 10, "hex encode len")
	def back = hex_decode("68656c6c6f")
	assert(bget(back, 0) == 104, "hex decode h")
	assert(bget(back, 4) == 111, "hex decode o")
	def enc = b64_encode("hello")
	def dec = b64_decode(enc)
	assert(eq(dec, "hello"), "base64 roundtrip")
}

fn test_regex_and_url(){
	print("Runtime edge: regex/url...")
	assert(regex_match("^a.*z$", "abcz") == 1, "regex anchors")
	assert(regex_find("b.*z", "xxabcz") == 3, "regex find")
	def q = parse_query("x=1&y=2")
	assert(getitem(q, "x", 0) == "1", "parse_query x")
}

fn test_path_norm(){
	print("Runtime edge: path normalize...")
	def n = normalize("/a/./b/../c/")
	assert(n == "/a/c", "normalize")
}

fn test_main(){
	test_bytes_and_base64()
	test_regex_and_url()
	test_path_norm()
	print("✓ Runtime stdlib edge usage passed")
}

test_main()
