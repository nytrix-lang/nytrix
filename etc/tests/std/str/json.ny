use std.core *
use std.str.json *
use std.str *

;; std.str.json (Test)

def n = json_decode("123")
assert(n == 123, "json number")

def t = json_decode("true")
assert(t == 1, "json true")

def f = json_decode("false")
assert(f == 0, "json false")

def nul = json_decode("null")
assert(nul == 0, "json null")

def s = json_decode("\"hi\"")
assert(is_str(s), "json string")
assert(str_len(s) >= 2, "json string len")

def arr = json_decode("[1,2]")
assert(is_list(arr), "json array list")

def obj = json_decode("{\"a\":1}")
assert(is_dict(obj), "json object dict")

print("✓ std.str.json tests passed")
