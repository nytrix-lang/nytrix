;; Keywords: sound streaming frame range seek wav aiff au
;; File-backed decoding. Header scanning skips payloads, and each read loads
;; only the PCM span or independent ADPCM packets covering the requested range.
module core.os.sound.stream(open, close, read, read_range, seek, tell, length, format)
use core
use core.os.sound.wav as wav
use core.os.sound.aiff as aiff
use core.os.sound.au as au
use core.os.sound.source as source

extern "" {
   fn fopen(str path, str mode) ptr
   fn fclose(ptr file) i32
   fn fseek(ptr file, i64 offset, i32 origin) i32
   fn ftell(ptr file) i64
   fn fread(ptr data, u64 width, u64 count, ptr file) u64
}

fn _at(ptr file, int at, int count) bytes {
   if at < 0 || count < 0 || fseek(file, at, 0) != 0 { return bytes(0) }
   def out = bytes(count)
   if count > 0 && fread(out, 1, count, file) != count { return bytes(0) }
   out
}
fn _tag(any d, int at, str tag) bool {
   mut i = 0
   while i < 4 { if load8(d, at + i) != load8(tag, i) { return false } i += 1 }
   true
}
fn _u16(any d, int at, bool big) int {
   big ? (load8(d, at) << 8) | load8(d, at + 1) : load8(d, at) | (load8(d, at + 1) << 8)
}
fn _u32(any d, int at, bool big) int {
   big ? (_u16(d, at, true) << 16) | _u16(d, at + 2, true) : _u16(d, at, false) | (_u16(d, at + 2, false) << 16)
}
fn _put32(any d, int at, int value, bool big) int {
   mut i = 0
   while i < 4 { store8(d, (value >> ((big ? 3 - i : i) * 8)) & 255, at + i) i += 1 }
   0
}
fn _text(any d, int at, str text) int {
   mut i = 0
   while i < text.len { store8(d, load8(text, i), at + i) i += 1 }
   0
}

fn _scan(ptr file, int size) any {
   def h = _at(file, 0, 24)
   if h.len < 24 { return 0 }
   mut kind = ""
   mut big = true
   mut pos = 12
   mut fmt = bytes(0)
   mut data_at = -1
   mut data_size = 0
   mut frames = 0
   mut block_frames = 1
   mut block = 0
   mut channels = 0
   if _tag(h, 0, ".snd") {
      data_at = _u32(h, 4, true)
      data_size = _u32(h, 8, true)
      if data_size == 0xffffffff { data_size = size - data_at }
      def enc = _u32(h, 12, true)
      channels = _u32(h, 20, true)
      def width = enc == 1 || enc == 2 || enc == 27 ? 1 : (enc >= 3 && enc <= 5 ? enc - 1 : (enc == 6 ? 4 : (enc == 7 ? 8 : 0)))
      if width == 0 { return 0 }
      block = width * channels
      kind = "au"
      fmt = h
   } elif _tag(h, 0, "FORM") && (_tag(h, 8, "AIFF") || _tag(h, 8, "AIFC")) {
      kind = _tag(h, 8, "AIFF") ? "aiff" : "aifc"
   } elif (_tag(h, 0, "RIFF") || _tag(h, 0, "RIFX") || _tag(h, 0, "RF64")) && _tag(h, 8, "WAVE") {
      kind = "wav"
      big = _tag(h, 0, "RIFX")
   } else { return 0 }
   mut rf_size = -1
   if kind != "au" {
      while pos <= size - 8 {
         def ch = _at(file, pos, 8)
         if ch.len != 8 { return 0 }
         mut n = _u32(ch, 4, big)
         def body = pos + 8
         if kind == "wav" && _tag(ch, 0, "ds64") && n >= 28 {
            def ds = _at(file, body, 28)
            if ds.len != 28 { return 0 }
            rf_size = load64(ds, 8)
         }
         if kind == "wav" && _tag(ch, 0, "data") && n == 0xffffffff && rf_size >= 0 { n = rf_size }
         if n < 0 || n > size - body { return 0 }
         if (kind == "wav" && _tag(ch, 0, "fmt ")) || (kind != "wav" && _tag(ch, 0, "COMM")) {
            if n > 65536 { return 0 }
            fmt = _at(file, body, n)
         } elif kind == "wav" && _tag(ch, 0, "data") {
            data_at = body
            data_size = n
         } elif kind != "wav" && _tag(ch, 0, "SSND") && n >= 8 {
            def ss = _at(file, body, 8)
            def offset = _u32(ss, 0, true)
            if offset > n - 8 { return 0 }
            data_at = body + 8 + offset
            data_size = n - 8 - offset
         }
         pos = body + n + (n & 1)
      }
      if kind == "wav" {
         if fmt.len < 16 { return 0 }
         channels = _u16(fmt, 2, big)
         block = _u16(fmt, 12, big)
         mut tag = _u16(fmt, 0, big)
         if tag == 0xfffe && fmt.len >= 40 { tag = _u16(fmt, 24, big) }
         if tag == 2 || tag == 17 {
            if channels < 1 || channels > 2 { return 0 }
            def head = (tag == 2 ? 7 : 4) * channels
            if block < head { return 0 }
            block_frames = (block - head) * 2 / channels + (tag == 2 ? 2 : 1)
            if tag == 2 && fmt.len >= 20 {
               def stated = _u16(fmt, 18, big)
               if stated >= 2 && stated <= block_frames { block_frames = stated }
            }
            def tail = data_size % block
            frames = data_size / block * block_frames
            if tail >= head { frames += (tail - head) * 2 / channels + (tag == 2 ? 2 : 1) }
         } elif tag != 1 && tag != 3 && tag != 6 && tag != 7 { return 0 }
      } else {
         if fmt.len < (kind == "aifc" ? 22 : 18) { return 0 }
         channels = _u16(fmt, 0, true)
         frames = _u32(fmt, 2, true)
         block = ((_u16(fmt, 6, true) + 7) / 8) * channels
         if kind == "aifc" {
            if _tag(fmt, 18, "ima4") { block = 34 * channels block_frames = 64 }
            elif _tag(fmt, 18, "alaw") || _tag(fmt, 18, "ulaw") { block = channels }
            elif _tag(fmt, 18, "fl32") || _tag(fmt, 18, "FL32") { block = channels * 4 }
            elif _tag(fmt, 18, "fl64") || _tag(fmt, 18, "FL64") { block = channels * 8 }
         }
         if block <= 0 || ((frames + block_frames - 1) / block_frames) * block > data_size { return 0 }
      }
   }
   if channels <= 0 || channels > 32 || block <= 0 || data_at < 0 || data_at > size || data_size > size - data_at { return 0 }
   if kind == "au" || (kind == "wav" && block_frames == 1) {
      if data_size % block != 0 { return 0 }
      frames = data_size / block
   }
   {"file": file, "kind": kind, "big": big, "header": fmt, "data_at": data_at,
    "data_size": data_size, "block": block, "block_frames": block_frames,
    "frames": frames, "cursor": 0, "channels": channels}
}

fn open(str path) any {
   ;; Returns a stream dictionary or 0. The caller owns close().
   def file = fopen(path, "rb")
   if !file { return 0 }
   if fseek(file, 0, 2) != 0 { fclose(file) return 0 }
   def size = ftell(file)
   def st = _scan(file, size)
   if !st { fclose(file) return 0 }
   ;; Decode at most one frame to validate the format and obtain output metadata.
   def probe = read_range(st, 0, 1)
   if !probe { fclose(file) return 0 }
   st.set("format", source.format(probe))
   st.set("frame_size", probe.get(1).get("frame_size"))
   free(probe.get(1).get("ptr"))
   st
}

fn read_range(any st, int start, int count) any {
   ;; Returns an independently owned memory sound source without moving cursor.
   if !is_dict(st) || !st.get("file", 0) || start < 0 || count < 0 { return 0 }
   def total = int(st.get("frames"))
   if start > total { start = total }
   if count > total - start { count = total - start }
   def block = int(st.get("block"))
   def per = int(st.get("block_frames"))
   def first = start / per
   def skip = start % per
   def packets = (skip + count + per - 1) / per
   def offset = first * block
   mut n = packets * block
   if n > st.get("data_size") - offset { n = st.get("data_size") - offset }
   if count == 0 { n = 0 }
   def raw = _at(st.get("file"), st.get("data_at") + offset, n)
   if raw.len != n { return 0 }
   def fmt = st.get("header")
   def kind = st.get("kind")
   def big = st.get("big")
   mut decoded = 0
   if kind == "au" {
      def d = bytes(24 + n)
      memcpy(d, fmt, 24)
      _put32(d, 4, 24, true)
      _put32(d, 8, n, true)
      memcpy(d + 24, raw, n)
      decoded = au.decode_bytes(d)
   } elif kind == "wav" {
      def head = 28 + fmt.len + (fmt.len & 1)
      def d = bytes(head + n)
      _text(d, 0, big ? "RIFX" : "RIFF")
      _put32(d, 4, head + n - 8, big)
      _text(d, 8, "WAVEfmt ")
      _put32(d, 16, fmt.len, big)
      memcpy(d + 20, fmt, fmt.len)
      _text(d, head - 8, "data")
      _put32(d, head - 4, n, big)
      memcpy(d + head, raw, n)
      decoded = wav.decode_bytes(d)
   } else {
      def head = 36 + fmt.len + (fmt.len & 1)
      def d = bytes(head + n)
      _text(d, 0, "FORM")
      _put32(d, 4, head + n - 8, true)
      _text(d, 8, kind == "aiff" ? "AIFF" : "AIFC")
      _text(d, 12, "COMM")
      _put32(d, 16, fmt.len, true)
      memcpy(d + 20, fmt, fmt.len)
      _put32(d, 22, skip + count, true)
      _text(d, head - 16, "SSND")
      _put32(d, head - 12, n + 8, true)
      memcpy(d + head, raw, n)
      decoded = aiff.decode_bytes(d)
   }
   if !decoded { return 0 }
   def meta = decoded.get(1)
   def frame_size = int(meta.get("frame_size"))
   if skip + count > meta.get("total_frames") { free(meta.get("ptr")) return 0 }
   def out = malloc(count * frame_size + 1)
   memcpy(out, meta.get("ptr") + skip * frame_size, count * frame_size)
   free(meta.get("ptr"))
   source.make_memory_source(out, count * frame_size, meta.get("channels"), meta.get("rate"),
      meta.get("bits"), meta.get("sample_fmt"), meta.get("format_tag"))
}

fn read(any st, any out, int count) int {
   if !is_dict(st) || count <= 0 { return 0 }
   def src = read_range(st, tell(st), count)
   if !src { return 0 }
   def meta = src.get(1)
   def got = int(meta.get("total_frames"))
   memcpy(out, meta.get("ptr"), meta.get("len"))
   free(meta.get("ptr"))
   st.set("cursor", tell(st) + got)
   got
}
fn seek(any st, int frame) bool {
   if !is_dict(st) || !st.get("file", 0) { return false }
   if frame < 0 { frame = 0 }
   if frame > length(st) { frame = length(st) }
   st.set("cursor", frame)
   true
}
fn tell(any st) int { is_dict(st) ? int(st.get("cursor", 0)) : 0 }
fn length(any st) int { is_dict(st) ? int(st.get("frames", 0)) : 0 }
fn format(any st) any { is_dict(st) ? st.get("format", [0, 0, 0]) : [0, 0, 0] }
fn close(any st) bool {
   if !is_dict(st) || !st.get("file", 0) { return false }
   def rc = fclose(st.get("file"))
   st.set("file", 0)
   rc == 0
}
