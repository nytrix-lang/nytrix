use std.util.zlib as z
use std.core.dict *
use std.core.error *
use std.str *

;; std.util.zlib (Test)

fn _from_list(xs){
   def n = len(xs)
   def out = malloc(n + 1)
   init_str(out, n)
   mut i = 0
   while(i < n){
      store8(out, get(xs, i), i)
      i = i + 1
   }
   store8(out, 0, n)
   out
}

def payload = "hello nytrix zlib\n"
def z_bytes_list = [120,156,203,72,205,201,201,87,200,171,44,41,202,172,80,168,202,201,76,226,2,0,67,45,6,190]
def g_bytes_list = [31,139,8,0,0,0,0,0,0,255,203,72,205,201,201,87,200,171,44,41,202,172,80,168,202,201,76,226,2,0,139,77,171,160,18,0,0,0]

def z_bytes = _from_list(z_bytes_list)
def g_bytes = _from_list(g_bytes_list)

assert(z.available(), "zlib available")
assert(z.is_zlib(z_bytes), "zlib signature")
assert(z.is_gzip(g_bytes), "gzip signature")

mut zh = z.parse_zlib_header(z_bytes)
assert(dict_get(zh, "ok", false), "zlib header parse")
assert(dict_get(zh, "header_size", 0) == 2, "zlib header size")

mut gh = z.parse_gzip_header(g_bytes)
assert(dict_get(gh, "ok", false), "gzip header parse")
assert(dict_get(gh, "header_size", 0) == 10, "gzip header size")

assert(eq(z.decompress_zlib(z_bytes), payload), "zlib decompress")
assert(eq(z.decompress_gzip(g_bytes), payload), "gzip decompress")
assert(eq(z.decompress(z_bytes), payload), "auto zlib decompress")
assert(eq(z.decompress(g_bytes), payload), "auto gzip decompress")

def comp = z.compress(payload, 6)
assert(z.is_zlib(comp), "compress emits zlib stream")
assert(eq(z.decompress(comp), payload), "compress/decompress roundtrip")

;; Corrupt one byte in gzip trailer (CRC) and expect failure.
store8(g_bytes, bxor(load8(g_bytes, str_len(g_bytes) - 8), 1), str_len(g_bytes) - 8)
def bad = z.decompress_gzip(g_bytes)
assert(eq(bad, ""), "gzip bad stream returns empty")
assert(str_len(z.error()) > 0, "gzip bad stream has error")

print("✓ std.util.zlib tests passed")
