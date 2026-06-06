;; Keywords: image gif parse
;; Graphics Interchange Format (GIF) Image Loader for Nytrix
;; Reference:
;; - https://en.wikipedia.org/wiki/GIF
;; References:
;; - std.parse.img
;; - std.parse
module std.parse.img.gif(decode, decode_frames, encode)
use std.core
use std.core.dict_mod
use std.math.bin as pbin

fn _g_sb(any data, int p) dict {
   def n = data.len
   mut q = p
   mut total = 0
   while(q < n){
      def sz = load8(data, q)
      q += 1
      if(sz == 0){ break }
      total += sz
      q += sz
   }
   def out = init_str(malloc(total + 1), total)
   mut w = 0
   mut q2 = p
   while(q2 < n){
      def sz = load8(data, q2)
      q2 += 1
      if(sz == 0){ break }
      __copy_mem(out + w, data + q2, sz)
      w += sz
      q2 += sz
   }
   mut res = dict(2)
   res = res.set("blob", out)
   res = res.set("next", q2)
   return res
}

fn _g_lzw_dec(any comp, int mcs, int olen) any {
   if(!is_str(comp) || olen <= 0){ return 0 }
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
   mut guard = 0
   def guard_limit = comp.len * 16 + olen * 32 + 8192
   def out = init_str(malloc(olen + 1), olen)
   memset(out, 0, olen)
   while(op < olen){
      guard += 1
      if(guard > guard_limit){ break }
      mut code = 0
      mut k = 0
      while(k < csz){
         def bit_off = bpos + k
         if(bit_off >= (comp.len * 8)){
            code = -1
            break
         }
         def byte = load8(comp, bit_off / 8)
         if((byte >> (bit_off & 7)) & 1){ code = code | (1 << k) }
         k += 1
      }
      bpos += csz
      if(code == -1 || code == end_code){ break }
      if(code == clr){
         csz = mcs + 1
         nextcode = end_code + 1
         oldcode = -1
         continue
      }
      mut cur = code
      mut extra_char = -1
      if(code >= nextcode){
         if(oldcode == -1){ break }
         cur = oldcode
         extra_char = 1
      }
      mut sp = 0
      mut xVal = cur
      while(xVal >= clr && sp < 4096){
         stk[sp] = suf.get(xVal)
         sp += 1
         xVal = pre.get(xVal)
      }
      def first = xVal
      if(op < olen){
         store8(out, first, op)
         op += 1
      }
      while(sp > 0 && op < olen){
         sp -= 1
         store8(out, stk.get(sp), op)
         op += 1
      }
      if(extra_char == 1 && op < olen){
         store8(out, first, op)
         op += 1
      }
      if(oldcode != -1 && nextcode < 4096){
         pre[nextcode] = oldcode
         suf[nextcode] = first
         nextcode += 1
         if(nextcode == (1 << (csz)) && csz < 12){ csz += 1 }
      }
      oldcode = code
   }
   return out
}

fn _g_di(any src, int w, int h) str {
   def out = init_str(malloc(w * h + 1), w * h)
   memset(out, 0, w * h)
   mut p = 0
   def sta, ste = [0, 4, 2, 1], [8, 8, 4, 2]
   mut passcount = 0
   while(passcount < 4){
      mut y = sta.get(passcount)
      def st = ste.get(passcount)
      while(y < h){
         mut x = 0
         while(x < w){
            if(p < src.len){
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

fn _g_new_canvas(int bytes) str {
   def canv = init_str(malloc(bytes + 1), bytes)
   memset(canv, 0, bytes)
   canv
}

fn _g_copy_canvas(any canv, int bytes) str {
   def out = init_str(malloc(bytes + 1), bytes)
   memcpy(out, canv, bytes)
   out
}

fn _g_clear_rect(any canv, int w, int h, int fx, int fy, int fw, int fh) any {
   mut y = 0
   while(y < fh){
      mut x = 0
      while(x < fw){
         if((fy + y) >= 0 && (fy + y) < h && (fx + x) >= 0 && (fx + x) < w){
            def off_pix = ((fy + y) * w + (fx + x)) * 4
            store32(canv, 0, off_pix)
         }
         x += 1
      }
      y += 1
   }
   canv
}

fn _g_draw_indexed_frame(any data, any canv, int w, int h, int fx, int fy, int fw, int fh, int cp_addr, int trans_idx, any idx_data) bool {
   mut y_loop = 0
   while(y_loop < fh){
      mut x_loop = 0
      while(x_loop < fw){
         if((fy + y_loop) < h && (fx + x_loop) < w){
            def pi = load8(idx_data, (y_loop * fw + x_loop))
            if(pi != trans_idx && cp_addr != -1){
               def off_pix = ((fy + y_loop) * w + (fx + x_loop)) * 4
               store8(canv, load8(data, cp_addr + pi * 3), off_pix)
               store8(canv, load8(data, cp_addr + pi * 3 + 1), off_pix + 1)
               store8(canv, load8(data, cp_addr + pi * 3 + 2), off_pix + 2)
               store8(canv, 255, off_pix + 3)
            }
         }
         x_loop += 1
      }
      y_loop += 1
   }
   true
}

fn decode_frames(any data) any {
   "Decodes a GIF into composed RGBA frames. Returns a dict with `frames`, `width`, `height`, and `channels`."
   if(!is_str(data)){ return 0 }
   def data_n = data.len
   if(data_n < 13){ return 0 }
   if(load8(data, 0) != 71 || load8(data, 1) != 73 || load8(data, 2) != 70){ return 0 }
   def w, h = pbin.u16le(data, 6), pbin.u16le(data, 8)
   def flags = load8(data, 10)
   mut p = 13
   mut gaddr = -1
   if(flags & 128){
      gaddr = p
      p += (1 << ((flags & 7) + 1)) * 3
   }
   def canvas_bytes = w * h * 4
   mut canv = _g_new_canvas(canvas_bytes)
   mut frames = []
   mut trans_idx = -1
   mut disposal = 0
   mut delay_cs = 0
   mut guard = 0
   while(p < (data_n - 1)){
      guard += 1
      if(guard > data_n * 4){ break }
      def bt = load8(data, p)
      p += 1
      if(bt == 0x3B){ break }
      if(bt == 0x21){
         def ext_type = load8(data, p)
         p += 1
         if(ext_type == 0xF9){
            def sz = load8(data, p)
            if(sz == 4){
               def gce_f = load8(data, p + 1)
               disposal = (gce_f >> 2) & 7
               delay_cs = pbin.u16le(data, p + 2)
               if(gce_f & 1){ trans_idx = load8(data, p + 4) } else { trans_idx = -1 }
            }
            p += sz + 1
            while(p < data_n && load8(data, p) != 0){ p += (load8(data, p) + 1) }
            if(p < data_n){ p += 1 }
            continue
         }
         while(p < data_n){
            def sz_ext = load8(data, p)
            p += 1
            if(sz_ext == 0){ break }
            p += sz_ext
         }
         continue
      }
      if(bt != 0x2C){ continue }
      if(p + 8 >= data_n){ break }
      def fx, fy = pbin.u16le(data, p), pbin.u16le(data, p + 2)
      def fw, fh = pbin.u16le(data, p + 4), pbin.u16le(data, p + 6)
      def ffl = load8(data, p + 8)
      if(fw <= 0 || fh <= 0 || fx < 0 || fy < 0 || fx + fw > w || fy + fh > h){ continue }
      p += 9
      mut cp_addr = gaddr
      if(ffl & 128){
         cp_addr = p
         p += (1 << ((ffl & 7) + 1)) * 3
      }
      if(p >= data.len){ break }
      def mcs = load8(data, p)
      p += 1
      def blocks_res = _g_sb(data, p)
      if(!blocks_res){ return 0 }
      p = blocks_res.get("next")
      mut idx_data = _g_lzw_dec(blocks_res.get("blob"), mcs, (fw * fh))
      if(is_int(idx_data)){ return 0 }
      if(ffl & 64){ idx_data = _g_di(idx_data, fw, fh) }
      def restore_prev = disposal == 3 ? _g_copy_canvas(canv, canvas_bytes) : 0
      _g_draw_indexed_frame(data, canv, w, h, fx, fy, fw, fh, cp_addr, trans_idx, idx_data)
      frames = frames.append({"data": _g_copy_canvas(canv, canvas_bytes), "delay": delay_cs, "x": fx, "y": fy, "w": fw, "h": fh})
      if(disposal == 2){
         _g_clear_rect(canv, w, h, fx, fy, fw, fh)
      } elif(disposal == 3 && restore_prev){
         canv = restore_prev
      }
      trans_idx = -1
      disposal = 0
      delay_cs = 0
   }
   if(frames.len == 0){ return 0 }
   {"frames": frames, "width": w, "height": h, "channels": 4}
}

fn decode(any data) any {
   "Decodes a GIF image into the standard image dictionary shape."
   def anim = decode_frames(data)
   if(!is_dict(anim)){ return 0 }
   def frames = anim.get("frames", [])
   if(!is_list(frames) || frames.len == 0){ return 0 }
   def first = frames.get(0, {})
   if(!is_dict(first)){ return 0 }
   mut out_dict = dict(8)
   out_dict = out_dict.set("data", first.get("data", ""))
   out_dict = out_dict.set("width", anim.get("width", 0))
   out_dict = out_dict.set("height", anim.get("height", 0))
   out_dict = out_dict.set("channels", 4)
   out_dict = out_dict.set("frame_count", frames.len)
   return out_dict
}

fn _gif_palette_info(any d, int w, int h, int ch) list {
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
            pal_list = pal_list.append(0)
            pal_list = pal_list.append(0)
            pal_list = pal_list.append(0)
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
      if(ch >= 4){ if(load8(d, off_c + 3) < 128){ skip_p = true } }
      if(!skip_p){
         def r = load8(d, off_c)
         mut g = r
         if(ch >= 3){ g = load8(d, off_c + 1) }
         mut b = r
         if(ch >= 3){ b = load8(d, off_c + 2) }
         def rgb = (r << 16) | (g << 8) | b
         if(!col_map.contains(rgb)){
            if(n_cols < 256){
               col_map = col_map.set(rgb, n_cols)
               pal_list = pal_list.append(r)
               pal_list = pal_list.append(g)
               pal_list = pal_list.append(b)
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
   if(n_cols == 0){ use_fix = true }
   [col_map, pal_list, trans_idx_enc, n_cols, has_trans_enc, use_fix]
}

fn _gif_index_buffer(any d, int w, int h, int ch, any col_map, int trans_idx_enc, bool has_trans_enc, bool use_fix) any {
   def idx_buf = init_str(malloc(w * h + 1), w * h)
   mut iC2 = 0
   while(iC2 < (w * h)){
      def off_c2 = iC2 * ch
      mut is_t = false
      if(ch >= 4){ if(load8(d, off_c2 + 3) < 128){ is_t = true } }
      if(is_t){ store8(idx_buf, trans_idx_enc, iC2) } else {
         def r2 = load8(d, off_c2)
         mut g2 = r2
         if(ch >= 3){ g2 = load8(d, off_c2 + 1) }
         mut b2 = r2
         if(ch >= 3){ b2 = load8(d, off_c2 + 2) }
         if(use_fix){
            def ri, gi = (r2 * 7 + 127) / 255, (g2 * 7 + 127) / 255
            def bi = (b2 * 3 + 127) / 255
            mut clr_i = (ri << 5) | (gi << 2) | bi
            if(clr_i == trans_idx_enc && has_trans_enc){
               if(clr_i < 255){ clr_i += 1 } else { clr_i -= 1 }
            }
            store8(idx_buf, clr_i, iC2)
         } else {
            def rgb2 = (r2 << 16) | (g2 << 8) | b2
            store8(idx_buf, col_map.get(rgb2, 0), iC2)
         }
      }
      iC2 += 1
   }
   idx_buf
}

fn _gif_lzw_emit_code(list state, int code, int csz) list {
   mut out_l = state.get(0)
   mut acc_bits = int(state.get(1, 0))
   mut bits_count = int(state.get(2, 0))
   mut k = 0
   while(k < csz){
      if((code >> k) & 1){ acc_bits = acc_bits | (1 << bits_count) }
      bits_count += 1
      if(bits_count == 8){
         out_l = out_l.append(acc_bits)
         acc_bits = 0
         bits_count = 0
      }
      k += 1
   }
   state[0] = out_l
   state[1] = acc_bits
   state[2] = bits_count
   state
}

fn _gif_lzw_encode(any idx_buf, int pixel_count) list {
   def clr_code = 256
   def end_code_lzw = 257
   mut out_l = list(pixel_count * 2)
   mut acc_bits = 0
   mut bits_count = 0
   mut csz_enc = 9
   mut next_code = 258
   mut state = list(3)
   state = state.append(out_l)
   state = state.append(acc_bits)
   state = state.append(bits_count)
   state = _gif_lzw_emit_code(state, clr_code, csz_enc)
   mut dict_lzw = dict(4096)
   mut ent = load8(idx_buf, 0)
   mut iIdx = 1
   while(iIdx < pixel_count){
      def c_sym = load8(idx_buf, iIdx)
      def key = (ent << 16) | c_sym
      if(dict_lzw.contains(key)){ ent = dict_lzw.get(key) } else {
         state = _gif_lzw_emit_code(state, ent, csz_enc)
         if(next_code < 4096){
            dict_lzw = dict_lzw.set(key, next_code)
            next_code += 1
            if(next_code == (1 << csz_enc) + 1 && csz_enc < 12){ csz_enc += 1 }
         } else {
            dict_lzw = dict(4096)
            state = _gif_lzw_emit_code(state, clr_code, csz_enc)
            csz_enc = 9
            next_code = 258
         }
         ent = c_sym
      }
      iIdx += 1
   }
   state = _gif_lzw_emit_code(state, ent, csz_enc)
   state = _gif_lzw_emit_code(state, end_code_lzw, csz_enc)
   out_l = state.get(0)
   acc_bits = int(state.get(1, 0))
   bits_count = int(state.get(2, 0))
   if(bits_count > 0){ out_l = out_l.append(acc_bits) }
   out_l
}

fn _gif_emit_file(list out_l, int w, int h, list pal_list, int n_cols, bool has_trans_enc, int trans_idx_enc, bool use_fix) any {
   mut res_l = list(out_l.len + 1024)
   res_l = res_l.append(71)
   res_l = res_l.append(73)
   res_l = res_l.append(70)
   res_l = res_l.append(56)
   res_l = res_l.append(57)
   res_l = res_l.append(97)
   res_l = res_l.append((w & 255))
   res_l = res_l.append((w >> 8))
   res_l = res_l.append((h & 255))
   res_l = res_l.append((h >> 8))
   if(use_fix){
      res_l = res_l.append(247)
      res_l = res_l.append(0)
      res_l = res_l.append(0)
      mut iPal = 0
      while(iPal < 256){
         res_l = res_l.append((iPal & 0xE0))
         res_l = res_l.append(((iPal << 3) & 0xE0))
         res_l = res_l.append(((iPal << 6) & 0xC0))
         iPal += 1
      }
   } else {
      mut psz = 0
      while((1 << (psz + 1)) < n_cols){ psz += 1 }
      res_l = res_l.append((128 | psz))
      res_l = res_l.append(0)
      res_l = res_l.append(0)
      mut iPal2 = 0
      def pal_list_n = pal_list.len
      while(iPal2 < (1 << (psz + 1))){
         if((iPal2 * 3 + 2) < pal_list_n){
            res_l = res_l.append(pal_list.get(iPal2 * 3))
            res_l = res_l.append(pal_list.get(iPal2 * 3 + 1))
            res_l = res_l.append(pal_list.get(iPal2 * 3 + 2))
         } else {
            res_l = res_l.append(0)
            res_l = res_l.append(0)
            res_l = res_l.append(0)
         }
         iPal2 += 1
      }
   }
   if(has_trans_enc){
      res_l = res_l.append(0x21)
      res_l = res_l.append(0xF9)
      res_l = res_l.append(4)
      res_l = res_l.append(1)
      res_l = res_l.append(0)
      res_l = res_l.append(0)
      res_l = res_l.append(trans_idx_enc)
      res_l = res_l.append(0)
   }
   res_l = res_l.append(0x2C)
   res_l = res_l.append(0)
   res_l = res_l.append(0)
   res_l = res_l.append(0)
   res_l = res_l.append(0)
   res_l = res_l.append((w & 255))
   res_l = res_l.append((w >> 8))
   res_l = res_l.append((h & 255))
   res_l = res_l.append((h >> 8))
   res_l = res_l.append(0)
   res_l = res_l.append(8)
   mut pOut = 0
   def out_l_n = out_l.len
   while(pOut < out_l_n){
      def remain = out_l_n - pOut
      mut sz_chunk = remain
      if(sz_chunk > 255){ sz_chunk = 255 }
      res_l = res_l.append(sz_chunk)
      mut iChunk = 0
      while(iChunk < sz_chunk){
         res_l = res_l.append(out_l.get(pOut + iChunk))
         iChunk += 1
      }
      pOut += sz_chunk
   }
   res_l = res_l.append(0)
   res_l = res_l.append(0x3B)
   return pbin.from_list(res_l)
}

fn encode(any img) any {
   "Encodes an indexed or RGBA image dictionary as a GIF byte string."
   def w, h = img.get("width"), img.get("height")
   def d = img.get("data")
   def ch = img.get("channels", 4)
   def palette = _gif_palette_info(d, w, h, ch)
   def col_map = palette.get(0)
   def pal_list = palette.get(1)
   def trans_idx_enc = int(palette.get(2, -1))
   def n_cols = int(palette.get(3, 0))
   def has_trans_enc = palette.get(4)
   def use_fix = palette.get(5)
   def idx_buf = _gif_index_buffer(d, w, h, ch, col_map, trans_idx_enc, has_trans_enc, use_fix)
   def out_l = _gif_lzw_encode(idx_buf, w * h)
   _gif_emit_file(out_l, w, h, pal_list, n_cols, has_trans_enc, trans_idx_enc, use_fix)
}
