;; Keywords: sound wav riff decoder os
;; Native WAV decoding: RIFF, RIFX, RF64, and Wave64 containers holding PCM,
;; IEEE float, A-law, mu-law, MS ADPCM, or IMA ADPCM audio.
;; References:
;; - core.os.sound.res
;; - core.os.sound.source
module core.os.sound.wav(decode_bytes, encode_bytes)
use core
use core.os.sound.source
use core.tbuf

def _TAG_PCM = 1
def _TAG_ADPCM = 2
def _TAG_FLOAT = 3
def _TAG_ALAW = 6
def _TAG_MULAW = 7
def _TAG_IMA = 0x11
def _TAG_EXTENSIBLE = 0xfffe
def _FMT_S16 = 1
def _FMT_U8 = 2
def _FMT_S24 = 3
def _FMT_S32 = 4
def _FMT_F32 = 5

fn _put_text(any out, int at, str s) int {
   mut i = 0
   while i < s.len { store8(out, load8(s, i), at + i) i += 1 }
   0
}

fn encode_bytes(any src, bool rf64=false) bytes {
   "Serializes a memory sound source as RIFF or RF64 PCM/32-bit float WAV."
   if !is_list(src) || src.get(0) != "SOUND_SOURCE" { return bytes(0) }
   def meta = src.get(1, 0)
   if !is_dict(meta) { return bytes(0) }
   def ptr = meta.get("ptr", 0)
   def data_len = int(meta.get("len", 0))
   def channels = int(meta.get("channels", 0))
   def rate = int(meta.get("rate", 0))
   def bits = int(meta.get("bits", 0))
   def tag = int(meta.get("format_tag", 1))
   def width = (bits + 7) / 8
   if !ptr || data_len <= 0 || channels <= 0 || channels > 65535 || rate <= 0 ||
   (tag != 1 && !(tag == 3 && bits == 32)) ||
   (tag == 1 && bits != 8 && bits != 16 && bits != 24 && bits != 32) ||
   data_len % (channels * width) != 0 { return bytes(0) }
   if !rf64 && data_len > 4294967259 { return bytes(0) }
   def base = rf64 ? 80 : 44
   def out = bytes(base + data_len)
   if rf64 {
      _put_text(out, 0, "RF64")
      store32(out, 0xffffffff, 4)
      _put_text(out, 8, "WAVE")
      _put_text(out, 12, "ds64")
      store32(out, 28, 16)
      store64(out, base + data_len - 8, 20)
      store64(out, data_len, 28)
      store64(out, data_len / (channels * width), 36)
      store32(out, 0, 44)
      _put_text(out, 48, "fmt ")
      store32(out, 16, 52)
      store16(out, tag, 56)
      store16(out, channels, 58)
      store32(out, rate, 60)
      store32(out, rate * channels * width, 64)
      store16(out, channels * width, 68)
      store16(out, bits, 70)
      _put_text(out, 72, "data")
      store32(out, 0xffffffff, 76)
   } else {
      _put_text(out, 0, "RIFF")
      store32(out, base + data_len - 8, 4)
      _put_text(out, 8, "WAVE")
      _put_text(out, 12, "fmt ")
      store32(out, 16, 16)
      store16(out, tag, 20)
      store16(out, channels, 22)
      store32(out, rate, 24)
      store32(out, rate * channels * width, 28)
      store16(out, channels * width, 32)
      store16(out, bits, 34)
      _put_text(out, 36, "data")
      store32(out, data_len, 40)
   }
   memcpy(out + base, ptr, data_len)
   out
}

def _MS_ADAPT = [230, 230, 230, 230, 307, 409, 512, 614,
   768, 614, 512, 409, 307, 230, 230, 230]
def _MS_COEF1 = [256, 512, 0, 192, 240, 460, 392]
def _MS_COEF2 = [0, -256, 0, 64, 0, -208, -232]
def _IMA_INDEX = [-1, -1, -1, -1, 2, 4, 6, 8,
   -1, -1, -1, -1, 2, 4, 6, 8]
def _IMA_STEP = [
   7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
   50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
   253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
   1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
   3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442,
   11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
   32767
]

fn _table(list values) ptr {
   def out = i64buf_new(values.len)
   mut i = 0
   while i < values.len {
      i64buf_store(out, i, int(values[i]))
      i += 1
   }
   out
}

def _ms_adapt = _table(_MS_ADAPT)
def _ima_index = _table(_IMA_INDEX)
def _ima_step = _table(_IMA_STEP)

fn _u16(any d, int at, bool big) int {
   big ? (load8(d, at) << 8) | load8(d, at + 1) : load8(d, at) | (load8(d, at + 1) << 8)
}

fn _u32(any d, int at, bool big) int {
   big ? (_u16(d, at, true) << 16) | _u16(d, at + 2, true) : _u16(d, at, false) | (_u16(d, at + 2, false) << 16)
}

fn _u64(any d, int at, bool big) int {
   big ? (_u32(d, at, true) << 32) | _u32(d, at + 4, true) : _u32(d, at, false) | (_u32(d, at + 4, false) << 32)
}

fn _s16(int v) int { v >= 32768 ? v - 65536 : v }

fn _clamp16(int v) int { v < -32768 ? -32768 : (v > 32767 ? 32767 : v) }

fn _tag_at(any d, int at, str text) bool {
   mut i = 0
   while i < 4 {
      if load8(d, at + i) != load8(text, i) { return false }
      i += 1
   }
   true
}

;; Container layout: chunk header size, size field width, and alignment.
fn _container(any d) dict {
   if d.len < 12 { return dict() }
   if _tag_at(d, 0, "riff") && d.len >= 40 && load8(d, 4) == 0x2e &&
   load8(d, 5) == 0x91 {
      return {"kind": "w64", "big": 0, "start": 40, "head": 24,
         "wide": 1, "align": 8}
   }
   if !_tag_at(d, 8, "WAVE") { return dict() }
   if _tag_at(d, 0, "RIFF") {
      return {"kind": "riff", "big": 0, "start": 12, "head": 8,
         "wide": 0, "align": 2}
   }
   if _tag_at(d, 0, "RIFX") {
      return {"kind": "rifx", "big": 1, "start": 12, "head": 8,
         "wide": 0, "align": 2}
   }
   if _tag_at(d, 0, "RF64") {
      return {"kind": "rf64", "big": 0, "start": 12, "head": 8,
         "wide": 0, "align": 2}
   }
   dict()
}

fn _parse_fmt(any d, int at, int size, bool big) dict {
   if size < 16 { return dict() }
   mut tag = _u16(d, at, big)
   def bits = _u16(d, at + 14, big)
   mut valid = bits
   def extra = size >= 18 ? _u16(d, at + 16, big) : 0
   def extra_at = at + 18
   if tag == _TAG_EXTENSIBLE {
      if size < 40 { return dict() }
      valid = _u16(d, extra_at, big)
      tag = _u16(d, extra_at + 6, big)
   }
   {
      "tag": tag,
      "channels": _u16(d, at + 2, big),
      "rate": _u32(d, at + 4, big),
      "block_align": _u16(d, at + 12, big),
      "bits": bits,
      "valid_bits": valid,
      "extra": extra,
      "extra_at": extra_at,
      "extra_end": at + size
   }
}

fn _source(any out, int len, dict fmt, int bits, int sample_fmt, int tag) any {
   make_memory_source(out, len, fmt.get("channels", 0), fmt.get("rate", 0), bits, sample_fmt,
      tag)
}

fn _decode_pcm(any d, int at, int size, dict fmt, bool big) any {
   def width = fmt.get("bits", 0) <= 8 ? 1 : (fmt.get("bits", 0) + 7) / 8
   def count = size / width
   if count <= 0 { return 0 }
   if width == 1 {
      def out = malloc(count)
      if !out { return 0 }
      memcpy(out, ptr_add(d, at), count)
      return _source(out, count, fmt, 8, _FMT_U8, 1)
   }
   ;; Valid bits sit at the top of each container, so a 12-bit sample is
   ;; already scaled like a 16-bit one.
   if width == 2 {
      def out = malloc(count * 2)
      if !out { return 0 }
      mut i = 0
      while i < count {
         store16(out, _u16(d, at + i * 2, big), i * 2)
         i += 1
      }
      return _source(out, count * 2, fmt, 16, _FMT_S16, 1)
   }
   if width == 3 {
      def out = malloc(count * 3)
      if !out { return 0 }
      mut i = 0
      while i < count {
         def p = at + i * 3
         store8(out, load8(d, big ? p + 2 : p), i * 3)
         store8(out, load8(d, p + 1), i * 3 + 1)
         store8(out, load8(d, big ? p : p + 2), i * 3 + 2)
         i += 1
      }
      return _source(out, count * 3, fmt, 24, _FMT_S24, 1)
   }
   if width == 4 {
      def out = malloc(count * 4)
      if !out { return 0 }
      mut i = 0
      while i < count {
         store32(out, _u32(d, at + i * 4, big), i * 4)
         i += 1
      }
      return _source(out, count * 4, fmt, 32, _FMT_S32, 1)
   }
   if width == 8 {
      def out = malloc(count * 4)
      if !out { return 0 }
      mut i = 0
      while i < count {
         store32(out, _u32(d, at + i * 8 + (big ? 0 : 4), big), i * 4)
         i += 1
      }
      return _source(out, count * 4, fmt, 32, _FMT_S32, 1)
   }
   0
}

fn _decode_float(any d, int at, int size, dict fmt, bool big) any {
   if fmt.get("bits", 0) == 32 {
      def count = size / 4
      if count <= 0 { return 0 }
      def out = malloc(count * 4)
      if !out { return 0 }
      mut i = 0
      while i < count {
         store32(out, _u32(d, at + i * 4, big), i * 4)
         i += 1
      }
      return _source(out, count * 4, fmt, 32, _FMT_F32, 3)
   }
   if fmt.get("bits", 0) == 64 {
      def count = size / 8
      if count <= 0 { return 0 }
      def wide = malloc(8)
      if !wide { return 0 }
      def out = malloc(count * 4)
      if !out {
         free(wide)
         return 0
      }
      mut i = 0
      while i < count {
         mut b = 0
         while b < 8 {
            store8(wide, load8(d, at + i * 8 + (big ? 7 - b : b)), b)
            b += 1
         }
         store32_f32(out, load64_f64(wide, 0), i * 4)
         i += 1
      }
      free(wide)
      return _source(out, count * 4, fmt, 32, _FMT_F32, 3)
   }
   0
}

fn _alaw(int a) int {
   def v = a ^ 0x55
   def seg = (v & 0x70) >> 4
   mut t = (v & 0x0f) << 4
   if seg == 0 {
      t += 8
   } else {
      t = (t + 0x108) << (seg - 1)
   }
   (v & 0x80) != 0 ? t : -t
}

fn _mulaw(int u) int {
   def v = (~u) & 0xff
   def t = (((v & 0x0f) << 3) + 0x84) << ((v & 0x70) >> 4)
   (v & 0x80) != 0 ? 0x84 - t : t - 0x84
}

fn _decode_companded(any d, int at, int size, dict fmt, bool mu) any {
   if size <= 0 { return 0 }
   def out = malloc(size * 2)
   if !out { return 0 }
   mut i = 0
   while i < size {
      def b = load8(d, at + i)
      store16(out, mu ? _mulaw(b) : _alaw(b), i * 2)
      i += 1
   }
   _source(out, size * 2, fmt, 16, _FMT_S16, 1)
}

fn _ms_coefs(any d, dict fmt) list {
   mut int n = 7
   def int at = fmt.get("extra_at", 0)
   def int end_at = fmt.get("extra_end", 0)
   def bool custom = fmt.get("extra", 0) >= 4 && at + 4 <= end_at
   if custom {
      def int stated = _u16(d, at + 2, false)
      if stated > 0 && at + 4 + stated * 4 <= end_at { n = stated } else { n = 0 }
   } else {
      n = 0
   }
   def int count = n > 0 ? n : 7
   def c1 = i64buf_new(count)
   def c2 = i64buf_new(count)
   mut int i = 0
   while i < count {
      if n > 0 {
         i64buf_store(c1, i, _s16(_u16(d, at + 4 + i * 4, false)))
         i64buf_store(c2, i, _s16(_u16(d, at + 6 + i * 4, false)))
      } else {
         i64buf_store(c1, i, int(_MS_COEF1[i]))
         i64buf_store(c2, i, int(_MS_COEF2[i]))
      }
      i += 1
   }
   [c1, c2, count]
}

fn _decode_ms_adpcm(any d, int at, int size, dict fmt) any {
   def int channels = fmt.get("channels", 0)
   def int block = fmt.get("block_align", 0)
   if block < 7 * channels || channels < 1 || channels > 2 { return 0 }
   mut int per_block = (block - 7 * channels) * 2 / channels + 2
   def int extra_at = fmt.get("extra_at", 0)
   if fmt.get("extra", 0) >= 2 && extra_at + 2 <= fmt.get("extra_end", 0) {
      def int stated = _u16(d, extra_at, false)
      if stated >= 2 && stated <= per_block { per_block = stated }
   }
   def int blocks = size / block
   def int tail = size % block
   def int tail_frames = tail >= 7 * channels ? (tail - 7 * channels) * 2 / channels + 2 : 0
   def int max_frames = (9223372036854775807 - 16) / (channels * 2)
   if per_block <= 0 || tail_frames > max_frames ||
      blocks > (max_frames - tail_frames) / per_block { return 0 }
   def int total = blocks * per_block + tail_frames
   def out = malloc(total * channels * 2 + 16)
   if !out { return 0 }
   def tables = _ms_coefs(d, fmt)
   def ptr c1 = tables[0]
   def ptr c2 = tables[1]
   def int coef_count = tables[2]
   def state = i64buf_new(8)
   mut int written = 0
   mut int b = 0
   def int block_total = blocks + (tail_frames > 0 ? 1 : 0)
   while b < block_total {
      def int base = at + b * block
      def int avail = b < blocks ? block : tail
      def int frames = b < blocks ? per_block : tail_frames
      mut int ch = 0
      while ch < channels {
         def int p = load8(d, base + ch)
         i64buf_store(state, ch, p < coef_count ? p : 0)
         i64buf_store(state, 2 + ch, _s16(_u16(d, base + channels + ch * 2, false)))
         i64buf_store(state, 4 + ch, _s16(_u16(d, base + channels * 3 + ch * 2, false)))
         i64buf_store(state, 6 + ch, _s16(_u16(d, base + channels * 5 + ch * 2, false)))
         ch += 1
      }
      ch = 0
      while ch < channels {
         store16(out, i64buf_load(state, 6 + ch), (written + ch) * 2)
         store16(out, i64buf_load(state, 4 + ch), (written + channels + ch) * 2)
         ch += 1
      }
      written += channels * 2
      mut int nibble = 0
      def int nibbles = (frames - 2) * channels
      def int body = base + channels * 7
      def int body_len = avail - channels * 7
      while nibble < nibbles && nibble / 2 < body_len {
         def int packed = load8(d, body + nibble / 2)
         def int n = nibble % 2 == 0 ? (packed >> 4) & 15 : packed & 15
         def int c = nibble % channels
         def int signed = n >= 8 ? n - 16 : n
         def int coef = i64buf_load(state, c)
         def int s1 = i64buf_load(state, 4 + c)
         def int s2 = i64buf_load(state, 6 + c)
         def int delta = i64buf_load(state, 2 + c)
         ;; The reference predictor divides, truncating toward zero.
         def int mixed = s1 * i64buf_load(c1, coef) + s2 * i64buf_load(c2, coef)
         def int guess = mixed < 0 ? -((-mixed) >> 8) : mixed >> 8
         def int value = _clamp16(guess + signed * delta)
         i64buf_store(state, 6 + c, s1)
         i64buf_store(state, 4 + c, value)
         def int adapted = (i64buf_load(_ms_adapt, n) * delta) >> 8
         i64buf_store(state, 2 + c, adapted < 16 ? 16 : adapted)
         store16(out, value, written * 2)
         written += 1
         nibble += 1
      }
      b += 1
   }
   _source(out, written * 2, fmt, 16, _FMT_S16, 1)
}

fn _decode_ima_adpcm(any d, int at, int size, dict fmt) any {
   def int channels = fmt.get("channels", 0)
   def int block = fmt.get("block_align", 0)
   if block < 4 * channels || channels < 1 || channels > 2 { return 0 }
   def int per_block = (block - 4 * channels) * 2 / channels + 1
   def int blocks = size / block
   def int tail = size % block
   def int tail_frames = tail >= 4 * channels ? (tail - 4 * channels) * 2 / channels + 1 : 0
   def int max_frames = (9223372036854775807 - 16) / (channels * 2)
   if per_block <= 0 || tail_frames > max_frames ||
      blocks > (max_frames - tail_frames) / per_block { return 0 }
   def int total = blocks * per_block + tail_frames
   def out = malloc(total * channels * 2 + 16)
   if !out { return 0 }
   def state = i64buf_new(4)
   mut int written = 0
   mut int b = 0
   def int block_total = blocks + (tail_frames > 0 ? 1 : 0)
   while b < block_total {
      def int base = at + b * block
      def int avail = b < blocks ? block : tail
      def int frames = b < blocks ? per_block : tail_frames
      mut int ch = 0
      while ch < channels {
         def int first = _s16(_u16(d, base + ch * 4, false))
         def int index = load8(d, base + ch * 4 + 2)
         i64buf_store(state, ch, first)
         i64buf_store(state, 2 + ch, index > 88 ? 88 : index)
         store16(out, first, (written + ch) * 2)
         ch += 1
      }
      written += channels
      ;; Each channel owns groups of four bytes, eight samples per group.
      def int body = base + channels * 4
      def int body_len = avail - channels * 4
      def int samples = frames - 1
      mut int k = 0
      while k < samples {
         def int group = k / 8
         mut int c = 0
         while c < channels {
            def int offset = group * 4 * channels + c * 4 + (k % 8) / 2
            if offset < body_len {
               def int packed = load8(d, body + offset)
               def int n = k % 2 == 0 ? packed & 15 : (packed >> 4) & 15
               def int stepped = i64buf_load(_ima_step, i64buf_load(state, 2 + c))
               mut int diff = stepped >> 3
               if (n & 1) != 0 { diff += stepped >> 2 }
               if (n & 2) != 0 { diff += stepped >> 1 }
               if (n & 4) != 0 { diff += stepped }
               if (n & 8) != 0 { diff = -diff }
               def int value = _clamp16(i64buf_load(state, c) + diff)
               i64buf_store(state, c, value)
               def int moved = i64buf_load(state, 2 + c) + i64buf_load(_ima_index, n)
               i64buf_store(state, 2 + c, moved < 0 ? 0 : (moved > 88 ? 88 : moved))
               store16(out, value, (written + k * channels + c) * 2)
            } else {
               store16(out, 0, (written + k * channels + c) * 2)
            }
            c += 1
         }
         k += 1
      }
      written += samples * channels
      b += 1
   }
   _source(out, written * 2, fmt, 16, _FMT_S16, 1)
}

fn decode_bytes(any d) any {
   "Decodes WAV container bytes into a sound source, or returns 0."
   def wrapper = _container(d)
   if wrapper.len == 0 { return 0 }
   def big = wrapper.get("big", 0) == 1
   def head = wrapper.get("head", 0)
   def wide = wrapper.get("wide", 0) == 1
   def alignment = wrapper.get("align", 0)
   mut fmt = dict()
   mut data_at = -1
   mut data_size = 0
   mut rf64_data = -1
   mut pos = wrapper.get("start", 0)
   while pos + head <= d.len {
      def declared = wide ? _u64(d, pos + 16, false) - head : _u32(d, pos + 4, big)
      def body = pos + head
      if _tag_at(d, pos, "ds64") && !wide && body + 16 <= d.len {
         rf64_data = _u64(d, body + 8, false)
      } elif _tag_at(d, pos, "fmt ") && body + declared <= d.len {
         fmt = _parse_fmt(d, body, declared, big)
      } elif _tag_at(d, pos, "data") {
         data_at = body
         data_size = declared
         if rf64_data >= 0 && declared == 0xffffffff { data_size = rf64_data }
         if data_size > d.len - body { data_size = d.len - body }
         if fmt.len > 0 { break }
      }
      if declared < 0 { return 0 }
      def rem = declared % alignment
      pos = body + declared + (rem == 0 ? 0 : alignment - rem)
   }
   if fmt.len == 0 || data_at < 0 || fmt.get("channels", 0) <= 0 || fmt.get("rate", 0) <= 0 {
      return 0
   }
   def tag = fmt.get("tag", 0)
   if tag == _TAG_PCM { return _decode_pcm(d, data_at, data_size, fmt, big) }
   if tag == _TAG_FLOAT { return _decode_float(d, data_at, data_size, fmt, big) }
   if tag == _TAG_ALAW {
      return _decode_companded(d, data_at, data_size, fmt, false)
   }
   if tag == _TAG_MULAW {
      return _decode_companded(d, data_at, data_size, fmt, true)
   }
   if tag == _TAG_ADPCM { return _decode_ms_adpcm(d, data_at, data_size, fmt) }
   if tag == _TAG_IMA { return _decode_ima_adpcm(d, data_at, data_size, fmt) }
   0
}
