;; Keywords: image jpeg jpg parse
;; JPEG Image Loader and Encoder for Nytrix
;; Reference:
;; - https://jpeg.org/jpeg/index.html
;; - https://en.wikipedia.org/wiki/JPEG
;; - https://www.w3.org/Graphics/JPEG/itu-t81.pdf
;; - https://www.youtube.com/playlist?list=PLzH6n4zXuckoAod3z31QEST1ZaizBuNHh
;; References:
;; - std.parse.img
;; - std.parse
module std.parse.img.jpeg(encode, decode)
#include <turbojpeg.h>
extern "turbojpeg" {
   fn tjInitDecompress() ptr
   fn tjDecompressHeader3(ptr tj, ptr jpeg_buf, u64 jpeg_size, ptr width_p, ptr height_p, ptr subsamp_p, ptr colorspace_p) i32
   fn tjDecompress2(ptr tj, ptr jpeg_buf, u64 jpeg_size, ptr dst_buf, i32 width, i32 pitch, i32 height, i32 pixel_format, i32 flags) i32
   fn tjDestroy(ptr tj) i32
   fn _tj_init_decompress() ptr as "tjInitDecompress"
   fn _tj_decompress_header3(ptr tj, ptr jpeg_buf, u64 jpeg_size, ptr width_p, ptr height_p, ptr subsamp_p, ptr colorspace_p) i32 as "tjDecompressHeader3"
   fn _tj_decompress2(ptr tj, ptr jpeg_buf, u64 jpeg_size, ptr dst_buf, i32 width, i32 pitch, i32 height, i32 pixel_format, i32 flags) i32 as "tjDecompress2"
   fn _tj_destroy(ptr tj) i32 as "tjDestroy"
}

use std.core
use std.core.dict_mod
use std.math
use std.math.bin as pbin
use std.core.str as str
use std.core.common as common

def _QY = [16, 11, 10, 16, 24, 40, 51, 61, 12, 12, 14, 19, 26, 58, 60, 55, 14, 13, 16, 24, 40, 57, 69, 56, 14, 17, 22, 29, 51, 87, 80, 62, 18, 22, 37, 56, 68, 109, 103, 77, 24, 35, 55, 64, 81, 104, 113, 92, 49, 64, 78, 87, 103, 121, 120, 101, 72, 92, 95, 98, 112, 100, 103, 99]
def _QC = [17, 18, 24, 47, 99, 99, 99, 99, 18, 21, 26, 66, 99, 99, 99, 99, 24, 26, 56, 99, 99, 99, 99, 99, 47, 66, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99]
def _DCL = [0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0]
def _DCC = [0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0]
def _VDC = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11]
def _ACL = [0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 125]
def _VAL = [1, 2, 3, 0, 4, 17, 5, 18, 33, 49, 65, 6, 19, 81, 97, 7, 34, 113, 20, 50, 129, 145, 161, 8, 35, 66, 177, 193, 21, 82, 209, 240, 36, 51, 98, 114, 130, 9, 10, 22, 23, 24, 25, 26, 37, 38, 39, 40, 41, 42, 52, 53, 54, 55, 56, 57, 58, 67, 68, 69, 70, 71, 72, 73, 74, 83, 84, 85, 86, 87, 88, 89, 90, 99, 100, 101, 102, 103, 104, 105, 106, 115, 116, 117, 118, 119, 120, 121, 122, 131, 132, 133, 134, 135, 136, 137, 138, 146, 147, 148, 149, 150, 151, 152, 153, 154, 162, 163, 164, 165, 166, 167, 168, 169, 170, 178, 179, 180, 181, 182, 183, 184, 185, 186, 194, 195, 196, 197, 198, 199, 200, 201, 202, 210, 211, 212, 213, 214, 215, 216, 217, 218, 225, 226, 227, 228, 229, 230, 231, 232, 233, 234, 241, 242, 243, 244, 245, 246, 247, 248, 249, 250]
def _ACC = [0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 119]
def _VAC = [0, 1, 2, 3, 17, 4, 5, 33, 49, 6, 18, 65, 81, 7, 97, 113, 19, 34, 50, 129, 8, 20, 66, 145, 161, 177, 193, 9, 35, 51, 82, 240, 21, 98, 114, 209, 10, 22, 36, 52, 225, 37, 241, 23, 24, 25, 26, 38, 39, 40, 41, 42, 53, 54, 55, 56, 57, 58, 67, 68, 69, 70, 71, 72, 73, 74, 83, 84, 85, 86, 87, 88, 89, 90, 99, 100, 101, 102, 103, 104, 105, 106, 115, 116, 117, 118, 119, 120, 121, 122, 130, 131, 132, 133, 134, 135, 136, 137, 138, 146, 147, 148, 149, 150, 151, 152, 153, 154, 162, 163, 164, 165, 166, 167, 168, 169, 170, 178, 179, 180, 181, 182, 183, 184, 185, 186, 194, 195, 196, 197, 198, 199, 200, 201, 202, 210, 211, 212, 213, 214, 215, 216, 217, 218, 226, 227, 228, 229, 230, 231, 232, 233, 234, 242, 243, 244, 245, 246, 247, 248, 249, 250]
def _ZZ = [0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63]
mut _jpeg_diag_total_blocks = 0
mut _jpeg_diag_dc_fail = 0
mut _jpeg_diag_ac_fail = 0
mut _jpeg_trace_cache = -1
mut _jpeg_disable_turbo_cache = -1
mut _jpeg_max_scan_cache = -1

fn _raw_ptr(any p) any {
   if(!p){ return 0 }
   if(is_int(p)){ return to_int(p) }
   p
}

fn _jpeg_decode_turbo(any data) any {
   def handle = _tj_init_decompress()
   if(!handle){ return 0 }
   def w_p = zalloc(4) def h_p = zalloc(4) def ss_p = zalloc(4) def cs_p = zalloc(4)
   if(!w_p || !h_p || !ss_p || !cs_p){
      free(w_p, h_p, ss_p, cs_p)
      _tj_destroy(handle)
      return 0
   }
   def hdr_res = _tj_decompress_header3(handle, _raw_ptr(data), data.len, w_p, h_p, ss_p, cs_p)
   if(int(hdr_res) != 0){
      free(w_p, h_p, ss_p, cs_p)
      _tj_destroy(handle)
      return 0
   }
   def wVal, hVal = load32(w_p, 0), load32(h_p, 0)
   free(w_p, h_p, ss_p, cs_p)
   if(wVal <= 0 || hVal <= 0 || wVal > 16384 || hVal > 16384){
      _tj_destroy(handle)
      return 0
   }
   def pix_len = wVal * hVal * 4
   def pix_raw = malloc(pix_len + 32)
   if(!pix_raw){
      _tj_destroy(handle)
      return 0
   }
   def dec_res = _tj_decompress2(handle, _raw_ptr(data), data.len, pix_raw, wVal, 0, hVal, 7, 0)
   _tj_destroy(handle)
   if(int(dec_res) != 0){
      free(pix_raw)
      return 0
   }
   def pix = init_str(pix_raw, pix_len)
   mut rd = dict(8)
   rd = rd.set("data", pix)
   rd = rd.set("width", wVal)
   rd = rd.set("height", hVal)
   rd = rd.set("channels", 4)
   rd
}

fn _jpeg_diag_enabled() bool {
   _jpeg_trace_cache = common.cached_env_truthy(_jpeg_trace_cache, "NY_JPEG_TRACE")
   _jpeg_trace_cache == 1
}

fn _jpeg_diag_on() bool { _jpeg_diag_enabled() }

fn _jpeg_disable_turbo() bool {
   _jpeg_disable_turbo_cache = common.cached_env_truthy(_jpeg_disable_turbo_cache, "NY_JPEG_DISABLE_TURBO")
   _jpeg_disable_turbo_cache == 1
}

fn _jpeg_prog_max_scan() int {
   if(_jpeg_max_scan_cache != -1){ return _jpeg_max_scan_cache }
   _jpeg_max_scan_cache = common.parse_nonneg_int(common.env_trim("NY_JPEG_MAX_SCAN"))
   _jpeg_max_scan_cache
}

fn _c_u8(any v) int {
   if(v < 0){ return 0 }
   if(v > 255){ return 255 }
   v
}

fn _m_hm(list b, list val_list) list {
   mut cm, sm = pbin.zero_list(256), pbin.zero_list(256)
   mut cCount, kCount = 0, 0
   mut lIdx = 1
   while(lIdx <= 16){
      def ctVal = b.get((lIdx - 1))
      mut j = 0
      while(j < ctVal){
         def sSym = val_list.get(kCount)
         cm[sSym] = cCount
         sm[sSym] = lIdx
         cCount += 1
         kCount += 1
         j += 1
      }
      cCount = cCount << 1
      lIdx += 1
   }
   return [cm, sm]
}

fn _h_new(list ctx) int {
   mut nodeList = ctx.get(0)
   def idx = nodeList.len
   mut n = list(4)
   n = n.append(0)
   n = n.append(0)
   n = n.append(-1)
   n = n.append(-1)
   nodeList = nodeList.append(n)
   ctx[0] = nodeList
   return idx
}

fn _h_add(list ctx, int root, int code, int len_bits, int sym) any {
   mut cur = root
   mut iBit = len_bits
   while(iBit > 0){
      def nodeList = ctx.get(0)
      def nObj = nodeList.get(cur)
      def bitVal = (code >> (iBit - 1)) & 1
      if(bitVal){
         mut rVal = nObj.get(3)
         if(rVal == -1){
            rVal = _h_new(ctx)
            def nUpd = ctx.get(0).get(cur)
            nUpd[3] = rVal
         }
         cur = rVal
      } else {
         mut lVal = nObj.get(2)
         if(lVal == -1){
            lVal = _h_new(ctx)
            def nUpd = ctx.get(0).get(cur)
            nUpd[2] = lVal
         }
         cur = lVal
      }
      iBit -= 1
   }
   def nodeListF = ctx.get(0)
   def leaf = nodeListF.get(cur)
   leaf[0] = sym
   leaf[1] = 1
   0
}

fn _h_parse(any data, int off) list {
   mut ctx = list(1)
   ctx = ctx.append(list(512))
   _h_new(ctx)
   mut counts = list(16)
   mut iC = 0
   while(iC < 16){
      counts = counts.append(load8(data, (off + iC)))
      iC += 1
   }
   mut pPtr = off + 16
   mut cvVal = 0
   mut clIdx = 1
   while(clIdx <= 16){
      def cntVal = counts.get((clIdx - 1))
      mut j = 0
      while(j < cntVal){
         _h_add(ctx, 0, cvVal, clIdx, load8(data, pPtr))
         pPtr += 1
         cvVal += 1
         j += 1
      }
      cvVal = cvVal << 1
      clIdx += 1
   }
   return [ctx.get(0), (pPtr - off)]
}

fn _h_dec(list nodes, list bs) int {
   mut cur = 0
   while(1){
      mut bl = bs.get(3)
      if(bl == 0){
         def db = bs.get(0)
         mut bp = bs.get(1)
         if(bp >= db.len){ return -1 }
         mut cV = load8(db, bp)
         bp += 1
         if(cV == 255){
            if(bp < db.len){ if(load8(db, bp) == 0){ bp += 1 } }
         }
         bs[2] = cV
         bs[3] = 8
         bs[1] = bp
         bl = 8
      }
      bl -= 1
      def bit = (bs.get(2) >> bl) & 1
      bs[3] = bl
      def n = nodes.get(cur)
      if(bit){ cur = n.get(3) } else { cur = n.get(2) }
      if(cur == -1){ return -1 }
      def cn = nodes.get(cur)
      if(cn.get(1)){ return cn.get(0) }
   }
}

fn _bs_m(any data, int start) list {
   mut bs = list(5)
   bs = bs.append(data)
   bs = bs.append(start)
   bs = bs.append(0)
   bs = bs.append(0)
   bs = bs.append(0)
   return bs
}

fn _bs_skip_restart(any bs, int expected_rst=-1) bool {
   if(!is_list(bs)){ return false }
   bs[3] = 0
   bs[2] = 0
   def db = bs.get(0)
   mut bp = bs.get(1)
   if(!is_str(db) || bp < 0 || bp + 1 >= db.len){ return false }
   while(bp + 1 < db.len && load8(db, bp) == 255 && load8(db, bp + 1) == 255){ bp += 1 }
   if(bp + 1 >= db.len || load8(db, bp) != 255){ return false }
   def mk = load8(db, bp + 1)
   if(mk < 208 || mk > 215){ return false }
   if(expected_rst >= 0 && mk != (208 + (expected_rst & 7))){ return false }
   bp += 2
   bs[1] = bp
   true
}

fn _bs_gbs(list bs, int n) int {
   mut v = 0
   mut iCount = 0
   while(iCount < n){
      mut bl = bs.get(3)
      if(bl == 0){
         def db = bs.get(0)
         mut bp = bs.get(1)
         if(bp < db.len){
            mut cV = load8(db, bp)
            bp += 1
            if(cV == 255){
               if(bp < db.len){ if(load8(db, bp) == 0){ bp += 1 } }
            }
            bs[2] = cV
            bs[3] = 8
            bs[1] = bp
            bl = 8
         } else {
            break
         }
      }
      bl -= 1
      v = (v << 1) | ((bs.get(2) >> bl) & 1)
      bs[3] = bl
      iCount += 1
   }
   return v
}

fn _d_cf(int sz, int bits) int {
   if(sz == 0){ return 0 }
   if((bits >> (sz - 1)) & 1){ return bits }
   return bits - (1 << sz) + 1
}

fn _jpeg_find_next_marker(any data, int start) int {
   if(!is_str(data)){ return data.len }
   mut i = start
   def n = data.len
   while(i + 1 < n){
      def b = load8(data, i)
      if(b != 255){
         i += 1
         continue
      }
      mut j = i + 1
      while(j < n && load8(data, j) == 255){ j += 1 }
      if(j >= n){ return n }
      def c = load8(data, j)
      if(c == 0){
         i = j + 1
         continue
      }
      if(c >= 208 && c <= 215){
         i = j + 1
         continue
      }
      return i
   }
   n
}

fn _jpeg_prog_block_dims(int wVal, int hVal, int mh, int mv, int hs, int vs) list {
   def mcu_w, mcu_h = mh * 8, mv * 8
   def bw, bh = max(1, ((wVal + mcu_w - 1) / mcu_w) * hs), max(1, ((hVal + mcu_h - 1) / mcu_h) * vs)
   [bw, bh, bw * bh]
}

fn _jpeg_prog_alloc_component(int wVal, int hVal, int mh, int mv, any dobj) list {
   def hs, vs = int(dobj.get(1, 1)), int(dobj.get(2, 1))
   def dims = _jpeg_prog_block_dims(wVal, hVal, mh, mv, hs, vs)
   def bw = int(dims.get(0, 0))
   def bh = int(dims.get(1, 0))
   def full_blocks = max(1, ((wVal + 7) / 8) * ((hVal + 7) / 8))
   def blocks = max(int(dims.get(2, 0)), full_blocks)
   def coeff_bytes = blocks * 64 * 4
   if(coeff_bytes <= 0 || coeff_bytes > (256 * 1024 * 1024)){ return [bw, bh, blocks, 0] }
   def coeff = malloc(coeff_bytes)
   if(!coeff){ return [bw, bh, blocks, 0] }
   mut zi = 0
   while(zi < coeff_bytes){
      store8(coeff, 0, zi)
      zi += 1
   }
   [bw, bh, blocks, coeff]
}

fn _jpeg_prog_coeff_at(any coeff, int block_idx, int pos) int {
   mut v = load32(coeff, (block_idx * 64 + pos) * 4)
   if(v >= 2147483648){ v -= 4294967296 }
   v
}

fn _jpeg_prog_set_coeff(any coeff, int block_idx, int pos, any value) any {
   store32(coeff, int(value), (block_idx * 64 + pos) * 4)
   0
}

fn _jpeg_prog_refine_nonzero(any coeff, int block_idx, int pos, int al, list bs) any {
   def cur = _jpeg_prog_coeff_at(coeff, block_idx, pos)
   if(cur == 0){ return 0 }
   def bit = _bs_gbs(bs, 1)
   if(bit == 0){ return 0 }
   def step = 1 << al
   if(cur > 0){
      if((cur & step) == 0){ _jpeg_prog_set_coeff(coeff, block_idx, pos, cur + step) }
   } else {
      def mag = -cur
      if((mag & step) == 0){ _jpeg_prog_set_coeff(coeff, block_idx, pos, cur - step) }
   }
   0
}

fn _jpeg_prog_render_component(any plane, int plane_len, int plane_w, any coeff, any qtbl, int bw, int bh, int step_x, int step_y) any {
   if(!plane || !coeff || bw <= 0 || bh <= 0 || !is_list(qtbl)){ return 0 }
   mut cf = pbin.zero_list(64)
   mut blk = pbin.zero_list(64)
   mut nat = pbin.zero_list(64)
   mut by = 0
   while(by < bh){
      mut bx = 0
      while(bx < bw){
         def bi = by * bw + bx
         mut k = 0
         while(k < 64){
            cf[k] = _jpeg_prog_coeff_at(coeff, bi, k)
            k += 1
         }
         def off = (by * 8 * step_y * plane_w) + (bx * 8 * step_x)
         _idct(cf, qtbl, plane, plane_len, off, step_x, step_y, plane_w, blk, nat)
         bx += 1
      }
      by += 1
   }
   0
}

fn _jpeg_alloc_planes(int plane_sz) any {
   def alloc_sz = plane_sz + 32
   def cyB = init_str(malloc(alloc_sz), plane_sz)
   def ccbB = init_str(malloc(alloc_sz), plane_sz)
   def ccrB = init_str(malloc(alloc_sz), plane_sz)
   if(!cyB || !ccbB || !ccrB){
      if(cyB){ free(cyB) }
      if(ccbB){ free(ccbB) }
      if(ccrB){ free(ccrB) }
      return 0
   }
   mut i = 0
   while(i < plane_sz){
      store8(cyB, 128, i)
      store8(ccbB, 128, i)
      store8(ccrB, 128, i)
      i += 1
   }
   [cyB, ccbB, ccrB]
}

fn _jpeg_planes_to_rgba(any cyB, any ccbB, any ccrB, int wVal, int hVal) any {
   def tpx = wVal * hVal
   def pix = init_str(malloc(tpx * 4 + 32), (tpx * 4))
   mut kL = 0
   while(kL + 4 <= tpx){
      def y0, y1 = load8(cyB, kL), load8(cyB, kL + 1)
      def y2, y3 = load8(cyB, kL + 2), load8(cyB, kL + 3)
      def cb0, cb1 = load8(ccbB, kL) - 128, load8(ccbB, kL + 1) - 128
      def cb2, cb3 = load8(ccbB, kL + 2) - 128, load8(ccbB, kL + 3) - 128
      def cr0, cr1 = load8(ccrB, kL) - 128, load8(ccrB, kL + 1) - 128
      def cr2, cr3 = load8(ccrB, kL + 2) - 128, load8(ccrB, kL + 3) - 128
      def r0, g0 = y0 + (cr0 * 1436) / 1024, y0 - (cb0 * 352) / 1024 - (cr0 * 731) / 1024
      def b0 = y0 + (cb0 * 1815) / 1024
      def r1 = y1 + (cr1 * 1436) / 1024
      def g1 = y1 - (cb1 * 352) / 1024 - (cr1 * 731) / 1024
      def b1 = y1 + (cb1 * 1815) / 1024
      def r2 = y2 + (cr2 * 1436) / 1024
      def g2 = y2 - (cb2 * 352) / 1024 - (cr2 * 731) / 1024
      def b2 = y2 + (cb2 * 1815) / 1024
      def r3 = y3 + (cr3 * 1436) / 1024
      def g3 = y3 - (cb3 * 352) / 1024 - (cr3 * 731) / 1024
      def b3 = y3 + (cb3 * 1815) / 1024
      def d0 = kL * 4
      store32(pix, _c_u8(r0)|(_c_u8(g0)<<8)|(_c_u8(b0)<<16)|(255<<24), d0)
      store32(pix, _c_u8(r1)|(_c_u8(g1)<<8)|(_c_u8(b1)<<16)|(255<<24), d0+4)
      store32(pix, _c_u8(r2)|(_c_u8(g2)<<8)|(_c_u8(b2)<<16)|(255<<24), d0+8)
      store32(pix, _c_u8(r3)|(_c_u8(g3)<<8)|(_c_u8(b3)<<16)|(255<<24), d0+12)
      kL = kL + 4
   }
   while(kL < tpx){
      def y = load8(cyB, kL)
      def cb = load8(ccbB, kL) - 128
      def cr = load8(ccrB, kL) - 128
      def r = y + (cr * 1436) / 1024
      def g = y - (cb * 352) / 1024 - (cr * 731) / 1024
      def b = y + (cb * 1815) / 1024
      def d = kL * 4
      store32(pix, _c_u8(r)|(_c_u8(g)<<8)|(_c_u8(b)<<16)|(255<<24), d)
      kL += 1
   }
   pix
}

fn _jpeg_image_result(any pix, int wVal, int hVal) any {
   mut rd = dict(8)
   rd = rd.set("data", pix)
   rd = rd.set("width", wVal)
   rd = rd.set("height", hVal)
   rd = rd.set("channels", 4)
   rd
}

fn _jpeg_prog_component_info(int wVal, int hVal, int mh, int mv, list cidMap, list cord) any {
   mut comp_info = dict(16)
   def cord_n = cord.len
   mut ci = 0
   while(ci < cord_n){
      def cid = cord.get(ci, -1)
      def dobj = cidMap.get(cid)
      if(is_list(dobj)){
         def alloc = _jpeg_prog_alloc_component(wVal, hVal, mh, mv, dobj)
         def bw = int(alloc.get(0, 0))
         def bh = int(alloc.get(1, 0))
         def coeff = alloc.get(3, 0)
         if(coeff){
            mut nd = list(6)
            nd = nd.append(dobj.get(0, 0))
            nd = nd.append(dobj.get(1, 1))
            nd = nd.append(dobj.get(2, 1))
            nd = nd.append(bw)
            nd = nd.append(bh)
            nd = nd.append(coeff)
            comp_info = comp_info.set(cid, nd)
         }
      }
      ci += 1
   }
   comp_info
}

fn _jpeg_progressive_result(list planes, int plane_sz, int wVal, int hVal, int mh, int mv, list cord, any comp_info, list qts) any {
   def cyB, ccbB = planes.get(0), planes.get(1)
   def ccrB = planes.get(2)
   mut Yid = 1
   if(cord.len > 0){ Yid = cord.get(0) }
   mut Cbid = Yid
   if(cord.len > 1){ Cbid = cord.get(1) }
   mut Crid = Cbid
   if(cord.len > 2){ Crid = cord.get(2) }
   mut yInfo = comp_info.get(Yid, 0)
   mut cbInfo = comp_info.get(Cbid, 0)
   mut crInfo = comp_info.get(Crid, 0)
   if(!is_list(yInfo) || !is_list(cbInfo) || !is_list(crInfo)){
      free(cyB) free(ccbB) free(ccrB)
      return 0
   }
   def yCoeff = yInfo.get(5, 0)
   def cbCoeff = cbInfo.get(5, 0)
   def crCoeff = crInfo.get(5, 0)
   def yQt = qts.get(int(yInfo.get(0, 0)), 0)
   def cbQt = qts.get(int(cbInfo.get(0, 0)), 0)
   def crQt = qts.get(int(crInfo.get(0, 0)), 0)
   def yStepX = max(1, mh / max(1, int(yInfo.get(1, 1))))
   def yStepY = max(1, mv / max(1, int(yInfo.get(2, 1))))
   def cbStepX = max(1, mh / max(1, int(cbInfo.get(1, 1))))
   def cbStepY = max(1, mv / max(1, int(cbInfo.get(2, 1))))
   def crStepX = max(1, mh / max(1, int(crInfo.get(1, 1))))
   def crStepY = max(1, mv / max(1, int(crInfo.get(2, 1))))
   _jpeg_prog_render_component(cyB, plane_sz, wVal, yCoeff, yQt, int(yInfo.get(3, 0)), int(yInfo.get(4, 0)), yStepX, yStepY)
   _jpeg_prog_render_component(ccbB, plane_sz, wVal, cbCoeff, cbQt, int(cbInfo.get(3, 0)), int(cbInfo.get(4, 0)), cbStepX, cbStepY)
   _jpeg_prog_render_component(ccrB, plane_sz, wVal, crCoeff, crQt, int(crInfo.get(3, 0)), int(crInfo.get(4, 0)), crStepX, crStepY)
   def pix = _jpeg_planes_to_rgba(cyB, ccbB, ccrB, wVal, hVal)
   def rd = _jpeg_image_result(pix, wVal, hVal)
   free(cyB, ccbB, ccrB)
   rd
}

fn _jpeg_prog_decode_interleaved_dc(int wVal, int hVal, int mh, int mv, any comp_info, list hdc, any comps, any comp_meta, int comps_n, int comp_meta_n, int ah, int al, list bs) any {
   mut pdc_map = dict(8)
   mut mcy = 0
   while((mcy * 8 * mv) < hVal){
      mut mcx = 0
      while((mcx * 8 * mh) < wVal){
         mut cii = 0
         while(cii < comps_n){
            def cid = comps.get(cii, -1)
            def info = comp_info.get(cid, 0)
            if(is_list(info)){
               mut meta = 0
               if(cii < comp_meta_n){ meta = comp_meta.get(cii, 0) }
               def hs, vs = int(info.get(1, 1)), int(info.get(2, 1))
               mut dcid = 0
               mut dh = 0
               if(is_list(meta)){
                  dcid = int(meta.get(1, 0))
                  dh = meta.get(3, 0)
               }
               def bw = int(info.get(3, 0))
               def coeff = info.get(5, 0)
               if(!dh){ dh = hdc.get(dcid, 0) }
               mut pdc = int(pdc_map.get(cid, 0))
               mut vb = 0
               while(vb < vs){
                  mut hb = 0
                  while(hb < hs){
                     def bi = (mcy * vs + vb) * bw + (mcx * hs + hb)
                     if(bi >= 0 && bi < (bw * int(info.get(4, 0)))){
                        if(ah == 0){
                           def sz = _h_dec(dh, bs)
                           if(sz < 0){ break }
                           def bits = _bs_gbs(bs, sz)
                           def d_val = _d_cf(sz, bits)
                           pdc = pdc + d_val
                           _jpeg_prog_set_coeff(coeff, bi, 0, pdc << al)
                        } else {
                           def bit = _bs_gbs(bs, 1)
                           if(bit != 0){
                              def cur = _jpeg_prog_coeff_at(coeff, bi, 0)
                              _jpeg_prog_set_coeff(coeff, bi, 0, cur | (1 << al))
                           }
                        }
                     }
                     hb += 1
                  }
                  vb += 1
               }
               pdc_map = pdc_map.set(cid, pdc)
            }
            cii += 1
         }
         mcx += 1
      }
      mcy += 1
   }
   0
}

fn _jpeg_prog_decode_ac_raster(any coeff, int bw, int bh, int ss, int se, int ah, int al, any ah_tbl, list bs) any {
   mut eobrun = 0
   mut by = 0
   while(by < bh){
      mut bx = 0
      while(bx < bw){
         def bi = by * bw + bx
         if(ah == 0){
            if(eobrun > 0){ eobrun -= 1 } else {
               mut k = ss
               while(k <= se){
                  def sym = _h_dec(ah_tbl, bs)
                  if(sym < 0){ break }
                  def run = sym >> 4
                  def size = sym & 15
                  if(size == 0){
                     if(run == 15){ k += 16 } else {
                        eobrun = (1 << run) - 1
                        if(run > 0){ eobrun += _bs_gbs(bs, run) }
                        break
                     }
                  } else {
                     k += run
                     if(k > se){ break }
                     def bits = _bs_gbs(bs, size)
                     def v = _d_cf(size, bits) << al
                     _jpeg_prog_set_coeff(coeff, bi, k, v)
                     k += 1
                  }
               }
            }
         } else {
            mut k = ss
            while(k <= se){
               if(eobrun > 0){
                  while(k <= se){
                     if(_jpeg_prog_coeff_at(coeff, bi, k) != 0){ _jpeg_prog_refine_nonzero(coeff, bi, k, al, bs) }
                     k += 1
                  }
                  eobrun -= 1
                  break
               }
               def sym = _h_dec(ah_tbl, bs)
               if(sym < 0){ break }
               mut run = sym >> 4
               def size = sym & 15
               if(size == 0){
                  if(run == 15){
                     mut z = 16
                     while(k <= se && z > 0){
                        if(_jpeg_prog_coeff_at(coeff, bi, k) != 0){ _jpeg_prog_refine_nonzero(coeff, bi, k, al, bs) } else { z -= 1 }
                        k += 1
                     }
                  } else {
                     eobrun = (1 << run)
                     if(run > 0){ eobrun += _bs_gbs(bs, run) }
                     eobrun -= 1
                     while(k <= se){
                        if(_jpeg_prog_coeff_at(coeff, bi, k) != 0){ _jpeg_prog_refine_nonzero(coeff, bi, k, al, bs) }
                        k += 1
                     }
                     break
                  }
               } else {
                  while(k <= se){
                     if(_jpeg_prog_coeff_at(coeff, bi, k) != 0){ _jpeg_prog_refine_nonzero(coeff, bi, k, al, bs) } else {
                        if(run == 0){ break }
                        run -= 1
                     }
                     k += 1
                  }
                  if(k <= se){
                     mut v = 1 << al
                     if(size != 1){ v = _d_cf(size, _bs_gbs(bs, size)) << al } else { if(_bs_gbs(bs, 1) == 0){ v = -(1 << al) } }
                     _jpeg_prog_set_coeff(coeff, bi, k, v)
                     k += 1
                  }
               }
            }
         }
         bx += 1
      }
      by += 1
   }
   0
}

fn _jpeg_decode_progressive(any data, int wVal, int hVal, int mh, int mv, list qts, list hdc, list hac, list cidMap, list cord, list scan_list) any {
   def tpx = wVal * hVal
   def mcu_w = mh * 8
   def mcu_h = mv * 8
   def plane_sz = ((wVal + mcu_w - 1) / mcu_w) * mcu_w * ((hVal + mcu_h - 1) / mcu_h) * mcu_h
   def planes = _jpeg_alloc_planes(plane_sz)
   if(!planes){ return 0 }
   def comp_info = _jpeg_prog_component_info(wVal, hVal, mh, mv, cidMap, cord)
   def max_scan = _jpeg_prog_max_scan()
   def scan_list_n = scan_list.len
   mut si = 0
   while(si < scan_list_n){
      if(max_scan > 0 && si >= max_scan){ break }
      def scan = scan_list.get(si, 0)
      if(is_list(scan) && scan.len >= 8){
         def scan_start = int(scan.get(0, 0))
         def comps = scan.get(2, 0)
         def comp_meta = scan.get(3, 0)
         def ss = int(scan.get(4, 0))
         def se = int(scan.get(5, 0))
         def ah = int(scan.get(6, 0))
         def al = int(scan.get(7, 0))
         def bs = _bs_m(data, scan_start)
         def comps_n = is_list(comps) ? comps.len : 0
         def comp_meta_n = is_list(comp_meta) ? comp_meta.len : 0
         def interleaved = comps_n > 1
         if(interleaved && ss == 0){
            _jpeg_prog_decode_interleaved_dc(wVal, hVal, mh, mv, comp_info, hdc, comps, comp_meta, comps_n, comp_meta_n, ah, al, bs)
            si += 1
            continue
         }
         mut cii = 0
         while(cii < comps_n){
            def cid = comps.get(cii, -1)
            def info = comp_info.get(cid, 0)
            if(is_list(info)){
               mut meta = 0
               if(cii < comp_meta_n){ meta = comp_meta.get(cii, 0) }
               def hs, vs = int(info.get(1, 1)), int(info.get(2, 1))
               mut dcid, acid = 0, 0
               mut dh = 0
               mut ah_tbl = 0
               if(is_list(meta)){
                  dcid, acid = int(meta.get(1, 0)), int(meta.get(2, 0))
                  dh, ah_tbl = meta.get(3, 0), meta.get(4, 0)
               }
               def bw, bh = int(info.get(3, 0)), int(info.get(4, 0))
               def coeff = info.get(5, 0)
               if(!dh){ dh = hdc.get(dcid, 0) }
               if(!ah_tbl){ ah_tbl = hac.get(acid, 0) }
               mut pdc = 0
               if(ss > 0 && !interleaved){
                  _jpeg_prog_decode_ac_raster(coeff, bw, bh, ss, se, ah, al, ah_tbl, bs)
               } else {
                  mut mcy = 0
                  while((mcy * 8 * mv) < hVal){
                     mut mcx = 0
                     while((mcx * 8 * mh) < wVal){
                        mut vb = 0
                        while(vb < vs){
                           mut hb = 0
                           while(hb < hs){
                              def bi = (mcy * vs + vb) * bw + (mcx * hs + hb)
                              if(bi >= 0 && bi < (bw * bh)){
                                 if(ss == 0){
                                    if(ah == 0){
                                       def sz = _h_dec(dh, bs)
                                       def bits = _bs_gbs(bs, sz)
                                       def d_val = _d_cf(sz, bits)
                                       pdc = pdc + d_val
                                       _jpeg_prog_set_coeff(coeff, bi, 0, pdc << al)
                                    } else {
                                       def bit = _bs_gbs(bs, 1)
                                       if(bit != 0){
                                          def cur = _jpeg_prog_coeff_at(coeff, bi, 0)
                                          _jpeg_prog_set_coeff(coeff, bi, 0, cur | (1 << al))
                                       }
                                    }
                                 }
                              }
                              hb += 1
                           }
                           vb += 1
                        }
                        mcx += 1
                     }
                     mcy += 1
                  }
               }
            }
            cii += 1
         }
      }
      si += 1
   }
   _jpeg_progressive_result(planes, plane_sz, wVal, hVal, mh, mv, cord, comp_info, qts)
}

def _JIDCT_CONST_BITS = 13
def _JIDCT_PASS1_BITS = 2
def _JIDCT_FIX_0_298631336 = 2446
def _JIDCT_FIX_0_390180644 = 3196
def _JIDCT_FIX_0_541196100 = 4433
def _JIDCT_FIX_0_765366865 = 6270
def _JIDCT_FIX_0_899976223 = 7373
def _JIDCT_FIX_1_175875602 = 9633
def _JIDCT_FIX_1_501321110 = 12299
def _JIDCT_FIX_1_847759065 = 15137
def _JIDCT_FIX_1_961570560 = 16069
def _JIDCT_FIX_2_053119869 = 16819
def _JIDCT_FIX_2_562915447 = 20995
def _JIDCT_FIX_3_072711026 = 25172

fn _jpeg_descale(int v, int n) int {
   if(n <= 0){ return v }
   def add = 1 << (n - 1)
   if(v >= 0){ return(v + add) >> n }
   -(((-v) + add) >> n)
}

fn _idct_has_ac(list cf) bool {
   mut ci = 1
   while(ci < 64){
      if(cf.get(ci) != 0){ return true }
      ci += 1
   }
   false
}

fn _idct_store_sample(any out, int out_len, int plane_w, int off, int step_x, int step_y, int x, int y, int fv) any {
   def py, px = (off / plane_w) + y * step_y, (off % plane_w) + x * step_x
   mut dy = 0
   while(dy < step_y){
      mut dx = 0
      while(dx < step_x){
         def pidx = (py + dy) * plane_w + (px + dx)
         if(pidx < out_len){ store8(out, fv, pidx) }
         dx += 1
      }
      dy += 1
   }
   0
}

fn _idct_store_dc(list cf, list qt, any out, int out_len, int off, int step_x, int step_y, int plane_w) any {
   def dcv = cf.get(0) * qt.get(0)
   def fv = _c_u8(_jpeg_descale(dcv, 3) + 128)
   mut y = 0
   while(y < 8){
      mut x = 0
      while(x < 8){
         _idct_store_sample(out, out_len, plane_w, off, step_x, step_y, x, y, fv)
         x += 1
      }
      y += 1
   }
   0
}

fn _idct_prepare_block(list cf, list qt, list blk, list natural) any {
   mut kIdx = 0
   while(kIdx < 64){
      blk.set(kIdx, cf.get(kIdx) * qt.get(kIdx))
      kIdx += 1
   }
   kIdx = 0
   while(kIdx < 64){
      natural.set(_ZZ.get(kIdx), blk.get(kIdx))
      kIdx += 1
   }
   0
}

fn _idct_pass_columns(list blk, list natural) any {
   mut col = 0
   while(col < 8){
      def c1, c2 = natural.get(8 + col), natural.get(16 + col)
      def c3, c4 = natural.get(24 + col), natural.get(32 + col)
      def c5, c6 = natural.get(40 + col), natural.get(48 + col)
      def c7 = natural.get(56 + col)
      if(c1 == 0 && c2 == 0 && c3 == 0 && c4 == 0 && c5 == 0 && c6 == 0 && c7 == 0){
         def dcval = natural.get(col) << _JIDCT_PASS1_BITS
         mut ry = 0
         while(ry < 8){
            blk.set(ry * 8 + col, dcval)
            ry += 1
         }
         col += 1
         continue
      }
      mut z2, z3 = natural.get(16 + col), natural.get(48 + col)
      mut z1 = (z2 + z3) * _JIDCT_FIX_0_541196100
      mut tmp2 = z1 + z3 * (-_JIDCT_FIX_1_847759065)
      mut tmp3 = z1 + z2 * _JIDCT_FIX_0_765366865
      z2, z3 = natural.get(col), natural.get(32 + col)
      mut tmp0, tmp1 = (z2 + z3) << _JIDCT_CONST_BITS, (z2 - z3) << _JIDCT_CONST_BITS
      mut tmp10, tmp13 = tmp0 + tmp3, tmp0 - tmp3
      mut tmp11, tmp12 = tmp1 + tmp2, tmp1 - tmp2
      tmp0, tmp1 = natural.get(56 + col), natural.get(40 + col)
      tmp2, tmp3 = natural.get(24 + col), natural.get(8 + col)
      z1, z2 = tmp0 + tmp3, tmp1 + tmp2
      z3 = tmp0 + tmp2
      mut z4, z5 = tmp1 + tmp3, (z3 + z4) * _JIDCT_FIX_1_175875602
      tmp0, tmp1 = tmp0 * _JIDCT_FIX_0_298631336, tmp1 * _JIDCT_FIX_2_053119869
      tmp2, tmp3 = tmp2 * _JIDCT_FIX_3_072711026, tmp3 * _JIDCT_FIX_1_501321110
      z1, z2 = z1 * (-_JIDCT_FIX_0_899976223), z2 * (-_JIDCT_FIX_2_562915447)
      z3, z4 = z3 * (-_JIDCT_FIX_1_961570560), z4 * (-_JIDCT_FIX_0_390180644)
      z3 += z5
      z4 += z5
      tmp0 += z1 + z3
      tmp1 += z2 + z4
      tmp2 += z2 + z3
      tmp3 += z1 + z4
      blk.set(col, _jpeg_descale(tmp10 + tmp3, _JIDCT_CONST_BITS - _JIDCT_PASS1_BITS))
      blk.set(56 + col, _jpeg_descale(tmp10 - tmp3, _JIDCT_CONST_BITS - _JIDCT_PASS1_BITS))
      blk.set(8 + col, _jpeg_descale(tmp11 + tmp2, _JIDCT_CONST_BITS - _JIDCT_PASS1_BITS))
      blk.set(48 + col, _jpeg_descale(tmp11 - tmp2, _JIDCT_CONST_BITS - _JIDCT_PASS1_BITS))
      blk.set(16 + col, _jpeg_descale(tmp12 + tmp1, _JIDCT_CONST_BITS - _JIDCT_PASS1_BITS))
      blk.set(40 + col, _jpeg_descale(tmp12 - tmp1, _JIDCT_CONST_BITS - _JIDCT_PASS1_BITS))
      blk.set(24 + col, _jpeg_descale(tmp13 + tmp0, _JIDCT_CONST_BITS - _JIDCT_PASS1_BITS))
      blk.set(32 + col, _jpeg_descale(tmp13 - tmp0, _JIDCT_CONST_BITS - _JIDCT_PASS1_BITS))
      col += 1
   }
   0
}

fn _idct_emit_rows(list blk, any out, int out_len, int off, int step_x, int step_y, int plane_w) any {
   mut y = 0
   while(y < 8){
      def row_off = y * 8
      mut r1, r2 = blk.get(row_off + 1), blk.get(row_off + 2)
      mut r3, r4 = blk.get(row_off + 3), blk.get(row_off + 4)
      mut r5, r6 = blk.get(row_off + 5), blk.get(row_off + 6)
      mut r7 = blk.get(row_off + 7)
      if(r1 == 0 && r2 == 0 && r3 == 0 && r4 == 0 && r5 == 0 && r6 == 0 && r7 == 0){
         def fv = _c_u8(_jpeg_descale(blk.get(row_off), _JIDCT_PASS1_BITS + 3) + 128)
         mut x = 0
         while(x < 8){
            _idct_store_sample(out, out_len, plane_w, off, step_x, step_y, x, y, fv)
            x += 1
         }
         y += 1
         continue
      }
      mut z2, z3 = r2, r6
      mut z1 = (z2 + z3) * _JIDCT_FIX_0_541196100
      mut tmp2 = z1 + z3 * (-_JIDCT_FIX_1_847759065)
      mut tmp3 = z1 + z2 * _JIDCT_FIX_0_765366865
      mut tmp0 = (blk.get(row_off) + r4) << _JIDCT_CONST_BITS
      mut tmp1 = (blk.get(row_off) - r4) << _JIDCT_CONST_BITS
      mut tmp10 = tmp0 + tmp3
      mut tmp13 = tmp0 - tmp3
      mut tmp11 = tmp1 + tmp2
      mut tmp12 = tmp1 - tmp2
      tmp0, tmp1 = r7, r5
      tmp2, tmp3 = r3, r1
      z1, z2 = tmp0 + tmp3, tmp1 + tmp2
      z3 = tmp0 + tmp2
      mut z4, z5 = tmp1 + tmp3, (z3 + z4) * _JIDCT_FIX_1_175875602
      tmp0, tmp1 = tmp0 * _JIDCT_FIX_0_298631336, tmp1 * _JIDCT_FIX_2_053119869
      tmp2, tmp3 = tmp2 * _JIDCT_FIX_3_072711026, tmp3 * _JIDCT_FIX_1_501321110
      z1, z2 = z1 * (-_JIDCT_FIX_0_899976223), z2 * (-_JIDCT_FIX_2_562915447)
      z3, z4 = z3 * (-_JIDCT_FIX_1_961570560), z4 * (-_JIDCT_FIX_0_390180644)
      z3 += z5
      z4 += z5
      tmp0 += z1 + z3
      tmp1 += z2 + z4
      tmp2 += z2 + z3
      tmp3 += z1 + z4
      def fv0 = _c_u8(_jpeg_descale(tmp10 + tmp3, _JIDCT_CONST_BITS + _JIDCT_PASS1_BITS + 3) + 128)
      def fv1 = _c_u8(_jpeg_descale(tmp11 + tmp2, _JIDCT_CONST_BITS + _JIDCT_PASS1_BITS + 3) + 128)
      def fv2 = _c_u8(_jpeg_descale(tmp12 + tmp1, _JIDCT_CONST_BITS + _JIDCT_PASS1_BITS + 3) + 128)
      def fv3 = _c_u8(_jpeg_descale(tmp13 + tmp0, _JIDCT_CONST_BITS + _JIDCT_PASS1_BITS + 3) + 128)
      def fv4 = _c_u8(_jpeg_descale(tmp13 - tmp0, _JIDCT_CONST_BITS + _JIDCT_PASS1_BITS + 3) + 128)
      def fv5 = _c_u8(_jpeg_descale(tmp12 - tmp1, _JIDCT_CONST_BITS + _JIDCT_PASS1_BITS + 3) + 128)
      def fv6 = _c_u8(_jpeg_descale(tmp11 - tmp2, _JIDCT_CONST_BITS + _JIDCT_PASS1_BITS + 3) + 128)
      def fv7 = _c_u8(_jpeg_descale(tmp10 - tmp3, _JIDCT_CONST_BITS + _JIDCT_PASS1_BITS + 3) + 128)
      mut x = 0
      while(x < 8){
         def fv = case x {
            1 -> fv1
            2 -> fv2
            3 -> fv3
            4 -> fv4
            5 -> fv5
            6 -> fv6
            7 -> fv7
            _ -> fv0
         }
         _idct_store_sample(out, out_len, plane_w, off, step_x, step_y, x, y, fv)
         x += 1
      }
      y += 1
   }
   0
}

fn _idct(list cf, list qt, any out, int out_len, int off, int step_x, int step_y, int plane_w, list blk, list natural) any {
   if(!_idct_has_ac(cf)){
      _idct_store_dc(cf, qt, out, out_len, off, step_x, step_y, plane_w)
      return 0
   }
   _idct_prepare_block(cf, qt, blk, natural)
   _idct_pass_columns(blk, natural)
   _idct_emit_rows(blk, out, out_len, off, step_x, step_y, plane_w)
   0
}

fn _d_du(list bs, any dh, any ah, any qt, int pdc, any plane, int plane_len, int off, int step_x, int step_y, int pw, list cf, list blk, list natural) any {
   if(_jpeg_diag_on()){ _jpeg_diag_total_blocks += 1 }
   def sz = _h_dec(dh, bs)
   if(sz < 0){
      if(_jpeg_diag_on()){ _jpeg_diag_dc_fail += 1 }
      bs[4] = pdc
      return 0
   }
   def bits = _bs_gbs(bs, sz)
   def d_val = _d_cf(sz, bits)
   def ndc = pdc + d_val
   bs[4] = ndc
   cf[0] = ndc
   mut iC = 1
   while(iC < 64){
      cf[iC] = 0
      iC += 1
   }
   mut jI = 1
   while(jI < 64){
      def sym = _h_dec(ah, bs)
      if(sym < 0){
         if(_jpeg_diag_on()){ _jpeg_diag_ac_fail += 1 }
         break
      }
      def run = sym >> 4
      def asz = sym & 15
      if(asz == 0){
         if(run == 15){ jI += 16 } else { break }
      } else {
         jI += run
         if(jI < 64){
            def bV = _bs_gbs(bs, asz)
            cf[jI] = _d_cf(asz, bV)
            jI += 1
         }
      }
   }
   _idct(cf, qt, plane, plane_len, off, step_x, step_y, pw, blk, natural)
   0
}

fn _jpeg_parse_headers(any data) any {
   mut wVal, hVal = 0, 0
   mut ncVal = 0
   mut ssVal = 0
   mut ptr = 2
   mut qts = pbin.zero_list(16)
   mut hdc = pbin.zero_list(16)
   mut hac = pbin.zero_list(16)
   mut cidMap = pbin.zero_list(256)
   mut cord = list(0)
   mut scan = list(0)
   mut scan_list = list(0)
   mut is_prog = false
   mut mh = 1
   mut mv = 1
   mut restart_interval = 0
   while(ptr < (data.len - 1)){
      if(load8(data, ptr) != 255){
         ptr += 1
         continue
      }
      def mark = (load8(data, ptr) << 8) | load8(data, (ptr + 1))
      ptr += 2
      if(mark == 0xFFD9){ break }
      def sl = (load8(data, ptr) << 8) | load8(data, (ptr + 1))
      if(sl < 2 || (ptr + sl) > data.len){ return 0 }
      def soff = ptr + 2
      if(mark == 0xFFC0 || mark == 0xFFC2){
         if(mark == 0xFFC2){ is_prog = true }
         hVal, wVal = (load8(data, (soff + 1)) << 8) | load8(data, (soff + 2)), (load8(data, (soff + 3)) << 8) | load8(data, (soff + 4))
         if(wVal > 16384 || hVal > 16384 || wVal == 0 || hVal == 0){ return 0 }
         ncVal = load8(data, (soff + 5))
         mut kC = 0
         while(kC < ncVal){
            def idV = load8(data, (soff + 6 + kC * 3))
            def sV = load8(data, (soff + 7 + kC * 3))
            def qV = load8(data, (soff + 8 + kC * 3))
            def hsV = sV >> 4
            def vsV = sV & 15
            if(hsV > mh){ mh = hsV }
            if(vsV > mv){ mv = vsV }
            mut dobj = list(5)
            dobj = dobj.append(qV)
            dobj = dobj.append(hsV)
            dobj = dobj.append(vsV)
            dobj = dobj.append(0)
            dobj = dobj.append(0)
            cidMap[idV] = dobj
            cord = cord.append(idV)
            kC += 1
         }
      } elif(mark == 0xFFDB){
         mut qoff = soff
         while(qoff < (soff + sl - 2)){
            def qid = load8(data, qoff) & 15
            qoff += 1
            mut qtl = list(64)
            mut kq = 0
            while(kq < 64){
               qtl = qtl.append(load8(data, (qoff + kq)))
               kq += 1
            }
            qoff += 64
            qts[qid] = qtl
         }
      } elif(mark == 0xFFC4){
         mut hoff = soff
         while(hoff < (soff + sl - 2)){
            def tiV, tcV = load8(data, hoff), tiV >> 4
            def tidV = tiV & 15
            def h_res = _h_parse(data, (hoff + 1))
            hoff += (1 + h_res.get(1))
            if(tcV == 0){ hdc[tidV] = h_res.get(0) } else { hac[tidV] = h_res.get(0) }
         }
      } elif(mark == 0xFFDD){
         if(sl >= 4){ restart_interval = (load8(data, soff) << 8) | load8(data, (soff + 1)) }
      } elif(mark == 0xFFDA){
         scan = list(4)
         mut scan_meta = list(0)
         def countC = load8(data, soff)
         mut sidx = 0
         while(sidx < countC){
            def sid = load8(data, (soff + 1 + sidx * 2))
            def tiv = load8(data, (soff + 2 + sidx * 2))
            def dobj = cidMap.get(sid)
            if(is_list(dobj)){
               dobj[3] = (tiv >> 4)
               dobj[4] = (tiv & 15)
            }
            scan = scan.append(sid)
            mut sm = list(0)
            sm = sm.append(sid)
            sm = sm.append(tiv >> 4)
            sm = sm.append(tiv & 15)
            sm = sm.append(hdc.get(tiv >> 4, 0))
            sm = sm.append(hac.get(tiv & 15, 0))
            scan_meta = scan_meta.append(sm)
            sidx += 1
         }
         ssVal = (soff + sl - 2)
         if(is_prog){
            def scan_ss, scan_se = load8(data, (soff + 1 + countC * 2)), load8(data, (soff + 2 + countC * 2))
            def scan_ahal = load8(data, (soff + 3 + countC * 2))
            mut rec = list(0)
            rec = rec.append(ssVal)
            rec = rec.append(_jpeg_find_next_marker(data, ssVal))
            rec = rec.append(scan)
            rec = rec.append(scan_meta)
            rec = rec.append(scan_ss)
            rec = rec.append(scan_se)
            rec = rec.append(scan_ahal >> 4)
            rec = rec.append(scan_ahal & 15)
            scan_list = scan_list.append(rec)
            ptr = _jpeg_find_next_marker(data, ssVal)
            continue
         }
         break
      }
      ptr = (soff + sl - 2)
   }
   [wVal, hVal, ncVal, ssVal, qts, hdc, hac, cidMap, cord, scan, scan_list, is_prog, mh, mv, restart_interval]
}

fn decode(any data) any {
   "Decodes a baseline JPEG byte string into an image dictionary."
   if(!is_str(data) || data.len < 4){ return 0 }
   if(load8(data, 0) != 255 || load8(data, 1) != 216){ return 0 }
   if(!_jpeg_disable_turbo()){
      def turbo = _jpeg_decode_turbo(data)
      if(turbo){ return turbo }
   }
   def diag_on = _jpeg_diag_on()
   if(diag_on){
      _jpeg_diag_total_blocks = 0
      _jpeg_diag_dc_fail = 0
      _jpeg_diag_ac_fail = 0
   }
   def parsed = _jpeg_parse_headers(data)
   if(!parsed){ return 0 }
   def wVal, hVal = int(parsed.get(0, 0)), int(parsed.get(1, 0))
   def ssVal = int(parsed.get(3, 0))
   def qts, hdc = parsed.get(4), parsed.get(5)
   def hac, cidMap = parsed.get(6), parsed.get(7)
   def cord, scan = parsed.get(8), parsed.get(9)
   def scan_list = parsed.get(10)
   def is_prog = parsed.get(11)
   def mh, mv = int(parsed.get(12, 1)), int(parsed.get(13, 1))
   def restart_interval = int(parsed.get(14, 0))
   if(is_prog){ return _jpeg_decode_progressive(data, wVal, hVal, mh, mv, qts, hdc, hac, cidMap, cord, scan_list) }
   if(wVal == 0 || hVal == 0 || ssVal == 0){ return 0 }
   def tpx = wVal * hVal
   def mcu_w, mcu_h = mh * 8, mv * 8
   def padded_w, padded_h = ((wVal + mcu_w - 1) / mcu_w) * mcu_w, ((hVal + mcu_h - 1) / mcu_h) * mcu_h
   def tpx_pad = padded_w * padded_h
   mut plane_sz = tpx
   if(tpx_pad > tpx){ plane_sz = tpx_pad }
   def planes = _jpeg_alloc_planes(plane_sz)
   if(!planes){ return 0 }
   def cyB, ccbB = planes.get(0), planes.get(1)
   def ccrB = planes.get(2)
   def bs = _bs_m(data, ssVal)
   mut pdc1, pdc2 = 0, 0
   mut pdc3 = 0
   mut mcu_count = 0
   mut restart_seq = 0
   mut Yid = 1
   if(cord.len > 0){ Yid = cord.get(0) }
   mut Cbid = 2
   if(cord.len > 1){ Cbid = cord.get(1) }
   mut decode_cf = list(64)
   mut decode_blk = list(64)
   mut decode_natural = list(64)
   mut i64 = 0
   while(i64 < 64){
      decode_cf = decode_cf.append(0)
      decode_blk = decode_blk.append(0)
      decode_natural = decode_natural.append(0)
      i64 += 1
   }
   mut mcy = 0
   while((mcy * 8 * mv) < hVal){
      mut mcx = 0
      while((mcx * 8 * mh) < wVal){
         if(restart_interval > 0 && mcu_count > 0 && (mcu_count % restart_interval) == 0){
            if(!_bs_skip_restart(bs, restart_seq)){ return 0 }
            restart_seq = (restart_seq + 1) & 7
            pdc1 = 0
            pdc2 = 0
            pdc3 = 0
         }
         def scan_n = scan.len
         mut sidx_l = 0
         while(sidx_l < scan_n){
            def cid = scan.get(sidx_l)
            def dobj = cidMap.get(cid)
            if(!is_list(dobj)){
               sidx_l += 1
               continue
            }
            def hsV, vsV = dobj.get(1), dobj.get(2)
            def qtV = qts.get(dobj.get(0))
            def dhV = hdc.get(dobj.get(3))
            def ahV = hac.get(dobj.get(4))
            mut vb = 0
            while(vb < vsV){
               mut hb = 0
               while(hb < hsV){
                  mut pdcV = pdc3
                  mut planeL = ccrB
                  if(cid == Yid){
                     pdcV = pdc1
                     planeL = cyB
                  } elif(cid == Cbid){
                     pdcV = pdc2
                     planeL = ccbB
                  }
                  def step_x, step_y = max(1, mh / max(1, hsV)), max(1, mv / max(1, vsV))
                  _d_du(bs, dhV, ahV, qtV, pdcV, planeL, plane_sz, ((mcy * 8 * mv + vb * 8) * wVal + (mcx * 8 * mh + hb * 8)), step_x, step_y, wVal, decode_cf, decode_blk, decode_natural)
                  def ndcV = bs.get(4)
                  if(cid == Yid){ pdc1 = ndcV } elif(cid == Cbid){
                     pdc2 = ndcV
                  } else {
                     pdc3 = ndcV
                  }
                  hb += 1
               }
               vb += 1
            }
            sidx_l += 1
         }
         mcu_count += 1
         mcx += 1
      }
      mcy += 1
   }
   def pix = _jpeg_planes_to_rgba(cyB, ccbB, ccrB, wVal, hVal)
   def rd = _jpeg_image_result(pix, wVal, hVal)
   free(cyB, ccbB, ccrB)
   if(diag_on){
      print("[jpeg] blocks=" + to_str(_jpeg_diag_total_blocks) +
         " dc_fail=" + to_str(_jpeg_diag_dc_fail) +
         " ac_fail=" + to_str(_jpeg_diag_ac_fail) +
      " size=" + to_str(wVal) + "x" + to_str(hVal))
   }
   return rd
}

fn _mgb(any v) int {
   mut xV = (v < 0) ? -v : v
   mut nC = 0
   while(xV > 0){
      xV = xV >> 1
      nC += 1
   }
   return nC
}

fn _apb(int v, int n) int {
   if(n == 0){ return 0 }
   if(v < 0){ return v + (1 << n) - 1 }
   return v
}

fn _ebn() list {
   mut eb = list(3)
   eb = eb.append(0)
   eb = eb.append(0)
   eb = eb.append(list(1024))
   return eb
}

fn _eeb(list eb, int b) any {
   def bV = b & 255
   mut bL = eb.get(2)
   bL = bL.append(bV)
   if(bV == 255){ bL = bL.append(0) }
   eb[2] = bL
   0
}

fn _epb(list eb, int b, int n) any {
   if(n == 0){ return 0 }
   mut acc = eb.get(0)
   mut num = eb.get(1)
   mut iC = n
   while(iC > 0){
      iC -= 1
      acc = (acc << 1) | ((b >> iC) & 1)
      num += 1
      if(num == 8){
         _eeb(eb, acc)
         acc = 0
         num = 0
      }
   }
   eb[0] = acc
   eb[1] = num
   0
}

fn _ebf(list eb) any {
   if(eb.get(1) > 0){
      def acc = eb.get(0)
      def num = eb.get(1)
      def pushV = (acc << (8 - num)) | ((1 << (8 - num)) - 1)
      _eeb(eb, pushV)
      eb[0] = 0
      eb[1] = 0
   }
   0
}

fn _fdct(list blk, list qn) list {
   mut nat = pbin.zero_list(64)
   mut vL = 0
   while(vL < 8){
      mut uL = 0
      while(uL < 8){
         mut sA = 0.0
         mut yC = 0
         while(yC < 8){
            mut xC = 0
            while(xC < 8){
               def pV = blk.get((yC * 8 + xC)) * 1.0
               def a1 = ((2.0 * xC + 1.0) * uL * 3.141592653589793) / 16.0
               def a2 = ((2.0 * yC + 1.0) * vL * 3.141592653589793) / 16.0
               def term = pV * cos(a1) * cos(a2)
               sA = sA + term
               xC += 1
            }
            yC += 1
         }
         def cu, cv = (uL == 0) ? 0.707106 : 1.0, (vL == 0) ? 0.707106 : 1.0
         def qv = qn.get((vL * 8 + uL)) * 1.0
         def qv_safe = (qv == 0.0) ? 1.0 : qv
         def resF = (0.25 * cu * cv * sA) / qv_safe
         def ri = resF >= 0.0 ? (resF + 0.5) : (resF - 0.5)
         nat[vL * 8 + uL] = ri
         uL += 1
      }
      vL += 1
   }
   mut zz = pbin.zero_list(64)
   mut kI = 0
   while(kI < 64){
      zz[kI] = nat.get(_ZZ.get(kI))
      kI += 1
   }
   return zz
}

fn _ebk(list eb, list zz, int pdc, list dm, list am) int {
   def dc = zz.get(0)
   def dff = dc - pdc
   def nb = _mgb(dff)
   _epb(eb, dm.get(0).get(nb), dm.get(1).get(nb))
   if(nb > 0){ _epb(eb, _apb(dff, nb), nb) }
   mut run = 0
   mut iI = 1
   while(iI < 64){
      def cV = zz.get(iI)
      if(cV == 0){ run += 1 } else {
         while(run >= 16){
            _epb(eb, am.get(0).get(0xF0), am.get(1).get(0xF0))
            run -= 16
         }
         def an = _mgb(cV)
         def sym = (run << 4) | an
         _epb(eb, am.get(0).get(sym), am.get(1).get(sym))
         _epb(eb, _apb(cV, an), an)
         run = 0
      }
      iI += 1
   }
   if(run > 0){ _epb(eb, am.get(0).get(0), am.get(1).get(0)) }
   return dc
}

fn _jpeg_append_bytes(list out, list bytes) list {
   def n = bytes.len
   mut i = 0
   while(i < n){
      out = out.append(bytes.get(i))
      i += 1
   }
   out
}

fn _jpeg_scaled_quant_tables(int qual) list {
   mut q1Val = qual
   if(q1Val < 1){ q1Val = 1 }
   if(q1Val > 100){ q1Val = 100 }
   def sc = (q1Val < 50) ? (5000 / q1Val) : (200 - q1Val * 2)
   mut qyn, qcn = list(64), list(64)
   mut iI = 0
   while(iI < 64){
      mut v1, v2 = (_QY.get(iI) * sc + 50) / 100, (_QC.get(iI) * sc + 50) / 100
      if(v1 < 1){ v1 = 1 }
      if(v1 > 255){ v1 = 255 }
      if(v2 < 1){ v2 = 1 }
      if(v2 > 255){ v2 = 255 }
      qyn, qcn = qyn.append(v1), qcn.append(v2)
      iI += 1
   }
   mut qyz, qcz = list(64), list(64)
   mut jI = 0
   while(jI < 64){
      qyz, qcz = qyz.append(qyn.get(_ZZ.get(jI))), qcz.append(qcn.get(_ZZ.get(jI)))
      jI += 1
   }
   [qyn, qcn, qyz, qcz]
}

fn _jpeg_append_quant_table(list out, int table_id, list qz) list {
   out = _jpeg_append_bytes(out, [255, 219, 0, 67, table_id])
   mut kQ = 0
   while(kQ < 64){
      out = out.append(qz.get(kQ))
      kQ += 1
   }
   out
}

fn _jpeg_append_huffman_table(list out, int table_class_id, list counts, list vals, int val_count) list {
   def seg_len = 3 + 16 + val_count
   out = _jpeg_append_bytes(out, [255, 196, (seg_len >> 8) & 255, seg_len & 255, table_class_id])
   mut kD = 0
   while(kD < 16){
      out = out.append(counts.get(kD))
      kD += 1
   }
   kD = 0
   while(kD < val_count){
      out = out.append(vals.get(kD))
      kD += 1
   }
   out
}

fn _jpeg_append_sof0(list out, int wVal, int hVal) list {
   _jpeg_append_bytes(out, [
         255, 192, 0, 17, 8,
         (hVal >> 8), (hVal & 255), (wVal >> 8), (wVal & 255),
         3,
         1, 17, 0,
         2, 17, 1,
         3, 17, 1
   ])
}

fn _jpeg_append_scan_header(list out) list {
   _jpeg_append_bytes(out, [255, 218, 0, 12, 3, 1, 0, 2, 17, 3, 17, 0, 63, 0])
}

fn _jpeg_append_baseline_headers(list out, int wVal, int hVal, list qyz, list qcz) list {
   out = _jpeg_append_bytes(out, [
         255, 216,
         255, 224, 0, 16, 74, 70, 73, 70, 0, 1, 1, 1, 0, 72, 0, 72, 0, 0
   ])
   out = _jpeg_append_quant_table(out, 0, qyz)
   out = _jpeg_append_quant_table(out, 1, qcz)
   out = _jpeg_append_sof0(out, wVal, hVal)
   out = _jpeg_append_huffman_table(out, 0, _DCL, _VDC, 12)
   out = _jpeg_append_huffman_table(out, 16, _ACL, _VAL, 162)
   out = _jpeg_append_huffman_table(out, 1, _DCC, _VDC, 12)
   out = _jpeg_append_huffman_table(out, 17, _ACC, _VAC, 162)
   _jpeg_append_scan_header(out)
}

fn _jpeg_sample_ycc_blocks(any dataV, int wVal, int hVal, int chV, int bx, int by) list {
   mut bYData = list(64)
   mut bCbData = list(64)
   mut bCrData = list(64)
   mut yI = 0
   while(yI < 8){
      mut xI = 0
      while(xI < 8){
         def px, py = ((bx + xI) < wVal) ? (bx + xI) : (wVal - 1), ((by + yI) < hVal) ? (by + yI) : (hVal - 1)
         def po = (py * wVal + px) * chV
         def rR = load8(dataV, po)
         mut rV, gV = rR, rR
         mut bV = rR
         if(chV >= 3){ gV, bV = load8(dataV, (po + 1)), load8(dataV, (po + 2)) }
         if(chV == 4){
            def aV_raw = load8(dataV, (po + 3))
            def ai, bi = aV_raw, 255 - ai
            rV, gV = (rV * ai + 255 * bi) / 255, (gV * ai + 255 * bi) / 255
            bV = (bV * ai + 255 * bi) / 255
         }
         def yV = (rV * 306 + gV * 601 + bV * 117) / 1024 - 128
         def cbV = (rV * -173 + gV * -339 + bV * 512) / 1024
         def crV = (rV * 512 + gV * -429 + bV * -83) / 1024
         bYData = bYData.append(yV)
         bCbData = bCbData.append(cbV)
         bCrData = bCrData.append(crV)
         xI += 1
      }
      yI += 1
   }
   [bYData, bCbData, bCrData]
}

fn _jpeg_append_entropy_payload(list out, list eb) list {
   _ebf(eb)
   def fb = eb.get(2)
   def fb_n = fb.len
   mut kI = 0
   while(kI < fb_n){
      out = out.append(fb.get(kI))
      kI += 1
   }
   out = out.append(255)
   out.append(217)
}

fn _jpeg_bytes_from_list(list out) any {
   def resL = out.len
   def resP = init_str(malloc(resL + 32), resL)
   mut rI = 0
   while(rI < resL){
      store8(resP, out.get(rI), rI)
      rI += 1
   }
   resP
}

fn encode(any img, int qual=90) any {
   "Encodes an image dictionary into a baseline JPEG byte string."
   def wVal, hVal = img.get("width"), img.get("height")
   def dataV = img.get("data")
   def chV = img.get("channels", 4)
   def qtabs = _jpeg_scaled_quant_tables(qual)
   def qyn, qcn = qtabs.get(0), qtabs.get(1)
   def qyz, qcz = qtabs.get(2), qtabs.get(3)
   def dmY, dmC = _m_hm(_DCL, _VDC), _m_hm(_DCC, _VDC)
   def amY, amC = _m_hm(_ACL, _VAL), _m_hm(_ACC, _VAC)
   mut out = list(4096)
   out = _jpeg_append_baseline_headers(out, wVal, hVal, qyz, qcz)
   def eb = _ebn()
   mut d1, d2 = 0, 0
   mut d3 = 0
   mut by = 0
   while(by < hVal){
      mut bx = 0
      while(bx < wVal){
         def blocks = _jpeg_sample_ycc_blocks(dataV, wVal, hVal, chV, bx, by)
         d1, d2 = _ebk(eb, _fdct(blocks.get(0), qyn), d1, dmY, amY), _ebk(eb, _fdct(blocks.get(1), qcn), d2, dmC, amC)
         d3 = _ebk(eb, _fdct(blocks.get(2), qcn), d3, dmC, amC)
         bx += 8
      }
      by += 8
   }
   out = _jpeg_append_entropy_payload(out, eb)
   _jpeg_bytes_from_list(out)
}
