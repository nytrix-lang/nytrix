;; Keywords: image gif lzw
;; References:
;; - https://en.wikipedia.org/wiki/GIF
;; - https://www.w3.org/Graphics/GIF/spec-gif89a.txt

module std.image.format.gif (
   decode, encode
)

use std.core *
use std.core.dict *

fn _u16le(s, i){
   "Internal helper for `u16le`."
   load8(s, i) | (load8(s, i + 1) << 8)
}

fn _mk_zero_list(n){
   "Internal helper for `mk_zero_list`."
   def xs = list(n)
   mut i = 0
   while(i < n){
      append(xs, 0)
      i += 1
   }
   xs
}

fn _list_to_bytes(xs){
   "Internal helper for `list_to_bytes`."
   def n = len(xs)
   def out = init_str(malloc(n + 1), n)
   mut i = 0
   while(i < n){
      store8(out, get(xs, i), i)
      i += 1
   }
   out
}

fn _gif_read_subblocks(data, p){
   "Internal helper for `gif_read_subblocks`."
   def n = len(data)
   mut q = p
   mut total = 0
   while(1){
      if(q >= n){ return 0 }
      def sz = load8(data, q)
      q += 1
      if(sz == 0){ break }
      if(q + sz > n){ return 0 }
      total += sz
      q += sz
   }
   def out = init_str(malloc(total + 1), total)
   mut w = 0
   q = p
   while(1){
      def sz = load8(data, q)
      q += 1
      if(sz == 0){ break }
      __copy_mem(out + w, data + q, sz)
      w += sz
      q += sz
   }
   mut res = dict(2)
   res = dict_set(res, "blob", out)
   res = dict_set(res, "next", q)
   res
}

fn _gif_lzw_decode(comp, min_code_size, out_len){
   "Internal helper for `gif_lzw_decode`."
   if(!is_str(comp) || out_len <= 0){ return 0 }
   if(min_code_size < 2 || min_code_size > 8){ return 0 }
   def clear = 1 << min_code_size
   def end_code = clear + 1
   def prefix = _mk_zero_list(4096)
   def suffix = _mk_zero_list(4096)
   def stack = _mk_zero_list(4096)
   mut bit_pos = 0
   mut code_size = min_code_size + 1
   mut next_code = end_code + 1
   mut old_code = -1
   mut out_p = 0
   def out = init_str(malloc(out_len + 1), out_len)
   fn _read_code(src, b_pos, c_size){
      "Internal helper for `read_code`."
      def bit_n = len(src) * 8
      if(b_pos + c_size > bit_n){ return [-1, b_pos] }
      mut v = 0
      mut i = 0
      while(i < c_size){
         def b = load8(src, (b_pos + i) / 8)
         v = v | (((b >> ((b_pos + i) & 7)) & 1) << i)
         i += 1
      }
      [v, b_pos + c_size]
   }
   while(out_p < out_len){
      def rr = _read_code(comp, bit_pos, code_size)
      def code = get(rr, 0)
      bit_pos = get(rr, 1)
      if(code < 0){ break }
      if(code == clear){
         code_size = min_code_size + 1
         next_code = end_code + 1
         old_code = -1
         continue
      }
      if(code == end_code){ break }
      if(old_code < 0){
         if(code >= clear){ return 0 }
         store8(out, code, out_p)
         out_p += 1
         old_code = code
         continue
      }
      mut in_code = code
      mut cur = code
      mut sp = 0
      mut special = false
      if(code == next_code){
         cur = old_code
         special = true
      } elif(code > next_code){
         return 0
      }
      while(cur >= clear){
         if(cur >= next_code || sp >= 4096){ return 0 }
         store_item(stack, sp, get(suffix, cur))
         sp += 1
         cur = get(prefix, cur)
      }
      if(cur < 0 || cur >= clear){ return 0 }
      def first = cur
      store_item(stack, sp, first)
      sp += 1
      if(special){
         if(sp >= 4096){ return 0 }
         store_item(stack, sp, first)
         sp += 1
      }
      while(sp > 0 && out_p < out_len){
         sp -= 1
         store8(out, get(stack, sp), out_p)
         out_p += 1
      }
      if(next_code < 4096){
         store_item(prefix, next_code, old_code)
         store_item(suffix, next_code, first)
         next_code += 1
         if(next_code == (1 << code_size) && code_size < 12){
            code_size += 1
         }
      }
      old_code = in_code
   }
   if(out_p != out_len){ return 0 }
   out
}

fn _gif_deinterlace(src, w, h){
   "Internal helper for `gif_deinterlace`."
   if(!is_str(src) || len(src) < w * h){ return 0 }
   def out = init_str(malloc(w * h + 1), w * h)
   mut p = 0
   def starts = [0, 4, 2, 1]
   def steps = [8, 8, 4, 2]
   mut pass = 0
   while(pass < 4){
      mut y = get(starts, pass)
      def st = get(steps, pass)
      while(y < h){
         mut x = 0
         while(x < w){
            if(p >= len(src)){ return 0 }
            store8(out, load8(src, p), y * w + x)
            p += 1
            x += 1
         }
         y += st
      }
      pass += 1
   }
   out
}

fn decode(data){
   "Decodes the first GIF frame into RGBA."
   if(!is_str(data) || len(data) < 13){ return 0 }
   if(load8(data, 0) != 71 || load8(data, 1) != 73 || load8(data, 2) != 70){ return 0 }
   def w = _u16le(data, 6)
   def h = _u16le(data, 8)
   if(w <= 0 || h <= 0){ return 0 }
   def packed = load8(data, 10)
   def gct_flag = (packed >> 7) & 1
   def gct_sz = 1 << ((packed & 7) + 1)
   mut p = 13
   mut gct = ""
   if(gct_flag){
      def n = gct_sz * 3
      if(p + n > len(data)){ return 0 }
      gct = init_str(malloc(n + 1), n)
      __copy_mem(gct, data + p, n)
      p += n
   }
   def canvas = init_str(malloc(w * h * 4 + 1), w * h * 4)
   memset(canvas, 0, w * h * 4)
   mut trans_valid = false
   mut trans_idx = 0
   mut got_frame = false
   while(p < len(data)){
      def b = load8(data, p)
      p += 1
      if(b == 0x3B){ break }
      if(b == 0x21){
         if(p >= len(data)){ return 0 }
         def label = load8(data, p)
         p += 1
         if(label == 0xF9){
            if(p + 6 > len(data)){ return 0 }
            def sz = load8(data, p)
            if(sz != 4){ return 0 }
            def gc_packed = load8(data, p + 1)
            trans_valid = (gc_packed & 1) != 0
            trans_idx = load8(data, p + 4)
            if(load8(data, p + 5) != 0){ return 0 }
            p += 6
         } else {
            if(p >= len(data)){ return 0 }
            def first_sz = load8(data, p)
            p += 1
            if(p + first_sz > len(data)){ return 0 }
            p += first_sz
            while(1){
               if(p >= len(data)){ return 0 }
               def sz = load8(data, p)
               p += 1
               if(sz == 0){ break }
               if(p + sz > len(data)){ return 0 }
               p += sz
            }
         }
         continue
      }
      if(b != 0x2C){ return 0 }
      if(p + 9 > len(data)){ return 0 }
      def left = _u16le(data, p)
      def top = _u16le(data, p + 2)
      def iw = _u16le(data, p + 4)
      def ih = _u16le(data, p + 6)
      def ipacked = load8(data, p + 8)
      p += 9
      if(iw <= 0 || ih <= 0){ return 0 }
      def lct_flag = (ipacked >> 7) & 1
      def interlace = (ipacked >> 6) & 1
      def lct_sz = 1 << ((ipacked & 7) + 1)
      mut pal = gct
      if(lct_flag){
         def n = lct_sz * 3
         if(p + n > len(data)){ return 0 }
         pal = init_str(malloc(n + 1), n)
         __copy_mem(pal, data + p, n)
         p += n
      }
      if(!is_str(pal) || len(pal) < 3){ return 0 }
      if(p >= len(data)){ return 0 }
      def min_code_size = load8(data, p)
      p += 1
      def sb = _gif_read_subblocks(data, p)
      if(!sb){ return 0 }
      def blob_comp = dict_get(sb, "blob")
      p = dict_get(sb, "next")
      def idx_raw = _gif_lzw_decode(blob_comp, min_code_size, iw * ih)
      if(!idx_raw){ return 0 }
      def idx = interlace ? _gif_deinterlace(idx_raw, iw, ih) : idx_raw
      if(!idx){ return 0 }
      mut y = 0
      while(y < ih){
         mut x = 0
         while(x < iw){
            def pi = load8(idx, y * iw + x)
            def pr = pi * 3
            mut r = 0 mut g = 0 mut bl = 0
            if(pr + 2 < len(pal)){
               r = load8(pal, pr)
               g = load8(pal, pr + 1)
               bl = load8(pal, pr + 2)
            }
            def dx = left + x
            def dy = top + y
            if(dx >= 0 && dx < w && dy >= 0 && dy < h){
               def off = (dy * w + dx) * 4
               store8(canvas, r, off)
               store8(canvas, g, off + 1)
               store8(canvas, bl, off + 2)
               def a = (trans_valid && pi == trans_idx) ? 0 : 255
               store8(canvas, a, off + 3)
            }
            x += 1
         }
         y += 1
      }
      got_frame = true
      break
   }
   if(!got_frame){ return 0 }
   mut out = dict(4)
   out = dict_set(out, "data", canvas)
   out = dict_set(out, "width", w)
   out = dict_set(out, "height", h)
   out = dict_set(out, "channels", 4)
   out
}

fn _gif_palette_332(){
   "Internal helper for `gif_palette_332`."
   def pal = init_str(malloc(256 * 3 + 1), 256 * 3)
   mut i = 0
   while(i < 256){
      def r3 = (i >> 5) & 7
      def g3 = (i >> 2) & 7
      def b2 = i & 3
      store8(pal, (r3 * 255) / 7, i * 3)
      store8(pal, (g3 * 255) / 7, i * 3 + 1)
      store8(pal, (b2 * 255) / 3, i * 3 + 2)
      i += 1
   }
   pal
}

fn _gif_lzw_pack_uncompressed(idx){
   "Internal helper for `gif_lzw_pack_uncompressed`."
   if(!is_str(idx)){ return 0 }
   def clear = 256
   def end_code = 257
   def out = list(len(idx) * 2)
   mut acc = 0
   mut bits = 0
   fn _put(p_out, p_acc, p_bits, code){
      "Internal helper for `put`."
      mut a = p_acc
      mut b = p_bits
      mut i = 0
      while(i < 9){
         a = a | (((code >> i) & 1) << b)
         b += 1
         if(b == 8){
            append(p_out, a & 255)
            a = 0
            b = 0
         }
         i += 1
      }
      [a, b]
   }
   mut i = 0
   while(i < len(idx)){
      def r1 = _put(out, acc, bits, clear)
      acc = get(r1, 0) bits = get(r1, 1)
      def r2 = _put(out, acc, bits, load8(idx, i))
      acc = get(r2, 0) bits = get(r2, 1)
      i += 1
   }
   def r3 = _put(out, acc, bits, clear)
   acc = get(r3, 0) bits = get(r3, 1)
   def r4 = _put(out, acc, bits, end_code)
   acc = get(r4, 0) bits = get(r4, 1)
   if(bits > 0){ append(out, acc & 255) }
   _list_to_bytes(out)
}

fn _out_u8(out, v){
   "Internal helper for `out_u8`."
   append(out, v & 255)
}
fn _out_u16le(out, v){
   "Internal helper for `out_u16le`."
   _out_u8(out, v) _out_u8(out, v >> 8)
}
fn _out_bytes(out, s){
   "Internal helper for `out_bytes`."
   mut i = 0
   while(i < len(s)){
      _out_u8(out, load8(s, i))
      i += 1
   }
}

fn encode(img){
   "Encodes image dict to GIF89a (single frame, fixed 256-color palette)."
   if(!is_dict(img)){ return 0 }
   def w = dict_get(img, "width", 0)
   def h = dict_get(img, "height", 0)
   def data = dict_get(img, "data", 0)
   mut ch = dict_get(img, "channels", 4)
   if(w <= 0 || h <= 0 || !is_str(data)){ return 0 }
   if(ch < 1){ ch = 1 } elif(ch > 4){ ch = 4 }
   if(len(data) < w * h * ch){ return 0 }
   def pal = _gif_palette_332()
   def idx = init_str(malloc(w * h + 1), w * h)
   mut has_trans = false
   mut i = 0
   while(i < w * h){
      def off = i * ch
      def r = load8(data, off)
      def g = (ch >= 3) ? load8(data, off + 1) : r
      def b = (ch >= 3) ? load8(data, off + 2) : r
      def a = (ch == 2 || ch == 4) ? load8(data, off + (ch - 1)) : 255
      if(a < 128){
         has_trans = true
         store8(idx, 0, i)
      } else {
         def pi = ((r >> 5) << 5) | ((g >> 5) << 2) | (b >> 6)
         store8(idx, pi, i)
      }
      i += 1
   }
   def lzw = _gif_lzw_pack_uncompressed(idx)
   if(!lzw){ return 0 }
   def out = list(4096)
   _out_u8(out, 71) _out_u8(out, 73) _out_u8(out, 70) _out_u8(out, 56) _out_u8(out, 57) _out_u8(out, 97)
   _out_u16le(out, w)
   _out_u16le(out, h)
   _out_u8(out, 0xF7)
   _out_u8(out, 0)
   _out_u8(out, 0)
   _out_bytes(out, pal)
   if(has_trans){
      _out_u8(out, 0x21) _out_u8(out, 0xF9) _out_u8(out, 4)
      _out_u8(out, 0x01)
      _out_u16le(out, 0)
      _out_u8(out, 0)
      _out_u8(out, 0)
   }
   _out_u8(out, 0x2C)
   _out_u16le(out, 0) _out_u16le(out, 0)
   _out_u16le(out, w) _out_u16le(out, h)
   _out_u8(out, 0)
   _out_u8(out, 8)
   mut p = 0
   while(p < len(lzw)){
      mut n = len(lzw) - p
      if(n > 255){ n = 255 }
      _out_u8(out, n)
      mut j = 0
      while(j < n){
         _out_u8(out, load8(lzw, p + j))
         j += 1
      }
      p += n
   }
   _out_u8(out, 0)
   _out_u8(out, 0x3B)
   _list_to_bytes(out)
}

if(comptime{__main()}){
   use std.core.error *
   def w = 4 def h = 4
   def px = init_str(malloc(w * h * 4), w * h * 4)
   mut i = 0
   while(i < w * h){
      def o = i * 4
      store8(px, (i * 40) & 255, o)
      store8(px, (i * 70) & 255, o + 1)
      store8(px, (i * 20) & 255, o + 2)
      store8(px, 255, o + 3)
      i += 1
   }
   mut img = dict(4)
   img = dict_set(img, "width", w)
   img = dict_set(img, "height", h)
   img = dict_set(img, "channels", 4)
   img = dict_set(img, "data", px)
   def enc = encode(img)
   assert(enc && len(enc) > 32, "gif encode")
   def dec = decode(enc)
   assert(dec != 0, "gif decode")
   assert(dict_get(dec, "width") == w, "gif width")
   assert(dict_get(dec, "height") == h, "gif height")
   print("✓ std.image.gif tests passed")
}
