;; Keywords: audio aac adts formats
;; Reference:
;; - https://en.wikipedia.org/wiki/Advanced_Audio_Coding
;; - https://wiki.multimedia.cx/index.php/ADTS

module std.audio.formats.acc (
   get_info, decode, encode
)

use std.core *
use std.core.dict *

def _SR = [96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350]

fn _sync(data, off){
   "Internal helper for `sync`."
   if(off + 1 >= len(data)){ return false }
   load8(data, off) == 0xFF && (load8(data, off + 1) & 0xF6) == 0xF0
}

fn _synchsafe32(data, off){
   "Internal helper for `synchsafe32`."
   ((load8(data, off) & 127) << 21) |
   ((load8(data, off + 1) & 127) << 14) |
   ((load8(data, off + 2) & 127) << 7) |
   (load8(data, off + 3) & 127)
}

fn _skip_id3(data){
   "Internal helper for `skip_id3`."
   if(len(data) < 10){ return 0 }
   if(load8(data, 0) != 73 || load8(data, 1) != 68 || load8(data, 2) != 51){ return 0 }
   def flags = load8(data, 5)
   mut n = 10 + _synchsafe32(data, 6)
   if((flags & 0x10) != 0){ n += 10 }
   if(n < 0){ n = 0 }
   if(n > len(data)){ n = len(data) }
   n
}

fn _profile_name(p){
   "Internal helper for `profile_name`."
   if(p == 1){ return "main" }
   if(p == 2){ return "lc" }
   if(p == 3){ return "ssr" }
   if(p == 4){ return "ltp" }
   "unknown"
}

fn _find_sync(data, start){
   "Internal helper for `find_sync`."
   mut i = start
   while(i + 1 < len(data)){
      if(_sync(data, i)){ return i }
      i += 1
   }
   -1
}

fn _parse_adts_frame(data, off){
   "Internal helper for `parse_adts_frame`."
   if(off + 7 > len(data)){ return 0 }
   if(!_sync(data, off)){ return 0 }
   def b1 = load8(data, off + 1)
   def b2 = load8(data, off + 2)
   def b3 = load8(data, off + 3)
   def b4 = load8(data, off + 4)
   def b5 = load8(data, off + 5)
   def b6 = load8(data, off + 6)
   def protection_absent = b1 & 1
   def profile = ((b2 >> 6) & 3) + 1
   def sr_idx = (b2 >> 2) & 15
   def ch_cfg = ((b2 & 1) << 2) | ((b3 >> 6) & 3)
   def frame_len = ((b3 & 3) << 11) | (b4 << 3) | ((b5 >> 5) & 7)
   def fullness = ((b5 & 31) << 6) | ((b6 >> 2) & 63)
   def raw_blocks = b6 & 3
   def hdr_len = (protection_absent != 0) ? 7 : 9
   if(sr_idx >= len(_SR)){ return 0 }
   if(ch_cfg == 0){ return 0 }
   if(frame_len < hdr_len){ return 0 }
   if(off + frame_len > len(data)){ return 0 }
   mut f = dict(14)
   f = dict_set(f, "offset", off)
   f = dict_set(f, "next", off + frame_len)
   f = dict_set(f, "header_len", hdr_len)
   f = dict_set(f, "frame_len", frame_len)
   f = dict_set(f, "profile", profile)
   f = dict_set(f, "sample_rate_index", sr_idx)
   f = dict_set(f, "sample_rate", get(_SR, sr_idx))
   f = dict_set(f, "channels", ch_cfg)
   f = dict_set(f, "fullness", fullness)
   f = dict_set(f, "raw_blocks", raw_blocks)
   f = dict_set(f, "protection_absent", protection_absent)
   f
}

fn _parse_adts(data, start){
   "Internal helper for `parse_adts`."
   def first_off = _find_sync(data, start)
   if(first_off < 0){ return 0 }
   mut off = first_off
   mut frames = 0
   mut total_samples = 0
   mut total_bytes = 0
   mut profile = 0
   mut sr = 0
   mut ch = 0
   while(off + 7 <= len(data)){
      def f = _parse_adts_frame(data, off)
      if(!f){ break }
      if(frames == 0){
         profile = dict_get(f, "profile")
         sr = dict_get(f, "sample_rate")
         ch = dict_get(f, "channels")
      }
      def flen = dict_get(f, "frame_len")
      total_bytes += flen
      total_samples += (dict_get(f, "raw_blocks") + 1) * 1024
      frames += 1
      off = dict_get(f, "next")
      if(off >= len(data)){ break }
      if(!_sync(data, off)){
         def next_off = _find_sync(data, off + 1)
         if(next_off < 0){ break }
         if(next_off - off > 4096){ break }
         off = next_off
      }
   }
   if(frames <= 0 || sr <= 0 || ch <= 0){ return 0 }
   def dur = (total_samples + 0.0) / (sr + 0.0)
   def br = (total_samples > 0) ? ((total_bytes * 8 * sr) / total_samples) : 0
   mut info = dict(18)
   info = dict_set(info, "container", "aac")
   info = dict_set(info, "codec", "aac")
   info = dict_set(info, "transport", "adts")
   info = dict_set(info, "profile", profile)
   info = dict_set(info, "profile_name", _profile_name(profile))
   info = dict_set(info, "channels", ch)
   info = dict_set(info, "sample_rate", sr)
   info = dict_set(info, "rate", sr)
   info = dict_set(info, "frames", frames)
   info = dict_set(info, "total_samples", total_samples)
   info = dict_set(info, "duration", dur)
   info = dict_set(info, "bitrate", br)
   info = dict_set(info, "payload_offset", first_off)
   info
}

fn get_info(data){
   "Parses AAC metadata from ADTS/ADIF transport streams."
   if(!is_str(data) || len(data) < 4){ return 0 }
   if(load8(data, 0) == 65 && load8(data, 1) == 68 && load8(data, 2) == 73 && load8(data, 3) == 70){
      mut info = dict(8)
      info = dict_set(info, "container", "aac")
      info = dict_set(info, "codec", "aac")
      info = dict_set(info, "transport", "adif")
      info = dict_set(info, "channels", 0)
      info = dict_set(info, "sample_rate", 0)
      return info
   }
   def start = _skip_id3(data)
   _parse_adts(data, start)
}

fn decode(data){
   "Decodes AAC bytes into a memory source when a backend is available."
   if(!get_info(data)){ return 0 }
   0
}

fn encode(pcm, channels, rate, bits){
   "Encodes PCM bytes to AAC transport stream when an encoder backend is available."
   if(!pcm || channels <= 0 || rate <= 0 || bits <= 0){ return 0 }
   0
}

if(comptime{__main()}){
   use std.core.error *

   def n = 11
   def b = init_str(malloc(n + 1), n)
   memset(b, 0, n)
   store8(b, 0xFF, 0)
   store8(b, 0xF1, 1)
   store8(b, 0x50, 2)
   store8(b, 0x80, 3)
   store8(b, 0x01, 4)
   store8(b, 0x7F, 5)
   store8(b, 0xFC, 6)

   def info = get_info(b)
   assert(info != 0, "aac get_info")
   assert(dict_get(info, "transport", "") == "adts", "aac transport")
   assert(dict_get(info, "channels", 0) == 2, "aac channels")
   assert(dict_get(info, "sample_rate", 0) == 44100, "aac rate")
   assert(dict_get(info, "frames", 0) == 1, "aac frames")
   print("✓ std.audio.formats.acc tests passed")
}
