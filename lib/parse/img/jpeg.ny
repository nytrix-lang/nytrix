;; Keywords: image jpeg rfc2035
;; JPEG Image Loader and Encoder for Nytrix
;; Reference:
;; - https://jpeg.org/jpeg/index.html
;; - https://en.wikipedia.org/wiki/JPEG
;; - https://www.w3.org/Graphics/JPEG/itu-t81.pdf
;; - https://www.youtube.com/playlist?list=PLzH6n4zXuckoAod3z31QEST1ZaizBuNHh

module std.image.format.jpeg (
   encode, decode
)

use std.core *
use std.core.dict_mod *
use std.math *
use std.math.float as math_flt
use std.parse.bin as pbin

def _QY = [16, 11, 10, 16, 24, 40, 51, 61, 12, 12, 14, 19, 26, 58, 60, 55, 14, 13, 16, 24, 40, 57, 69, 56, 14, 17, 22, 29, 51, 87, 80, 62, 18, 22, 37, 56, 68, 109, 103, 77, 24, 35, 55, 64, 81, 104, 113, 92, 49, 64, 78, 87, 103, 121, 120, 101, 72, 92, 95, 98, 112, 100, 103, 99]
def _QC = [17, 18, 24, 47, 99, 99, 99, 99, 18, 21, 26, 66, 99, 99, 99, 99, 24, 26, 56, 99, 99, 99, 99, 99, 47, 66, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99]
def _DCL = [0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0]
def _DCC = [0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0]
def _VDC = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11]
def _ACL = [0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 125]
def _VAL = [1, 2, 3, 0, 4, 17, 5, 18, 33, 49, 65, 6, 19, 81, 97, 7, 34, 113, 20, 50, 129, 145, 161, 8, 35, 66, 177, 193, 21, 82, 209, 240, 36, 51, 98, 114, 130, 9, 10, 22, 23, 24, 25, 26, 37, 38, 39, 40, 41, 42, 52, 53, 54, 55, 56, 57, 58, 67, 68, 69, 70, 71, 72, 73, 74, 83, 84, 85, 86, 87, 88, 89, 90, 99, 100, 101, 102, 103, 104, 105, 106, 115, 116, 117, 118, 119, 120, 121, 122, 131, 132, 133, 134, 135, 136, 137, 138, 146, 147, 148, 149, 150, 151, 152, 153, 154, 162, 163, 164, 165, 166, 167, 168, 169, 170, 178, 179, 180, 181, 182, 183, 184, 185, 186, 194, 195, 196, 197, 198, 199, 200, 201, 202, 210, 211, 212, 213, 214, 215, 216, 217, 218, 225, 226, 227, 228, 229, 230, 231, 232, 233, 234, 241, 242, 243, 244, 245, 246, 247, 248, 249, 250]
def _ACC = [0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 119]
def _VAC = [0, 1, 2, 3, 17, 4, 5, 33, 49, 6, 18, 65, 81, 7, 97, 113, 19, 34, 50, 129, 8, 20, 66, 145, 161, 177, 193, 9, 35, 51, 82, 240, 21, 98, 114, 209, 10, 22, 36, 52, 225, 37, 241, 23, 24, 25, 26, 38, 39, 40, 41, 42, 53, 54, 55, 56, 57, 58, 67, 68, 69, 70, 71, 72, 73, 74, 83, 84, 85, 86, 87, 88, 89, 90, 99, 100, 101, 102, 103, 104, 105, 106, 115, 116, 117, 118, 119, 120, 121, 122, 130, 131, 132, 133, 134, 135, 136, 137, 138, 146, 147, 148, 149, 150, 151, 152, 153, 154, 162, 163, 164, 165, 166, 167, 168, 169, 170, 178, 179, 180, 181, 182, 183, 184, 185, 186, 194, 195, 196, 197, 198, 199, 200, 201, 202, 210, 211, 212, 213, 214, 215, 216, 217, 218, 226, 227, 228, 229, 230, 231, 232, 233, 234, 242, 243, 244, 245, 246, 247, 248, 249, 250]

def _IDCT_T = [0.707107, 0.707107, 0.707107, 0.707107, 0.707107, 0.707107, 1.0, 1.0, 0.980785, 0.831470, 0.555570, 0.195090, -0.195090, -0.555570, -0.831470, -0.980785, 0.923880, 0.382683, -0.382683, -0.923880, -0.923880, -0.382683, 0.382683, 0.923880, 0.831470, -0.195090, -0.980785, -0.555570, 0.555570, 0.980785, 0.195090, -0.831470, 0.707107, -0.707107, -0.707107, 0.707107, 0.707107, -0.707107, -0.707107, 0.707107, 0.555570, -0.980785, 0.195090, 0.831470, -0.831470, -0.195090, 0.980785, -0.555570, 0.382683, -0.923880, 0.923880, -0.382683, -0.382683, 0.923880, -0.923880, 0.382683, 0.195090, -0.555570, 0.831470, -0.980785, 0.980785, -0.831470, 0.555570, -0.195090]
def _ZZ = [0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63]

fn _c_u8(v){
   "Internal helper for `c_u8` in JPEG processing."
   if(math_flt.flt(math_flt.float(v), math_flt.float(0.0))){
      return 0
   }
   if(math_flt.fgt(math_flt.float(v), math_flt.float(255.0))){
      return 255
   }
   return math_flt.int(math_flt.float(v))
}

fn _r_i(v){
   "Internal helper for `r_i` in JPEG processing."
   def fv = math_flt.float(v)
   def half = math_flt.float(0.5)
   if(math_flt.fgt(fv, math_flt.float(0.0))){
      return math_flt.int(math_flt.fadd(fv, half))
   }
   return math_flt.int(math_flt.fsub(fv, half))
}

fn _m_hm(b, val_list){
   "Internal helper for `m_hm` in JPEG processing."
   mut cm = pbin.zero_list(256)
   mut sm = pbin.zero_list(256)
   mut cCount = 0
   mut kCount = 0
   mut lIdx = 1
   while(lIdx <= 16){
      def ctVal = get(b, (lIdx - 1))
      mut j = 0
      while(j < ctVal){
         def sSym = get(val_list, kCount)
         store_item(cm, sSym, cCount)
         store_item(sm, sSym, lIdx)
         cCount += 1
         kCount += 1
         j += 1
      }
      cCount = cCount << 1
      lIdx += 1
   }
   return [cm, sm]
}

fn _h_new(ctx){
   "Internal helper for `h_new` in JPEG processing."
   mut nodeList = get(ctx, 0)
   def idx = len(nodeList)
   mut n = list(4)
   n = append(n, 0)
   n = append(n, 0)
   n = append(n, -1)
   n = append(n, -1)
   nodeList = append(nodeList, n)
   store_item(ctx, 0, nodeList)
   return idx
}

fn _h_add(ctx, root, code, len_bits, sym){
   "Internal helper for `h_add` in JPEG processing."
   mut cur = root
   mut iBit = len_bits
   while(iBit > 0){
      def nodeList = get(ctx, 0)
      def nObj = get(nodeList, cur)
      def bitVal = (code >> (iBit - 1)) & 1
      if(bitVal){
         mut rVal = get(nObj, 3)
         if(rVal == -1){
         rVal = _h_new(ctx)
         def nUpd = get(get(ctx, 0), cur)
         store_item(nUpd, 3, rVal)
         }
         cur = rVal
      } else {
         mut lVal = get(nObj, 2)
         if(lVal == -1){
         lVal = _h_new(ctx)
         def nUpd = get(get(ctx, 0), cur)
         store_item(nUpd, 2, lVal)
         }
         cur = lVal
      }
      iBit -= 1
   }
   def nodeListF = get(ctx, 0)
   def leaf = get(nodeListF, cur)
   store_item(leaf, 0, sym)
   store_item(leaf, 1, 1)
}

fn _h_parse(data, off){
   "Internal helper for `h_parse` in JPEG processing."
   mut ctx = list(1)
   ctx = append(ctx, list(512))
   _h_new(ctx)
   mut counts = list(16)
   mut iC = 0
   while(iC < 16){
      counts = append(counts, load8(data, (off + iC)))
      iC += 1
   }
   mut pPtr = off + 16
   mut cvVal = 0
   mut clIdx = 1
   while(clIdx <= 16){
      def cntVal = get(counts, (clIdx - 1))
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
   return [get(ctx, 0), (pPtr - off)]
}

fn _h_dec(nodes, bs){
   "Internal helper for `h_dec` in JPEG processing."
   mut cur = 0
   while(1){
      mut bl = get(bs, 3)
      if(bl == 0){
         def db = get(bs, 0)
         mut bp = get(bs, 1)
         if(bp >= len(db)){
         return -1
         }
         mut cV = load8(db, bp)
         bp += 1
         if(cV == 255){
         if(bp < len(db)){
               if(load8(db, bp) == 0){
                  bp += 1
               }
         }
         }
         store_item(bs, 2, cV)
         store_item(bs, 3, 8)
         store_item(bs, 1, bp)
         bl = 8
      }
      bl -= 1
      def bit = (get(bs, 2) >> bl) & 1
      store_item(bs, 3, bl)
      def n = get(nodes, cur)
      if(bit){
         cur = get(n, 3)
      } else {
         cur = get(n, 2)
      }
      if(cur == -1){
         return -1
      }
      def cn = get(nodes, cur)
      if(get(cn, 1)){
         return get(cn, 0)
      }
   }
}

fn _bs_m(data, start){
   "Internal helper for `bs_m` in JPEG processing."
   mut bs = list(5)
   bs = append(bs, data)
   bs = append(bs, start)
   bs = append(bs, 0)
   bs = append(bs, 0)
   bs = append(bs, 0)
   return bs
}

fn _bs_gbs(bs, n){
   "Internal helper for `bs_gbs` in JPEG processing."
   mut v = 0
   mut iCount = 0
   while(iCount < n){
      mut bl = get(bs, 3)
      if(bl == 0){
         def db = get(bs, 0)
         mut bp = get(bs, 1)
         if(bp < len(db)){
         mut cV = load8(db, bp)
         bp += 1
         if(cV == 255){
               if(bp < len(db)){
                  if(load8(db, bp) == 0){
                     bp += 1
                  }
               }
         }
         store_item(bs, 2, cV)
         store_item(bs, 3, 8)
         store_item(bs, 1, bp)
         bl = 8
         } else {
         break
         }
      }
      bl -= 1
      v = (v << 1) | ((get(bs, 2) >> bl) & 1)
      store_item(bs, 3, bl)
      iCount += 1
   }
   return v
}

fn _d_cf(sz, bits){
   "Internal helper for `d_cf` in JPEG processing."
   if(sz == 0){
      return 0
   }
   if((bits >> (sz - 1)) & 1){
      return bits
   }
   return bits - (1 << sz) + 1
}

fn _idct(cf, qt, out, off, step, plane_w){
   "Internal helper for `idct` in JPEG processing."
   mut cValues = list(64)
   mut kIdx = 0
   while(kIdx < 64){
      cValues = append(cValues, math_flt.float(0.0))
      kIdx += 1
   }
   kIdx = 0
   while(kIdx < 64){
      def val = math_flt.fmul(math_flt.float(get(cf, kIdx)), math_flt.float(get(qt, kIdx)))
      store_item(cValues, get(_ZZ, kIdx), val)
      kIdx += 1
   }
   mut tmpValues = list(64)
   mut k2 = 0
   while(k2 < 64){
      tmpValues = append(tmpValues, math_flt.float(0.0))
      k2 += 1
   }
   mut vIdx = 0
   while(vIdx < 8){
      mut xIdx = 0
      while(xIdx < 8){
         mut sV = math_flt.float(0.0)
         mut uI = 0
         while(uI < 8){
         def prod = math_flt.fmul(get(cValues, (vIdx * 8 + uI)), math_flt.float(get(_IDCT_T, (uI * 8 + xIdx))))
         sV = math_flt.fadd(sV, prod)
         uI += 1
         }
         store_item(tmpValues, (vIdx * 8 + xIdx), sV)
         xIdx += 1
      }
      vIdx += 1
   }
   mut yL = 0
   while(yL < 8){
      mut xL = 0
      while(xL < 8){
         mut sumV = math_flt.float(0.0)
         mut jI = 0
         while(jI < 8){
         def prod2 = math_flt.fmul(math_flt.float(get(_IDCT_T, (jI * 8 + yL))), get(tmpValues, (jI * 8 + xL)))
         sumV = math_flt.fadd(sumV, prod2)
         jI += 1
         }
         def resVal = math_flt.fadd(math_flt.fdiv(sumV, math_flt.float(4.0)), math_flt.float(128.0))
         def fv = _c_u8(resVal)
         mut dy = 0
         while(dy < step){
         mut dx = 0
         while(dx < step){
               def py = (off / plane_w) + (yL * step) + dy
               def px = (off % plane_w) + (xL * step) + dx
               if((py * plane_w + px) < len(out)){
                  store8(out, fv, (py * plane_w + px))
               }
               dx += 1
         }
         dy += 1
         }
         xL += 1
      }
      yL += 1
   }
}

fn _d_du(bs, dh, ah, qt, pdc, plane, off, step, pw){
   "Internal helper for `d_du` in JPEG processing."
   def sz = _h_dec(dh, bs)
   def bits = _bs_gbs(bs, sz)
   def d_val = _d_cf(sz, bits)
   def ndc = pdc + d_val
   store_item(bs, 4, ndc)
   mut cf = list(64)
   cf = append(cf, ndc)
   mut iC = 1
   while(iC < 64){
      cf = append(cf, 0)
      iC += 1
   }
   mut jI = 1
   while(jI < 64){
      def sym = _h_dec(ah, bs)
      def run = sym >> 4
      def asz = sym & 15
      if(asz == 0){
         if(run == 15){
         jI += 16
         } else {
         break
         }
      } else {
         jI += run
         if(jI < 64){
         def bV = _bs_gbs(bs, asz)
         store_item(cf, jI, _d_cf(asz, bV))
         jI += 1
         }
      }
   }
   _idct(cf, qt, plane, off, step, pw)
}

fn decode(data){
   "Decodes a baseline JPEG byte string into an image dictionary."
   if(!is_str(data) || len(data) < 4){
      return 0
   }
   if(load8(data, 0) != 255 || load8(data, 1) != 216){
      return 0
   }
   mut wVal = 0
   mut hVal = 0
   mut ncVal = 0
   mut ssVal = 0
   mut ptr = 2
   mut qts = pbin.zero_list(4)
   mut hdc = pbin.zero_list(4)
   mut hac = pbin.zero_list(4)
   mut cidMap = pbin.zero_list(256)
   mut cord = list(4)
   mut scan = list(4)
   mut mh = 1
   mut mv = 1
   while(ptr < (len(data) - 1)){
      if(load8(data, ptr) != 255){
         ptr += 1
         continue
      }
      def mark = (load8(data, ptr) << 8) | load8(data, (ptr + 1))
      ptr += 2
      if(mark == 0xFFD9){
         break
      }
      def sl = (load8(data, ptr) << 8) | load8(data, (ptr + 1))
      def soff = ptr + 2
      if(mark == 0xFFC0){
         hVal = (load8(data, (soff + 1)) << 8) | load8(data, (soff + 2))
         wVal = (load8(data, (soff + 3)) << 8) | load8(data, (soff + 4))
         ncVal = load8(data, (soff + 5))
         mut kC = 0
         while(kC < ncVal){
         def idV = load8(data, (soff + 6 + kC * 3))
         def sV = load8(data, (soff + 7 + kC * 3))
         def qV = load8(data, (soff + 8 + kC * 3))
         def hsV = sV >> 4
         def vsV = sV & 15
         if(hsV > mh){
               mh = hsV
         }
         if(vsV > mv){
               mv = vsV
         }
         mut dobj = list(5)
         dobj = append(dobj, qV)
         dobj = append(dobj, hsV)
         dobj = append(dobj, vsV)
         dobj = append(dobj, 0)
         dobj = append(dobj, 0)
         store_item(cidMap, idV, dobj)
         cord = append(cord, idV)
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
               qtl = append(qtl, load8(data, (qoff + kq)))
               kq += 1
         }
         qoff += 64
         store_item(qts, qid, qtl)
         }
      } elif(mark == 0xFFC4){
         mut hoff = soff
         while(hoff < (soff + sl - 2)){
         def tiV = load8(data, hoff)
         def tcV = tiV >> 4
         def tidV = tiV & 15
         def h_res = _h_parse(data, (hoff + 1))
         hoff += (1 + get(h_res, 1))
         if(tcV == 0){
               store_item(hdc, tidV, get(h_res, 0))
         } else {
               store_item(hac, tidV, get(h_res, 0))
         }
         }
      } elif(mark == 0xFFDA){
         scan = list(4)
         def countC = load8(data, soff)
         mut sidx = 0
         while(sidx < countC){
         def sid = load8(data, (soff + 1 + sidx * 2))
         def tiv = load8(data, (soff + 2 + sidx * 2))
         def dobj = get(cidMap, sid)
         if(is_list(dobj)){
               store_item(dobj, 3, (tiv >> 4))
               store_item(dobj, 4, (tiv & 15))
         }
         scan = append(scan, sid)
         sidx += 1
         }
         ssVal = (soff + sl - 2)
         break
      }
      ptr = (soff + sl - 2)
   }
   if(wVal == 0 || hVal == 0 || ssVal == 0){
      return 0
   }
   def tpx = wVal * hVal
   def cyB = init_str(malloc(tpx + 32) + 16, tpx)
   def ccbB = init_str(malloc(tpx + 32) + 16, tpx)
   def ccrB = init_str(malloc(tpx + 32) + 16, tpx)
   memset(cyB, 128, tpx)
   memset(ccbB, 128, tpx)
   memset(ccrB, 128, tpx)
   def bs = _bs_m(data, ssVal)
   mut pdc1 = 0
   mut pdc2 = 0
   mut pdc3 = 0
   def Yid = (len(cord) > 0) ? get(cord, 0) : 1
   def Cbid = (len(cord) > 1) ? get(cord, 1) : 2
   mut mcy = 0
   while((mcy * 8 * mv) < hVal){
      mut mcx = 0
      while((mcx * 8 * mh) < wVal){
         mut sidx_l = 0
         while(sidx_l < len(scan)){
         def cid = get(scan, sidx_l)
         def dobj = get(cidMap, cid)
         if(!is_list(dobj)){
               sidx_l += 1
               continue
         }
         def hsV = get(dobj, 1)
         def vsV = get(dobj, 2)
         def qtV = get(qts, get(dobj, 0))
         def dhV = get(hdc, get(dobj, 3))
         def ahV = get(hac, get(dobj, 4))
         mut vb = 0
         while(vb < vsV){
               mut hb = 0
               while(hb < hsV){
                  def pdcV = (cid == Yid) ? pdc1 : (cid == Cbid) ? pdc2 : pdc3
                  def planeL = (cid == Yid) ? cyB : (cid == Cbid) ? ccbB : ccrB
                  _d_du(bs, dhV, ahV, qtV, pdcV, planeL, ((mcy * 8 * mv + vb * 8) * wVal + (mcx * 8 * mh + hb * 8)), (mh / hsV), wVal)
                  def ndcV = get(bs, 4)
                  if(cid == Yid){
                     pdc1 = ndcV
                  } elif(cid == Cbid){
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
         mcx += 1
      }
      mcy += 1
   }
   def pix = init_str(malloc(tpx * 4 + 32) + 16, (tpx * 4))
   mut kL = 0
   while(kL < tpx){
      def Yf = math_flt.float(load8(cyB, kL))
      def Cbf = math_flt.float(load8(ccbB, kL))
      def Crf = math_flt.float(load8(ccrB, kL))
      def Cr128 = math_flt.fsub(Crf, math_flt.float(128.0))
      def Cb128 = math_flt.fsub(Cbf, math_flt.float(128.0))
      def rP = math_flt.fadd(Yf, math_flt.fmul(math_flt.float(1.402), Cr128))
      def gP = math_flt.fsub(math_flt.fsub(Yf, math_flt.fmul(math_flt.float(0.344136), Cb128)), math_flt.fmul(math_flt.float(0.714136), Cr128))
      def bP = math_flt.fadd(Yf, math_flt.fmul(math_flt.float(1.772), Cb128))
      store8(pix, _c_u8(rP), (kL * 4))
      store8(pix, _c_u8(gP), (kL * 4 + 1))
      store8(pix, _c_u8(bP), (kL * 4 + 2))
      store8(pix, 255, (kL * 4 + 3))
      kL += 1
   }
   mut rd = dict(4)
   dict_set(rd, "data", pix)
   dict_set(rd, "width", wVal)
   dict_set(rd, "height", hVal)
   dict_set(rd, "channels", 4)
   return rd
}

fn _mgb(v){
   "Internal helper for `mgb` in JPEG processing."
   mut xV = (v < 0) ? -v : v
   mut nC = 0
   while(xV > 0){
      xV = xV >> 1
      nC += 1
   }
   return nC
}

fn _apb(v, n){
   "Internal helper for `apb` in JPEG processing."
   if(n == 0){
      return 0
   }
   if(v < 0){
      return v + (1 << n) - 1
   }
   return v
}

fn _ebn(){
   "Internal helper for `ebn` in JPEG processing."
   mut eb = list(3)
   eb = append(eb, 0)
   eb = append(eb, 0)
   eb = append(eb, list(1024))
   return eb
}

fn _eeb(eb, b){
   "Internal helper for `eeb` in JPEG processing."
   def bV = b & 255
   mut bL = get(eb, 2)
   bL = append(bL, bV)
   if(bV == 255){
      bL = append(bL, 0)
   }
   store_item(eb, 2, bL)
}

fn _epb(eb, b, n){
   "Internal helper for `epb` in JPEG processing."
   if(n == 0){
      return 0
   }
   mut acc = get(eb, 0)
   mut num = get(eb, 1)
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
   store_item(eb, 0, acc)
   store_item(eb, 1, num)
}

fn _ebf(eb){
   "Internal helper for `ebf` in JPEG processing."
   if(get(eb, 1) > 0){
      def acc = get(eb, 0)
      def num = get(eb, 1)
      def pushV = (acc << (8 - num)) | ((1 << (8 - num)) - 1)
      _eeb(eb, pushV)
      store_item(eb, 0, 0)
      store_item(eb, 1, 0)
   }
}

fn _fdct(blk, qn){
   "Internal helper for `fdct` in JPEG processing."
   mut nat = pbin.zero_list(64)
   mut vL = 0
   while(vL < 8){
      mut uL = 0
      while(uL < 8){
         mut sA = math_flt.float(0.0)
         mut yC = 0
         while(yC < 8){
         mut xC = 0
         while(xC < 8){
               def a1 = math_flt.fdiv(math_flt.fmul(math_flt.fadd(math_flt.fmul(math_flt.float(2.0), math_flt.float(xC)), math_flt.float(1.0)), math_flt.fmul(math_flt.float(uL), math_flt.float(3.141592653589793))), math_flt.float(16.0))
               def a2 = math_flt.fdiv(math_flt.fmul(math_flt.fadd(math_flt.fmul(math_flt.float(2.0), math_flt.float(yC)), math_flt.float(1.0)), math_flt.fmul(math_flt.float(vL), math_flt.float(3.141592653589793))), math_flt.float(16.0))
               def pV = get(blk, (yC * 8 + xC))
               def term = math_flt.fmul(math_flt.float(pV), math_flt.fmul(cos(a1), cos(a2)))
               sA = math_flt.fadd(sA, term)
               xC += 1
         }
         yC += 1
         }
         def cu = (uL == 0) ? 0.707106 : 1.0
         def cv = (vL == 0) ? 0.707106 : 1.0
         def resF = math_flt.fdiv(math_flt.fmul(math_flt.fmul(math_flt.fmul(math_flt.float(0.25), math_flt.float(cu)), math_flt.float(cv)), sA), math_flt.float(get(qn, (vL * 8 + uL))))
         store_item(nat, (vL * 8 + uL), _r_i(resF))
         uL += 1
      }
      vL += 1
   }
   mut zz = pbin.zero_list(64)
   mut kI = 0
   while(kI < 64){
      store_item(zz, kI, get(nat, get(_ZZ, kI)))
      kI += 1
   }
   return zz
}

fn _ebk(eb, zz, pdc, dm, am){
   "Internal helper for `ebk` in JPEG processing."
   def dc = get(zz, 0)
   def dff = dc - pdc
   def nb = _mgb(dff)
   _epb(eb, get(get(dm, 0), nb), get(get(dm, 1), nb))
   if(nb > 0){
      _epb(eb, _apb(dff, nb), nb)
   }
   mut run = 0
   mut iI = 1
   while(iI < 64){
      def cV = get(zz, iI)
      if(cV == 0){
         run += 1
      } else {
         while(run >= 16){
         _epb(eb, get(get(am, 0), 0xF0), get(get(am, 1), 0xF0))
         run -= 16
         }
         def an = _mgb(cV)
         def sym = (run << 4) | an
         _epb(eb, get(get(am, 0), sym), get(get(am, 1), sym))
         _epb(eb, _apb(cV, an), an)
         run = 0
      }
      iI += 1
   }
   if(run > 0){
      _epb(eb, get(get(am, 0), 0), get(get(am, 1), 0))
   }
   return dc
}

fn encode(img, qual=90){
   "Encodes an image dictionary into a baseline JPEG byte string."
   def wVal = dict_get(img, "width")
   def hVal = dict_get(img, "height")
   def dataV = dict_get(img, "data")
   def chV = dict_get(img, "channels", 4)
   mut q1Val = qual
   if(q1Val < 1){
      q1Val = 1
   }
   if(q1Val > 100){
      q1Val = 100
   }
   def sc = (q1Val < 50) ? (5000 / q1Val) : (200 - q1Val * 2)
   mut qyn = list(64)
   mut qcn = list(64)
   mut iI = 0
   while(iI < 64){
      mut v1 = (get(_QY, iI) * sc + 50) / 100
      mut v2 = (get(_QC, iI) * sc + 50) / 100
      if(v1 < 1){
         v1 = 1
      }
      if(v1 > 255){
         v1 = 255
      }
      if(v2 < 1){
         v2 = 1
      }
      if(v2 > 255){
         v2 = 255
      }
      qyn = append(qyn, v1)
      qcn = append(qcn, v2)
      iI += 1
   }
   mut qyz = list(64)
   mut qcz = list(64)
   mut jI = 0
   while(jI < 64){
      qyz = append(qyz, get(qyn, get(_ZZ, jI)))
      qcz = append(qcz, get(qcn, get(_ZZ, jI)))
      jI += 1
   }
   def dmY = _m_hm(_DCL, _VDC)
   def dmC = _m_hm(_DCC, _VDC)
   def amY = _m_hm(_ACL, _VAL)
   def amC = _m_hm(_ACC, _VAC)
   mut out = list(4096)
   out = append(out, 255)
   out = append(out, 216)
   out = append(out, 255)
   out = append(out, 224)
   out = append(out, 0)
   out = append(out, 16)
   out = append(out, 74)
   out = append(out, 70)
   out = append(out, 73)
   out = append(out, 70)
   out = append(out, 0)
   out = append(out, 1)
   out = append(out, 1)
   out = append(out, 1)
   out = append(out, 0)
   out = append(out, 72)
   out = append(out, 0)
   out = append(out, 72)
   out = append(out, 0)
   out = append(out, 0)
   out = append(out, 255)
   out = append(out, 219)
   out = append(out, 0)
   out = append(out, 67)
   out = append(out, 0)
   mut kQ = 0
   while(kQ < 64){
      out = append(out, get(qyz, kQ))
      kQ += 1
   }
   out = append(out, 255)
   out = append(out, 219)
   out = append(out, 0)
   out = append(out, 67)
   out = append(out, 1)
   kQ = 0
   while(kQ < 64){
      out = append(out, get(qcz, kQ))
      kQ += 1
   }
   out = append(out, 255)
   out = append(out, 192)
   out = append(out, 0)
   out = append(out, 17)
   out = append(out, 8)
   out = append(out, (hVal >> 8))
   out = append(out, (hVal & 255))
   out = append(out, (wVal >> 8))
   out = append(out, (wVal & 255))
   out = append(out, 3)
   out = append(out, 1)
   out = append(out, 17)
   out = append(out, 0)
   out = append(out, 2)
   out = append(out, 17)
   out = append(out, 1)
   out = append(out, 3)
   out = append(out, 17)
   out = append(out, 1)
   out = append(out, 255)
   out = append(out, 196)
   out = append(out, 0)
   out = append(out, 31)
   out = append(out, 0)
   mut kD = 0
   while(kD < 16){
      out = append(out, get(_DCL, kD))
      kD += 1
   }
   kD = 0
   while(kD < 12){
      out = append(out, get(_VDC, kD))
      kD += 1
   }
   out = append(out, 255)
   out = append(out, 196)
   out = append(out, 0)
   out = append(out, 181)
   out = append(out, 16)
   kD = 0
   while(kD < 16){
      out = append(out, get(_ACL, kD))
      kD += 1
   }
   kD = 0
   while(kD < 162){
      out = append(out, get(_VAL, kD))
      kD += 1
   }
   out = append(out, 255)
   out = append(out, 196)
   out = append(out, 0)
   out = append(out, 31)
   out = append(out, 1)
   kD = 0
   while(kD < 16){
      out = append(out, get(_DCC, kD))
      kD += 1
   }
   kD = 0
   while(kD < 12){
      out = append(out, get(_VDC, kD))
      kD += 1
   }
   out = append(out, 255)
   out = append(out, 196)
   out = append(out, 0)
   out = append(out, 181)
   out = append(out, 17)
   kD = 0
   while(kD < 16){
      out = append(out, get(_ACC, kD))
      kD += 1
   }
   kD = 0
   while(kD < 162){
      out = append(out, get(_VAC, kD))
      kD += 1
   }
   out = append(out, 255)
   out = append(out, 218)
   out = append(out, 0)
   out = append(out, 12)
   out = append(out, 3)
   out = append(out, 1)
   out = append(out, 0)
   out = append(out, 2)
   out = append(out, 17)
   out = append(out, 3)
   out = append(out, 17)
   out = append(out, 0)
   out = append(out, 63)
   out = append(out, 0)
   def eb = _ebn()
   mut d1 = 0
   mut d2 = 0
   mut d3 = 0
   mut by = 0
   while(by < hVal){
      mut bx = 0
      while(bx < wVal){
         mut bYData = list(64)
         mut bCbData = list(64)
         mut bCrData = list(64)
         mut yI = 0
         while(yI < 8){
         mut xI = 0
         while(xI < 8){
               def px = ((bx + xI) < wVal) ? (bx + xI) : (wVal - 1)
               def py = ((by + yI) < hVal) ? (by + yI) : (hVal - 1)
               def po = (py * wVal + px) * chV
               def rR = load8(dataV, po)
               mut rF = math_flt.float(rR)
               mut gF = rF
               mut bF = rF
               if(chV >= 3){
                  gF = math_flt.float(load8(dataV, (po + 1)))
                  bF = math_flt.float(load8(dataV, (po + 2)))
               }
               if(chV == 4){
                  def aV_raw = load8(dataV, (po + 3))
                  def aF = math_flt.fdiv(math_flt.float(aV_raw), math_flt.float(255.0))
                  def bg = math_flt.fmul(math_flt.float(255.0), math_flt.fsub(math_flt.float(1.0), aF))
                  rF = math_flt.fadd(math_flt.fmul(rF, aF), bg)
                  gF = math_flt.fadd(math_flt.fmul(gF, aF), bg)
                  bF = math_flt.fadd(math_flt.fmul(bF, aF), bg)
               }
               def yV = math_flt.fsub(math_flt.fadd(math_flt.fadd(math_flt.fmul(math_flt.float(0.299), rF), math_flt.fmul(math_flt.float(0.587), gF)), math_flt.fmul(math_flt.float(0.114), bF)), math_flt.float(128.0))
               def cbV = math_flt.fadd(math_flt.fmul(math_flt.float(-0.1687), rF), math_flt.fadd(math_flt.fmul(math_flt.float(-0.3313), gF), math_flt.fmul(math_flt.float(0.5), bF)))
               def crV = math_flt.fadd(math_flt.fmul(math_flt.float(0.5), rF), math_flt.fadd(math_flt.fmul(math_flt.float(-0.4187), gF), math_flt.fmul(math_flt.float(-0.0813), bF)))
               bYData = append(bYData, yV)
               bCbData = append(bCbData, cbV)
               bCrData = append(bCrData, crV)
               xI += 1
         }
         yI += 1
         }
         d1 = _ebk(eb, _fdct(bYData, qyn), d1, dmY, amY)
         d2 = _ebk(eb, _fdct(bCbData, qcn), d2, dmC, amC)
         d3 = _ebk(eb, _fdct(bCrData, qcn), d3, dmC, amC)
         bx += 8
      }
      by += 8
   }
   _ebf(eb)
   def fb = get(eb, 2)
   mut kI = 0
   while(kI < len(fb)){
      out = append(out, get(fb, kI))
      kI += 1
   }
   out = append(out, 255)
   out = append(out, 217)
   def resL = len(out)
   def resP = init_str(malloc(resL + 32) + 16, resL)
   mut rI = 0
   while(rI < resL){
      store8(resP, get(out, rI), rI)
      rI += 1
   }
   return resP
}
