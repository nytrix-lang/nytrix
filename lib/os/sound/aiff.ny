;; Keywords: sound aiff aifc decoder os
;; Pure Nytrix decoder for AIFF and AIFC containers.
module core.os.sound.aiff(decode_bytes)
use core
use core.os.sound.source

fn _tag(any d, int at, str s) bool {
   mut i = 0
   while i < 4 { if load8(d, at + i) != load8(s, i) { return false } i += 1 }
   true
}

fn _be16(any d, int at) int { (load8(d, at) << 8) | load8(d, at + 1) }
fn _be32(any d, int at) int {
   (load8(d, at) << 24) | (load8(d, at + 1) << 16) |
   (load8(d, at + 2) << 8) | load8(d, at + 3)
}
fn _rate(any d, int at) int {
   def exp = _be16(d, at) & 0x7fff
   def mant_hi = _be32(d, at + 2)
   if exp == 0 || exp == 0x7fff { return 0 }
   def shift = exp - 16383 - 31
   if shift < -62 || shift > 30 { return 0 }
   shift >= 0 ? mant_hi << shift : mant_hi >> (0 - shift)
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
   if d.len < 12 || !_tag(d, 0, "FORM") || (!_tag(d, 8, "AIFF") && !_tag(d, 8, "AIFC")) { return 0 }
   def aifc = _tag(d, 8, "AIFC")
   mut channels = 0
   mut frames = 0
   mut bits = 0
   mut rate = 0
   mut comp = "NONE"
   mut sound_at = -1
   mut sound_size = 0
   mut pos = 12
   while pos + 8 <= d.len {
      def size = _be32(d, pos + 4)
      def body = pos + 8
      if size < 0 || size > d.len - body { break }
      if _tag(d, pos, "COMM") && size >= (aifc ? 22 : 18) {
         channels = _be16(d, body)
         frames = _be32(d, body + 2)
         bits = _be16(d, body + 6)
         rate = _rate(d, body + 8)
         if aifc {
            mut i = 0
            mut c = ""
            while i < 4 {
               c = c + chr(load8(d, body + 18 + i))
               i += 1
            }
            comp = c
         }
      } elif _tag(d, pos, "SSND") && size >= 8 {
         def offset = _be32(d, body)
         if offset <= size - 8 {
            sound_at = body + 8 + offset
            sound_size = size - 8 - offset
         }
      }
      pos = body + size + (size & 1)
   }
   if channels <= 0 || frames < 0 || rate <= 0 || sound_at < 0 { return 0 }
   def need = frames * channels
   if comp == "sowt" || comp == "NONE" || comp == "twos" {
      if bits != 8 && bits != 16 && bits != 24 && bits != 32 { return 0 }
      def width = (bits + 7) / 8
      if need * width > sound_size { return 0 }
      if bits == 8 {
         def out = malloc(need * 2)
         mut i = 0
         while i < need {
            def b = load8(d, sound_at + i)
            def s = b >= 128 ? b - 256 : b
            store16(out, (s << 8) & 0xffff, i * 2)
            i += 1
         }
         return make_memory_source(out, need * 2, channels, rate, 16, SAMPLE_FMT_S16, 1)
      }
      def out = malloc(need * width)
      mut i = 0
      while i < need {
         mut b = 0
         while b < width {
            def src_b = comp == "sowt" ? b : width - 1 - b
            store8(out, load8(d, sound_at + i * width + src_b), i * width + b)
            b += 1
         }
         i += 1
      }
      mut sf = SAMPLE_FMT_S32
      if bits == 16 { sf = SAMPLE_FMT_S16 }
      if bits == 24 { sf = SAMPLE_FMT_S24 }
      return make_memory_source(out, need * width, channels, rate, bits, sf, 1)
   }
   if comp == "fl32" || comp == "FL32" || comp == "fl64" || comp == "FL64" {
      def width = (comp == "fl32" || comp == "FL32") ? 4 : 8
      if need * width > sound_size { return 0 }
      def out = malloc(need * 4)
      mut i = 0
      while i < need {
         if width == 4 {
            store32(out, _be32(d, sound_at + i * 4), i * 4)
         } else {
            def wide = malloc(8)
            mut b = 0
            while b < 8 {
               store8(wide, load8(d, sound_at + i * 8 + 7 - b), b)
               b += 1
            }
            store32_f32(out, load64_f64(wide, 0), i * 4)
            free(wide)
         }
         i += 1
      }
      return make_memory_source(out, need * 4, channels, rate, 32, SAMPLE_FMT_F32, 3)
   }
   if comp == "alaw" || comp == "ulaw" {
      if need > sound_size { return 0 }
      def out = malloc(need * 2)
      mut i = 0
      while i < need {
         def v = load8(d, sound_at + i)
         mut a = v ^ 0x55
         def seg = (a & 0x70) >> 4
         mut sample = (a & 15) << 4
         if comp == "ulaw" {
            def u = (~v) & 255
            sample = (((u & 15) << 3) + 0x84) << ((u & 0x70) >> 4)
            if (u & 0x80) != 0 { sample = 0x84 - sample } else { sample = sample - 0x84 }
         } else {
            if seg == 0 { sample += 8 } else { sample = (sample + 0x108) << (seg - 1) }
            if (a & 0x80) == 0 { sample = -sample }
         }
         store16(out, sample, i * 2)
         i += 1
      }
      return make_memory_source(out, need * 2, channels, rate, 16, SAMPLE_FMT_S16, 1)
   }
   0
}
