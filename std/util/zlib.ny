;; Keywords: compression zlib gzip deflate
;; Zlib/gzip helpers in Ny.

module std.util.zlib (
   available, error,
   is_zlib, is_gzip,
   parse_zlib_header, parse_gzip_header,
   decompress, decompress_zlib, decompress_gzip,
   compress, compress_zlib
)
use std.core *
use std.core.dict *
use std.math.hash as hash
use std.os *
use std.os.ffi *
use std.os.sys *
use std.os.time *
use std.str *

mut _lib = 0
mut _uncompress = 0
mut _compress2 = 0
mut _compressBound = 0
mut _gzopen = 0
mut _gzread = 0
mut _gzclose = 0
mut _error = ""

fn error(){
   "Returns the last zlib/gzip error message (empty on success)."
   _error
}

fn _set_error(msg){
   "Internal: sets the error message and returns an empty string."
   _error = msg
   ""
}

fn _u16_le(s, i){
   "Internal: reads a little-endian uint16 from string `s` at offset `i`."
   return (load8(s, i) | (load8(s, i + 1) << 8)) & 65535
}
fn _u32_le(s, i){
   "Internal: reads a little-endian uint32 from string `s` at offset `i`."
   return (load8(s, i) | (load8(s, i + 1) << 8) | (load8(s, i + 2) << 16) | (load8(s, i + 3) << 24)) & 4294967295
}
fn _u32_be(s, i){
   "Internal: reads a big-endian uint32 from string `s` at offset `i`."
   return (load8(s, i) << 24) | (load8(s, i + 1) << 16) | (load8(s, i + 2) << 8) | load8(s, i + 3)
}

fn _ok_map(){
   "Internal: returns an ok map `{ok: true}`."
   mut m = dict(4)
   m = dict_set(m, "ok", true)
   m
}

fn _err_map(kind, msg){
   "Internal: returns an error map `{ok: false, error: kind, message: msg}`."
   mut m = dict(4)
   m = dict_set(m, "ok", false)
   m = dict_set(m, "error", kind)
   m = dict_set(m, "message", msg)
   m
}

fn _load_sym(name){
   "Internal: resolves a symbol from the loaded libz handle."
   dlsym(_lib, name)
}

fn _init(){
   "Internal: loads libz and caches required symbols."
   if(_lib != 0){
      if(!_uncompress){ _uncompress = _load_sym("uncompress") }
      if(!_compress2){ _compress2 = _load_sym("compress2") }
      if(!_compressBound){ _compressBound = _load_sym("compressBound") }
      if(!_gzopen){ _gzopen = _load_sym("gzopen") }
      if(!_gzread){ _gzread = _load_sym("gzread") }
      if(!_gzclose){ _gzclose = _load_sym("gzclose") }
      return !!_uncompress && !!_compress2 && !!_compressBound
   }
   _lib = __dlopen("/usr/lib/libz.so.1", RTLD_NOW() | RTLD_LOCAL())
   if(_lib == 0){ _lib = __dlopen("libz.so.1", RTLD_NOW() | RTLD_LOCAL()) }
   if(_lib == 0){
      _lib = __dlopen("/usr/lib/libz.so", RTLD_NOW() | RTLD_LOCAL())
      if(_lib == 0){ _lib = __dlopen("libz.so", RTLD_NOW() | RTLD_LOCAL()) }
   }
   if(_lib == 0){ return false }
   _uncompress = _load_sym("uncompress")
   _compress2 = _load_sym("compress2")
   _compressBound = _load_sym("compressBound")
   _gzopen = _load_sym("gzopen")
   _gzread = _load_sym("gzread")
   _gzclose = _load_sym("gzclose")
   !!_uncompress && !!_compress2 && !!_compressBound
}

fn available(){
   "Returns true if libz symbols are available; sets `error()` otherwise."
   if(_init()){ _error = "" return true }
   _error = "libz symbols unavailable"
   false
}

fn is_gzip(s){
   "Returns true if string `s` has a gzip magic header."
   is_str(s) && str_len(s) >= 2 && load8(s, 0) == 31 && load8(s, 1) == 139
}

fn is_zlib(s){
   "Returns true if string `s` has a valid zlib header."
   if(!is_str(s) || str_len(s) < 2){ return false }
   def cmf = load8(s, 0)
   def flg = load8(s, 1)
   if((cmf & 15) != 8){ return false }
   if(((cmf >> 4) & 15) > 7){ return false }
   (((cmf << 8) + flg) % 31) == 0
}

fn parse_zlib_header(s){
   "Parses a zlib header and returns an ok/error map."
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
      dictid = _u32_be(s, 2)
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
   "Parses a gzip header and returns an ok/error map."
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
      def xlen = _u16_le(s, p)
      p = p + 2
      if(p + xlen > n){ return _err_map("format", "truncated gzip extra") }
      p = p + xlen
   }
   if((flg & 8) != 0){
      while(p < n && load8(s, p) != 0){ p = p + 1 }
      if(p >= n){ return _err_map("format", "truncated gzip filename") }
      p = p + 1
   }
   if((flg & 16) != 0){
      while(p < n && load8(s, p) != 0){ p = p + 1 }
      if(p >= n){ return _err_map("format", "truncated gzip comment") }
      p = p + 1
   }
   if((flg & 2) != 0){
      if(p + 2 > n){ return _err_map("format", "truncated gzip hdr crc") }
      p = p + 2
   }
   mut m = _ok_map()
   m = dict_set(m, "flags", flg)
   m = dict_set(m, "mtime", _u32_le(s, 4))
   m = dict_set(m, "xfl", load8(s, 8))
   m = dict_set(m, "os", load8(s, 9))
   m = dict_set(m, "header_size", p)
   m
}

fn _uncompress_zlib_data(s){
   "Internal: decompresses a zlib stream body using libz."
   if(!_init()){ return _set_error("libz unavailable") }
   def n = str_len(s)
   mut out_cap = n * 3 + 256
   if(out_cap < 256){ out_cap = 256 }
   mut out = malloc(out_cap + 1)
   if(out == 0){ return _set_error("output alloc failed") }
   def out_len_p = zalloc(8)
   if(out_len_p == 0){ free(out) return _set_error("output len alloc failed") }
   while(true){
      store32(out_len_p, out_cap, 0)
      def r = call4(_uncompress, out, out_len_p, s, n)
      if(r == 0){ break }
      if(r == -5){
         out_cap = out_cap * 2
         def grown = realloc(out, out_cap + 1)
         if(grown == 0){ free(out_len_p) free(out) return _set_error("output realloc failed") }
         out = grown
      } else {
         free(out_len_p)
         free(out)
         return _set_error("uncompress failed: " + to_str(r))
      }
   }
   def out_len = load32(out_len_p, 0)
   free(out_len_p)
   if(out_len < 0 || out_len > out_cap){ free(out) return _set_error("invalid uncompressed size") }
   store8(out, 0, out_len)
   init_str(out, out_len)
   _error = ""
   out
}

fn decompress_zlib(s){
   "Decompresses a zlib stream and validates the Adler-32 checksum."
   _error = ""
   def h = parse_zlib_header(s)
   if(!dict_get(h, "ok", false)){
      return _set_error(dict_get(h, "message", "invalid zlib header"))
   }
   if(dict_get(h, "fdict", false)){ return _set_error("preset dictionary zlib stream unsupported") }
   def out = _uncompress_zlib_data(s)
   if(str_len(_error) > 0){ return "" }
   def n = str_len(s)
   if(n < 6){ free(out) return _set_error("truncated zlib stream") }
   def expect_adler = _u32_be(s, n - 4)
   def got_adler = hash.adler32(out, 0, str_len(out))
   if(got_adler != expect_adler){ free(out) return _set_error("zlib adler32 mismatch") }
   out
}

fn _gzip_tmp_path(){
   "Internal: returns a temporary gzip file path."
   "/tmp/ny_gzip_" + to_str(syscall(39)) + "_" + to_str(ticks()) + ".gz"
}

fn decompress_gzip(s){
   "Decompresses a gzip stream and validates CRC/ISIZE when present."
   _error = ""
   def h = parse_gzip_header(s)
   if(!dict_get(h, "ok", false)){
      return _set_error(dict_get(h, "message", "invalid gzip header"))
   }
   if(!_init()){ return _set_error("libz unavailable") }
   if(!_gzopen || !_gzread || !_gzclose){ return _set_error("gzip symbols unavailable") }
   def path = _gzip_tmp_path()
   match file_write(path, s) {
      err(_) -> { return _set_error("failed to stage gzip data") }
      ok(_) -> { 0 }
   }
   def gz = call2(_gzopen, path, "rb")
   if(gz == 0){
      syscall(87, path, 0,0,0,0,0)
      return _set_error("gzopen failed")
   }
   mut out = ""
   def buf = malloc(65537)
   if(buf == 0){
      call1(_gzclose, gz)
      syscall(87, path, 0,0,0,0,0)
      return _set_error("gzip read buffer alloc failed")
   }
   mut loops = 0
   while(true){
      loops = loops + 1
      if(loops > 10000){
         free(buf)
         call1(_gzclose, gz)
         syscall(87, path, 0,0,0,0,0)
         return _set_error("gzread loop limit reached")
      }
      def r = call3(_gzread, gz, buf, 65536)
      def rr = to_int(r)
      if(rr > 65536){
         free(buf)
         call1(_gzclose, gz)
         syscall(87, path, 0,0,0,0,0)
         return _set_error("gzread returned invalid size")
      }
      if(rr < 0){
         free(buf)
         call1(_gzclose, gz)
         syscall(87, path, 0,0,0,0,0)
         return _set_error("gzread failed")
      }
      if(rr == 0){ break }
      store8(buf, 0, rr)
      init_str(buf, rr)
      out = out + buf
   }
   free(buf)
   call1(_gzclose, gz)
   syscall(87, path, 0,0,0,0,0)
   def n = str_len(s)
   if(n >= 8){
      def expect_crc = _u32_le(s, n - 8)
      def expect_isize = _u32_le(s, n - 4)
      def got_crc = hash.crc32(out, 0, str_len(out))
      if(got_crc != expect_crc){ return _set_error("gzip crc32 mismatch") }
      if((str_len(out) & 4294967295) != expect_isize){ return _set_error("gzip isize mismatch") }
   }
   _error = ""
   out
}

fn decompress(s){
   "Auto-detects gzip/zlib and decompresses `s`."
   _error = ""
   if(is_gzip(s)){ return decompress_gzip(s) }
   if(is_zlib(s)){ return decompress_zlib(s) }
   _set_error("unknown compression container")
}

fn compress_zlib(s, level=6){
   "Compresses `s` into a zlib stream at compression `level` (-1..9)."
   _error = ""
   if(!_init()){ return _set_error("libz unavailable") }
   if(!is_str(s)){ s = to_str(s) }
   if(level < -1){ level = -1 }
   if(level > 9){ level = 9 }
   def n = str_len(s)
   def bound = call1(_compressBound, n)
   if(bound <= 0){ return _set_error("compressBound failed") }
   def out = malloc(bound + 1)
   if(out == 0){ return _set_error("compress output alloc failed") }
   def out_len_p = zalloc(8)
   if(out_len_p == 0){ free(out) return _set_error("compress len alloc failed") }
   store32(out_len_p, bound, 0)
   def res = call5(_compress2, out, out_len_p, s, n, level)
   if(res != 0){
      free(out_len_p)
      free(out)
      return _set_error("compress2 failed: " + to_str(res))
   }
   def out_len = load32(out_len_p, 0)
   free(out_len_p)
   if(out_len < 0 || out_len > bound){ free(out) return _set_error("compress produced invalid size") }
   store8(out, 0, out_len)
   init_str(out, out_len)
   out
}

fn compress(s, level=6){
   "Alias for `compress_zlib`."
   compress_zlib(s, level)
}
