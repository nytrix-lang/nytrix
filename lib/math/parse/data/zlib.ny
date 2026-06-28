;; Keywords: data serialization zlib deflate compression parse
;; Zlib Compression and Decompression Library for Nytrix
;; References:
;; - std.math.parse.data
;; - std.math.parse
module std.math.parse.data.zlib(decompress_zlib, compress_zlib, error, last_out_len, decompress_zlib_limit)
use std.core
use std.core.mem as core_mem
use std.os (env)
use std.os.prim

mut _error = ""
mut _last_out_len = 0

fn error() str {
   "Runs the error operation."
   _error
}

fn last_out_len() int {
   "Runs the last out len operation."
   _last_out_len
}

fn _set_error(str msg) str {
   _error = msg
   _last_out_len = 0
   ""
}

fn decompress_zlib_limit(any s, int out_cap) str {
   "Inflates zlib data into a fixed-size buffer to avoid realloc loops."
   def n = s.len
   if out_cap < 256 { out_cap = 256 }
   mut raw_buf = malloc(out_cap + 32)
   if raw_buf == 0 { return _set_error("output alloc failed") }
   def out_len_p = zalloc(8)
   if out_len_p == 0 { free(raw_buf) return _set_error("output len alloc failed") }
   store64_h(out_len_p, out_cap, 0)
   def r = __zlib_uncompress(raw_buf, out_len_p, to_int(s), n)
   if r != 0 {
      def err_msg = "uncompress failed: " + to_str(r)
      _set_error(err_msg)
      free(out_len_p, raw_buf)
      return ""
   }
   def out_len = load64_h(out_len_p, 0)
   free(out_len_p)
   if out_len < 0 || out_len > out_cap {
      free(raw_buf)
      return _set_error("invalid uncompressed size")
   }
   def out = init_str(raw_buf, out_len)
   store8(out, 0, out_len)
   _last_out_len = out_len
   _error = ""
   out
}

fn decompress_zlib(any s, int out_cap=0) str {
   "Inflates zlib data. If `out_cap` is 0, tries to guess or use incremental growth."
   if out_cap > 0 { return decompress_zlib_limit(s, out_cap) }
   def n = s.len
   mut cap = n * 4
   if cap < 4096 { cap = 4096 }
   mut raw_buf = malloc(cap + 32)
   if raw_buf == 0 { return _set_error("output alloc failed") }
   def out_len_p = zalloc(8)
   if out_len_p == 0 { free(raw_buf) return _set_error("output len alloc failed") }
   store64_h(out_len_p, cap, 0)
   mut r = __zlib_uncompress(raw_buf, out_len_p, to_int(s), n)
   while r == -5 {
      cap = cap * 2
      def next_buf = realloc(raw_buf, cap + 32)
      if next_buf == 0 {
         free(out_len_p, raw_buf)
         return _set_error("output realloc failed")
      }
      raw_buf = next_buf
      store64_h(out_len_p, cap, 0)
      r = __zlib_uncompress(raw_buf, out_len_p, to_int(s), n)
      if cap > 1024 * 1024 * 128 { break }
   }
   if r != 0 {
      free(out_len_p, raw_buf)
      return _set_error("uncompress failed: " + to_str(r))
   }
   def out_len = load64_h(out_len_p, 0)
   free(out_len_p)
   def out = init_str(raw_buf, out_len)
   store8(out, 0, out_len)
   _last_out_len = out_len
   _error = ""
   out
}

fn compress_zlib(any s, int level=6) str {
   "Deflates data using zlib."
   if level < -1 { level = -1 }
   if level > 9 { level = 9 }
   def n = s.len
   if env("NY_ZLIB_DEBUG") { print("[zlib] n=" + to_str(n)) }
   def out = __zlib_compress_str(to_int(s), n, level)
   if !out { return _set_error("compress failed") }
   if env("NY_ZLIB_DEBUG") { print("[zlib] out_len=" + to_str(out.len)) }
   _last_out_len = out.len
   _error = ""
   out
}

#main {
   def plain = "nytrix compression smoke abcabcabc"
   def packed = compress_zlib(plain)
   assert(packed.len > 0, "zlib compressed output")
   assert_eq(decompress_zlib(packed, 4096), plain, "zlib round trip")
   assert(error() == "", "zlib no error after round trip")
   print("✓ std.math.parse.data.zlib self-test passed")
}
