use std.io
use std.util.msgpack
use std.core
use std.core.test
use std.strings.str
use std.collections.dict

print("Testing msgpack...")

fn test_encode_decode_int(){
   def n = 123
   def encoded = msgpack_encode(n)
   def decoded = msgpack_decode(encoded)
   assert(decoded == n, "encode/decode int")
   def n_neg = -123
   def encoded_neg = msgpack_encode(n_neg)
   def decoded_neg = msgpack_decode(encoded_neg)
   assert(decoded_neg == n_neg, "encode/decode negative int")
}

fn test_encode_decode_str(){
   def s = "hello"
   def encoded = msgpack_encode(s)
   def decoded = msgpack_decode(encoded)
   assert(eq(decoded, s), "encode/decode str")
   def s_long = "this is a long string that should be encoded with str 8"
   def encoded_long = msgpack_encode(s_long)
   def decoded_long = msgpack_decode(encoded_long)
   assert(eq(decoded_long, s_long), "encode/decode long str")
}

fn test_encode_decode_list(){
   def lst = [1, "two", 3]
   def encoded = msgpack_encode(lst)
   def decoded = msgpack_decode(encoded)
   assert(list_len(decoded) == 3, "decoded list length")
   assert(get(decoded, 0) == 1, "decoded list item 0")
   assert(eq(get(decoded, 1), "two"), "decoded list item 1")
   assert(get(decoded, 2) == 3, "decoded list item 2")
}

fn test_encode_decode_dict(){
   def d = dict()
   dict_set(d, "a", 1)
   dict_set(d, "b", "two")
   def encoded = msgpack_encode(d)
   def decoded = msgpack_decode(encoded)
   assert(list_len(decoded) == 2, "decoded dict length")
   assert(dict_get(decoded, "a") == 1, "decoded dict item 'a'")
   assert(eq(dict_get(decoded, "b"), "two"), "decoded dict item 'b'")
}

test_encode_decode_int()
test_encode_decode_str()
test_encode_decode_list()
test_encode_decode_dict()

print("✓ std.util.msgpack tests passed")
