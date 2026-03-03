;; Keywords: image gif lzw
;; Reference:
;; - https://en.wikipedia.org/wiki/GIF

module std.image.format.gif (
   decode, encode
)

use std.core *
use std.core.dict_mod *
use std.parse.bin as pbin

;; Decoder

fn _g_sb(data, p){
   "Reads a GIF sub-block chain starting at `p`."
   def n = len(data)
   mut q = p
   mut total = 0
   while(q < n){
      def sz = load8(data, q)
      q += 1
      if(sz == 0){
         break
      }
      total += sz
      q += sz
   }
   def out = init_str(malloc(total + 1 + 16) + 16, total)
   mut w = 0
   mut q2 = p
   while(q2 < n){
      def sz = load8(data, q2)
      q2 += 1
      if(sz == 0){
         break
      }
      __copy_mem(out + w, data + q2, sz)
      w += sz
      q2 += sz
   }
   mut res = dict(2)
   dict_set(res, "blob", out)
   dict_set(res, "next", q2)
   return res
}

fn _g_lzw_dec(comp, mcs, olen){
   "Decodes GIF LZW-compressed pixel indices."
   if(!is_str(comp) || olen <= 0){
      return 0
   }
   def clr = 1 << mcs
   def end_code = clr + 1
   def pre = pbin.zero_list(4096)
   def suf = pbin.zero_list(4096)
   def stk = pbin.zero_list(4096)
   mut bpos = 0
   mut csz = mcs + 1
   mut nextcode = end_code + 1
   mut oldcode = -1
   mut op = 0
   def out = init_str(malloc(olen + 1 + 16) + 16, olen)
   memset(out, 0, olen)
   while(op < olen){
      mut code = 0
      mut k = 0
      while(k < csz){
         def bit_off = bpos + k
         if(bit_off >= (len(comp) * 8)){
         code = -1
         break
         }
         def byte = load8(comp, bit_off / 8)
         if((byte >> (bit_off & 7)) & 1){
         code = code | (1 << k)
         }
         k += 1
      }
      bpos += csz
      if(code == -1 || code == end_code){
         break
      }
      if(code == clr){
         csz = mcs + 1
         nextcode = end_code + 1
         oldcode = -1
         continue
      }
      mut cur = code
      mut extra_char = -1
      if(code >= nextcode){
         if(oldcode == -1){
         break
         }
         cur = oldcode
         extra_char = 1
      }
      mut sp = 0
      mut xVal = cur
      while(xVal >= clr && sp < 4096){
         store_item(stk, sp, get(suf, xVal))
         sp += 1
         xVal = get(pre, xVal)
      }
      def first = xVal
      if(op < olen){
         store8(out, first, op)
         op += 1
      }
      while(sp > 0 && op < olen){
         sp -= 1
         store8(out, get(stk, sp), op)
         op += 1
      }
      if(extra_char == 1 && op < olen){
         store8(out, first, op)
         op += 1
      }
      if(oldcode != -1 && nextcode < 4096){
         store_item(pre, nextcode, oldcode)
         store_item(suf, nextcode, first)
         nextcode += 1
         if(nextcode == (1 << (csz)) && csz < 12){
         csz += 1
         }
      }
      oldcode = code
   }
   return out
}

fn _g_di(src, w, h){
   "Reorders interlaced GIF indices into scanline order."
   def out = init_str(malloc(w * h + 1 + 16) + 16, w * h)
   memset(out, 0, w * h)
   mut p = 0
   def sta = [0, 4, 2, 1]
   def ste = [8, 8, 4, 2]
   mut passcount = 0
   while(passcount < 4){
      mut y = get(sta, passcount)
      def st = get(ste, passcount)
      while(y < h){
         mut x = 0
         while(x < w){
         if(p < len(src)){
               store8(out, load8(src, p), y * w + x)
               p += 1
         }
         x += 1
         }
         y += st
      }
      passcount += 1
   }
   return out
}

fn decode(data){
   "Decodes a GIF image into the standard image dictionary shape."
   if(!is_str(data) || len(data) < 13){
      return 0
   }
   if(load8(data, 0) != 71 || load8(data, 1) != 73 || load8(data, 2) != 70){
      return 0
   }
   def w = pbin.u16le(data, 6)
   def h = pbin.u16le(data, 8)
   def flags = load8(data, 10)
   mut p = 13
   mut gaddr = -1
   if(flags & 128){
      gaddr = p
      p += (1 << ((flags & 7) + 1)) * 3
   }
   def canv = init_str(malloc(w * h * 4 + 1 + 16) + 16, (w * h * 4))
   memset(canv, 0, (w * h * 4))
   mut trans_idx = -1
   while(p < (len(data) - 1)){
      def bt = load8(data, p)
      p += 1
      if(bt == 0x3B){
         break
      }
      if(bt == 0x21){
         def ext_type = load8(data, p)
         p += 1
         if(ext_type == 0xF9){
         def sz = load8(data, p)
         if(sz == 4){
               def gce_f = load8(data, p + 1)
               if(gce_f & 1){
                  trans_idx = load8(data, p + 4)
               } else {
                  trans_idx = -1
               }
         }
         p += sz + 1
         while(p < len(data) && load8(data, p) != 0){
               p += (load8(data, p) + 1)
         }
         if(p < len(data)){
               p += 1
         }
         continue
         }
         while(p < len(data)){
         def sz_ext = load8(data, p)
         p += 1
         if(sz_ext == 0){
               break
         }
         p += sz_ext
         }
         continue
      }
      if(bt != 0x2C){
         continue
      }
      def fx = _u16le(data, p)
      def fy = _u16le(data, p + 2)
      def fw = _u16le(data, p + 4)
      def fh = _u16le(data, p + 6)
      def ffl = load8(data, p + 8)
      p += 9
      mut cp_addr = gaddr
      if(ffl & 128){
         cp_addr = p
         p += (1 << ((ffl & 7) + 1)) * 3
      }
      if(p >= len(data)){
         break
      }
      def mcs = load8(data, p)
      p += 1
      def blocks_res = _g_sb(data, p)
      if(!blocks_res){
         return 0
      }
      p = dict_get(blocks_res, "next")
      mut idx_data = _g_lzw_dec(dict_get(blocks_res, "blob"), mcs, (fw * fh))
      if(is_int(idx_data)){
         return 0
      }
      if(ffl & 64){
         idx_data = _g_di(idx_data, fw, fh)
      }
      mut y_loop = 0
      while(y_loop < fh){
         mut x_loop = 0
         while(x_loop < fw){
         if((fy + y_loop) < h && (fx + x_loop) < w){
               def pi = load8(idx_data, (y_loop * fw + x_loop))
               if(pi != trans_idx){
                  def off_pix = ((fy + y_loop) * w + (fx + x_loop)) * 4
                  if(cp_addr != -1){
                     store8(canv, load8(data, cp_addr + pi * 3), off_pix)
                     store8(canv, load8(data, cp_addr + pi * 3 + 1), off_pix + 1)
                     store8(canv, load8(data, cp_addr + pi * 3 + 2), off_pix + 2)
                     store8(canv, 255, off_pix + 3)
                  }
               }
         }
         x_loop += 1
         }
         y_loop += 1
      }
      trans_idx = -1
      break
   }
   mut out_dict = dict(4)
   dict_set(out_dict, "data", canv)
   dict_set(out_dict, "width", w)
   dict_set(out_dict, "height", h)
   dict_set(out_dict, "channels", 4)
   return out_dict
}

;; Encoder section

fn encode(img){
   "Encodes an indexed or RGBA image dictionary as a GIF byte string."
   def w = dict_get(img, "width")
   def h = dict_get(img, "height")
   def d = dict_get(img, "data")
   def ch = dict_get(img, "channels", 4)

   mut col_map = dict(256)
   mut pal_list = list(768)
   mut trans_idx_enc = -1
   mut n_cols = 0
   mut has_trans_enc = false

   mut iScan = 0
   while(iScan < (w * h)){
      if(ch >= 4){
         if(load8(d, iScan * ch + 3) < 128){
         has_trans_enc = true
         trans_idx_enc = 0
         n_cols = 1
         pal_list = append(pal_list, 0)
         pal_list = append(pal_list, 0)
         pal_list = append(pal_list, 0)
         break
         }
      }
      iScan += 1
   }

   mut iC = 0
   mut over = false
   while(iC < (w * h)){
      def off_c = iC * ch
      mut skip_p = false
      if(ch >= 4){
         if(load8(d, off_c + 3) < 128){
         skip_p = true
         }
      }

      if(!skip_p){
         def r = load8(d, off_c)
         def g = (ch >= 3) ? load8(d, off_c + 1) : r
         def b = (ch >= 3) ? load8(d, off_c + 2) : r
         def rgb = (r << 16) | (g << 8) | b
         if(!dict_has(col_map, rgb)){
         if(n_cols < 256){
               dict_set(col_map, rgb, n_cols)
               pal_list = append(pal_list, r)
               pal_list = append(pal_list, g)
               pal_list = append(pal_list, b)
               n_cols += 1
         } else {
               over = true
               iC = (w * h)
         }
         }
      }
      iC += 1
   }

   mut use_fix = over
   if(n_cols == 0){
      use_fix = true
   }

   def idx_buf = init_str(malloc(w * h + 1 + 16) + 16, (w * h))
   mut iC2 = 0
   while(iC2 < (w * h)){
      def off_c2 = iC2 * ch
      mut is_t = false
      if(ch >= 4){
         if(load8(d, off_c2 + 3) < 128){
         is_t = true
         }
      }

      if(is_t){
         store8(idx_buf, trans_idx_enc, iC2)
      } else {
         def r2 = load8(d, off_c2)
         def g2 = (ch >= 3) ? load8(d, off_c2 + 1) : r2
         def b2 = (ch >= 3) ? load8(d, off_c2 + 2) : r2
         if(use_fix){
         def ri = (r2 * 7 + 127) / 255
         def gi = (g2 * 7 + 127) / 255
         def bi = (b2 * 3 + 127) / 255
         mut clr_i = (ri << 5) | (gi << 2) | bi
         if(clr_i == trans_idx_enc && has_trans_enc){
               if(clr_i < 255){
                  clr_i += 1
               } else {
                  clr_i -= 1
               }
         }
         store8(idx_buf, clr_i, iC2)
         } else {
         def rgb2 = (r2 << 16) | (g2 << 8) | b2
         store8(idx_buf, dict_get(col_map, rgb2, 0), iC2)
         }
      }
      iC2 += 1
   }

   def clr_code = 256
   def end_code_lzw = 257
   mut out_l = list(w * h * 2)
   mut acc_bits = 0
   mut bits_count = 0
   mut csz_enc = 9
   mut next_code = 258

   mut kClr = 0
   while(kClr < csz_enc){
      if((clr_code >> kClr) & 1){
         acc_bits = acc_bits | (1 << bits_count)
      }
      bits_count += 1
      if(bits_count == 8){
         out_l = append(out_l, acc_bits)
         acc_bits = 0
         bits_count = 0
      }
      kClr += 1
   }

   mut dict_lzw = dict(4096)
   mut ent = load8(idx_buf, 0)
   mut iIdx = 1
   while(iIdx < (w * h)){
      def c_sym = load8(idx_buf, iIdx)
      def key = (ent << 16) | c_sym
      if(dict_has(dict_lzw, key)){
         ent = dict_get(dict_lzw, key)
      } else {
         mut kEnt = 0
         while(kEnt < csz_enc){
         if((ent >> kEnt) & 1){
               acc_bits = acc_bits | (1 << bits_count)
         }
         bits_count += 1
         if(bits_count == 8){
               out_l = append(out_l, acc_bits)
               acc_bits = 0
               bits_count = 0
         }
         kEnt += 1
         }
         if(next_code < 4096){
         dict_set(dict_lzw, key, next_code)
         next_code += 1
         if(next_code == (1 << csz_enc) + 1 && csz_enc < 12){
               csz_enc += 1
         }
         } else {
         dict_lzw = dict(4096)
         mut kClr2 = 0
         while(kClr2 < csz_enc){
               if((clr_code >> kClr2) & 1){
                  acc_bits = acc_bits | (1 << bits_count)
               }
               bits_count += 1
               if(bits_count == 8){
                  out_l = append(out_l, acc_bits)
                  acc_bits = 0
                  bits_count = 0
               }
               kClr2 += 1
         }
         csz_enc = 9
         next_code = 258
         }
         ent = c_sym
      }
      iIdx += 1
   }

   mut kFin = 0
   while(kFin < csz_enc){
      if((ent >> kFin) & 1){
         acc_bits = acc_bits | (1 << bits_count)
      }
      bits_count += 1
      if(bits_count == 8){
         out_l = append(out_l, acc_bits)
         acc_bits = 0
         bits_count = 0
      }
      kFin += 1
   }

   mut kEnd = 0
   while(kEnd < csz_enc){
      if((end_code_lzw >> kEnd) & 1){
         acc_bits = acc_bits | (1 << bits_count)
      }
      bits_count += 1
      if(bits_count == 8){
         out_l = append(out_l, acc_bits)
         acc_bits = 0
         bits_count = 0
      }
      kEnd += 1
   }
   if(bits_count > 0){
      out_l = append(out_l, acc_bits)
   }

   mut res_l = list(len(out_l) + 1024)
   res_l = append(res_l, 71)
   res_l = append(res_l, 73)
   res_l = append(res_l, 70)
   res_l = append(res_l, 56)
   res_l = append(res_l, 57)
   res_l = append(res_l, 97)
   res_l = append(res_l, (w & 255))
   res_l = append(res_l, (w >> 8))
   res_l = append(res_l, (h & 255))
   res_l = append(res_l, (h >> 8))

   if(use_fix){
      res_l = append(res_l, 247)
      res_l = append(res_l, 0)
      res_l = append(res_l, 0)
      mut iPal = 0
      while(iPal < 256){
         res_l = append(res_l, (iPal & 0xE0))
         res_l = append(res_l, ((iPal << 3) & 0xE0))
         res_l = append(res_l, ((iPal << 6) & 0xC0))
         iPal += 1
      }
   } else {
      mut psz = 0
      while((1 << (psz + 1)) < n_cols){ psz += 1 }
      res_l = append(res_l, (128 | psz))
      res_l = append(res_l, 0)
      res_l = append(res_l, 0)
      mut iPal2 = 0
      while(iPal2 < (1 << (psz + 1))){
         if((iPal2 * 3 + 2) < len(pal_list)){
         res_l = append(res_l, get(pal_list, iPal2 * 3))
         res_l = append(res_l, get(pal_list, iPal2 * 3 + 1))
         res_l = append(res_l, get(pal_list, iPal2 * 3 + 2))
         } else {
         res_l = append(res_l, 0)
         res_l = append(res_l, 0)
         res_l = append(res_l, 0)
         }
         iPal2 += 1
      }
   }

   if(has_trans_enc){
      res_l = append(res_l, 0x21)
      res_l = append(res_l, 0xF9)
      res_l = append(res_l, 4)
      res_l = append(res_l, 1)
      res_l = append(res_l, 0)
      res_l = append(res_l, 0)
      res_l = append(res_l, trans_idx_enc)
      res_l = append(res_l, 0)
   }

   res_l = append(res_l, 0x2C)
   res_l = append(res_l, 0)
   res_l = append(res_l, 0)
   res_l = append(res_l, 0)
   res_l = append(res_l, 0)
   res_l = append(res_l, (w & 255))
   res_l = append(res_l, (w >> 8))
   res_l = append(res_l, (h & 255))
   res_l = append(res_l, (h >> 8))
   res_l = append(res_l, 0)
   res_l = append(res_l, 8)

   mut pOut = 0
   while(pOut < len(out_l)){
      def remain = len(out_l) - pOut
      def sz_chunk = (remain > 255) ? 255 : remain
      res_l = append(res_l, sz_chunk)
      mut iChunk = 0
      while(iChunk < sz_chunk){
         res_l = append(res_l, get(out_l, pOut + iChunk))
         iChunk += 1
      }
      pOut += sz_chunk
   }
   res_l = append(res_l, 0)
   res_l = append(res_l, 0x3B)

   return pbin.from_list(res_l)
}
