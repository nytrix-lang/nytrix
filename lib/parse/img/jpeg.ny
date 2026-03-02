;; Keywords: image jpeg rfc2035
;; Reference:
;; - https://en.wikipedia.org/wiki/JPEG
;; - https://www.rfc-editor.org/rfc/rfc2035.html

module std.image.format.jpeg (
   encode, decode
)

use std.core *
use std.core.dict *
use std.math *

fn _huff_new_node(ctx){
   "Internal helper for `huff_new_node`."
   mut nodes = dict_get(ctx, "nodes")
   def idx = len(nodes)
   def n = dict(4)
   dict_set(n, "sym", 0)
   dict_set(n, "has_sym", 0)
   dict_set(n, "left", -1)
   dict_set(n, "right", -1)
   dict_set(ctx, "nodes", append(nodes, n))
   idx
}

fn _huff_add(ctx, root_idx, code, code_len, sym){
   "Internal helper for `huff_add`."
   mut cur = root_idx
   mut i = code_len
   while(i > 0){
      mut nodes = dict_get(ctx, "nodes")
      def n = get(nodes, cur)
      def bit = (code >> (i - 1)) & 1
      if(bit){
         mut r = dict_get(n, "right")
         if(r == -1){
            r = _huff_new_node(ctx)
            nodes = dict_get(ctx, "nodes")
            def n2 = get(nodes, cur)
            dict_set(n2, "right", r)
         }
         cur = r
      } else {
         mut l = dict_get(n, "left")
         if(l == -1){
            l = _huff_new_node(ctx)
            nodes = dict_get(ctx, "nodes")
            def n2 = get(nodes, cur)
            dict_set(n2, "left", l)
         }
         cur = l
      }
      i -= 1
   }
   mut nodes_final = dict_get(ctx, "nodes")
   def leaf = get(nodes_final, cur)
   dict_set(leaf, "sym", sym)
   dict_set(leaf, "has_sym", 1)
}

fn _huff_parse(data, offset){
   "Internal helper for `huff_parse`."
   mut ctx = dict(2)
   dict_set(ctx, "nodes", list(256))
   _huff_new_node(ctx)
   mut off = offset
   def counts = list(16)
   mut i = 0
   while(i < 16){
      append(counts, load8(data, off + i))
      i += 1
   }
   off += 16
   mut code_val = 0
   mut code_len = 1
   while(code_len <= 16){
      def cnt = get(counts, code_len - 1)
      mut j = 0
      while(j < cnt){
         def sym = load8(data, off)
         off += 1
         _huff_add(ctx, 0, code_val, code_len, sym)
         code_val += 1
         j += 1
      }
      code_val = code_val << 1
      code_len += 1
   }
   [dict_get(ctx, "nodes"), off - offset]
}

fn _huff_decode(nodes, bs){
   "Internal helper for `huff_decode`."
   mut cur = 0
   while(1){
      def bit = _bs_get_bit(bs)
      def n = get(nodes, cur)
      if(bit){
         cur = dict_get(n, "right")
      } else {
         cur = dict_get(n, "left")
      }
      if(cur == -1){ return -1 }
      def cn = get(nodes, cur)
      if(dict_get(cn, "has_sym")){ return dict_get(cn, "sym") }
   }
}

fn _bs_make(data, start){
   "Internal helper for `bs_make`."
   def bs = dict(5)
   dict_set(bs, "data", data)
   dict_set(bs, "pos", start)
   dict_set(bs, "cur", 0)
   dict_set(bs, "left", 0)
   bs
}

fn _bs_load(bs){
   "Internal helper for `bs_load`."
   def data = dict_get(bs, "data")
   mut pos = dict_get(bs, "pos")
   mut c = load8(data, pos)
   pos += 1
   if(c == 0xFF){
      def next = load8(data, pos)
      if(next == 0x00){ pos += 1 }
   }
   dict_set(bs, "cur", c)
   dict_set(bs, "left", 8)
   dict_set(bs, "pos", pos)
}

fn _bs_get_bit(bs){
   "Internal helper for `bs_get_bit`."
   if(dict_get(bs, "left") == 0){ _bs_load(bs) }
   mut left = dict_get(bs, "left")
   left -= 1
   def bit = (dict_get(bs, "cur") >> left) & 1
   dict_set(bs, "left", left)
   bit
}

fn _bs_get_bits(bs, n){
   "Internal helper for `bs_get_bits`."
   mut val = 0
   mut i = 0
   while(i < n){
      val = (val << 1) | _bs_get_bit(bs)
      i += 1
   }
   val
}

fn _decode_coeff(size, bits){
   "Internal helper for `decode_coeff`."
   if(size == 0){ return 0 }
   if((bits >> (size - 1)) & 1){ return bits }
   bits - (1 << size) + 1
}

def _IDCT = [
   0.707107,  0.707107,  0.707107,  0.707107,  0.707107,  0.707107,  0.707107,  0.707107,
   0.980785,  0.831470,  0.555570,  0.195090, -0.195090, -0.555570, -0.831470, -0.980785,
   0.923880,  0.382683, -0.382683, -0.923880, -0.923880, -0.382683,  0.382683,  0.923880,
   0.831470, -0.195090, -0.980785, -0.555570,  0.555570,  0.980785,  0.195090, -0.831470,
   0.707107, -0.707107, -0.707107,  0.707107,  0.707107, -0.707107, -0.707107,  0.707107,
   0.555570, -0.980785,  0.195090,  0.831470, -0.831470, -0.195090,  0.980785, -0.555570,
   0.382683, -0.923880,  0.923880, -0.382683, -0.382683,  0.923880, -0.923880,  0.382683,
   0.195090, -0.555570,  0.831470, -0.980785,  0.980785, -0.831470,  0.555570, -0.195090
]

def _ZIGZAG = [
    0,  1,  8, 16,  9,  2,  3, 10,
   17, 24, 32, 25, 18, 11,  4,  5,
   12, 19, 26, 33, 40, 48, 41, 34,
   27, 20, 13,  6,  7, 14, 21, 28,
   35, 42, 49, 56, 57, 50, 43, 36,
   29, 22, 15, 23, 30, 37, 44, 51,
   58, 59, 52, 45, 38, 31, 39, 46,
   53, 60, 61, 54, 47, 55, 62, 63
]

fn _clamp_u8(v){
   "Clamps integer-like value `v` to `[0,255]`."
   if(v < 0){ return 0 }
   if(v > 255){ return 255 }
   __flt_to_int(v + 0.0)
}

fn _round_i(v){
   "Rounds float `v` to nearest int (ties away from zero)."
   __flt_to_int(v + ((v >= 0.0) ? 0.5 : -0.5))
}

fn _idct_and_dequant(coeff_lin, quant_table, out, out_off, out_w){
   "Dequantizes one 8x8 block and runs IDCT using the precomputed basis matrix."
   mut c = list(64)
   mut m = 0
   while(m < 64){ append(c, 0) m += 1 }
   m = 0
   while(m < 64){
      store_item(c, get(_ZIGZAG, m), get(coeff_lin, m) * get(quant_table, m))
      m += 1
   }
   mut tmp = list(64)
   m = 0
   while(m < 64){ append(tmp, 0.0) m += 1 }
   mut v = 0
   while(v < 8){
      mut x = 0
      while(x < 8){
         mut s = 0.0
         mut u = 0
         while(u < 8){
            s += (get(c, v * 8 + u) + 0.0) * get(_IDCT, u * 8 + x)
            u += 1
         }
         store_item(tmp, v * 8 + x, s)
         x += 1
      }
      v += 1
   }
   mut y = 0
   while(y < 8){
      mut x = 0
      while(x < 8){
         mut sum = 0.0
         v = 0
         while(v < 8){
            sum += get(_IDCT, v * 8 + y) * get(tmp, v * 8 + x)
            v += 1
         }
         def val = _clamp_u8(_round_i(sum / 4.0 + 128.0))
         store8(out, val, out_off + (y * out_w + x))
         x += 1
      }
      y += 1
   }
}

fn _decode_data_unit(bs, dc_huff, ac_huff, quant_table, dc_coeff, plane, plane_off, plane_w){
   "Internal helper for `decode_data_unit`."
   def dc_size = _huff_decode(dc_huff, bs)
   def dc_bits = _bs_get_bits(bs, dc_size)
   dc_coeff += _decode_coeff(dc_size, dc_bits)
   dict_set(bs, "__dc_coeff", dc_coeff)
   mut coeff = list(64)
   append(coeff, dc_coeff)
   mut i = 1
   while(i < 64){ append(coeff, 0) i += 1 }
   mut j = 1
   while(j < 64){
      def ac_sym = _huff_decode(ac_huff, bs)
      def zero_run = ac_sym >> 4
      def ac_size  = ac_sym & 0xF
      if(ac_size == 0){
         if(zero_run == 0xF){ j += 16 }
         else { break }
      } else {
         j += zero_run
         if(j < 64){
            def ac_bits = _bs_get_bits(bs, ac_size)
            store_item(coeff, j, _decode_coeff(ac_size, ac_bits))
            j += 1
         }
      }
   }
   _idct_and_dequant(coeff, quant_table, plane, plane_off, plane_w)
}

fn decode(data){
   "Decodes a Baseline JPEG (SOF0) from a byte string. Returns image dict or 0."
   if(!is_str(data) || len(data) < 4){ return 0 }
   if(load8(data, 0) != 0xFF || load8(data, 1) != 0xD8){ return 0 }
   mut w = 0 mut h = 0 mut n_comp = 0
   mut quant_tables = list(4)
   mut i = 0 while(i < 4){ append(quant_tables, 0) i += 1 }
   mut huff_dc = list(4)
   mut huff_ac = list(4)
   i = 0 while(i < 4){ append(huff_dc, 0) append(huff_ac, 0) i += 1 }
   mut comp_info = list(256)
   i = 0 while(i < 256){ append(comp_info, 0) i += 1 }
   mut comp_order = list(4)
   mut scan_comps = list(4)
   mut max_h = 1 mut max_v = 1
   mut scan_start = 0
   mut p = 2
   def n = len(data)
   mut done = 0
   while(p < n - 1 && !done){
      if(load8(data, p) != 0xFF){
         p += 1
         continue
      }
      def mark = (load8(data, p) << 8) | load8(data, p + 1)
      p += 2
      if(mark == 0xFFD9){ done = 1 break }
      if(mark == 0xFFD8){ }
      else {
         def seg_len = (load8(data, p) << 8) | load8(data, p + 1)
         if(seg_len < 2){ return 0 }
         def data_len = seg_len - 2
         def seg_off = p + 2
         if(mark == 0xFFC0){
            h          = (load8(data, seg_off + 1) << 8) | load8(data, seg_off + 2)
            w          = (load8(data, seg_off + 3) << 8) | load8(data, seg_off + 4)
            n_comp     = load8(data, seg_off + 5)
            if(n_comp < 1 || n_comp > 3){ return 0 }
            mut ci = 0
            while(ci < n_comp){
               def cid  = load8(data, seg_off + 6 + ci * 3)
               def samp = load8(data, seg_off + 7 + ci * 3)
               def qid  = load8(data, seg_off + 8 + ci * 3)
               def hs = samp >> 4 def vs = samp & 0xF
               if(hs > max_h){ max_h = hs }
               if(vs > max_v){ max_v = vs }
               def ci_dict = dict(5)
               dict_set(ci_dict, "quant_id", qid)
               dict_set(ci_dict, "h_samp", hs)
               dict_set(ci_dict, "v_samp", vs)
               dict_set(ci_dict, "dc_id", 0)
               dict_set(ci_dict, "ac_id", 0)
               store_item(comp_info, cid, ci_dict)
               append(comp_order, cid)
               ci += 1
            }
         } elif(mark == 0xFFC2){
            return 0
         } elif(mark == 0xFFDB){
            mut off = seg_off
            while(off < seg_off + data_len){
               def qid = load8(data, off) & 0xF
               off += 1
               def qt = list(64)
               mut qi = 0
               while(qi < 64){ append(qt, load8(data, off + qi)) qi += 1 }
               off += 64
               store_item(quant_tables, qid, qt)
            }
         } elif(mark == 0xFFC4){
            mut off = seg_off
            while(off < seg_off + data_len){
               def tc = load8(data, off) >> 4
               def tid = load8(data, off) & 0xF
               def parsed = _huff_parse(data, off + 1)
               def nodes = get(parsed, 0)
               def consumed = get(parsed, 1)
               off += 1 + consumed
               if(tc == 0){ store_item(huff_dc, tid, nodes) }
               else { store_item(huff_ac, tid, nodes) }
            }
         } elif(mark == 0xFFDA){
            scan_comps = list(4)
            def sc_count = load8(data, seg_off)
            if(sc_count < 1 || sc_count > n_comp){ return 0 }
            mut si = 0
            while(si < sc_count){
               def scid  = load8(data, seg_off + 1 + si * 2)
               def tids  = load8(data, seg_off + 2 + si * 2)
               def dc_id = tids >> 4
               def ac_id = tids & 0xF
               def ci_dict = get(comp_info, scid)
               if(is_dict(ci_dict)){
                  dict_set(ci_dict, "dc_id", dc_id)
                  dict_set(ci_dict, "ac_id", ac_id)
               }
               append(scan_comps, scid)
               si += 1
            }
            scan_start = seg_off + data_len
            done = 1
         }
         p = seg_off + data_len
      }
   }
   if(w == 0 || h == 0){ return 0 }
   if(scan_start == 0){ return 0 }
   def total_px = w * h
   def ch_y  = init_str(malloc(total_px), total_px)
   def ch_cb = init_str(malloc(total_px), total_px)
   def ch_cr = init_str(malloc(total_px), total_px)
   memset(ch_y, 0, total_px)
   memset(ch_cb, 0, total_px)
   memset(ch_cr, 0, total_px)
   def bs = _bs_make(data, scan_start)
   def du_tmp = init_str(malloc(64), 64)
   if(!du_tmp){ return 0 }
   def mcu_w = 8 * max_h
   def mcu_h = 8 * max_v
   mut dc1 = 0 mut dc2 = 0 mut dc3 = 0
   def y_id = (len(comp_order) > 0) ? get(comp_order, 0) : 1
   def cb_id = (len(comp_order) > 1) ? get(comp_order, 1) : 2
   def cr_id = (len(comp_order) > 2) ? get(comp_order, 2) : 3
   def sc_n = len(scan_comps)
   mut my = 0
   while(my * mcu_h < h){
      mut mx = 0
      while(mx * mcu_w < w){
         mut ci = 0
         while(ci < sc_n){
            def cid = get(scan_comps, ci)
            def cinfo = get(comp_info, cid)
            if(!is_dict(cinfo)){
               ci += 1
               continue
            }
            def hs = dict_get(cinfo, "h_samp")
            def vs = dict_get(cinfo, "v_samp")
            def dc_id = dict_get(cinfo, "dc_id")
            def ac_id = dict_get(cinfo, "ac_id")
            def qid   = dict_get(cinfo, "quant_id")
            def qt = get(quant_tables, qid)
            def dc_huff = get(huff_dc, dc_id)
            def ac_huff = get(huff_ac, ac_id)
            mut x_scale = max_h / hs
            mut y_scale = max_v / vs
            if(x_scale <= 0){ x_scale = 1 }
            if(y_scale <= 0){ y_scale = 1 }
            mut vi = 0
            while(vi < vs){
               mut hi = 0
               while(hi < hs){
                  def gx = mx * mcu_w + hi * 8 * x_scale
                  def gy = my * mcu_h + vi * 8 * y_scale
                  def plane = (cid == y_id) ? ch_y : (cid == cb_id) ? ch_cb : ch_cr
                  def cur_dc = (cid == y_id) ? dc1 : (cid == cb_id) ? dc2 : dc3
                  _decode_data_unit(bs, dc_huff, ac_huff, qt, cur_dc, du_tmp, 0, 8)
                  mut ty = 0
                  while(ty < 8){
                     mut tx = 0
                     while(tx < 8){
                        def sv = load8(du_tmp, ty * 8 + tx)
                        mut ry = 0
                        while(ry < y_scale){
                           def py = gy + ty * y_scale + ry
                           if(py < h){
                              mut rx = 0
                              while(rx < x_scale){
                                 def px = gx + tx * x_scale + rx
                                 if(px < w){
                                    store8(plane, sv, py * w + px)
                                 }
                                 rx += 1
                              }
                           }
                           ry += 1
                        }
                        tx += 1
                     }
                     ty += 1
                  }
                  def new_dc = dict_get(bs, "__dc_coeff")
                  if(cid == y_id){ dc1 = new_dc }
                  elif(cid == cb_id){ dc2 = new_dc }
                  else { dc3 = new_dc }
                  hi += 1
               }
               vi += 1
            }
            ci += 1
         }
         mx += 1
      }
      my += 1
   }
   free(du_tmp)
   def pixels = malloc(total_px * 4)
   init_str(pixels, total_px * 4)
   mut k = 0
   while(k < total_px){
      def Y  = load8(ch_y,  k)
      def Cb = (n_comp > 1) ? load8(ch_cb, k) : 128
      def Cr = (n_comp > 1) ? load8(ch_cr, k) : 128
      def R = _clamp_u8(_round_i(Y + 1.402  * (Cr - 128.0)))
      def G = _clamp_u8(_round_i(Y - 0.34414 * (Cb - 128.0) - 0.71414 * (Cr - 128.0)))
      def B = _clamp_u8(_round_i(Y + 1.772  * (Cb - 128.0)))
      store8(pixels, R, k * 4)
      store8(pixels, G, k * 4 + 1)
      store8(pixels, B, k * 4 + 2)
      store8(pixels, 255, k * 4 + 3)
      k += 1
   }
   mut res = dict(4)
   res = dict_set(res, "data", pixels)
   res = dict_set(res, "width", w)
   res = dict_set(res, "height", h)
   res = dict_set(res, "channels", 4)
   res
}

def _QY_STD = [
   16,11,10,16,24,40,51,61, 12,12,14,19,26,58,60,55,
   14,13,16,24,40,57,69,56, 14,17,22,29,51,87,80,62,
   18,22,37,56,68,109,103,77, 24,35,55,64,81,104,113,92,
   49,64,78,87,103,121,120,101, 72,92,95,98,112,100,103,99
]

def _QC_STD = [
   17,18,24,47,99,99,99,99, 18,21,26,66,99,99,99,99,
   24,26,56,99,99,99,99,99, 47,66,99,99,99,99,99,99,
   99,99,99,99,99,99,99,99, 99,99,99,99,99,99,99,99,
   99,99,99,99,99,99,99,99, 99,99,99,99,99,99,99,99
]

def _BITS_DC_LUMA = [0,1,5,1,1,1,1,1,1,0,0,0,0,0,0,0]
def _BITS_DC_CHROMA = [0,3,1,1,1,1,1,1,1,1,1,0,0,0,0,0]
def _VALS_DC = [0,1,2,3,4,5,6,7,8,9,10,11]

def _BITS_AC_LUMA = [0,2,1,3,3,2,4,3,5,5,4,4,0,0,1,125]
def _VALS_AC_LUMA = [
   1,2,3,0,4,17,5,18,33,49,65,6,19,81,97,7,34,113,20,50,129,145,161,8,35,66,177,193,21,82,209,240,
   36,51,98,114,130,9,10,22,23,24,25,26,37,38,39,40,41,42,52,53,54,55,56,57,58,67,68,69,70,71,72,73,
   74,83,84,85,86,87,88,89,90,99,100,101,102,103,104,105,106,115,116,117,118,119,120,121,122,131,132,
   133,134,135,136,137,138,146,147,148,149,150,151,152,153,154,162,163,164,165,166,167,168,169,170,178,
   179,180,181,182,183,184,185,186,194,195,196,197,198,199,200,201,202,210,211,212,213,214,215,216,217,
   218,225,226,227,228,229,230,231,232,233,234,241,242,243,244,245,246,247,248,249,250
]

def _BITS_AC_CHROMA = [0,2,1,2,4,4,3,4,7,5,4,4,0,1,2,119]
def _VALS_AC_CHROMA = [
   0,1,2,3,17,4,5,33,49,6,18,65,81,7,97,113,19,34,50,129,8,20,66,145,161,177,193,9,35,51,82,240,
   21,98,114,209,10,22,36,52,225,37,241,23,24,25,26,38,39,40,41,42,53,54,55,56,57,58,67,68,69,70,71,
   72,73,74,83,84,85,86,87,88,89,90,99,100,101,102,103,104,105,106,115,116,117,118,119,120,121,122,130,
   131,132,133,134,135,136,137,138,146,147,148,149,150,151,152,153,154,162,163,164,165,166,167,168,169,
   170,178,179,180,181,182,183,184,185,186,194,195,196,197,198,199,200,201,202,210,211,212,213,214,215,
   216,217,218,226,227,228,229,230,231,232,233,234,242,243,244,245,246,247,248,249,250
]

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

fn _mk_huff_map(bits, vals){
   "Internal helper for `mk_huff_map`."
   def code_map = _mk_zero_list(256)
   def size_map = _mk_zero_list(256)
   mut code = 0
   mut k = 0
   mut nlen = 1
   while(nlen <= 16){
      def cnt = get(bits, nlen - 1)
      mut j = 0
      while(j < cnt){
         def sym = get(vals, k)
         store_item(code_map, sym, code)
         store_item(size_map, sym, nlen)
         code += 1
         k += 1
         j += 1
      }
      code = code << 1
      nlen += 1
   }
   mut out = dict(2)
   out = dict_set(out, "code", code_map)
   out = dict_set(out, "size", size_map)
   out
}

fn _henc(eb, map, sym){
   "Internal helper for `henc`."
   def code = get(dict_get(map, "code"), sym)
   def nbits = get(dict_get(map, "size"), sym)
   if(nbits <= 0){
      panic("jpeg encode: missing huffman symbol " + to_str(sym))
   }
   _eb_put_bits(eb, code, nbits)
}

fn _mag_bits(v){
   "Internal helper for `mag_bits`."
   mut x = (v < 0) ? -v : v
   mut n = 0
   while(x > 0){
      x = x >> 1
      n += 1
   }
   n
}

fn _amp_bits(v, nbits){
   "Internal helper for `amp_bits`."
   if(nbits == 0){ return 0 }
   if(v < 0){ return v + ((1 << nbits) - 1) }
   v
}

fn _eb_new(){
   "Internal helper for `eb_new`."
   mut eb = dict(3)
   eb = dict_set(eb, "buf", list(1024))
   eb = dict_set(eb, "acc", 0)
   eb = dict_set(eb, "bits", 0)
   eb
}

fn _eb_emit_byte(eb, b){
   "Internal helper for `eb_emit_byte`."
   def v = b & 255
   def buf = dict_get(eb, "buf")
   append(buf, v)
   if(v == 255){ append(buf, 0) }
}

fn _eb_put_bits(eb, bits, nbits){
   "Internal helper for `eb_put_bits`."
   mut acc = dict_get(eb, "acc")
   mut n = dict_get(eb, "bits")
   mut i = nbits
   while(i > 0){
      i -= 1
      acc = (acc << 1) | ((bits >> i) & 1)
      n += 1
      if(n == 8){
         _eb_emit_byte(eb, acc)
         acc = 0
         n = 0
      }
   }
   dict_set(eb, "acc", acc)
   dict_set(eb, "bits", n)
}

fn _eb_flush(eb){
   "Internal helper for `eb_flush`."
   def n = dict_get(eb, "bits")
   if(n > 0){
      def acc = dict_get(eb, "acc")
      def pad = (1 << (8 - n)) - 1
      _eb_emit_byte(eb, (acc << (8 - n)) | pad)
      dict_set(eb, "acc", 0)
      dict_set(eb, "bits", 0)
   }
}

fn _out_u8(out, v){
   "Internal helper for `out_u8`."
   append(out, v & 255)
}
fn _out_u16(out, v){
   "Internal helper for `out_u16`."
   _out_u8(out, (v >> 8) & 255) _out_u8(out, v & 255)
}
fn _out_marker(out, m){
   "Internal helper for `out_marker`."
   _out_u8(out, 255) _out_u8(out, m)
}

fn _out_list_bytes(out, xs){
   "Internal helper for `out_list_bytes`."
   mut i = 0
   while(i < len(xs)){
      _out_u8(out, get(xs, i))
      i += 1
   }
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

fn _scaled_qtables(quality){
   "Internal helper for `scaled_qtables`."
   mut q = quality
   if(q < 1){ q = 1 }
   if(q > 100){ q = 100 }
   mut scale = 0
   if(q < 50){ scale = 5000 / q }
   else { scale = 200 - q * 2 }
   def qy_nat = _mk_zero_list(64)
   def qc_nat = _mk_zero_list(64)
   mut i = 0
   while(i < 64){
      mut v1 = (get(_QY_STD, i) * scale + 50) / 100
      mut v2 = (get(_QC_STD, i) * scale + 50) / 100
      if(v1 < 1){ v1 = 1 } elif(v1 > 255){ v1 = 255 }
      if(v2 < 1){ v2 = 1 } elif(v2 > 255){ v2 = 255 }
      store_item(qy_nat, i, v1)
      store_item(qc_nat, i, v2)
      i += 1
   }
   def qy_zz = _mk_zero_list(64)
   def qc_zz = _mk_zero_list(64)
   i = 0
   while(i < 64){
      def nat = get(_ZIGZAG, i)
      store_item(qy_zz, i, get(qy_nat, nat))
      store_item(qc_zz, i, get(qc_nat, nat))
      i += 1
   }
   mut out = dict(4)
   out = dict_set(out, "qy_nat", qy_nat)
   out = dict_set(out, "qc_nat", qc_nat)
   out = dict_set(out, "qy_zz", qy_zz)
   out = dict_set(out, "qc_zz", qc_zz)
   out
}

fn _fdct_quant_zz(block, q_nat){
   "Internal helper for `fdct_quant_zz`."
   def pi = 3.141592653589793
   def nat = _mk_zero_list(64)
   mut v = 0
   while(v < 8){
      mut u = 0
      while(u < 8){
         mut sum = 0.0
         mut y = 0
         while(y < 8){
            mut x = 0
            while(x < 8){
               def sx = cos(((2.0 * x + 1.0) * u * pi) / 16.0)
               def sy = cos(((2.0 * y + 1.0) * v * pi) / 16.0)
               sum += get(block, y * 8 + x) * sx * sy
               x += 1
            }
            y += 1
         }
         def cu = (u == 0) ? 0.7071067811865476 : 1.0
         def cv = (v == 0) ? 0.7071067811865476 : 1.0
         def coeff = 0.25 * cu * cv * sum
         def q = get(q_nat, v * 8 + u)
         store_item(nat, v * 8 + u, _round_i(coeff / (q + 0.0)))
         u += 1
      }
      v += 1
   }
   def zz = _mk_zero_list(64)
   mut i = 0
   while(i < 64){
      store_item(zz, i, get(nat, get(_ZIGZAG, i)))
      i += 1
   }
   zz
}

fn _encode_block(eb, zz, prev_dc, dc_map, ac_map){
   "Internal helper for `encode_block`."
   def dc = get(zz, 0)
   def diff = dc - prev_dc
   def dc_n = _mag_bits(diff)
   _henc(eb, dc_map, dc_n)
   if(dc_n > 0){
      _eb_put_bits(eb, _amp_bits(diff, dc_n), dc_n)
   }
   mut run = 0
   mut i = 1
   while(i < 64){
      def c = get(zz, i)
      if(c == 0){
         run += 1
      } else {
         while(run >= 16){
            _henc(eb, ac_map, 0xF0)
            run -= 16
         }
         def an = _mag_bits(c)
         _henc(eb, ac_map, (run << 4) | an)
         _eb_put_bits(eb, _amp_bits(c, an), an)
         run = 0
      }
      i += 1
   }
   if(run > 0){ _henc(eb, ac_map, 0) }
   dc
}

fn _pix_ch(data, w, h, ch, x, y, ci){
   "Internal helper for `pix_ch`."
   mut px = x
   mut py = y
   if(px < 0){ px = 0 } elif(px >= w){ px = w - 1 }
   if(py < 0){ py = 0 } elif(py >= h){ py = h - 1 }
   def off = (py * w + px) * ch
   if(ci < ch){ return load8(data, off + ci) }
   load8(data, off)
}

fn encode(img, quality=90){
   "Encodes image dict (RGBA/RGB/gray) to Baseline JPEG (SOF0, 4:4:4)."
   if(!is_dict(img)){ return 0 }
   def w = dict_get(img, "width", 0)
   def h = dict_get(img, "height", 0)
   def data = dict_get(img, "data", 0)
   mut ch = dict_get(img, "channels", 4)
   if(w <= 0 || h <= 0 || !is_str(data)){ return 0 }
   if(ch < 1){ ch = 1 } elif(ch > 4){ ch = 4 }
   if(len(data) < w * h * ch){ return 0 }
   def qtabs = _scaled_qtables(quality)
   def qy_nat = dict_get(qtabs, "qy_nat")
   def qc_nat = dict_get(qtabs, "qc_nat")
   def qy_zz = dict_get(qtabs, "qy_zz")
   def qc_zz = dict_get(qtabs, "qc_zz")
   def dc_luma = _mk_huff_map(_BITS_DC_LUMA, _VALS_DC)
   def dc_chroma = _mk_huff_map(_BITS_DC_CHROMA, _VALS_DC)
   def ac_luma = _mk_huff_map(_BITS_AC_LUMA, _VALS_AC_LUMA)
   def ac_chroma = _mk_huff_map(_BITS_AC_CHROMA, _VALS_AC_CHROMA)
   def out = list(4096)
   _out_marker(out, 0xD8)
   _out_marker(out, 0xE0)
   _out_u16(out, 16)
   _out_u8(out, 74) _out_u8(out, 70) _out_u8(out, 73) _out_u8(out, 70) _out_u8(out, 0)
   _out_u8(out, 1) _out_u8(out, 1) _out_u8(out, 0)
   _out_u16(out, 1) _out_u16(out, 1)
   _out_u8(out, 0) _out_u8(out, 0)
   _out_marker(out, 0xDB)
   _out_u16(out, 2 + (1 + 64) * 2)
   _out_u8(out, 0)
   _out_list_bytes(out, qy_zz)
   _out_u8(out, 1)
   _out_list_bytes(out, qc_zz)
   _out_marker(out, 0xC0)
   _out_u16(out, 17)
   _out_u8(out, 8)
   _out_u16(out, h)
   _out_u16(out, w)
   _out_u8(out, 3)
   _out_u8(out, 1) _out_u8(out, 0x11) _out_u8(out, 0)
   _out_u8(out, 2) _out_u8(out, 0x11) _out_u8(out, 1)
   _out_u8(out, 3) _out_u8(out, 0x11) _out_u8(out, 1)
   _out_marker(out, 0xC4)
   _out_u16(out, 2 + (1 + 16 + len(_VALS_DC)) + (1 + 16 + len(_VALS_AC_LUMA)) + (1 + 16 + len(_VALS_DC)) + (1 + 16 + len(_VALS_AC_CHROMA)))
   _out_u8(out, 0x00) _out_list_bytes(out, _BITS_DC_LUMA) _out_list_bytes(out, _VALS_DC)
   _out_u8(out, 0x10) _out_list_bytes(out, _BITS_AC_LUMA) _out_list_bytes(out, _VALS_AC_LUMA)
   _out_u8(out, 0x01) _out_list_bytes(out, _BITS_DC_CHROMA) _out_list_bytes(out, _VALS_DC)
   _out_u8(out, 0x11) _out_list_bytes(out, _BITS_AC_CHROMA) _out_list_bytes(out, _VALS_AC_CHROMA)
   _out_marker(out, 0xDA)
   _out_u16(out, 12)
   _out_u8(out, 3)
   _out_u8(out, 1) _out_u8(out, 0x00)
   _out_u8(out, 2) _out_u8(out, 0x11)
   _out_u8(out, 3) _out_u8(out, 0x11)
   _out_u8(out, 0) _out_u8(out, 63) _out_u8(out, 0)
   def eb = _eb_new()
   mut dc_y = 0 mut dc_cb = 0 mut dc_cr = 0
   mut by = 0
   while(by < h){
      mut bx = 0
      while(bx < w){
         def block_y = _mk_zero_list(64)
         def block_cb = _mk_zero_list(64)
         def block_cr = _mk_zero_list(64)
         mut yy = 0
         while(yy < 8){
            mut xx = 0
            while(xx < 8){
               def x = bx + xx
               def y = by + yy
               def r = _pix_ch(data, w, h, ch, x, y, 0)
               def g = (ch >= 3) ? _pix_ch(data, w, h, ch, x, y, 1) : r
               def b = (ch >= 3) ? _pix_ch(data, w, h, ch, x, y, 2) : r
               def fy =  0.29900 * r + 0.58700 * g + 0.11400 * b - 128.0
               def fcb = -0.16874 * r - 0.33126 * g + 0.50000 * b
               def fcr =  0.50000 * r - 0.41869 * g - 0.08131 * b
               store_item(block_y, yy * 8 + xx, fy)
               store_item(block_cb, yy * 8 + xx, fcb)
               store_item(block_cr, yy * 8 + xx, fcr)
               xx += 1
            }
            yy += 1
         }
         def zz_y = _fdct_quant_zz(block_y, qy_nat)
         def zz_cb = _fdct_quant_zz(block_cb, qc_nat)
         def zz_cr = _fdct_quant_zz(block_cr, qc_nat)
         if(env("NY_JPEG_DEBUG") && by == 0 && bx == 0){
            print("JPEG ENC DC Y/Cb/Cr = " + to_str(get(zz_y, 0)) + "/" + to_str(get(zz_cb, 0)) + "/" + to_str(get(zz_cr, 0)))
         }
         dc_y = _encode_block(eb, zz_y, dc_y, dc_luma, ac_luma)
         dc_cb = _encode_block(eb, zz_cb, dc_cb, dc_chroma, ac_chroma)
         dc_cr = _encode_block(eb, zz_cr, dc_cr, dc_chroma, ac_chroma)
         bx += 8
      }
      by += 8
   }
   _eb_flush(eb)
   _out_list_bytes(out, dict_get(eb, "buf"))
   _out_marker(out, 0xD9)
   _list_to_bytes(out)
}

if(comptime{__main()}){
   use std.core.error *

   def w = 16
   def h = 16
   def px = init_str(malloc(w * h * 4), w * h * 4)
   mut y = 0
   while(y < h){
      mut x = 0
      while(x < w){
         def o = (y * w + x) * 4
         store8(px, (x * 13 + y * 7) & 255, o)
         store8(px, (x * 5 + y * 17) & 255, o + 1)
         store8(px, (x * 11 + y * 3) & 255, o + 2)
         store8(px, 255, o + 3)
         x += 1
      }
      y += 1
   }

   mut img = dict(4)
   img = dict_set(img, "width", w)
   img = dict_set(img, "height", h)
   img = dict_set(img, "channels", 4)
   img = dict_set(img, "data", px)

   def enc = encode(img, 90)
   assert(is_str(enc) && len(enc) > 100, "jpeg encode bytes")
   assert(load8(enc, 0) == 0xFF && load8(enc, 1) == 0xD8, "jpeg soi")
   assert(load8(enc, len(enc) - 2) == 0xFF && load8(enc, len(enc) - 1) == 0xD9, "jpeg eoi")

   def dec = decode(enc)
   assert(dec != 0, "jpeg decode encoded")
   assert(dict_get(dec, "width") == w, "jpeg width")
   assert(dict_get(dec, "height") == h, "jpeg height")
   print("✓ std.image.jpeg tests passed")
}
