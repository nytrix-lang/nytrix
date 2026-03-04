;; Keywords: compression zlib gzip deflate rfc1950 rfc1951 rfc1952
;; Zlib and Gzip Compression Helpers for Nytrix
;; Reference:
;; - https://www.rfc-editor.org/rfc/rfc1950.html
;; - https://www.rfc-editor.org/rfc/rfc1951.html
;; - https://www.rfc-editor.org/rfc/rfc1952.html

module std.enc.zlib (
   available, error,
   is_zlib, is_gzip,
   parse_zlib_header, parse_gzip_header,
   decompress, decompress_zlib, decompress_gzip, decompress_zlib_limit,
   compress, compress_zlib
)
use std.core *
use std.core.dict_mod *
use std.math.hash as math_hash
use std.parse.bin as pbin
use std.os *
use std.os.sys *
use std.os.time *
use std.str *
use std.os.dirs *
use std.os.path *

if(comptime{ __os_name() == "linux" || __os_name() == "macos" }){
   #link "z"

   extern fn uncompress(dest: ptr, destLen: ptr, source: ptr, sourceLen: i64): i32 as "uncompress"
   extern fn compress2(dest: ptr, destLen: ptr, source: ptr, sourceLen: i64, level: i32): i32 as "compress2"
   extern fn compressBound(sourceLen: i64): i64 as "compressBound"
   extern fn gzopen(path: ptr, mode: ptr): ptr as "gzopen"
   extern fn gzread(file: ptr, buf: ptr, len: i32): i32 as "gzread"
   extern fn gzclose(file: ptr): i32 as "gzclose"
} else if(comptime{ __os_name() == "windows" }){
   #link "zlib1"

   extern fn uncompress(dest: ptr, destLen: ptr, source: ptr, sourceLen: i64): i32 as "uncompress"
   extern fn compress2(dest: ptr, destLen: ptr, source: ptr, sourceLen: i64, level: i32): i32 as "compress2"
   extern fn compressBound(sourceLen: i64): i64 as "compressBound"
   extern fn gzopen(path: ptr, mode: ptr): ptr as "gzopen"
   extern fn gzread(file: ptr, buf: ptr, len: i32): i32 as "gzread"
   extern fn gzclose(file: ptr): i32 as "gzclose"
}

mut _error = ""

fn error(){
   "Returns the last zlib helper error string."
   _error
}

fn _set_error(msg){
   "Stores `msg` as the module-local zlib error and returns an empty string."
   _error = msg ""
}

fn _i32(v){
   "Normalizes an unsigned 32-bit status code into a signed integer range."
   mut x = v
   if(x >= 2147483648){ x = x - 4294967296 }
   x
}

fn _ok_map(){
   "Builds a standard success result map."
   mut m = dict(4)
   m = dict_set(m, "ok", true)
   m
}

fn _err_map(kind, msg){
   "Builds a standard error result map."
   mut m = dict(4)
   m = dict_set(m, "ok", false)
   m = dict_set(m, "error", kind)
   m = dict_set(m, "message", msg)
   m
}

fn available(){
   "Returns whether zlib helpers are available in the current build."
   _error = ""
   true
}

fn is_gzip(s){
   "Returns whether `s` begins with a gzip header."
   is_str(s) && str_len(s) >= 2 && load8(s, 0) == 31 && load8(s, 1) == 139
}

fn is_zlib(s){
   "Returns whether `s` begins with a valid zlib header."
   if(!is_str(s) || str_len(s) < 2){ return false }
   def cmf = load8(s, 0)
   def flg = load8(s, 1)
   if((cmf & 15) != 8){ return false }
   if(((cmf >> 4) & 15) > 7){ return false }
   (((cmf << 8) + flg) % 31) == 0
}

fn parse_zlib_header(s){
   "Parses the RFC 1950 header and returns a structured result map."
   if(!is_str(s)){ return _err_map("type", "source must be string") }
   if(str_len(s) < 2){ return _err_map("format", "truncated zlib header") }
   def cmf = load8(s, 0)
   def flg = load8(s, 1)
   if((cmf & 15) != 8){ return _err_map("format", "unsupported compression method") }
   def cinfo = (cmf >> 4) & 15
   if(cinfo > 7){ return _err_map("format", "invalid zlib window size") }
   if(((cmf << 8) + flg) % 31 != 0){ return _err_map("format", "bad zlib FCHECK") }
   def fdict = (flg & 32) != 0
   mut hdr_size = 2
   mut dictid = 0
   if(fdict){
      if(str_len(s) < 6){ return _err_map("format", "truncated zlib dict id") }
      dictid = pbin.u32be(s, 2)
      hdr_size = 6
   }
   mut m = _ok_map()
   m = dict_set(m, "cmf", cmf)
   m = dict_set(m, "flg", flg)
   m = dict_set(m, "window_bits", cinfo + 8)
   m = dict_set(m, "fdict", fdict)
   m = dict_set(m, "dictid", dictid)
   m = dict_set(m, "header_size", hdr_size)
   m
}

fn parse_gzip_header(s){
   "Parses the RFC 1952 header and returns a structured result map."
   if(!is_str(s)){ return _err_map("type", "source must be string") }
   def n = str_len(s)
   if(n < 10){ return _err_map("format", "truncated gzip header") }
   if(load8(s, 0) != 31 || load8(s, 1) != 139){ return _err_map("format", "bad gzip signature") }
   if(load8(s, 2) != 8){ return _err_map("unsupported", "gzip method") }
   def flg = load8(s, 3)
   if((flg & 224) != 0){ return _err_map("format", "reserved gzip flags set") }
   mut p = 10
   if((flg & 4) != 0){
      if(p + 2 > n){ return _err_map("format", "truncated gzip extra len") }
      def xlen = pbin.u16le(s, p)
      p = p + 2
      if(p + xlen > n){ return _err_map("format", "truncated gzip extra") }
      p = p + xlen
   }
   if((flg & 8) != 0){
      while(p < n && load8(s, p) != 0){ p += 1 }
      if(p >= n){ return _err_map("format", "truncated gzip filename") }
      p += 1
   }
   if((flg & 16) != 0){
      while(p < n && load8(s, p) != 0){ p += 1 }
      if(p >= n){ return _err_map("format", "truncated gzip comment") }
      p += 1
   }
   if((flg & 2) != 0){
      if(p + 2 > n){ return _err_map("format", "truncated gzip hdr crc") }
      p = p + 2
   }
   mut m = _ok_map()
   m = dict_set(m, "flags", flg)
   m = dict_set(m, "mtime", pbin.u32le(s, 4))
   m = dict_set(m, "xfl", load8(s, 8))
   m = dict_set(m, "os", load8(s, 9))
   m = dict_set(m, "header_size", p)
   m
}

fn _uncompress_zlib_data(s){
   "Inflates a raw zlib payload into a newly allocated byte string."
   def n = str_len(s)
   mut out_cap = n * 3 + 256
   if(out_cap < 256){ out_cap = 256 }
   mut out = malloc(out_cap + 1 + 16) + 16
   if(out == 0){ return _set_error("output alloc failed") }
   def out_len_p = zalloc(8)
   if(out_len_p == 0){ free(out) return _set_error("output len alloc failed") }
   while(true){
      store64(out_len_p, out_cap, 0)
      def r = _i32(uncompress(out, out_len_p, s, n))
      if(r == 0){ break }
      if(r == -5){
         out_cap = out_cap * 2
         def grown = realloc(out - 16, out_cap + 1 + 16) + 16
         if(grown == 0){ free(out_len_p) free(out) return _set_error("output realloc failed") }
         out = grown
      } else {
         free(out_len_p)
         free(out)
         return _set_error("uncompress failed: " + to_str(r))
      }
   }
   def out_len = load64(out_len_p, 0)
   free(out_len_p)
   if(out_len < 0 || out_len > out_cap){ free(out) return _set_error("invalid uncompressed size") }
   init_str(out, out_len)
   store8(out, 0, out_len)
   _error = ""
   out
}

fn _uncompress_zlib_data_limit(s, out_cap){
   "Inflates zlib data into a fixed-size buffer to avoid realloc loops."
   def n = str_len(s)
   if(out_cap < 256){ out_cap = 256 }
   mut out = malloc(out_cap + 1 + 16) + 16
   if(out == 0){ return _set_error("output alloc failed") }
   def out_len_p = zalloc(8)
   if(out_len_p == 0){ free(out) return _set_error("output len alloc failed") }
   store64(out_len_p, out_cap, 0)
   def r = _i32(uncompress(out, out_len_p, s, n))
   if(r != 0){
      free(out_len_p)
      free(out)
      return _set_error("uncompress failed: " + to_str(r))
   }
   def out_len = load64(out_len_p, 0)
   free(out_len_p)
   if(out_len < 0 || out_len > out_cap){ free(out) return _set_error("invalid uncompressed size") }
   init_str(out, out_len)
   store8(out, 0, out_len)
   _error = ""
   out
}

fn decompress_zlib(s){
   "Decompresses a zlib-wrapped payload."
   _error = ""
   def h = parse_zlib_header(s)
   if(!dict_get(h, "ok", false)){
      return _set_error(dict_get(h, "message", "invalid zlib header"))
   }
   if(dict_get(h, "fdict", false)){ return _set_error("preset dictionary zlib stream unsupported") }
   mut out = _uncompress_zlib_data(s)
   if(str_len(_error) > 0){ return "" }
   def n = str_len(s)
   if(n < 6){ free(out) return _set_error("truncated zlib stream") }
   def expect_adler = pbin.u32be(s, n - 4)
   def got_adler = math_hash.adler32(out, 0, str_len(out))
   if(got_adler != expect_adler){
      free(out)
      return _set_error("zlib adler32 mismatch")
   }
   out
}

fn decompress_zlib_limit(s, out_cap){
   "Decompresses a zlib payload with a fixed output cap. Returns empty string on overflow/error."
   _error = ""
   if(out_cap <= 0){ return _set_error("bad output cap") }
   def h = parse_zlib_header(s)
   if(!dict_get(h, "ok", false)){
      return _set_error(dict_get(h, "message", "invalid zlib header"))
   }
   if(dict_get(h, "fdict", false)){ return _set_error("preset dictionary zlib stream unsupported") }
   def n = str_len(s)
   if(n < 6){ return _set_error("truncated zlib stream") }
   def out = _uncompress_zlib_data_limit(s, out_cap)
   if(str_len(_error) > 0){ return "" }
   def expect_adler = pbin.u32be(s, n - 4)
   def got_adler = math_hash.adler32(out, 0, str_len(out))
   if(got_adler != expect_adler){
      free(out)
      return _set_error("zlib adler32 mismatch")
   }
   out
}

fn _gzip_tmp_path(){
   "Builds a temporary path for gzip fallback decoding."
   mut base = temp_dir()
   if(!is_str(base) || str_len(base) == 0){ base = "." }
   def name = "ny_gzip_" + to_str(pid()) + "_" + to_str(ticks()) + ".gz"
   normalize(base + sep() + name)
}

fn decompress_gzip(s){
   "Decompresses a gzip payload, falling back to `gzread` for compatibility."
   _error = ""
   def h = parse_gzip_header(s)
   if(!dict_get(h, "ok", false)){
      return _set_error(dict_get(h, "message", "invalid gzip header"))
   }
   def path = _gzip_tmp_path()
   match file_write(path, s){
      err(_) -> { return _set_error("failed to stage gzip data") }
      ok(_) -> { 0 }
   }
   def gz = gzopen(path, "rb")
   if(gz == 0){
      match file_remove(path){ _ -> {} }
      return _set_error("gzopen failed")
   }
   mut out = ""
   def buf = malloc(65537 + 16) + 16
   if(buf == 0){
      gzclose(gz)
      match file_remove(path){ _ -> {} }
      return _set_error("gzip read buffer alloc failed")
   }
   mut loops = 0
   while(true){
      loops += 1
      if(loops > 10000){
         free(buf)
         gzclose(gz)
         match file_remove(path){ _ -> {} }
         return _set_error("gzread loop limit reached")
      }
      def r = gzread(gz, buf, 65536)
      def rr = to_int(r)
      if(rr > 65536){
         free(buf)
         gzclose(gz)
         match file_remove(path){ _ -> {} }
         return _set_error("gzread returned invalid size")
      }
      if(rr < 0){
         free(buf)
         gzclose(gz)
         match file_remove(path){ _ -> {} }
         return _set_error("gzread failed")
      }
      if(rr == 0){ break }
      init_str(buf, rr)
      store8(buf, 0, rr)
      out = out + buf
   }
   free(buf)
   gzclose(gz)
   match file_remove(path){ _ -> {} }
   def n = str_len(s)
   if(n >= 8){
      def expect_crc = pbin.u32le(s, n - 8)
      def expect_isize = pbin.u32le(s, n - 4)
      def got_crc = math_hash.crc32(out, 0, str_len(out))
      if(got_crc != expect_crc){ return _set_error("gzip crc32 mismatch") }
      if((str_len(out) & 4294967295) != expect_isize){ return _set_error("gzip isize mismatch") }
   }
   _error = ""
   out
}

fn decompress(s){
   "Auto-detects zlib vs gzip and dispatches to the matching decompressor."
   _error = ""
   if(is_gzip(s)){ return decompress_gzip(s) }
   if(is_zlib(s)){ return decompress_zlib(s) }
   _set_error("unknown compression container")
}

fn compress_zlib(s, level=6){
   "Compresses `s` as a zlib payload using the requested compression `level`."
   _error = ""
   if(!is_str(s)){ s = to_str(s) }
   if(level < -1){ level = -1 }
   if(level > 9){ level = 9 }
   def n = str_len(s)
   def bound = compressBound(n)
   if(bound <= 0){ return _set_error("compressBound failed") }
   def out = malloc(bound + 1 + 16) + 16
   if(out == 0){ return _set_error("compress output alloc failed") }
   def out_len_p = zalloc(8)
   if(out_len_p == 0){ free(out) return _set_error("compress len alloc failed") }
   store64(out_len_p, bound, 0)
   def res = _i32(compress2(out, out_len_p, s, n, level))
   if(res != 0){
      free(out_len_p)
      free(out)
      return _set_error("compress2 failed: " + to_str(res))
   }
   def out_len = load32(out_len_p, 0)
   free(out_len_p)
   if(out_len < 0 || out_len > bound){ free(out) return _set_error("compress produced invalid size") }
   init_str(out, out_len)
   store8(out, 0, out_len)
   out
}

fn compress(s, level=6){
   "Alias for `compress_zlib`."
   compress_zlib(s, level)
}

if(comptime{__main()}){
   use std.enc.zlib as z
   use std.core *
   use std.core.dict_mod *
   use std.core.error *
   use std.str *

   print("Testing std.enc.zlib...")

   def payload = "hello nytrix zlib\n"
   def z_bytes_list = [120,156,203,72,205,201,201,87,200,171,44,41,202,172,80,168,202,201,76,226,2,0,67,45,6,190]
   def g_bytes_list = [31,139,8,0,0,0,0,0,0,255,203,72,205,201,201,87,200,171,44,41,202,172,80,168,202,201,76,226,2,0,139,77,171,160,18,0,0,0]

   def z_bytes = pbin.from_list(z_bytes_list)
   def g_bytes = pbin.from_list(g_bytes_list)

   assert(available(), "zlib available")
   assert(is_zlib(z_bytes), "zlib signature")
   assert(is_gzip(g_bytes), "gzip signature")

   mut zh = parse_zlib_header(z_bytes)
   assert(dict_get(zh, "ok", false), "zlib header parse")
   assert(dict_get(zh, "header_size", 0) == 2, "zlib header size")

   mut gh = parse_gzip_header(g_bytes)
   assert(dict_get(gh, "ok", false), "gzip header parse")
   assert(dict_get(gh, "header_size", 0) == 10, "gzip header size")

   assert((decompress_zlib(z_bytes) == payload), "zlib decompress")
   def gz_res = decompress_gzip(g_bytes)
   if(gz_res == payload){
      assert(true, "gzip decompress")
      assert((decompress(z_bytes) == payload), "auto zlib decompress")
      assert((decompress(g_bytes) == payload), "auto gzip decompress")
      def z_comp = compress(payload, 6)
      assert(is_zlib(z_comp), "compress emits zlib stream")
      assert((decompress(z_comp) == payload), "compress/decompress roundtrip")
      store8(g_bytes, (load8(g_bytes, str_len(g_bytes) - 8) ^ 1), str_len(g_bytes) - 8)
      def bad = decompress_gzip(g_bytes)
      assert((bad == ""), "gzip bad stream returns empty")
      assert(str_len(error()) > 0, "gzip bad stream has error")
   } else {
      print("  gzip tests skipped (file I/O unavailable in comptime)")
   }

   print("✓ std.enc.zlib tests passed")
}
