;; Keywords: font truetype ttf text bitmap
;; TrueType / OpenType font parser and rasterizer in Nytrix.
;; Inspired from stb_truetype.h.
;; Supports: TTF glyph loading, metrics, cmap lookup, bitmap rasterization.

module std.ui.font.truetype (
   load, scale_for_pixel_height, scale_for_em,
   get_vmetrics, get_hmetrics, get_kern,
   get_glyph_index, get_glyph_bitmap, get_glyph_box,
   get_ascent_descent_gap
)

use std.core *
use std.math *
use std.text *

;; Low-level byte readers (big-endian)

fn _be_byte(d, p){
   "Returns an unsigned 8-bit byte from the data buffer at offset p."
   mut v = load8(d, p)
   if(v < 0){ v += 256 }
   v
}
fn _i8(d, p){
   "Returns a signed 8-bit byte from the data buffer."
   def v = _be_byte(d, p)
   if(v >= 128){ return v - 256 }
   v
}
fn _be_word(d, p){
   "Returns a big-endian unsigned 16-bit word."
   def hi = _be_byte(d, p)
   def lo = _be_byte(d, p + 1)
   return (hi * 256) + lo
}
fn _be_sword(d, p){
   "Returns a big-endian signed 16-bit word."
   def v = _be_word(d,p)
   if(v >= 32768){ return v - 65536 }
   return v
}
fn _be_dword(d, p){
   "Returns a big-endian unsigned 32-bit double word."
   def hi = _be_word(d, p)
   def lo = _be_word(d, p + 2)
   return (hi * 65536) + lo
}

fn _tag4(d, p, a, b, c, e){
   "Validation helper to check 4-byte signature tags."
   _be_byte(d, p) == a && _be_byte(d, p + 1) == b && _be_byte(d, p + 2) == c && _be_byte(d, p + 3) == e
}
fn _tag(d, p, t){
   "Validation helper to check string-based 4-byte tags."
   _tag4(d, p, load8(t,0), load8(t,1), load8(t,2), load8(t,3))
}

fn _is_font(d){
   "Returns true if the data buffer starts with a known font signature."
   if(_tag4(d,0,49,0,0,0)){ return 1 }  ;; TrueType 1  (0x31, 0, 0, 0 = '1' 0 0 0)
   if(_tag(d,0,"OTTO")){ return 1 }     ;; OTF/CFF
   if(_tag4(d,0,0,1,0,0)){ return 1 }   ;; OpenType 1.0
   if(_tag(d,0,"true")){ return 1 }     ;; Apple TTF
   if(_tag(d,0,"typ1")){ return 1 }     ;; Type1 (unsupported but accept)
   0
}

fn _find_table(d, fontstart, tag_code){
   "Returns byte offset of named table in TTF data, or 0."
   def n_tables = (_be_byte(d, fontstart + 4) * 256) + _be_byte(d, fontstart + 5)
   def dir = fontstart + 12
   mut i = 0
   while(i < n_tables){
      def loc = dir + 16 * i
      def t = (((_be_byte(d, loc) * 256) + _be_byte(d, loc + 1)) * 65536) +
              ((_be_byte(d, loc + 2) * 256) + _be_byte(d, loc + 3))
      if(t == tag_code){
         return ((((_be_byte(d, loc + 8) * 256) + _be_byte(d, loc + 9)) * 65536) +
                 ((_be_byte(d, loc + 10) * 256) + _be_byte(d, loc + 11)))
      }
      i += 1
   }
   0
}

fn _get_font_offset(d, index){
   "Returns the byte offset for the N-th font in a collection (TTC)."
   if(_is_font(d)){
      if(index == 0){ return 0 }
      return -1
   }
   if(_tag(d, 0, "ttcf")){
      def ver = _be_dword(d, 4)
      if(ver == 0x00010000 || ver == 0x00020000){
         def n = _be_dword(d, 8)
         if(index >= n){ return -1 }
         return _be_dword(d, 12 + index * 4)
      }
   }
   -1
}

;; Font info dict
;; Keys: data, start, cmap_idx, loca, head, glyf, hhea, hmtx, kern, loca_fmt, n_glyphs

fn _make_info(d, fontstart){
   "Internal helper to construct a font info dictionary."
   mut info = dict(12)
   info = dict_set(info, "data",  d)
   info = dict_set(info, "start", fontstart)
   def cmap = _find_table(d, fontstart, 0x636D6170) ;; 'cmap'
   def loca = _find_table(d, fontstart, 0x6C6F6361) ;; 'loca'
   def head = _find_table(d, fontstart, 0x68656164) ;; 'head'
   def glyf = _find_table(d, fontstart, 0x676C7966) ;; 'glyf'
   def hhea = _find_table(d, fontstart, 0x68686561) ;; 'hhea'
   def hmtx = _find_table(d, fontstart, 0x686D7478) ;; 'hmtx'
   def kern = _find_table(d, fontstart, 0x6B65726E) ;; 'kern'
   def maxp = _find_table(d, fontstart, 0x6D617870) ;; 'maxp'
   if(!cmap || !head || !hhea || !hmtx || !glyf || !loca){ return 0 }
   info = dict_set(info, "loca", loca)
   info = dict_set(info, "head", head)
   info = dict_set(info, "glyf", glyf)
   info = dict_set(info, "hhea", hhea)
   info = dict_set(info, "hmtx", hmtx)
   info = dict_set(info, "kern", kern)
   mut n_glyphs = 0xFFFF
   if(maxp){ n_glyphs = _be_word(d, maxp + 4) }
   info = dict_set(info, "n_glyphs", n_glyphs)
   def loca_fmt = _be_sword(d, head + 50)
   info = dict_set(info, "loca_fmt", loca_fmt)
   ;; Find best cmap subtable (prefer Unicode / Microsoft BMP)
   def n_cmap = _be_word(d, cmap + 2)
   mut cmap_idx = 0
   mut i = 0
   while(i < n_cmap){
      def rec = cmap + 4 + 8 * i
      def plat = _be_word(d, rec)
      def enc  = _be_word(d, rec + 2)
      def off  = _be_dword(d, rec + 4)
      if(plat == 3 && (enc == 1 || enc == 10)){
         cmap_idx = cmap + off
         break
      }
      if(plat == 0){ cmap_idx = cmap + off }
      i += 1
   }
   if(!cmap_idx){ return 0 }
   info = dict_set(info, "cmap_idx", cmap_idx)
   info
}

fn _l2(a, b){
   "Internal helper to create a 2-element list."
   mut l = list(2)
   l = append(l, a)
   l = append(l, b)
   l
}
fn _l3(a, b, c){
   "Internal helper to create a 3-element list."
   mut l = list(3)
   l = append(l, a)
   l = append(l, b)
   l = append(l, c)
   l
}
fn _l4(a, b, c, d){
   "Internal helper to create a 4-element list."
   mut l = list(4)
   l = append(l, a)
   l = append(l, b)
   l = append(l, c)
   l = append(l, d)
   l
}

fn _float_to_int(v){
   "Converts float-like values to integer using string parsing (runtime-safe fallback)."
   if(is_int(v)){ return v }
   def s = to_str(v)
   if(!is_str(s) || str_len(s) == 0){ return 0 }
   mut i = 0
   mut neg = false
   if(load8(s, 0) == 45){ ;; '-'
      neg = true
      i = 1
   }
   mut n = 0
   while(i < str_len(s)){
      def c = load8(s, i)
      if(c == 46){ break } ;; '.'
      if(c < 48 || c > 57){ break }
      n = (n * 10) + (c - 48)
      i += 1
   }
   if(neg){ return -n }
   n
}

fn _round_int(v){
   "Rounds a float-like value to nearest integer."
   if(v >= 0.0){ return _float_to_int(v + 0.5) }
   -_float_to_int((-v) + 0.5)
}

fn load(data, index=0){
   "Loads a TrueType font from a byte string. Returns info dict or 0."
   if(!is_str(data)){ return 0 }
   if(index != 0){ return 0 } ;; TTC collection index handling is not implemented yet
   def b0 = _be_byte(data, 0)
   def b1 = _be_byte(data, 1)
   def b2 = _be_byte(data, 2)
   def b3 = _be_byte(data, 3)
   def sig_ok = (b0 == 0 && b1 == 1 && b2 == 0 && b3 == 0) ||
                (b0 == 79 && b1 == 84 && b2 == 84 && b3 == 79) ||
                (b0 == 116 && b1 == 114 && b2 == 117 && b3 == 101) ||
                (b0 == 49 && b1 == 0 && b2 == 0 && b3 == 0)
   if(!sig_ok){ return 0 }

   def cmap = _find_table(data, 0, 0x636D6170)
   def loca = _find_table(data, 0, 0x6C6F6361)
   def head = _find_table(data, 0, 0x68656164)
   def glyf = _find_table(data, 0, 0x676C7966)
   def hhea = _find_table(data, 0, 0x68686561)
   def hmtx = _find_table(data, 0, 0x686D7478)
   def kern = _find_table(data, 0, 0x6B65726E)
   def maxp = _find_table(data, 0, 0x6D617870)
   if(!cmap || !head || !hhea || !hmtx || !glyf || !loca){ return 0 }

   mut info = dict(12)
   info = dict_set(info, "data", data)
   info = dict_set(info, "start", 0)
   info = dict_set(info, "loca", loca)
   info = dict_set(info, "head", head)
   info = dict_set(info, "glyf", glyf)
   info = dict_set(info, "hhea", hhea)
   info = dict_set(info, "hmtx", hmtx)
   info = dict_set(info, "kern", kern)
   if(maxp){
      info = dict_set(info, "n_glyphs", _be_word(data, maxp + 4))
   } else {
      info = dict_set(info, "n_glyphs", 0xFFFF)
   }
   info = dict_set(info, "loca_fmt", _be_sword(data, head + 50))

   ;; Find best cmap subtable (prefer Microsoft Unicode BMP/Full Unicode).
   def n_cmap = _be_word(data, cmap + 2)
   mut cmap_idx = 0
   mut i = 0
   while(i < n_cmap){
      def rec = cmap + 4 + 8 * i
      def plat = _be_word(data, rec)
      def enc  = _be_word(data, rec + 2)
      def off  = _be_dword(data, rec + 4)
      if(plat == 3 && (enc == 1 || enc == 10)){
         cmap_idx = cmap + off
         break
      }
      if(plat == 0){ cmap_idx = cmap + off }
      i += 1
   }
   if(!cmap_idx){ return 0 }
   info = dict_set(info, "cmap_idx", cmap_idx)
   info
}

;; Scale helpers

fn get_vmetrics(info){
   "Returns [ascent, descent, line_gap] in font units."
   def d    = dict_get(info, "data")
   def hhea = dict_get(info, "hhea")
   _l3(_be_sword(d, hhea + 4), _be_sword(d, hhea + 6), _be_sword(d, hhea + 8))
}

fn get_ascent_descent_gap(info){
   "Alias for get_vmetrics."
   get_vmetrics(info)
}

fn scale_for_pixel_height(info, pixels){
   "Returns scale factor so ascent-descent = `pixels`."
   def vm = get_vmetrics(info)
   def ascent  = get(vm, 0)
   def descent = get(vm, 1)
   pixels / (ascent - descent)
}

fn scale_for_em(info, pixels){
   "Returns scale factor so 1 em = `pixels`."
   def d    = dict_get(info, "data")
   def head = dict_get(info, "head")
   def units_per_em = _be_word(d, head + 18)
   pixels / units_per_em
}

;; Glyph index lookup

fn _cmap_fmt4_find(d, cmap_idx, cp){
   "Internal helper for cmap Format 4 (segment mapping to delta values)."
   def seg_count_x2 = _be_word(d, cmap_idx + 6)
   def seg_count = seg_count_x2 / 2
   def end_arr   = cmap_idx + 14
   def start_arr = end_arr + seg_count_x2 + 2
   def delta_arr = start_arr + seg_count_x2
   def range_arr = delta_arr + seg_count_x2
   ;; Binary search for cp in end_code array
   mut lo = 0 mut hi = seg_count
   while(lo < hi){
      def mid = (lo + hi) / 2
      if(_be_word(d, end_arr + mid * 2) < cp){ lo = mid + 1 }
      else { hi = mid }
   }
   if(lo >= seg_count){ return 0 }
   if(_be_word(d, start_arr + lo * 2) > cp){ return 0 }
   def range_off = _be_word(d, range_arr + lo * 2)
   if(range_off == 0){
      return (cp + _be_sword(d, delta_arr + lo * 2)) & 0xFFFF
   }
   def glyph_addr = range_arr + lo * 2 + range_off + (cp - _be_word(d, start_arr + lo * 2)) * 2
   def glyph_id   = _be_word(d, glyph_addr)
   if(glyph_id == 0){ return 0 }
   (glyph_id + _be_sword(d, delta_arr + lo * 2)) & 0xFFFF
}

fn _cmap_fmt12_find(d, cmap_idx, cp){
   "Internal helper for cmap Format 12 (segmented coverage)."
   def n_groups = _be_dword(d, cmap_idx + 12)
   mut lo = 0 mut hi = n_groups
   while(lo < hi){
      def mid = (lo + hi) / 2
      def base = cmap_idx + 16 + mid * 12
      def start_cp = _be_dword(d, base)
      def end_cp   = _be_dword(d, base + 4)
      if(cp < start_cp){ hi = mid }
      elif(cp > end_cp){ lo = mid + 1 }
      else { return _be_dword(d, base + 8) + (cp - start_cp) }
   }
   0
}

fn get_glyph_index(info, cp){
   "Returns glyph index for Unicode codepoint `cp`."
   def d   = dict_get(info, "data")
   def idx = dict_get(info, "cmap_idx")
   def fmt = _be_word(d, idx)
   if(fmt == 4){ return _cmap_fmt4_find(d, idx, cp) }
   if(fmt == 12 || fmt == 13){ return _cmap_fmt12_find(d, idx, cp) }
   0
}

;; Glyph metrics

fn _glyph_offset(info, gi){
   "Returns offset of glyph data in 'glyf' table."
   def d       = dict_get(info, "data")
   def loca    = dict_get(info, "loca")
   def glyf    = dict_get(info, "glyf")
   def loca_fmt = dict_get(info, "loca_fmt")
   if(loca_fmt){
      def g1 = _be_dword(d, loca + gi * 4)
      def g2 = _be_dword(d, loca + gi * 4 + 4)
      if(g1 == g2){ return -1 }
      return glyf + g1
   } else {
      def g1 = _be_word(d, loca + gi * 2) * 2
      def g2 = _be_word(d, loca + gi * 2 + 2) * 2
      if(g1 == g2){ return -1 }
      return glyf + g1
   }
}

fn get_glyph_box(info, gi){
   "Returns [x0, y0, x1, y1] bounding box in font units, or 0 if glyph is empty."
   def d   = dict_get(info, "data")
   def off = _glyph_offset(info, gi)
   if(off < 0){ return 0 }
   _l4(_be_sword(d,off+2), _be_sword(d,off+4), _be_sword(d,off+6), _be_sword(d,off+8))
}

fn get_hmetrics(info, gi){
   "Returns [advance_width, lsb] in font units."
   def d    = dict_get(info, "data")
   def hmtx = dict_get(info, "hmtx")
   def hhea = dict_get(info, "hhea")
   def n_hm = _be_word(d, hhea + 34)
   if(gi < n_hm){
      return _l2(_be_sword(d, hmtx + gi * 4), _be_sword(d, hmtx + gi * 4 + 2))
   }
   ;; mono-spaced fallback: reuse last entry, only lsb varies
   def last_aw = _be_sword(d, hmtx + (n_hm - 1) * 4)
   def lsb     = _be_sword(d, hmtx + n_hm * 4 + (gi - n_hm) * 2)
   _l2(last_aw, lsb)
}

fn get_kern(info, g1, g2){
   "Returns kern advance (font units) between glyph g1 and g2."
   def d    = dict_get(info, "data")
   def kern = dict_get(info, "kern")
   if(!kern){ return 0 }
   def n_tables = _be_word(d, kern + 2)
   mut off = kern + 4
   mut i = 0
   while(i < n_tables){
      def length  = _be_word(d, off + 2)
      def coverage = _be_word(d, off + 4)
      if((coverage >> 8) == 1 && (coverage & 1)){ ;; horizontal kern, format 0
         def n_pairs = _be_word(d, off + 6)
         def first   = off + 14
         mut lo2 = 0 mut hi2 = n_pairs
         def key = (g1 << 16) | g2
         while(lo2 < hi2){
            def mid = (lo2 + hi2) / 2
            def pair_off = first + mid * 6
            def l = _be_word(d, pair_off)
            def r = _be_word(d, pair_off + 2)
            def pair_key = (l << 16) | r
            if(pair_key < key){ lo2 = mid + 1 }
            elif(pair_key > key){ hi2 = mid }
            else { return _be_sword(d, pair_off + 4) }
         }
      }
      off += length
      i += 1
   }
   0
}

;; Glyph outline extraction
;; Returns list of contours, each contour is list of [x,y,on_curve]

fn _glyph_contours(info, gi){
   "Extracts raw contour points for glyph `gi` from the 'glyf' table."
   def d   = dict_get(info, "data")
   def off = _glyph_offset(info, gi)
   if(off < 0){ return list(0) }
   def n_contours = _be_sword(d, off)
   if(n_contours < 0){ return list(0) } ;; composite - skip for now
   mut n_pts_arr = 0
   if(n_contours > 0){
      n_pts_arr = _be_word(d, off + 10 + (n_contours - 1) * 2) + 1
   }
   ;; End point indices
   mut end_pts = list(n_contours)
   mut c = 0
   while(c < n_contours){
      end_pts = append(end_pts, _be_word(d, off + 10 + c * 2))
      c += 1
   }
   ;; Skip instructions
   def inst_len = _be_word(d, off + 10 + n_contours * 2)
   def flags_off = off + 10 + n_contours * 2 + 2 + inst_len
   ;; Expand flags (with repeat)
   mut flags = list(n_pts_arr)
   mut fi = flags_off
   mut pi = 0
   while(pi < n_pts_arr){
      def f = _be_byte(d, fi)
      fi += 1
      flags = append(flags, f)
      if(f & 8){  ;; repeat flag
         mut rp = _be_byte(d, fi)
         fi += 1
         while(rp > 0){
            flags = append(flags, f)
            rp -= 1
            pi += 1
         }
      }
      pi += 1
   }
   ;; Read x coords (delta encoded)
   mut xs = list(n_pts_arr)
   mut xi_off = fi
   mut cx = 0
   pi = 0
   while(pi < n_pts_arr){
      def f = get(flags, pi)
      mut dx = 0
      if(f & 2){
         dx = _be_byte(d, xi_off)
         xi_off += 1
         if(!(f & 16)){ dx = -dx }
      } elif(!(f & 16)){
         dx = _be_sword(d, xi_off)
         xi_off += 2
      }
      cx += dx
      xs = append(xs, cx)
      pi += 1
   }
   ;; Read y coords
   mut ys = list(n_pts_arr)
   mut yi_off = xi_off
   mut cy2 = 0
   pi = 0
   while(pi < n_pts_arr){
      def f = get(flags, pi)
      mut dy = 0
      if(f & 4){
         dy = _be_byte(d, yi_off)
         yi_off += 1
         if(!(f & 32)){ dy = -dy }
      } elif(!(f & 32)){
         dy = _be_sword(d, yi_off)
         yi_off += 2
      }
      cy2 += dy
      ys = append(ys, cy2)
      pi += 1
   }
   ;; Build contours
   mut contours = list(n_contours)
   c = 0
   mut start_pt = 0
   while(c < n_contours){
      def end_pt = get(end_pts, c)
      mut contour = list(end_pt - start_pt + 1)
      mut p = start_pt
      while(p <= end_pt){
         contour = append(contour, _l3(get(xs,p), get(ys,p), (get(flags,p) & 1)))
         p += 1
      }
      contours = append(contours, contour)
      start_pt = end_pt + 1
      c += 1
   }
   contours
}

;; Scanline rasterizer
;; Rasterizes contours to a 1-channel 8bpp alpha bitmap.

fn _lerp(t, a, b){
   "Linearly interpolates between two scalar values."
   a + t * (b - a)
}

fn _flatten_bezier(pts, x0, y0, cx, cy, x1, y1, flatness){
   "Recursively flatten quadratic bezier into pts list of [x,y]."
   def dx = x1 - x0 def dy = y1 - y0
   def d = abs((cx - (x0+x1)*0.5) * dy - (cy - (y0+y1)*0.5) * dx)
   if(d * d <= flatness * flatness * (dx*dx + dy*dy)){
      pts = append(pts, _l2(x1, y1))
      return pts
   }
   def mx0 = (x0 + cx) * 0.5 def my0 = (y0 + cy) * 0.5
   def mx1 = (cx + x1) * 0.5 def my1 = (cy + y1) * 0.5
   def mx  = (mx0 + mx1) * 0.5 def my  = (my0 + my1) * 0.5
   pts = _flatten_bezier(pts, x0, y0, mx0, my0, mx, my, flatness)
   pts = _flatten_bezier(pts, mx, my, mx1, my1, x1, y1, flatness)
   pts
}

fn _contour_to_lines(contour, scale_x, scale_y, ox, oy, flatness){
   "Returns flat list of [x,y] line segments for one contour (already closed)."
   def n  = len(contour)
   mut pts = list(n * 2)
   mut i = 0
   ;; Expand implicit on-curve points between two off-curve points
   mut expanded = list(n * 2)
   while(i < n){
      def cur = get(contour, i)
      def nxt = get(contour, (i + 1) % n)
      expanded = append(expanded, cur)
      if(!(get(cur, 2)) && !(get(nxt, 2))){
         ;; two off-curve in a row → insert implied on-curve midpoint
         def mx = (get(cur,0) + get(nxt,0)) * 0.5
         def my = (get(cur,1) + get(nxt,1)) * 0.5
         expanded = append(expanded, _l3(mx, my, 1))
      }
      i += 1
   }
   def ne = len(expanded)
   ;; Find first on-curve point as start
   mut start = 0
   mut si = 0
   while(si < ne){
      if(get(get(expanded, si), 2)){ start = si break }
      si += 1
   }
   def fp = get(expanded, start)
   def sp = _l2((get(fp, 0) - ox) * scale_x, (get(fp, 1) - oy) * scale_y)
   pts = append(pts, sp)
   i = 1
   while(i <= ne){
      def ci = get(expanded, (start + i) % ne)
      def on = get(ci, 2)
      def prev_pt = get(pts, len(pts) - 1)
      def px = get(prev_pt, 0) def py = get(prev_pt, 1)
      def cx2 = get(ci, 0) * scale_x - ox * scale_x
      def cy2 = get(ci, 1) * scale_y - oy * scale_y
      if(on){
         pts = append(pts, _l2(cx2, cy2))
      } else {
         def nxt_ci = get(expanded, (start + i + 1) % ne)
         mut ex = 0.0 mut ey = 0.0
         if(get(nxt_ci, 2)){
            ex = get(nxt_ci,0) * scale_x - ox * scale_x
            ey = get(nxt_ci,1) * scale_y - oy * scale_y
         } else {
            ex = (get(ci,0) + get(nxt_ci,0)) * 0.5 * scale_x - ox * scale_x
            ey = (get(ci,1) + get(nxt_ci,1)) * 0.5 * scale_y - oy * scale_y
         }
         ;; Deterministic fixed-step quadratic flattening avoids recursion/pathological stack growth.
         mut steps = 8
         if(flatness > 0.0){
            def est = int(1.0 / flatness)
            if(est > steps){ steps = est }
            if(steps > 24){ steps = 24 }
         }
         mut s = 1
         while(s <= steps){
            def t = float(s) / float(steps)
            def mt = 1.0 - t
            def qx = mt * mt * px + 2.0 * mt * t * cx2 + t * t * ex
            def qy = mt * mt * py + 2.0 * mt * t * cy2 + t * t * ey
            pts = append(pts, _l2(qx, qy))
            s += 1
         }
         i += 1  ;; skip next if it was used as end point
      }
      i += 1
   }
   pts
}

fn _rasterize(bitmap, bw, bh, all_pts_segments){
   "Scanline fill using even-odd rule."
   ;; all_pts_segments: list of contour point-lists (each is list of [x,y])
   mut y = 0
   while(y < bh){
      def scanline_y = y + 0.5
      mut x_crosses = list(64)
      def ns = len(all_pts_segments)
      mut si = 0
      while(si < ns){
         def pts = get(all_pts_segments, si)
         def np = len(pts)
         mut pi = 0
         while(pi < np){
            def p0 = get(pts, pi)
            def p1 = get(pts, (pi + 1) % np)
            def y0 = get(p0, 1) def y1 = get(p1, 1)
            if((y0 <= scanline_y && y1 > scanline_y) || (y1 <= scanline_y && y0 > scanline_y)){
               def t = (scanline_y - y0) / (y1 - y0)
               def xi = get(p0,0) + t * (get(p1,0) - get(p0,0))
               x_crosses = append(x_crosses, xi)
            }
            pi += 1
         }
         si += 1
      }
      ;; Sort crossings (insertion sort, typically small)
      def nc = len(x_crosses)
      mut i = 1
      while(i < nc){
         def kv = get(x_crosses, i)
         mut j = i - 1
         while(j >= 0 && get(x_crosses, j) > kv){
            store_item(x_crosses, j + 1, get(x_crosses, j))
            j -= 1
         }
         store_item(x_crosses, j + 1, kv)
         i += 1
      }
      ;; Fill between pairs of crossings
      i = 0
      while(i + 1 < nc){
         def left  = _round_int(get(x_crosses, i))
         def right = _round_int(get(x_crosses, i + 1))
         mut xi2 = left
         while(xi2 < right && xi2 < bw){
            if(xi2 >= 0){ store8(bitmap, 255, y * bw + xi2) }
            xi2 += 1
         }
         i += 2
      }
      y += 1
   }
}

fn get_glyph_bitmap(info, scale_x, scale_y, gi){
   "Returns {data, width, height, xoff, yoff} for glyph `gi` at the given scale."
   def box = get_glyph_box(info, gi)
   if(!box){ return 0 }
   def gx0 = get(box, 0) def gy0 = get(box, 1)
   def gx1 = get(box, 2) def gy1 = get(box, 3)
   ;; Bitmap dimensions
   def bw = _round_int((gx1 - gx0) * scale_x) + 1
   def bh = _round_int((gy1 - gy0) * scale_y) + 1
   if(bw <= 0 || bh <= 0){ return 0 }
   def bitmap = malloc(bw * bh)
   init_str(bitmap, bw * bh)
   def contours = _glyph_contours(info, gi)
   def nc = len(contours)
   if(nc == 0){
      mut ret = dict(5)
      ret = dict_set(ret, "data", bitmap)
      ret = dict_set(ret, "width", bw)
      ret = dict_set(ret, "height", bh)
      ret = dict_set(ret, "xoff", 0)
      ret = dict_set(ret, "yoff", 0)
      return ret
   }
   ;; 3x supersampling for smoother edge coverage in grayscale glyphs.
   def os = 3
   def sbw = bw * os
   def sbh = bh * os
   mut sample = malloc(sbw * sbh)
   init_str(sample, sbw * sbh)
   ;; Flatten each contour to line segments (scaled to supersample grid)
   def flatness = 0.35 / float(os)
   mut all_segs = list(nc)
   mut i = 0
   while(i < nc){
      def seg = _contour_to_lines(get(contours, i), scale_x * os, scale_y * os, gx0, gy0, flatness)
      all_segs = append(all_segs, seg)
      i += 1
   }
   _rasterize(sample, sbw, sbh, all_segs)
   ;; Downsample supersample mask into final 8bpp alpha bitmap.
   def denom = os * os
   mut y = 0
   while(y < bh){
      mut x = 0
      while(x < bw){
         mut acc = 0
         mut sy = 0
         while(sy < os){
            def row = (y * os + sy) * sbw
            mut sx = 0
            while(sx < os){
               acc += _be_byte(sample, row + (x * os + sx))
               sx += 1
            }
            sy += 1
         }
         def a8 = (acc + (denom / 2)) / denom
         store8(bitmap, a8, y * bw + x)
         x += 1
      }
      y += 1
   }
   free(sample)
   ;; Y-flip (TTF is y-up, bitmap is y-down)
   mut top = 0 mut bot = bh - 1
   while(top < bot){
      mut xi = 0
      while(xi < bw){
         def ta = load8(bitmap, top * bw + xi)
         def ba = load8(bitmap, bot * bw + xi)
         store8(bitmap, ba, top * bw + xi)
         store8(bitmap, ta, bot * bw + xi)
         xi += 1
      }
      top += 1 bot -= 1
   }
   mut res = dict(5)
   res = dict_set(res, "data",   bitmap)
   res = dict_set(res, "width",  bw)
   res = dict_set(res, "height", bh)
   res = dict_set(res, "xoff",   _round_int(gx0 * scale_x))
   res = dict_set(res, "yoff",   _round_int(gy0 * scale_y))
   res
}

fn _ord_at(s, i){
   "Returns the unsigned byte value at offset i in string s."
   def v = load8(s, i)
   if(v >= 0){ v } else { v + 256 }
}

if(comptime{__main()}){
   use std.core *
   use std.ui.font.truetype *

   ;; We can't bundle a TTF file, so do a structural test on load()
   def fake = malloc(4) init_str(fake, 4)
   store8(fake, 116, 0)  ;; 't'
   store8(fake, 114, 1)  ;; 'r'
   store8(fake, 117, 2)  ;; 'u'
   store8(fake, 101, 3)  ;; 'e'
   def result = load(fake, 0)
   assert(result == 0, "load returns 0 for invalid font")
   print("✓ std.ui.font.truetype structural tests passed")

   ;; System TTF test
   use std.os *
   def sys_font = eq(os_name(), "windows") ? "C:/Windows/Fonts/arial.ttf" : "/usr/share/fonts/TTF/DejaVuSans.ttf"
   def font_res = file_read(sys_font)
   if(is_ok(font_res)){
      def font_data = unwrap(font_res)
      def info = load(font_data, 0)
      assert(info != 0, "System TTF loaded successfully")
      def scale = scale_for_pixel_height(info, 32.0)
      def gi = get_glyph_index(info, 65) ;; 'A'
      if(gi > 0){
         def bm = get_glyph_bitmap(info, scale, scale, gi)
         assert(dict_get(bm, "width") > 0, "has width")
      }
      print("✓ std.ui.font.truetype (System TTF) test passed")
   }
}
