use std.io
use std.util.json
use std.collections.dict
use std.core.error

;; std.util.json (Test)
;; Tests JSON encode and decode.

print("Testing JSON encode...")

assert(eq(json_encode(123), "123"), "encode int")
assert(eq(json_encode("hello"), "\"hello\""), "encode string")

def lst = list()
lst = append(lst, 1)
lst = append(lst, "a")
assert(eq(json_encode(lst), "[1,\"a\"]"), "encode list")

def d = dict()
dict_set(d, "k", "v")
assert(eq(json_encode(d), "{\"k\":\"v\"}"), "encode dict")

print("Testing JSON decode...")

assert(json_decode("123") == 123, "decode int")
assert(eq(json_decode("\"hello\""), "hello"), "decode string")

def lst2 = json_decode("[1, \"a\"]")
assert(list_len(lst2) == 2, "decode list len")
assert(get(lst2, 0) == 1, "decode list 0")
assert(eq(get(lst2, 1), "a"), "decode list 1")

def d2 = json_decode("{\"k\": \"v\"}")
assert(eq(dict_get(d2, "k", 0), "v"), "decode dict")

print("✓ std.util.json tests passed")
