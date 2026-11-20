;; Keywords: sound flac ogg rice lpc crc seek
;; FLAC bitstream reconstruction in Nytrix. Reference: RFC 9639 and dr_flac.
module core.os.sound.flac(open_bytes, read_range, decode_bytes)
use core
use core.tbuf
use core.os.sound.source

fn _u(any d, int at, int n) int {
   mut v = 0
   mut i = 0
   while i < n { v = (v << 8) | load8(d, at + i) i += 1 }
   v
}

fn _tag(any d, int at, str tag) bool {
   if at < 0 || at + tag.len > d.len { return false }
   mut i = 0
   while i < tag.len {
      if load8(d, at + i) != load8(tag, i) { return false }
      i += 1
   }
   true
}

fn _crc(any d, int start, int end, int width, int polynomial) int {
   mut value = 0
   def mask = (1 << width) - 1
   mut i = start
   while i < end {
      value = value ^ (load8(d, i) << (width - 8))
      mut bit = 0
      while bit < 8 {
         value = ((value << 1) ^ ((value & (1 << (width - 1))) != 0 ? polynomial : 0)) & mask
         bit += 1
      }
      i += 1
   }
   value
}

fn _ogg(any d) bytes {
   def out = bytes(d.len)
   mut written = 0
   mut at = 0
   mut serial = -1
   mut sequence = 0
   mut packet = 0
   mut first = true
   mut continued = false
   while at < d.len {
      if at + 27 > d.len || !_tag(d, at, "OggS") || load8(d, at + 4) != 0 { return bytes(0) }
      def segments = load8(d, at + 26)
      if at + 27 + segments > d.len { return bytes(0) }
      mut size = 0
      mut i = 0
      while i < segments { size += load8(d, at + 27 + i) i += 1 }
      def end = at + 27 + segments + size
      if end > d.len { return bytes(0) }
      mut checksum = 0
      i = at
      while i < end {
         def byte = i >= at + 22 && i < at + 26 ? 0 : load8(d, i)
         checksum = checksum ^ (byte << 24)
         mut b = 0
         while b < 8 {
            checksum = ((checksum << 1) ^ (((checksum & 0x80000000) != 0) ? 0x04c11db7 : 0)) & 0xffffffff
            b += 1
         }
         i += 1
      }
      def stored = load8(d, at + 22) | (load8(d, at + 23) << 8) |
      (load8(d, at + 24) << 16) | (load8(d, at + 25) << 24)
      if stored != checksum { return bytes(0) }
      def page_serial = load8(d, at + 14) | (load8(d, at + 15) << 8) |
      (load8(d, at + 16) << 16) | (load8(d, at + 17) << 24)
      def payload = at + 27 + segments
      if serial == -1 && size >= 9 && load8(d, payload) == 127 && _tag(d, payload + 1, "FLAC") {
         serial = page_serial
         sequence = load8(d, at + 18) | (load8(d, at + 19) << 8) |
         (load8(d, at + 20) << 16) | (load8(d, at + 21) << 24)
      }
      if page_serial == serial {
         def seq = load8(d, at + 18) | (load8(d, at + 19) << 8) |
         (load8(d, at + 20) << 16) | (load8(d, at + 21) << 24)
         if seq != sequence || ((load8(d, at + 5) & 1) != 0) != continued { return bytes(0) }
         sequence = (sequence + 1) & 0xffffffff
         mut pos = payload
         i = 0
         while i < segments {
            def n = load8(d, at + 27 + i)
            mut byte = 0
            while byte < n {
               store8(out, load8(d, pos + byte), written + byte)
               byte += 1
            }
            written += n
            packet += n
            pos += n
            continued = n == 255
            if !continued {
               if first {
                  if packet < 51 || load8(out, 5) != 1 || !_tag(out, 9, "fLaC") { return bytes(0) }
                  mut j = 9
                  while j < written { store8(out, load8(out, j), j - 9) j += 1 }
                  written -= 9
                  first = false
               }
               packet = 0
            }
            i += 1
         }
      }
      at = end
   }
   if first || continued { return bytes(0) }
   def raw = bytes(written)
   memcpy(raw, out, written)
   raw
}

;; Reader slots: bit position and sticky error. Checked before every fetch.
fn _bits(any d, any r, int n) int {
   def pos = i64buf_load(r, 0)
   if n < 0 || n > 36 || pos < 0 || n > d.len * 8 - pos {
      i64buf_store(r, 1, 1)
      return 0
   }
   mut value = 0
   mut left = n
   mut at = pos
   while left > 0 {
      def take = (8 - (at & 7)) < left ? 8 - (at & 7) : left
      value = (value << take) | ((load8(d, at >> 3) >> (8 - (at & 7) - take)) & ((1 << take) - 1))
      at += take
      left -= take
   }
   i64buf_store(r, 0, at)
   value
}

fn _signed(any d, any r, int n) int {
   if n == 0 { return 0 }
   def v = _bits(d, r, n)
   (v & (1 << (n - 1))) != 0 ? v - (1 << n) : v
}

fn _unary(any d, any r) int {
   mut n = 0
   while i64buf_load(r, 1) == 0 {
      if _bits(d, r, 1) != 0 { return n }
      n += 1
   }
   0
}

fn _number(any d, any r) int {
   def first = _bits(d, r, 8)
   if first < 128 { return first }
   mut count = 0
   mut mask = 128
   while (first & mask) != 0 && mask > 0 { count += 1 mask = mask >> 1 }
   if count < 2 || count > 7 { i64buf_store(r, 1, 1) return 0 }
   mut value = first & ((1 << (7 - count)) - 1)
   mut i = 1
   while i < count {
      def byte = _bits(d, r, 8)
      if (byte & 192) != 128 { i64buf_store(r, 1, 1) return 0 }
      value = (value << 6) | (byte & 63)
      i += 1
   }
   if value < (count == 2 ? 128 : (1 << (5 * count - 4))) { i64buf_store(r, 1, 1) }
   value
}

fn _residual(any d, any r, any samples, int n, int order) bool {
   def method = _bits(d, r, 2)
   def partition = _bits(d, r, 4)
   if method > 1 || n % (1 << partition) != 0 || (n >> partition) < order { return false }
   def width = method == 0 ? 4 : 5
   mut p = 0
   mut at = order
   while p < (1 << partition) {
      def rice = _bits(d, r, width)
      def raw = rice == ((1 << width) - 1) ? _bits(d, r, 5) : -1
      def count = (n >> partition) - (p == 0 ? order : 0)
      mut i = 0
      while i < count {
         mut value = 0
         if raw >= 0 { value = _signed(d, r, raw) }
         else {
            def quotient = _unary(d, r)
            if quotient > (0x7fffffffffffffff >> rice) { return false }
            def folded = (quotient << rice) | _bits(d, r, rice)
            value = (folded >> 1) ^ (0 - (folded & 1))
         }
         i64buf_store(samples, at, value)
         at += 1
         i += 1
         if i64buf_load(r, 1) != 0 { return false }
      }
      p += 1
   }
   true
}

fn _subframe(any d, any r, any samples, int n, int width) bool {
   if _bits(d, r, 1) != 0 { return false }
   def kind = _bits(d, r, 6)
   def wasted = _bits(d, r, 1) != 0 ? _unary(d, r) + 1 : 0
   def bits = width - wasted
   if bits < 1 || bits > 33 { return false }
   mut i = 0
   if kind == 0 {
      def value = _signed(d, r, bits)
      while i < n { i64buf_store(samples, i, value) i += 1 }
   } elif kind == 1 {
      while i < n { i64buf_store(samples, i, _signed(d, r, bits)) i += 1 }
   } else {
      if (kind < 8 || kind > 12) && kind < 32 { return false }
      def order = kind < 32 ? kind - 8 : kind - 31
      if order > n { return false }
      while i < order { i64buf_store(samples, i, _signed(d, r, bits)) i += 1 }
      mut coefficients = 0
      mut shift = 0
      if kind >= 32 {
         def precision = _bits(d, r, 4) + 1
         if precision == 16 { return false }
         shift = _signed(d, r, 5)
         coefficients = i64buf_new(order)
         mut j = 0
         while j < order { i64buf_store(coefficients, j, _signed(d, r, precision)) j += 1 }
      }
      if !_residual(d, r, samples, n, order) { return false }
      while i < n {
         mut prediction = 0
         if kind >= 32 {
            mut j = 0
            while j < order {
               prediction += i64buf_load(coefficients, j) * i64buf_load(samples, i - j - 1)
               j += 1
            }
            if shift < 0 { prediction = prediction << (0 - shift) }
            else { prediction = prediction >> shift }
         } elif order == 1 { prediction = i64buf_load(samples, i - 1) }
         elif order == 2 { prediction = 2 * i64buf_load(samples, i - 1) - i64buf_load(samples, i - 2) }
         elif order == 3 { prediction = 3 * (i64buf_load(samples, i - 1) - i64buf_load(samples, i - 2)) + i64buf_load(samples, i - 3) }
         elif order == 4 { prediction = 4 * (i64buf_load(samples, i - 1) + i64buf_load(samples, i - 3)) - 6 * i64buf_load(samples, i - 2) - i64buf_load(samples, i - 4) }
         i64buf_store(samples, i, i64buf_load(samples, i) + prediction)
         i += 1
      }
   }
   if wasted > 0 {
      i = 0
      while i < n { i64buf_store(samples, i, i64buf_load(samples, i) << wasted) i += 1 }
   }
   i64buf_load(r, 1) == 0
}

def _rates = [0,88200,176400,192000,8000,16000,22050,24000,32000,44100,48000,96000]
def _widths = [0,8,12,0,16,20,24,32]

fn _frame(any ctx, int at) any {
   def d = ctx.get("data")
   def r = i64buf_new(2)
   i64buf_store(r, 0, at * 8)
   i64buf_store(r, 1, 0)
   if _bits(d, r, 15) != 0x7ffc { return 0 }
   def variable = _bits(d, r, 1)
   def blockcode = _bits(d, r, 4)
   def ratecode = _bits(d, r, 4)
   def assignment = _bits(d, r, 4)
   def sizecode = _bits(d, r, 3)
   if _bits(d, r, 1) != 0 || blockcode == 0 || ratecode == 15 || assignment > 10 || sizecode == 3 { return 0 }
   def number = _number(d, r)
   mut n = 0
   if blockcode == 1 { n = 192 }
   elif blockcode <= 5 { n = 576 << (blockcode - 2) }
   elif blockcode == 6 { n = _bits(d, r, 8) + 1 }
   elif blockcode == 7 { n = _bits(d, r, 16) + 1 }
   else { n = 256 << (blockcode - 8) }
   mut rate = ratecode == 0 ? int(ctx.get("rate")) : (ratecode < 12 ? int(_rates[ratecode]) : 0)
   if ratecode == 12 { rate = _bits(d, r, 8) * 1000 }
   elif ratecode == 13 { rate = _bits(d, r, 16) }
   elif ratecode == 14 { rate = _bits(d, r, 16) * 10 }
   def width = sizecode == 0 ? int(ctx.get("bits")) : int(_widths[sizecode])
   def channels = assignment < 8 ? assignment + 1 : 2
   def header_end = i64buf_load(r, 0) / 8
   def checksum = _bits(d, r, 8)
   if i64buf_load(r, 1) != 0 || _crc(d, at, header_end, 8, 7) != checksum ||
   n > ctx.get("max_block") || rate != ctx.get("rate") || width != ctx.get("bits") || channels != ctx.get("channels") { return 0 }
   def buffers = ctx.get("buffers")
   mut ch = 0
   while ch < channels {
      def side = (assignment == 8 && ch == 1) || (assignment == 9 && ch == 0) || (assignment == 10 && ch == 1)
      if !_subframe(d, r, buffers[ch], n, width + (side ? 1 : 0)) { return 0 }
      ch += 1
   }
   def padding = (8 - (i64buf_load(r, 0) & 7)) & 7
   if _bits(d, r, padding) != 0 { return 0 }
   def frame_end = i64buf_load(r, 0) / 8
   def crc16 = _bits(d, r, 16)
   if i64buf_load(r, 1) != 0 || _crc(d, at, frame_end, 16, 0x8005) != crc16 { return 0 }
   if assignment >= 8 {
      def left = buffers[0]
      def right = buffers[1]
      mut i = 0
      while i < n {
         mut a = i64buf_load(left, i)
         mut b = i64buf_load(right, i)
         if assignment == 8 { b = a - b }
         elif assignment == 9 { a += b }
         else {
            def mid = (a << 1) | (b & 1)
            a = (mid + b) >> 1
            b = (mid - b) >> 1
         }
         i64buf_store(left, i, a)
         i64buf_store(right, i, b)
         i += 1
      }
   }
   {"next": frame_end + 2, "frames": n, "sample": variable != 0 ? number : number * int(ctx.get("max_block"))}
}

fn open_bytes(any input) any {
   mut bytes d = input
   if _tag(d, 0, "OggS") { d = _ogg(d) if !d { return 0 } }
   if d.len < 42 || !_tag(d, 0, "fLaC") || (load8(d, 4) & 127) != 0 || _u(d, 5, 3) != 34 { return 0 }
   def max_block = _u(d, 10, 2)
   def rate = (load8(d, 18) << 12) | (load8(d, 19) << 4) | (load8(d, 20) >> 4)
   def channels = ((load8(d, 20) >> 1) & 7) + 1
   def bits = (((load8(d, 20) & 1) << 4) | (load8(d, 21) >> 4)) + 1
   def total = ((load8(d, 21) & 15) << 32) | _u(d, 22, 4)
   if max_block < 16 || bits < 4 || bits > 32 || rate <= 0 { return 0 }
   mut buffers = []
   mut ch = 0
   while ch < channels { buffers = buffers.append(i64buf_new(max_block)) ch += 1 }
   mut at = 4
   mut seek = []
   mut last = false
   while !last {
      if at + 4 > d.len { return 0 }
      last = (load8(d, at) & 128) != 0
      def kind = load8(d, at) & 127
      def size = _u(d, at + 1, 3)
      if size > d.len - at - 4 || kind == 127 { return 0 }
      if kind == 3 {
         if size % 18 != 0 { return 0 }
         mut i = at + 4
         mut previous = -1
         while i < at + 4 + size {
            if _u(d, i, 4) != 0xffffffff || _u(d, i + 4, 4) != 0xffffffff {
               def sample = _u(d, i, 8)
               def offset = _u(d, i + 8, 8)
               if sample < 0 || sample <= previous || offset < 0 { return 0 }
               seek = seek.append([sample, offset])
               previous = sample
            }
            i += 18
         }
      }
      at += 4 + size
   }
   def ctx = {"data": d, "start": at, "rate": rate, "channels": channels,
      "bits": bits, "frames": total, "max_block": max_block, "buffers": buffers,
      "seek": seek}
   if total == 0 {
      mut count = 0
      while at < d.len {
         def frame = _frame(ctx, at)
         if !frame { return 0 }
         count += int(frame.get("frames"))
         at = int(frame.get("next"))
      }
      ctx.set("frames", count)
   }
   ctx
}

fn read_range(any ctx, int start, int count) any {
   if !is_dict(ctx) || start < 0 || count < 0 { return 0 }
   def total = int(ctx.get("frames"))
   if start > total { start = total }
   if count > total - start { count = total - start }
   def channels = int(ctx.get("channels"))
   def bits = int(ctx.get("bits"))
   def width = bits <= 16 ? 2 : 4
   mut cleanup = [0]
   defer { if cleanup[0] { free(cleanup[0]) } }
   def out = malloc(count * channels * width + 1)
   if !out { return 0 }
   cleanup[0] = out
   mut at = int(ctx.get("start"))
   mut sample = 0
   def points = ctx.get("seek")
   mut i = 0
   while i < points.len {
      def point = points[i]
      if int(point[0]) <= start {
         sample = int(point[0])
         at = int(ctx.get("start")) + int(point[1])
      }
      i += 1
   }
   mut written = 0
   while written < count {
      def frame = _frame(ctx, at)
      if !frame { return 0 }
      def n = int(frame.get("frames"))
      if int(frame.get("sample")) != sample { return 0 }
      def buffers = ctx.get("buffers")
      mut j = start > sample ? start - sample : 0
      while j < n && written < count {
         mut ch = 0
         while ch < channels {
            def value = i64buf_load(buffers[ch], j) << (width * 8 - bits)
            if width == 2 { store16(out, value, (written * channels + ch) * 2) }
            else { store32(out, value, (written * channels + ch) * 4) }
            ch += 1
         }
         written += 1
         j += 1
      }
      sample += n
      at = int(frame.get("next"))
   }
   def source = make_memory_source(out, count * channels * width, channels, ctx.get("rate"),
      width * 8, width == 2 ? SAMPLE_FMT_S16 : SAMPLE_FMT_S32, 1)
   cleanup[0] = 0
   source
}

fn decode_bytes(any d) any {
   def ctx = open_bytes(d)
   if !ctx { return 0 }
   read_range(ctx, 0, int(ctx.get("frames")))
}
