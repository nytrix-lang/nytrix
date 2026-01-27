use std.io
use std.util.msgpack
use std.strings.str
use std.collections.dict
use std.core.error

;; std.util.msgpack (Test)
;; Tests msgpack encode/decode for ints, strings, lists, dicts, and heap strings.

print("Testing msgpack...")

def n = 123
def enc_n = msgpack_encode(n)
assert(msgpack_decode(enc_n) == n, "int")

def n2 = -123
def enc_n2 = msgpack_encode(n2)
assert(msgpack_decode(enc_n2) == n2, "neg int")

def s = "hello"
def enc_s = msgpack_encode(s)
assert(eq(msgpack_decode(enc_s), s), "str")

def s_long = "this is a long string that should be encoded with str 8"
def enc_sl = msgpack_encode(s_long)
assert(eq(msgpack_decode(enc_sl), s_long), "long str")

def lst = [1, "two", 3]
def enc_l = msgpack_encode(lst)
def dec_l = msgpack_decode(enc_l)
assert(list_len(dec_l) == 3, "list len")
assert(get(dec_l, 0) == 1, "list 0")
assert(eq(get(dec_l, 1), "two"), "list 1")
assert(get(dec_l, 2) == 3, "list 2")

def d = dict()
dict_set(d, "a", 1)
dict_set(d, "b", "two")
def enc_d = msgpack_encode(d)
def dec_d = msgpack_decode(enc_d)
assert(dict_get(dec_d, "a") == 1, "dict a")
assert(eq(dict_get(dec_d, "b"), "two"), "dict b")

print("Testing Msgpack with Manual Heap Strings...")

def src = "this is a long string that should be encoded with str 8"
def l = str_len(src)
def hs = __malloc(l + 1)
__init_str(hs, l)

def i = 0
while(i < l){
 __store8_idx(hs, i, load8(src, i))
 i = i + 1
}
__store8_idx(hs, i, 0)

def enc_h = msgpack_encode(hs)
def dec_h = msgpack_decode(enc_h)
assert(eq(dec_h, hs), "heap str")

__free(hs)

print("✓ std.util.msgpack tests passed")
