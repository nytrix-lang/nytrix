;; Keywords: data compression lz4 binding
;; LZ4 compression for Nytrix, bound to the system liblz4.
;; Block format (fast and HC levels) and the LZ4 frame format (self-describing,
;; interoperable with the `lz4` command-line tool).
;; References:
;; - https://github.com/lz4/lz4/blob/dev/doc/lz4_Block_format.md
;; - https://github.com/lz4/lz4/blob/dev/doc/lz4_Frame_format.md
module core.math.parse.data.lz4(available, version, error, compress_bound, compress, compress_hc, decompress, frame_bound, compress_frame, decompress_frame, HC_DEFAULT, HC_MAX)
use core

def int HC_DEFAULT = 9
def int HC_MAX = 12
#linux {
   #link "liblz4.so"
   extern "lz4" {
      fn LZ4_versionNumber() i32
      fn LZ4_compressBound(i32 input_size) i32
      fn LZ4_compress_fast(ptr src, ptr dst, i32 src_size, i32 dst_cap, i32 acceleration) i32
      fn LZ4_compress_HC(ptr src, ptr dst, i32 src_size, i32 dst_cap, i32 level) i32
      fn LZ4_decompress_safe(ptr src, ptr dst, i32 src_size, i32 dst_cap) i32
      fn LZ4F_compressFrameBound(u64 src_size, ptr prefs) u64
      fn LZ4F_compressFrame(ptr dst, u64 dst_cap, ptr src, u64 src_size, ptr prefs) u64
      fn LZ4F_isError(u64 code) u32
      fn LZ4F_getErrorName(u64 code) ptr
      fn LZ4F_createDecompressionContext(ptr dctx_out, u32 version) u64
      fn LZ4F_freeDecompressionContext(ptr dctx) u64
      fn LZ4F_decompress(ptr dctx, ptr dst, ptr dst_size, ptr src, ptr src_size, ptr opts) u64
   }
   def bool _have_lz4 = true
} #else {
   def bool _have_lz4 = false
   fn LZ4_versionNumber() int { 0 }
   fn LZ4_compressBound(int n) int { 0 }
   fn LZ4_compress_fast(..._args) int { 0 }
   fn LZ4_compress_HC(..._args) int { 0 }
   fn LZ4_decompress_safe(..._args) int { -1 }
   fn LZ4F_compressFrameBound(..._args) int { 0 }
   fn LZ4F_compressFrame(..._args) int { 0 }
   fn LZ4F_isError(..._args) int { 1 }
   fn LZ4F_getErrorName(..._args) int { 0 }
   fn LZ4F_createDecompressionContext(..._args) int { 1 }
   fn LZ4F_freeDecompressionContext(..._args) int { 0 }
   fn LZ4F_decompress(..._args) int { 1 }
}

def int _LZ4F_VERSION = 100
def int _MAX_INPUT = 0x7E000000
mut _error = ""

fn available() bool {
   "Returns whether liblz4 is linked on this platform."
   _have_lz4
}

fn version() int {
   "Returns the liblz4 version number(major*10000 + minor*100 + release)."
   int(LZ4_versionNumber())
}

fn error() str {
   "Returns the last error message, or an empty string after success."
   _error
}

fn _fail(str msg) str {
   _error = msg
   ""
}

fn _bytes_out(ptr buf, int n) str {
   def out = init_str(buf, n)
   store8(out, 0, n)
   _error = ""
   out
}

fn compress_bound(int n) int {
   "Returns the largest block-compressed size for an input of `n` bytes."
   int(LZ4_compressBound(n))
}

fn compress(any s, int acceleration=1) str {
   "Compresses `s` into one LZ4 block. Higher `acceleration` is faster and compresses less.
   The block holds no length: keep the original size for `decompress`."
   def n = s.len
   if n > _MAX_INPUT { return _fail("input too large for an LZ4 block") }
   def cap = compress_bound(n)
   if cap <= 0 && n > 0 { return _fail("lz4 unavailable") }
   def buf = malloc(cap + 1)
   if !buf { return _fail("output alloc failed") }
   def wrote = int(LZ4_compress_fast(to_int(s), buf, n, cap, acceleration < 1 ? 1 : acceleration))
   if wrote <= 0 && n > 0 {
      free(buf)
      return _fail("LZ4_compress_fast failed")
   }
   _bytes_out(buf, wrote)
}

fn compress_hc(any s, int level=HC_DEFAULT) str {
   "Compresses `s` into one LZ4 block with the high-compression encoder(`level` 3..12)."
   def n = s.len
   if n > _MAX_INPUT { return _fail("input too large for an LZ4 block") }
   def cap = compress_bound(n)
   if cap <= 0 { return _fail("lz4 unavailable") }
   def buf = malloc(cap + 1)
   if !buf { return _fail("output alloc failed") }
   def lv = level < 1 ? HC_DEFAULT : (level > HC_MAX ? HC_MAX : level)
   def wrote = int(LZ4_compress_HC(to_int(s), buf, n, cap, lv))
   if wrote <= 0 && n > 0 {
      free(buf)
      return _fail("LZ4_compress_HC failed")
   }
   _bytes_out(buf, wrote)
}

fn decompress(any s, int original_size) str {
   "Decompresses one LZ4 block whose decompressed size is `original_size`."
   if original_size < 0 { return _fail("negative original size") }
   if original_size == 9223372036854775807 { return _fail("output size overflow") }
   def buf = malloc(original_size + 1)
   if !buf { return _fail("output alloc failed") }
   def got = int(LZ4_decompress_safe(to_int(s), buf, s.len, original_size))
   if got < 0 || got != original_size {
      free(buf)
      return _fail("malformed LZ4 block or wrong original size")
   }
   _bytes_out(buf, got)
}

fn frame_bound(int n) int {
   "Returns the largest frame-compressed size for an input of `n` bytes."
   int(LZ4F_compressFrameBound(n, 0))
}

fn compress_frame(any s) str {
   "Compresses `s` into a self-describing LZ4 frame(readable by `lz4 -d`)."
   def n = s.len
   def cap = frame_bound(n)
   if cap <= 0 { return _fail("lz4 unavailable") }
   def buf = malloc(cap + 1)
   if !buf { return _fail("output alloc failed") }
   def wrote = LZ4F_compressFrame(buf, cap, to_int(s), n, 0)
   if LZ4F_isError(wrote) {
      free(buf)
      return _fail("LZ4F_compressFrame: " + cstr_to_str(LZ4F_getErrorName(wrote)))
   }
   _bytes_out(buf, int(wrote))
}

fn decompress_frame(any s, int max_out=268435456) str {
   "Decompresses a complete LZ4 frame. Output grows as needed up to `max_out` bytes."
   if max_out < 0 { return _fail("negative max_out") }
   if s.len > 9223372036854775807 / 4 { return _fail("input size overflow") }
   def ctx_slot = zalloc(8)
   if !ctx_slot { return _fail("context alloc failed") }
   if LZ4F_isError(LZ4F_createDecompressionContext(ctx_slot, _LZ4F_VERSION)) {
      free(ctx_slot)
      return _fail("LZ4F_createDecompressionContext failed")
   }
   def dctx = load64_h(ctx_slot, 0)
   free(ctx_slot)
   mut cap = s.len * 4
   if cap < 4096 { cap = 4096 }
   if cap > max_out { cap = max_out }
   mut out = malloc(cap + 1)
   def sizes = zalloc(16)
   if !out || !sizes {
      LZ4F_freeDecompressionContext(dctx)
      if out { free(out) }
      if sizes { free(sizes) }
      return _fail("output alloc failed")
   }
   mut used = 0
   mut consumed = 0
   def int total = s.len
   def base = to_int(s)
   mut done = false
   mut failed = ""
   while !done {
      if cap - used < 65536 {
         if cap >= max_out { failed = "frame output exceeds max_out" break }
         if cap > 9223372036854775807 / 2 {
            failed = "output size overflow"
            break
         }
         cap = cap * 2
         if cap > max_out { cap = max_out }
         def grown = realloc(out, cap + 1)
         if !grown { failed = "output realloc failed" break }
         out = grown
      }
      store64(sizes, cap - used, 0)
      store64(sizes, total - consumed, 8)
      def hint = LZ4F_decompress(dctx, out + used, sizes, base + consumed, sizes + 8, 0)
      if LZ4F_isError(hint) {
         failed = "LZ4F_decompress: " + cstr_to_str(LZ4F_getErrorName(hint))
         break
      }
      def int wrote = int(load64(sizes, 0))
      def int read = int(load64(sizes, 8))
      used += wrote
      consumed += read
      if int(hint) == 0 { done = true }
      elif consumed >= total && wrote == 0 { failed = "truncated LZ4 frame" break }
   }
   free(sizes)
   LZ4F_freeDecompressionContext(dctx)
   if failed != "" {
      free(out)
      return _fail(failed)
   }
   _bytes_out(out, used)
}

#main {
   if !available() {
      print("lz4 unavailable on this platform; skipping self-test")
   } else {
      mut text = ""
      mut i = 0
      while i < 400 {
         text = text + "nytrix lz4 round trip " + to_str(i % 17) + " "
         i += 1
      }
      def packed = compress(text)
      assert(packed.len > 0 && packed.len < text.len, "block compresses repetitive text")
      assert_eq(decompress(packed, text.len), text, "block round trip")
      def hc = compress_hc(text, HC_MAX)
      assert(hc.len > 0 && hc.len <= packed.len, "HC is at least as small as fast")
      assert_eq(decompress(hc, text.len), text, "HC round trip")
      assert_eq(decompress(packed, text.len - 1), "", "wrong size is rejected")
      assert(error() != "", "wrong size sets an error")
      def frame = compress_frame(text)
      assert(frame.len > 4 && load8(frame, 0) == 0x04 && load8(frame, 1) == 0x22, "frame magic")
      assert_eq(decompress_frame(frame), text, "frame round trip")
      assert_eq(decompress_frame("not a frame"), "", "garbage frame is rejected")
      assert_eq(decompress(compress(""), 0), "", "empty block round trip")
      print("✓ core.math.parse.data.lz4 self-test passed")
   }
}
