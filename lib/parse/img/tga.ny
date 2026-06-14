;; Keywords: image tga parse
;; Truevision TGA Image Loader and Encoder for Nytrix
;; Reference:
;; - https://en.wikipedia.org/wiki/Truevision_TGA
;; References:
;; - std.parse.img
;; - std.parse
module std.parse.img.tga(decode, encode, save)
use std.core
use std.core.dict_mod
use std.math.bin as pbin

fn decode(str data) any {
   "Decodes an uncompressed 24-bit or 32-bit TGA image."
   if data.len < 18 { return 0 }
   def id_len = load8(data, 0)
   def color_map_type = load8(data, 1)
   load8(data, 2)
   def w, h = pbin.u16le(data, 12), pbin.u16le(data, 14)
   def bpp = load8(data, 16)
   def desc = load8(data, 17)
   def flip_x = (desc >> 4) & 1
   def flip_y = (desc >> 5) & 1
   mut p = 18 + id_len
   if color_map_type == 1 {
      def map_len = pbin.u16le(data, 5)
      def map_entry_size = load8(data, 7)
      p += map_len * (map_entry_size / 8)
   }
   def tpx = w * h
   def pix = init_str(malloc(tpx * 4 + 1), tpx * 4)
   if !pix { return 0 }
   if bpp == 24 && !flip_x && !flip_y {
      mut y = 0
      while y < h {
         mut x = 0
         while x + 4 <= w {
            def src = p + (y * w + x) * 3
            def dst = (y * w + x) * 4
            def b0 = load8(data, src)
            def g0 = load8(data, src+1)
            def r0 = load8(data, src+2)
            def b1 = load8(data, src+3)
            def g1 = load8(data, src+4)
            def r1 = load8(data, src+5)
            def b2 = load8(data, src+6)
            def g2 = load8(data, src+7)
            def r2 = load8(data, src+8)
            def b3 = load8(data, src+9)
            def g3 = load8(data, src+10)
            def r3 = load8(data, src+11)
            store32(pix, r0|(g0<<8)|(b0<<16)|(255<<24), dst)
            store32(pix, r1|(g1<<8)|(b1<<16)|(255<<24), dst+4)
            store32(pix, r2|(g2<<8)|(b2<<16)|(255<<24), dst+8)
            store32(pix, r3|(g3<<8)|(b3<<16)|(255<<24), dst+12)
            x += 4
         }
         while x < w {
            def src = p + (y * w + x) * 3
            def dst = (y * w + x) * 4
            store32(pix, load8(data, src+2)|(load8(data, src+1)<<8)|(load8(data, src)<<16)|(255<<24), dst)
            x += 1
         }
         y += 1
      }
   } elif bpp == 32 && !flip_x && !flip_y {
      mut y = 0
      while y < h {
         mut x = 0
         while x < w {
            def src = p + (y * w + x) * 4
            def dst = (y * w + x) * 4
            store32(pix, load8(data, src+2)|(load8(data, src+1)<<8)|(load8(data, src)<<16)|(load8(data, src+3)<<24), dst)
            x += 1
         }
         y += 1
      }
   } else {
      mut y = 0
      while y < h {
         mut x = 0
         while x < w {
            mut ry = h - 1 - y
            if flip_y { ry = y }
            mut rx = x
            if flip_x { rx = w - 1 - x }
            def src_off = p + (y * w + x) * (bpp / 8)
            def dst_off = (ry * w + rx) * 4
            if bpp == 24 {
               store8(pix, load8(data, src_off + 2), dst_off)
               store8(pix, load8(data, src_off + 1), dst_off + 1)
               store8(pix, load8(data, src_off), dst_off + 2)
               store8(pix, 255, dst_off + 3)
            }
            elif bpp == 32 {
               store8(pix, load8(data, src_off + 2), dst_off)
               store8(pix, load8(data, src_off + 1), dst_off + 1)
               store8(pix, load8(data, src_off), dst_off + 2)
               store8(pix, load8(data, src_off + 3), dst_off + 3)
            }
            x += 1
         }
         y += 1
      }
   }
   mut res_d = dict(8)
   res_d["data"] = pix
   res_d["width"] = w
   res_d["height"] = h
   res_d["channels"] = 4
   res_d
}

fn encode(dict img) str {
   "Encodes an image dictionary as a 32-bit BGRA TGA byte string."
   def w, h = img.get("width"), img.get("height")
   def d = img.get("data")
   def ch = img.get("channels", 4)
   def hdr = init_str(malloc(19), 18)
   memset(hdr, 0, 18)
   store8(hdr, 2, 2)
   store8(hdr, w & 255, 12)
   store8(hdr, w >> 8, 13)
   store8(hdr, h & 255, 14)
   store8(hdr, h >> 8, 15)
   store8(hdr, 32, 16)
   store8(hdr, 40, 17)
   def out = init_str(malloc(w * h * 4 + 19), w * h * 4 + 18)
   __copy_mem(out, hdr, 18)
   mut y = 0
   while y < h {
      mut x = 0
      while x < w {
         def src_off = (y * w + x) * ch
         def dst_off = 18 + (y * w + x) * 4
         mut r, g = load8(d, src_off), r
         mut b, a = r, 255
         if ch >= 3 { g, b = load8(d, src_off + 1), load8(d, src_off + 2) }
         if ch == 4 { a = load8(d, src_off + 3) }
         store8(out, b, dst_off)
         store8(out, g, dst_off + 1)
         store8(out, r, dst_off + 2)
         store8(out, a, dst_off + 3)
         x += 1
      }
      y += 1
   }
   out
}

fn save(dict img, str path) Result {
   "Writes a 32-bit top-left TGA through the native snapshot fast path."
   def w = int(img.get("width", 0))
   def h = int(img.get("height", 0))
   def d = img.get("data", 0)
   def ch = int(img.get("channels", img.get("bpp", 4)))
   if !d || w <= 0 || h <= 0 || ch <= 0 { return err(-22) }
   def n = __save_tga_rgba(path, d, w, h, ch)
   if n >= 0 { return ok(n) }
   err(n)
}

#main {
   def w = 2
   def h = 2
   def data = init_str(malloc(w * h * 4 + 1), w * h * 4)
   store8(data, 255, 0)  store8(data, 0, 1)    store8(data, 0, 2)    store8(data, 255, 3)
   store8(data, 0, 4)    store8(data, 255, 5)  store8(data, 0, 6)    store8(data, 255, 7)
   store8(data, 0, 8)    store8(data, 0, 9)    store8(data, 255, 10)  store8(data, 255, 11)
   store8(data, 255, 12) store8(data, 255, 13) store8(data, 255, 14)  store8(data, 255, 15)
   def img = {"width": w, "height": h, "data": data, "channels": 4}
   def encoded = encode(img)
   assert(is_str(encoded) && encoded.len == 34, "tga encoded size")
   def decoded = decode(encoded)
   assert(is_dict(decoded) && decoded.get("width") == 2 && decoded.get("height") == 2 && decoded.get("channels") == 4, "tga decoded shape")
   match save(img, "build/tga-native-smoke.tga") {
      ok(n) -> { assert(n > 0, "tga native save bytes") }
      err(e) -> { panic("tga save failed: " + to_str(e)) }
   }
   print("✓ std.parse.img.tga self-test passed")
}
