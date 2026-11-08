;; Keywords: image webp parse
;; WebP decoder: optional libwebp via dlopen, with an ffmpeg VP8L fallback.
module core.math.parse.img.webp(decode)
use core
use core.dict_mod
use core.os (file_read, file_write, file_remove, temp_dir, ticks, pid)
use core.os.process as process
use core.os.path as path
use core.os.ffi as ffi
use core.str as str

mut _checked = false
mut _available = false
mut _lib = 0
mut _getinfo = 0
mut _decode_into = 0

fn _u32le(str s, int i) int {
   load8(s, i) | (load8(s, i + 1) << 8) | (load8(s, i + 2) << 16) | (load8(s, i + 3) << 24)
}

fn _valid_container(str data) bool {
   if data.len < 20 { return false }
   if str.str_slice(data, 0, 4) != "RIFF" || str.str_slice(data, 8, 12) != "WEBP" { return false }
   def size = _u32le(data, 4)
   size >= 12 && size <= data.len - 8
}

fn _libwebp_load() bool {
   if _checked { return _available }
   _checked = true
   mut lib = 0
   if comptime { __os_name() == "linux" } {
      lib = ffi.dlopen_checked("libwebp.so.7", "WebPGetInfo", ffi.RTLD_NOW())
      if !lib { lib = ffi.dlopen_checked("libwebp.so", "WebPGetInfo", ffi.RTLD_NOW()) }
   } elif comptime { __os_name() == "macos" } {
      lib = ffi.dlopen_checked("libwebp.7.dylib", "WebPGetInfo", ffi.RTLD_NOW())
      if !lib { lib = ffi.dlopen_checked("libwebp.dylib", "WebPGetInfo", ffi.RTLD_NOW()) }
   } else {
      lib = ffi.dlopen_checked("libwebp-7.dll", "WebPGetInfo", ffi.RTLD_NOW())
      if !lib { lib = ffi.dlopen_checked("libwebp.dll", "WebPGetInfo", ffi.RTLD_NOW()) }
   }
   if !lib { return false }
   def gi = ffi.dlsym(lib, "WebPGetInfo")
   def di = ffi.dlsym(lib, "WebPDecodeRGBAInto")
   if !gi || !di { return false }
   _lib = lib
   _getinfo = gi
   _decode_into = di
   _available = true
   true
}

fn _image(str rgba, int w, int h) dict {
   mut out = dict(4)
   out["data"] = rgba
   out["width"] = w
   out["height"] = h
   out["channels"] = 4
   out
}

fn _decode_native(str data) any {
   if !_libwebp_load() { return 0 }
   def wp = zalloc(4)
   def hp = zalloc(4)
   if !wp || !hp {
      if wp { free(wp) }
      if hp { free(hp) }
      return 0
   }
   def ok = ffi.any4(_getinfo, data, data.len, wp, hp)
   if int(ok) == 0 {
      free(wp, hp)
      return 0
   }
   def w = load32(wp, 0)
   def h = load32(hp, 0)
   free(wp, hp)
   if w < 1 || h < 1 || w > 16384 || h > 16384 || w * h > 16777216 { return 0 }
   def n = w * h * 4
   def buf = malloc(n + 32)
   if !buf { return 0 }
   def decoded = ffi.any5(_decode_into, data, data.len, buf, n, w * 4)
   if !decoded {
      free(buf)
      return 0
   }
   _image(init_str(buf, n), w, h)
}

fn _vp8l_dims(str data) list {
   if !_valid_container(data) { return [0, 0] }
   mut offset = 12
   def end = 8 + _u32le(data, 4)
   while offset + 8 <= end {
      def size = _u32le(data, offset + 4)
      if size < 0 || size > end - offset - 8 { return [0, 0] }
      if str.str_slice(data, offset, offset + 4) == "VP8L" {
         if size < 5 { return [0, 0] }
         def j = offset + 8
         if load8(data, j) != 0x2f || (load8(data, j + 4) >> 5) != 0 { return [0, 0] }
         def w = 1 + load8(data, j + 1) + ((load8(data, j + 2) & 63) << 8)
         def h = 1 + (load8(data, j + 2) >> 6) + (load8(data, j + 3) << 2) + ((load8(data, j + 4) & 15) << 10)
         if w < 1 || h < 1 || w > 16384 || h > 16384 || w * h > 16777216 { return [0, 0] }
         return [w, h]
      }
      offset += 8 + size + (size & 1)
   }
   [0, 0]
}

fn _decode_vp8l_fallback(str data) any {
   def dims = _vp8l_dims(data)
   def w = dims[0]
   def h = dims[1]
   if w == 0 || h == 0 { return 0 }
   def name = "ny_webp_" + to_str(pid()) + "_" + to_str(ticks())
   def input = path.join(temp_dir(), name + ".webp")
   def output = path.join(temp_dir(), name + ".rgba")
   defer {
      def _a = file_remove(input)
      def _b = file_remove(output)
   }
   def wr = file_write(input, data)
   if !is_ok(wr) { return 0 }
   def code = process.run("ffmpeg", ["-nostdin", "-v", "error", "-y", "-i", input, "-frames:v", "1", "-f", "rawvideo", "-pix_fmt", "rgba", output])
   if code != 0 { return 0 }
   def rr = file_read(output)
   if !is_ok(rr) { return 0 }
   def str raw = unwrap(rr)
   if raw.len != w * h * 4 { return 0 }
   _image(raw, w, h)
}

fn decode(any data) any {
   "Decode a WebP image into RGBA. libwebp is optional; the VP8L fallback needs ffmpeg."
   if !is_str(data) || !_valid_container(data) { return 0 }
   def disabled = str.lower(to_str(__env_get("NY_WEBP_DISABLE_LIBWEBP")))
   if disabled != "1" && disabled != "true" && disabled != "yes" && disabled != "on" {
      def native = _decode_native(data)
      if native { return native }
   }
   def fallback = _decode_vp8l_fallback(data)
   if fallback { return fallback }
   print("[webp] error: decoding requires libwebp or ffmpeg for VP8L; no synthetic output")
   0
}
