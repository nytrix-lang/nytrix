;; Keywords: image bmp bitmap
;; BitMap (BMP) Image Loader and Encoder for Nytrix
;; Reference:
;; - https://en.wikipedia.org/wiki/BMP_file_format
;; - https://www.rfc-editor.org/rfc/rfc7854.html
module std.parse.img.bmp(decode, encode)
use std.core
use std.core.dict_mod
use std.math.simmd as simmd
use std.os.path as ospath

fn _ctz32(int: x): int {
   if(x == 0){ return 0 }
   mut v, n = x, 0
   while((v & 1) == 0){
      v = v >> 1
      n += 1
   }
   n
}

fn _chan_from_mask(int: px, int: mask): int {
   if(mask == 0){ return 0 }
   def sh = _ctz32(mask)
   def raw = (px & mask) >> sh
   def m = mask >> sh
   if(m <= 0){ return 0 }
   ((raw * 255) + (m / 2)) / m
}

fn _bgr_to_rgba(int: p): int {
   ((p & 0xFF) << 16) | (p & 0xFF00FF00) | ((p >> 16) & 0xFF)
}

fn _bmp_header(any: data): dict {
   if(!is_str(data) || data.len < 54){ return {"ok": false} }
   if(load8(data, 0) != 66 || load8(data, 1) != 77){ return {"ok": false} }
   def offset = load32(data, 10)
   def hdr_size = load32(data, 14)
   if(hdr_size < 40){ return {"ok": false} }
   def w, h = load32(data, 18), load32(data, 22)
   def planes = load16(data, 26)
   def bpp = load16(data, 28)
   def compression = load32(data, 30)
   if(planes != 1 || w <= 0){ return {"ok": false} }
   if(bpp != 1 && bpp != 4 && bpp != 8 && bpp != 16 && bpp != 24 && bpp != 32){ return {"ok": false} }
   if(compression != 0 && compression != 3){ return {"ok": false} }
   if(compression == 3 && bpp != 16 && bpp != 32){ return {"ok": false} }
   mut abs_h = h
   mut top_down = false
   if(h < 0){
      abs_h = -h
      top_down = true
   }
   if(abs_h <= 0){ return {"ok": false} }
   if(offset < 14 + hdr_size || offset > data.len){ return {"ok": false} }
   def bpl = ((w * bpp + 31) / 32) * 4
   if(bpl <= 0){ return {"ok": false} }
   if(offset + (abs_h - 1) * bpl + bpl > data.len){ return {"ok": false} }
   return {
      "ok": true, "offset": offset, "hdr_size": hdr_size, "width": w, "height": h,
      "abs_h": abs_h, "top_down": top_down, "bpp": bpp, "compression": compression,
      "colors_used": load32(data, 46), "bpl": bpl
   }
}

fn _bmp_masks(any: data, int: hdr_size, int: offset, int: compression): dict {
   mut r_mask, g_mask = 0, 0
   mut b_mask, a_mask = 0, 0
   if(compression == 3){
      def mask_off = 14 + 40
      if(hdr_size >= 52 || offset >= mask_off + 12){
         r_mask, g_mask = load32(data, mask_off), load32(data, mask_off + 4)
         b_mask = load32(data, mask_off + 8)
      } else {
         return {"ok": false}
      }
      if(hdr_size >= 56 || offset >= mask_off + 16){ a_mask = load32(data, mask_off + 12) }
      if(r_mask == 0 || g_mask == 0 || b_mask == 0){ return {"ok": false} }
   }
   return {"ok": true, "r": r_mask, "g": g_mask, "b": b_mask, "a": a_mask}
}

fn _bmp_palette_entries(int: bpp, int: palette_off, int: offset, int: colors_used): int {
   if(bpp > 8){ return 0 }
   if(palette_off >= offset){ return -1 }
   def max_entries = (offset - palette_off) / 4
   if(max_entries <= 0){ return -1 }
   mut palette_entries = colors_used
   if(palette_entries <= 0 || palette_entries > max_entries){ palette_entries = max_entries }
   palette_entries > 0 ? palette_entries : -1
}

fn _bmp_bgra_shuffle(): any {
   def bgra_shuffle = malloc(16)
   if(bgra_shuffle){
      store8(bgra_shuffle, 2, 0) store8(bgra_shuffle, 1, 1) store8(bgra_shuffle, 0, 2) store8(bgra_shuffle, 3, 3)
      store8(bgra_shuffle, 6, 4) store8(bgra_shuffle, 5, 5) store8(bgra_shuffle, 4, 6) store8(bgra_shuffle, 7, 7)
      store8(bgra_shuffle, 10, 8) store8(bgra_shuffle, 9, 9) store8(bgra_shuffle, 8, 10) store8(bgra_shuffle, 11, 11)
      store8(bgra_shuffle, 14, 12) store8(bgra_shuffle, 13, 13) store8(bgra_shuffle, 12, 14) store8(bgra_shuffle, 15, 15)
   }
   bgra_shuffle
}

fn _bmp_store_row24(any: data, any: pixels, int: src_row, int: dst_row, int: w): bool {
   def n4 = w / 4
   mut xi = 0
   while(xi < n4){
      def s, d = src_row + xi * 12, dst_row + xi * 16
      def p0 = load8(data,s)    | (load8(data,s+1)<<8)  | (load8(data,s+2)<<16)  | 0xFF000000
      def p1 = load8(data,s+3)  | (load8(data,s+4)<<8)  | (load8(data,s+5)<<16)  | 0xFF000000
      def p2 = load8(data,s+6)  | (load8(data,s+7)<<8)  | (load8(data,s+8)<<16)  | 0xFF000000
      def p3 = load8(data,s+9)  | (load8(data,s+10)<<8) | (load8(data,s+11)<<16) | 0xFF000000
      store32(pixels, _bgr_to_rgba(p0), d)
      store32(pixels, _bgr_to_rgba(p1), d+4)
      store32(pixels, _bgr_to_rgba(p2), d+8)
      store32(pixels, _bgr_to_rgba(p3), d+12)
      xi += 1
   }
   mut xr = n4 * 4
   while(xr < w){
      def s, d = src_row + xr * 3, dst_row + xr * 4
      store8(pixels, load8(data, s+2), d)
      store8(pixels, load8(data, s+1), d+1)
      store8(pixels, load8(data, s),   d+2)
      store8(pixels, 255,               d+3)
      xr += 1
   }
   true
}

fn _bmp_store_row32(any: data, any: pixels, any: bgra_shuffle, int: src_row, int: dst_row, int: w): bool {
   def n4 = w / 4
   mut xi = 0
   while(xi < n4){
      def s, d = src_row + xi * 16, dst_row + xi * 16
      if(bgra_shuffle){
         simmd.u8x16_shuffle_ptr(ptr_add(data, s), bgra_shuffle, ptr_add(pixels, d))
      } else {
         store32(pixels, _bgr_to_rgba(load32(data, s)),    d)
         store32(pixels, _bgr_to_rgba(load32(data, s+4)),  d+4)
         store32(pixels, _bgr_to_rgba(load32(data, s+8)),  d+8)
         store32(pixels, _bgr_to_rgba(load32(data, s+12)), d+12)
      }
      xi += 1
   }
   mut xr = n4 * 4
   while(xr < w){
      def s, d = src_row + xr * 4, dst_row + xr * 4
      store8(pixels, load8(data, s+2), d)
      store8(pixels, load8(data, s+1), d+1)
      store8(pixels, load8(data, s),   d+2)
      store8(pixels, load8(data, s+3), d+3)
      xr += 1
   }
   true
}

fn _bmp_store_scalar_row(any: data, any: pixels, int: src_row, int: dst_row, int: w, int: bpp, int: compression, int: palette_entries, int: palette_off, int: r_mask, int: g_mask, int: b_mask, int: a_mask): bool {
   mut x = 0
   while(x < w){
      def dst = dst_row + x * 4
      if(bpp <= 8){
         mut pal_idx = 0
         if(bpp == 8){ pal_idx = load8(data, src_row + x) } elif(bpp == 4){
            def nibble_byte = load8(data, src_row + (x / 2))
            if((x & 1) == 0){ pal_idx = (nibble_byte >> 4) & 15 } else { pal_idx = nibble_byte & 15 }
         } else {
            def bit_byte = load8(data, src_row + (x / 8))
            pal_idx = (bit_byte >> (7 - (x & 7))) & 1
         }
         if(pal_idx < 0 || pal_idx >= palette_entries){ pal_idx = 0 }
         def pe = palette_off + pal_idx * 4
         store8(pixels, load8(data, pe+2), dst)
         store8(pixels, load8(data, pe+1), dst+1)
         store8(pixels, load8(data, pe),   dst+2)
         store8(pixels, 255,                dst+3)
      } else {
         mut px = 0
         mut rr, gg, bb, aa = 0, 0, 0, 255
         if(bpp == 16){
            px = load16(data, src_row + x * 2)
            if(compression == 0){
               rr = _chan_from_mask(px, 0x7C00)
               gg = _chan_from_mask(px, 0x03E0)
               bb = _chan_from_mask(px, 0x001F)
            } else {
               rr = _chan_from_mask(px, r_mask)
               gg = _chan_from_mask(px, g_mask)
               bb = _chan_from_mask(px, b_mask)
               if(a_mask != 0){ aa = _chan_from_mask(px, a_mask) }
            }
         } else {
            px = load32(data, src_row + x * 4)
            rr = _chan_from_mask(px, r_mask)
            gg = _chan_from_mask(px, g_mask)
            bb = _chan_from_mask(px, b_mask)
            if(a_mask != 0){ aa = _chan_from_mask(px, a_mask) }
         }
         store8(pixels, rr, dst)
         store8(pixels, gg, dst+1)
         store8(pixels, bb, dst+2)
         store8(pixels, aa, dst+3)
      }
      x += 1
   }
   true
}

fn decode(any: data): any {
   "Decodes a BMP image from a byte string."
   def header = _bmp_header(data)
   if(!bool(header.get("ok", false))){ return 0 }
   def offset = int(header.get("offset", 0))
   def hdr_size = int(header.get("hdr_size", 0))
   def w = int(header.get("width", 0))
   def abs_h = int(header.get("abs_h", 0))
   def top_down = bool(header.get("top_down", false))
   def bpp = int(header.get("bpp", 0))
   def compression = int(header.get("compression", 0))
   def bpl = int(header.get("bpl", 0))
   def masks = _bmp_masks(data, hdr_size, offset, compression)
   if(!bool(masks.get("ok", false))){ return 0 }
   def r_mask, g_mask = int(masks.get("r", 0)), int(masks.get("g", 0))
   def b_mask, a_mask = int(masks.get("b", 0)), int(masks.get("a", 0))
   def palette_off = 14 + hdr_size
   def palette_entries = _bmp_palette_entries(bpp, palette_off, offset, int(header.get("colors_used", 0)))
   if(palette_entries < 0){ return 0 }
   def pixels = init_str(malloc(w * abs_h * 4), w * abs_h * 4)
   memset(pixels, 0, w * abs_h * 4)
   def bgra_shuffle = _bmp_bgra_shuffle()
   mut y = 0
   while(y < abs_h){
      mut src_y = abs_h - 1 - y
      if(top_down){ src_y = y }
      def src_row = offset + (src_y * bpl)
      def dst_row = y * w * 4
      if(src_row + bpl > data.len){
         if(bgra_shuffle){ free(bgra_shuffle) }
         return 0
      }
      if(bpp == 24 && compression == 0){
         _bmp_store_row24(data, pixels, src_row, dst_row, w)
      } elif(bpp == 32 && compression == 0){
         _bmp_store_row32(data, pixels, bgra_shuffle, src_row, dst_row, w)
      } else {
         _bmp_store_scalar_row(data, pixels, src_row, dst_row, w, bpp, compression, palette_entries, palette_off, r_mask, g_mask, b_mask, a_mask)
      }
      y += 1
   }
   mut res = dict(4)
   res = res.set("data", pixels)
   res = res.set("width", w)
   res = res.set("height", abs_h)
   res = res.set("channels", 4)
   if(bgra_shuffle){ free(bgra_shuffle) }
   res
}

fn encode(dict: img): str {
   "Encodes an image dictionary(width, height, data) into a BMP byte string."
   def w, h = img.get("width"), img.get("height")
   def pixels = img.get("data")
   def bpp = 24
   def bpl = ((w * bpp + 31) / 32) * 4
   def data_size = bpl * h
   def total_size = 54 + data_size
   mut out = malloc(total_size)
   if(!out){ return "" }
   init_str(out, total_size)
   store8(out, 66, 0) store8(out, 77, 1)
   store32(out, total_size, 2)
   store32(out, 0, 6)
   store32(out, 54, 10)
   store32(out, 40, 14)
   store32(out, w, 18)
   store32(out, h, 22)
   store16(out, 1, 26)
   store16(out, bpp, 28)
   store32(out, 0, 30)
   store32(out, data_size, 34)
   store32(out, 0, 38) store32(out, 0, 42)
   store32(out, 0, 46) store32(out, 0, 50)
   mut y = 0
   while(y < h){
      def src_row = (h - 1 - y) * w * 4
      def dst_row = 54 + (y * bpl)
      def n4 = w / 4
      mut xi = 0
      while(xi < n4){
         def s, d = src_row + xi * 16, dst_row + xi * 12
         store8(out, load8(pixels, s+2), d)
         store8(out, load8(pixels, s+1), d+1)
         store8(out, load8(pixels, s),   d+2)
         store8(out, load8(pixels, s+6), d+3)
         store8(out, load8(pixels, s+5), d+4)
         store8(out, load8(pixels, s+4), d+5)
         store8(out, load8(pixels, s+10), d+6)
         store8(out, load8(pixels, s+9),  d+7)
         store8(out, load8(pixels, s+8),  d+8)
         store8(out, load8(pixels, s+14), d+9)
         store8(out, load8(pixels, s+13), d+10)
         store8(out, load8(pixels, s+12), d+11)
         xi += 1
      }
      mut xr = n4 * 4
      while(xr < w){
         def s, d = src_row + xr * 4, dst_row + xr * 3
         store8(out, load8(pixels, s+2), d)
         store8(out, load8(pixels, s+1), d+1)
         store8(out, load8(pixels, s),   d+2)
         xr += 1
      }
      y += 1
   }
   out
}
