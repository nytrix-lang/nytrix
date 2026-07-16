;; Keywords: sound source os
;; Sound source facade for memory-backed and synthesized audio.
;; References:
;; - std.os.sound
;; - std.os
module std.os.sound.source(make_memory_source, read, seek, tell, length, format, sample_format, source_channels, source_rate, source_bits, source_length, wav_bytes, write_wav, SAMPLE_FMT_S16, SAMPLE_FMT_U8, SAMPLE_FMT_S24, SAMPLE_FMT_S32, SAMPLE_FMT_F32)
use std.core
use std.core.dict_mod
use std.os (file_write)
use std.os.sound.source.memory as memory

def SAMPLE_FMT_S16 = 1
def SAMPLE_FMT_U8 = 2
def SAMPLE_FMT_S24 = 3
def SAMPLE_FMT_S32 = 4
def SAMPLE_FMT_F32 = 5

fn make_memory_source(any data_ptr, any byte_len, any channels, any rate, any bits, any sample_fmt=0, any format_tag=1) list {
   "Implements `make_memory_source`."
   memory.make(data_ptr, byte_len, channels, rate, bits, sample_fmt, format_tag)
}

fn read(any src, any mix_buf, any frames) int {
   "Reads up to 'frames' from the source into 'mix_buf'."
   if !is_list(src) || src.len < 3 { return 0 }
   mut data = src.get(1)
   def cursor = __flt_to_int(get_item(data, "cursor", 0.0) + 0.0)
   def total = data.get("total_frames")
   def frame_size = data.get("frame_size")
   def ptr = data.get("ptr")
   mut to_read = int(frames)
   if cursor + to_read > total { to_read = total - cursor }
   if to_read <= 0 { return 0 }
   memcpy(mix_buf, ptr + (cursor * frame_size), to_read * frame_size)
   data = data.set("cursor", cursor + to_read)
   src.set(1, data)
   to_read
}

fn seek(any src, any frame) bool {
   "Implements `seek`."
   if !is_list(src) { return false }
   mut data = src.get(1)
   def total = data.get("total_frames")
   mut f = frame
   if f < 0 { f = 0 }
   if f > total { f = total }
   data = data.set("cursor", f)
   src.set(1, data)
   true
}

fn tell(any src) int {
   "Implements `tell`."
   if !is_list(src) { return 0 }
   __flt_to_int(get_item(src.get(1), "cursor", 0.0) + 0.0)
}

fn length(any src) any {
   "Implements `length`."
   _source_meta(src, "total_frames")
}

fn _source_meta(any src, str key) any {
   if !is_list(src) { return 0 }
   def data = src.get(1, 0)
   if !is_dict(data) { return 0 }
   data.get(key, 0)
}

fn format(any src) list {
   "Returns list of channels, rate, bits"
   if !is_list(src) { return [0, 0, 0] }
   def d = src.get(1)
   [d.get("channels"), d.get("rate"), d.get("bits")]
}

fn source_channels(any src) any {
   "Returns the channel count for `src`."
   _source_meta(src, "channels")
}

fn source_rate(any src) any {
   "Returns the sample rate for `src`."
   _source_meta(src, "rate")
}

fn source_bits(any src) any {
   "Returns the sample bit depth for `src`."
   _source_meta(src, "bits")
}

fn source_length(any src) any {
   "Returns the total frame count for `src`."
   length(src)
}

fn sample_format(any src) int {
   "Returns source sample format enum."
   if !is_list(src) { return 0 }
   def d = src.get(1)
   def sf = d.get("sample_fmt", 0)
   if sf != 0 { return sf }
   def bits = d.get("bits", 16)
   def tag = d.get("format_tag", 1)
   case int(bits){
      8 -> SAMPLE_FMT_U8
      16 -> SAMPLE_FMT_S16
      24 -> SAMPLE_FMT_S24
      32 -> (int(tag) == 3 ? SAMPLE_FMT_F32 : SAMPLE_FMT_S32)
      _ -> SAMPLE_FMT_S16
   }
}

fn _put_ascii(any buf, int off, str text) int {
   mut i = 0
   while i < text.len {
      store8(buf, load8(text, i), off + i)
      i += 1
   }
   0
}

fn wav_bytes(any src) str {
   "Serializes a memory-backed sound source as a WAV byte string."
   if !is_list(src) { return "" }
   def data = src.get(1, 0)
   if !is_dict(data) { return "" }
   def pcm_ptr = data.get("ptr", 0)
   def pcm_len = int(data.get("len", 0))
   def channels = int(data.get("channels", 1))
   def rate = int(data.get("rate", 48000))
   def bits = int(data.get("bits", 16))
   def format_tag = int(data.get("format_tag", 1))
   if !pcm_ptr || pcm_len <= 0 || channels <= 0 || rate <= 0 || bits <= 0 { return "" }
   def bytes_per_frame = int((channels * bits) / 8)
   if bytes_per_frame <= 0 { return "" }
   def total = 44 + pcm_len
   def out = malloc(total + 1)
   if !out { return "" }
   _put_ascii(out, 0, "RIFF")
   store32(out, 36 + pcm_len, 4)
   _put_ascii(out, 8, "WAVE")
   _put_ascii(out, 12, "fmt ")
   store32(out, 16, 16)
   store16(out, format_tag, 20)
   store16(out, channels, 22)
   store32(out, rate, 24)
   store32(out, rate * bytes_per_frame, 28)
   store16(out, bytes_per_frame, 32)
   store16(out, bits, 34)
   _put_ascii(out, 36, "data")
   store32(out, pcm_len, 40)
   memcpy(out + 44, pcm_ptr, pcm_len)
   store8(out, 0, total)
   init_str(out, total)
}

fn write_wav(any src, any file) bool {
   "Writes a memory-backed sound source to `file` as WAV."
   def wav = wav_bytes(src)
   if wav.len == 0 { return false }
   match file_write(file, wav) {
      ok(_) -> true
      err(_) -> false
   }
}

fn get_item(dict d, any key, any default) any {
   "Returns `d[key]` or `default` when the dictionary lookup yields 0."
   def v = d.get(key)
   if v == 0 { return default }
   v
}

fn _sound_source_selftest_data(int n) ptr {
   def p = malloc(n)
   mut i = 0
   while i < n {
      store8(p, (i * 7) % 251, i)
      i += 1
   }
   p
}

fn _sound_source_selftest_format(any src, int channels, int rate, int bits) bool {
   def fmt = format(src)
   fmt.get(0) == channels && fmt.get(1) == rate && fmt.get(2) == bits
}

fn _sound_source_selftest_sample_fallback(any data, int bits, int tag, int expected) bool {
   mut src = make_memory_source(data, 64, 1, 44100, bits, 0, tag)
   mut meta = src.get(1)
   meta = meta.set("sample_fmt", 0)
   src.set(1, meta)
   sample_format(src) == expected
}

fn _sound_source_selftest_wav(any src) bool {
   def wav = wav_bytes(src)
   wav.len == 108 &&
   memcmp(wav, "RIFF", 4) == 0 &&
   memcmp(wav + 8, "WAVE", 4) == 0 &&
   load32(wav, 40) == 64
}

#main {
   def data = _sound_source_selftest_data(64)
   def buf = malloc(32)
   def src = make_memory_source(data, 64, 2, 48000, 16)
   assert(is_list(src) && src.get(0) == "SOUND_SOURCE", "sound source tag")
   assert(_sound_source_selftest_format(src, 2, 48000, 16), "sound source format")
   assert(source_channels(src) == 2 && source_rate(src) == 48000 && source_bits(src) == 16, "sound source format helpers")
   assert(source_length(src) == 16 && length(src) == 16 && sample_format(src) == SAMPLE_FMT_S16, "sound source length and sample format")
   assert(read(src, buf, 3) == 3 && tell(src) == 3 && memcmp(data, buf, 12) == 0, "sound source first read")
   assert(seek(src, 14) && read(src, buf, 8) == 2 && tell(src) == 16 && memcmp(data + 56, buf, 8) == 0, "sound source partial read")
   assert(seek(src, -5) && tell(src) == 0 && seek(src, 999) && tell(src) == 16, "sound source seek clamps")
   assert(_sound_source_selftest_sample_fallback(data, 8, 1, SAMPLE_FMT_U8), "sound source u8 fallback")
   assert(_sound_source_selftest_sample_fallback(data, 16, 1, SAMPLE_FMT_S16), "sound source s16 fallback")
   assert(_sound_source_selftest_sample_fallback(data, 24, 1, SAMPLE_FMT_S24), "sound source s24 fallback")
   assert(_sound_source_selftest_sample_fallback(data, 32, 1, SAMPLE_FMT_S32), "sound source s32 fallback")
   assert(_sound_source_selftest_sample_fallback(data, 32, 3, SAMPLE_FMT_F32), "sound source f32 fallback")
   assert(_sound_source_selftest_sample_fallback(data, 12, 1, SAMPLE_FMT_S16), "sound source unknown fallback")
   assert(_sound_source_selftest_wav(src), "sound source wav serialization")
   assert(wav_bytes(0) == "" && !write_wav(0, ""), "sound source wav invalid input")
   assert(format(0) == [0, 0, 0] && length(0) == 0 && tell(0) == 0 && !seek(0, 0) && read(0, buf, 4) == 0 && sample_format(0) == 0, "sound source invalid input")
   free(buf, data)
   print("✓ std.os.sound.source self-test passed")
}
