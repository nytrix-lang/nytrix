;; Keywords: sound au snd decoder os
;; Pure Nytrix decoder for Sun/NeXT AU audio data.
module core.os.sound.au(decode_bytes)
use core
use core.tbuf
use core.os.sound.source

fn _be32(any d, int at) int {
   (load8(d, at) << 24) | (load8(d, at + 1) << 16) |
   (load8(d, at + 2) << 8) | load8(d, at + 3)
}

fn _alaw(int a) int {
   def v = a ^ 0x55
   def seg = (v & 0x70) >> 4
   mut t = (v & 15) << 4
   if seg == 0 { t += 8 } else { t = (t + 0x108) << (seg - 1) }
   (v & 0x80) != 0 ? t : -t
}

fn _mulaw(int u) int {
   def v = (~u) & 255
   def t = (((v & 15) << 3) + 0x84) << ((v & 0x70) >> 4)
   (v & 0x80) != 0 ? 0x84 - t : t - 0x84
}

fn _clip(int v, int lo, int hi) int {
   if v < lo { return lo }
   if v > hi { return hi }
   v
}

fn _sign(int v) int { v < 0 ? -1 : 1 }

fn _abs(int v) int {
   if v < 0 { return 0 - v }
   v
}

fn _s16(int v) int { ((v + 32768) & 65535) - 32768 }

fn _get(any s, int i) int { i64buf_load(s, i) }

fn _put(any s, int i, int v) { i64buf_store(s, i, v) }

;; Packed Float11: sign/exponent/mantissa. All arithmetic stays in Ny integers.
fn _float11(int v) int {
   mut int magnitude = v
   if v < 0 { magnitude = 0 - v }
   mut exponent = 0
   mut n = magnitude
   while n > 0 { exponent += 1 n = n >> 1 }
   def mantissa = magnitude == 0 ? 32 : ((magnitude * 64) >> exponent)
   ((v < 0 ? 1 : 0) << 10) | (exponent << 6) | mantissa
}

fn _multiply(int a, int b) int {
   def exponent = ((a >> 6) & 15) + ((b >> 6) & 15)
   mut v = (((a & 63) * (b & 63)) + 48) >> 4
   if exponent > 19 { v = v << (exponent - 19) }
   else { v = v >> (19 - exponent) }
   if ((a ^ b) & 1024) != 0 { v = 0 - v }
   _s16(v)
}

def _iq3 = [-32768,135,273,373,373,273,135,-32768]
def _w3 = [-4,30,137,582,582,137,30,-4]
def _f3 = [0,1,2,7,7,2,1,0]
def _iq4 = [-32768,4,135,213,273,323,373,425,425,373,323,273,213,135,4,-32768]
def _w4 = [-12,18,41,64,112,198,355,1122,1122,355,198,112,64,41,18,-12]
def _f4 = [0,0,0,1,1,1,3,7,7,3,1,1,1,0,0,0]
def _iq5 = [-32768,-66,28,104,169,224,274,318,358,395,429,459,488,514,539,566,
   566,539,514,488,459,429,395,358,318,274,224,169,104,28,-66,-32768]
def _w5 = [14,14,24,39,40,41,58,100,141,179,219,280,358,440,529,696,
   696,529,440,358,280,219,179,141,100,58,41,40,39,24,14,14]
def _f5 = [0,0,0,0,0,1,1,1,1,1,2,3,4,5,6,6,6,6,5,4,3,2,1,1,1,1,1,0,0,0,0,0]

;; Slots: sr[2],dq[6],a[2],b[6],pk[2],ap,yu,yl,dms,dml,td,se,sez,y.
fn _g726(any s, int code, int bits) int {
   def iq = bits == 3 ? _iq3 : (bits == 4 ? _iq4 : _iq5)
   def wt = bits == 3 ? _w3 : (bits == 4 ? _w4 : _w5)
   def ft = bits == 3 ? _f3 : (bits == 4 ? _f4 : _f5)
   def int y = _get(s, 26)
   def int logarithm = int(iq[code]) + (y >> 2)
   mut int dq = 0
   if logarithm >= 0 { dq = ((128 + (logarithm & 127)) << ((logarithm >> 7) & 15)) >> 7 }
   def int yl = _get(s, 20)
   def yl_int = yl >> 15
   def threshold = yl_int > 9 ? 31744 : ((32 + ((yl >> 10) & 31)) << yl_int)
   def transition = _get(s, 23) == 1 && dq > ((3 * threshold) >> 2)
   def negative = code >> (bits - 1)
   if negative != 0 { dq = 0 - dq }
   def int reconstructed = _s16(_get(s, 24) + dq)
   def sum = _get(s, 25) + dq
   def pk = sum == 0 ? 0 : _sign(sum)
   def dqsign = dq == 0 ? 0 : _sign(dq)
   if transition {
      mut i = 8
      while i < 16 { _put(s, i, 0) i += 1 }
   } else {
      mut int a0 = _get(s, 8)
      mut int a1 = _get(s, 9)
      def correction = _clip((((0 - a0) * _get(s, 16) * pk) >> 5), -256, 255)
      a1 = _clip(a1 + 128 * pk * _get(s, 17) + correction - (a1 >> 7), -12288, 12288)
      a0 = _clip(a0 + 192 * pk * _get(s, 16) - (a0 >> 8), a1 - 15360, 15360 - a1)
      _put(s, 8, a0)
      _put(s, 9, a1)
      mut i = 0
      while i < 6 {
         def old = _get(s, 10 + i)
         def oldsign = (_get(s, 2 + i) & 1024) != 0 ? -1 : 1
         _put(s, 10 + i, old + 128 * dqsign * oldsign - (old >> 8))
         i += 1
      }
   }
   _put(s, 17, _get(s, 16))
   _put(s, 16, pk == 0 ? 1 : pk)
   _put(s, 1, _get(s, 0))
   _put(s, 0, _float11(reconstructed))
   mut i = 5
   while i > 0 { _put(s, 2 + i, _get(s, 1 + i)) i -= 1 }
   _put(s, 2, (_float11(dq) & 1023) | (negative << 10))
   def tone = _get(s, 9) < -11776
   _put(s, 23, tone ? 1 : 0)
   def dms = _get(s, 21) + (int(ft[code]) << 4) + ((0 - _get(s, 21)) >> 5)
   def dml = _get(s, 22) + (int(ft[code]) << 4) + ((0 - _get(s, 22)) >> 7)
   _put(s, 21, dms)
   _put(s, 22, dml)
   mut int ap = _get(s, 18)
   if transition { ap = 256 }
   else {
      ap += (0 - ap) >> 4
      if y <= 1535 || tone || _abs((dms << 2) - dml) >= (dml >> 3) { ap += 32 }
   }
   _put(s, 18, ap)
   def yu = _clip(y + int(wt[code]) + ((0 - y) >> 5), 544, 5120)
   def slow = yl + yu + ((0 - yl) >> 6)
   _put(s, 19, yu)
   _put(s, 20, slow)
   def mix = ap >= 256 ? 64 : (ap >> 2)
   _put(s, 26, (slow + (yu - (slow >> 6)) * mix) >> 6)
   mut int estimate = 0
   i = 0
   while i < 6 {
      estimate += _multiply(_float11(_get(s, 10 + i) >> 2), _get(s, 2 + i))
      i += 1
   }
   _put(s, 25, estimate >> 1)
   i = 0
   while i < 2 {
      estimate += _multiply(_float11(_get(s, 8 + i) >> 2), _get(s, i))
      i += 1
   }
   _put(s, 24, estimate >> 1)
   _s16(_clip(reconstructed * 4, -65535, 65535))
}

def _logscale = [2048,2093,2139,2186,2233,2282,2332,2383,
   2435,2489,2543,2599,2656,2714,2774,2834,2896,2960,3025,3091,
   3158,3228,3298,3371,3444,3520,3597,3676,3756,3838,3922,4008]
def _lowstep = [-60,3042,1198,538,334,172,58,-30,3042,1198,538,334,172,58,-30,-60]
def _low4 = [0,-2557,-1612,-1121,-786,-530,-323,-150,2557,1612,1121,786,530,323,150,0]
def _low6 = [-17,-17,-17,-17,-3101,-2738,-2376,-2088,
   -1873,-1689,-1535,-1399,-1279,-1170,-1072,-982,
   -899,-822,-750,-682,-618,-558,-501,-447,-396,-347,-300,-254,-211,-170,-130,-91,
   3101,2738,2376,2088,1873,1689,1535,1399,1279,1170,1072,982,
   899,822,750,682,618,558,501,447,396,347,300,254,211,170,130,91,54,17,-54,-17]
def _high2 = [-926,-202,926,202]
def _qmf = [3,-11,12,32,-210,951,3876,-805,362,-156,53,-11]

;; G.722 band: predictor, previous reconstruction, poles[2], zero predictor,
;; partial signs[2], logarithmic/linear scale, zero coefficients[6], deltas[6].
fn _predict722(any s, int base, int difference) {
   def partial = _get(s, base + 4) + difference < 0 ? 1 : 0
   def sg0 = partial != _get(s, base + 5) ? 1 : -1
   def sg1 = partial == _get(s, base + 6) ? 1 : -1
   _put(s, base + 6, _get(s, base + 5))
   _put(s, base + 5, partial)
   def pole0 = _get(s, base + 2)
   def pole1 = _clip(((sg0 * _clip(pole0, -8191, 8191)) >> 5) +
      sg1 * 128 + ((_get(s, base + 3) * 127) >> 7), -12288, 12288)
   def limit = 15360 - pole1
   def next0 = _clip(-192 * sg0 + ((pole0 * 255) >> 8), 0 - limit, limit)
   _put(s, base + 2, next0)
   _put(s, base + 3, pole1)
   mut zero = 0
   mut i = 5
   while i >= 0 {
      def olddiff = _get(s, base + 15 + i)
      def nextdiff = i == 0 ? difference * 2 : _get(s, base + 14 + i)
      def correction = difference == 0 ? 0 : ((olddiff ^ difference) < 0 ? -128 : 128)
      def coefficient = ((_get(s, base + 9 + i) * 255) >> 8) + correction
      _put(s, base + 9 + i, coefficient)
      _put(s, base + 15 + i, nextdiff)
      zero += (nextdiff * coefficient) >> 15
      i -= 1
   }
   _put(s, base + 4, zero)
   def reconstructed = _clip((_get(s, base) + difference) * 2, -32768, 32767)
   def predictor = zero + ((next0 * reconstructed) >> 15) +
   ((pole1 * _get(s, base + 1)) >> 15)
   _put(s, base, _clip(predictor, -32768, 32767))
   _put(s, base + 1, reconstructed)
}

fn _scale722(int logarithm) int {
   def v = int(_logscale[(logarithm >> 6) & 31])
   def shift = logarithm >> 11
   shift < 0 ? v >> (0 - shift) : v << shift
}

fn _g722(any s, int code, any out, int sample, int stride) {
   def lowcode = code & 63
   def highcode = code >> 6
   def low = _clip(((_get(s, 8) * int(_low6[lowcode])) >> 10) + _get(s, 0), -16384, 16383)
   def lowdiff = (_get(s, 8) * int(_low4[lowcode >> 2])) >> 10
   _predict722(s, 0, lowdiff)
   def lowlog = _clip(((_get(s, 7) * 127) >> 7) + int(_lowstep[lowcode >> 2]), 0, 18432)
   _put(s, 7, lowlog)
   _put(s, 8, _scale722(lowlog - 16384))
   def highdiff = (_get(s, 29) * int(_high2[highcode])) >> 10
   def high = _clip(highdiff + _get(s, 21), -16384, 16383)
   _predict722(s, 21, highdiff)
   def highlog = _clip(((_get(s, 28) * 127) >> 7) + ((highcode & 1) == 0 ? 798 : -214), 0, 22528)
   _put(s, 28, highlog)
   _put(s, 29, _scale722(highlog - 20480))
   mut i = 0
   while i < 22 { _put(s, 42 + i, _get(s, 44 + i)) i += 1 }
   _put(s, 64, low + high)
   _put(s, 65, low - high)
   mut even = 0
   mut odd = 0
   i = 0
   while i < 12 {
      odd += _get(s, 42 + 2 * i) * int(_qmf[i])
      even += _get(s, 43 + 2 * i) * int(_qmf[11 - i])
      i += 1
   }
   store16(out, _clip(even >> 11, -32768, 32767), sample * 2)
   store16(out, _clip(odd >> 11, -32768, 32767), (sample + stride) * 2)
}

fn _decode722(any d, int at, int size, int channels, int rate) any {
   if size <= 0 || size % channels != 0 { return 0 }
   def out = malloc(size * 4)
   if !out { return 0 }
   mut states = []
   mut ch = 0
   while ch < channels {
      def s = i64buf_new(66)
      mut i = 0
      while i < 66 { _put(s, i, 0) i += 1 }
      _put(s, 8, 8)
      _put(s, 29, 2)
      states = states.append(s)
      ch += 1
   }
   mut block = 0
   while block < size / channels {
      ch = 0
      while ch < channels {
         _g722(states[ch], load8(d, at + block * channels + ch), out,
            block * 2 * channels + ch, channels)
         ch += 1
      }
      block += 1
   }
   make_memory_source(out, size * 4, channels, rate, 16, SAMPLE_FMT_S16, 1)
}

fn decode_bytes(any d) any {
   "Decodes AU bytes to a memory sound source; unsupported encodings return 0."
   if d.len < 24 || load8(d, 0) != 46 || load8(d, 1) != 115 ||
   load8(d, 2) != 110 || load8(d, 3) != 100 { return 0 }
   def off = _be32(d, 4)
   def declared = _be32(d, 8)
   def enc = _be32(d, 12)
   def rate = _be32(d, 16)
   def channels = _be32(d, 20)
   if off < 24 || off > d.len || rate <= 0 || channels <= 0 || channels > 32 { return 0 }
   mut size = declared
   if declared == 0xffffffff { size = d.len - off }
   if size < 0 || size > d.len - off { return 0 }
   def at = off
   if enc >= 23 && enc <= 26 {
      return decode(d, at, size, enc, channels, rate)
   }
   if enc == 1 || enc == 27 {
      def out = malloc(size * 2)
      mut i = 0
      while i < size {
         def b = load8(d, at + i)
         store16(out, enc == 1 ? _mulaw(b) : _alaw(b), i * 2)
         i += 1
      }
      return make_memory_source(out, size * 2, channels, rate, 16, SAMPLE_FMT_S16, 1)
   }
   if enc == 2 { ;; signed 8-bit linear PCM
      def out = malloc(size * 2)
      mut i = 0
      while i < size {
         def b = load8(d, at + i)
         store16(out, ((b >= 128 ? b - 256 : b) << 8) & 0xffff, i * 2)
         i += 1
      }
      return make_memory_source(out, size * 2, channels, rate, 16, SAMPLE_FMT_S16, 1)
   }
   if enc == 3 || enc == 4 || enc == 5 {
      def width = enc == 3 ? 2 : (enc == 4 ? 3 : 4)
      if size % width != 0 { return 0 }
      def out = malloc(size)
      mut i = 0
      while i < size {
         mut b = 0
         while b < width {
            store8(out, load8(d, at + i + width - 1 - b), i + b)
            b += 1
         }
         i += width
      }
      return make_memory_source(out, size, channels, rate, width * 8,
         width == 2 ? SAMPLE_FMT_S16 : (width == 3 ? SAMPLE_FMT_S24 : SAMPLE_FMT_S32), 1)
   }
   if enc == 6 {
      if size % 4 != 0 { return 0 }
      def out = malloc(size)
      mut i = 0
      while i < size {
         def v = _be32(d, at + i)
         store32(out, v, i)
         i += 4
      }
      return make_memory_source(out, size, channels, rate, 32, SAMPLE_FMT_F32, 3)
   }
   if enc == 7 {
      if size % 8 != 0 { return 0 }
      def wide = malloc(8)
      def count = size / 8
      def out = malloc(count * 4)
      mut i = 0
      while i < count {
         mut b = 0
         while b < 8 {
            store8(wide, load8(d, at + i * 8 + 7 - b), b)
            b += 1
         }
         store32_f32(out, load64_f64(wide, 0), i * 4)
         i += 1
      }
      free(wide)
      return make_memory_source(out, count * 4, channels, rate, 32, SAMPLE_FMT_F32, 3)
   }
   0
}

fn decode(any d, int at, int size, int encoding, int channels, int rate) any {
   if channels <= 0 || channels > 32 || at < 0 || size < 0 || at > d.len ||
   size > d.len - at || rate <= 0 { return 0 }
   if encoding == 24 { return _decode722(d, at, size, channels, rate) }
   if encoding != 23 && encoding != 25 && encoding != 26 { return 0 }
   def bits = encoding == 25 ? 3 : (encoding == 26 ? 5 : 4)
   def frames = (size * 8) / (bits * channels)
   if frames <= 0 { return 0 }
   def out = malloc(frames * channels * 2)
   if !out { return 0 }
   mut states = []
   mut ch = 0
   while ch < channels {
      def s = i64buf_new(27)
      mut i = 0
      while i < 27 { _put(s, i, 0) i += 1 }
      i = 0
      while i < 8 { _put(s, i, 32) i += 1 }
      _put(s, 16, 1)
      _put(s, 17, 1)
      _put(s, 19, 544)
      _put(s, 20, 34816)
      _put(s, 26, 544)
      states = states.append(s)
      ch += 1
   }
   mut frame = 0
   mut bit = 0
   while frame < frames {
      ch = 0
      while ch < channels {
         mut code = 0
         mut b = 0
         while b < bits {
            code = code | (((load8(d, at + (bit >> 3)) >> (bit & 7)) & 1) << b)
            bit += 1
            b += 1
         }
         store16(out, _g726(states[ch], code, bits), (frame * channels + ch) * 2)
         ch += 1
      }
      frame += 1
   }
   make_memory_source(out, frames * channels * 2, channels, rate, 16, SAMPLE_FMT_S16, 1)
}
