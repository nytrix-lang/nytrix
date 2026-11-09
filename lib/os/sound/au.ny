;; Keywords: sound au snd decoder os
;; Pure Nytrix decoder for Sun/NeXT AU audio data.
module core.os.sound.au(decode_bytes)
use core
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

fn decode_bytes(any d) any {
   "Decodes AU bytes to a memory sound source; unsupported encodings return 0."
   ;; Encodings 23, 24, 25, and 26 (G.721, G.722, and G.723/G.726 ADPCM)
   ;; are not implemented here. The existing PCM, float, G.711, and A-law
   ;; paths below are kept pure Nytrix and malformed/unsupported input returns 0.
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
