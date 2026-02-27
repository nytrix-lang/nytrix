;; Keywords: sound ogg formats
;; Reference:
;; - https://xiph.org/ogg/doc/framing.html

module std.audio.formats.ogg (
   get_info, decode, encode
)

use std.core *
use std.core.dict *
use std.audio.source *
use std.audio.formats.vorbis as vorbis
use std.audio.formats.opus as opus

fn _u32le(s, i){
   "Internal helper for `u32le`."
   load8(s, i) | (load8(s, i + 1) << 8) | (load8(s, i + 2) << 16) | (load8(s, i + 3) << 24)
}

fn _u64le(s, i){
   "Internal helper for `u64le`."
   def lo = _u32le(s, i)
   def hi = _u32le(s, i + 4)
   lo + hi * 4294967296.0
}

fn _is_oggs(data, p){
   "Internal helper for `is_oggs`."
   if(p + 4 > len(data)){ return false }
   load8(data, p) == 79 &&
   load8(data, p + 1) == 103 &&
   load8(data, p + 2) == 103 &&
   load8(data, p + 3) == 83
}

fn _parse_page(data, p){
   "Internal helper for `parse_page`."
   def n = len(data)
   if(p + 27 > n){ return 0 }
   if(!_is_oggs(data, p)){ return 0 }
   def ver = load8(data, p + 4)
   if(ver != 0){ return 0 }
   def htype = load8(data, p + 5)
   def granule = _u64le(data, p + 6)
   def serial = _u32le(data, p + 14)
   def seq = _u32le(data, p + 18)
   def segs = load8(data, p + 26)
   def lacing = p + 27
   def payload = lacing + segs
   if(payload > n){ return 0 }
   mut body = 0
   mut i = 0
   while(i < segs){
      body += load8(data, lacing + i)
      i += 1
   }
   def next = payload + body
   if(next > n){ return 0 }
   mut out = dict(12)
   out = dict_set(out, "start", p)
   out = dict_set(out, "header_type", htype)
   out = dict_set(out, "granule", granule)
   out = dict_set(out, "serial", serial)
   out = dict_set(out, "seq", seq)
   out = dict_set(out, "segs", segs)
   out = dict_set(out, "lacing", lacing)
   out = dict_set(out, "payload", payload)
   out = dict_set(out, "next", next)
   out
}

fn _list_to_bytes(xs){
   "Internal helper for `list_to_bytes`."
   def n = len(xs)
   def out = init_str(malloc(n + 1), n)
   mut i = 0
   while(i < n){
      store8(out, get(xs, i), i)
      i += 1
   }
   out
}

fn _collect_packets(data, max_packets=0){
   "Internal helper for `collect_packets`."
   if(!is_str(data) || len(data) < 27){ return 0 }
   if(!_is_oggs(data, 0)){ return 0 }
   mut off = 0
   mut packets = list(16)
   mut cur = list(256)
   mut saw_page = false
   while(off < len(data)){
      def pg = _parse_page(data, off)
      if(!pg){ return 0 }
      saw_page = true
      def segs = dict_get(pg, "segs")
      def lacing = dict_get(pg, "lacing")
      mut pay = dict_get(pg, "payload")
      mut i = 0
      while(i < segs){
         def seglen = load8(data, lacing + i)
         mut j = 0
         while(j < seglen){
            append(cur, load8(data, pay + j))
            j += 1
         }
         pay += seglen
         if(seglen < 255){
            append(packets, _list_to_bytes(cur))
            cur = list(256)
            if(max_packets > 0 && len(packets) >= max_packets){
               return packets
            }
         }
         i += 1
      }
      off = dict_get(pg, "next")
   }
   if(!saw_page){ return 0 }
   packets
}

fn _codec_from_packet0(p0){
   "Internal helper for `codec_from_packet0`."
   if(!is_str(p0)){ return "unknown" }
   def n = len(p0)
   if(n >= 8 &&
      load8(p0, 0) == 79 && load8(p0, 1) == 112 && load8(p0, 2) == 117 && load8(p0, 3) == 115 &&
      load8(p0, 4) == 72 && load8(p0, 5) == 101 && load8(p0, 6) == 97 && load8(p0, 7) == 100){
      return "opus"
   }
   if(n >= 7 &&
      load8(p0, 0) == 1 &&
      load8(p0, 1) == 118 && load8(p0, 2) == 111 && load8(p0, 3) == 114 &&
      load8(p0, 4) == 98 && load8(p0, 5) == 105 && load8(p0, 6) == 115){
      return "vorbis"
   }
   if(n >= 5 &&
      load8(p0, 0) == 127 &&
      load8(p0, 1) == 102 && load8(p0, 2) == 76 && load8(p0, 3) == 97 && load8(p0, 4) == 67){
      return "flac"
   }
   "unknown"
}

fn _float_list_to_ptr(xs){
   "Internal helper for `float_list_to_ptr`."
   if(!is_list(xs)){ return 0 }
   def n = len(xs)
   if(n <= 0){ return 0 }
   def ptr = malloc(n * 4)
   if(!ptr){ return 0 }
   mut i = 0
   while(i < n){
      store32_f32(ptr, get(xs, i, 0.0) + 0.0, i * 4)
      i += 1
   }
   ptr
}

fn _as_source(decoded){
   "Internal helper for `as_source`."
   if(!decoded){ return 0 }
   if(is_list(decoded) && len(decoded) > 1 && eq(get(decoded, 0), "SOUND_SOURCE")){
      return decoded
   }
   if(!is_dict(decoded)){ return 0 }
   def pcm = dict_get(decoded, "data")
   def ch = dict_get(decoded, "channels", 0)
   def rate = dict_get(decoded, "sample_rate", dict_get(decoded, "rate", 0))
   mut bits = dict_get(decoded, "bits", 32)
   mut sf = dict_get(decoded, "sample_fmt", 0)
   mut fmt = dict_get(decoded, "format", 3)
   if(sf == 0 && bits == 32){
      sf = SAMPLE_FMT_F32
      fmt = 3
   }
   if(ch <= 0 || rate <= 0){ return 0 }
   if(is_int(pcm) && pcm != 0){
      def n = dict_get(decoded, "byte_len", dict_get(decoded, "len", 0))
      if(n <= 0){ return 0 }
      return make_memory_source(pcm, n, ch, rate, bits, sf, fmt)
   }
   if(is_str(pcm)){
      def n = len(pcm)
      if(n <= 0){ return 0 }
      def ptr = malloc(n)
      if(!ptr){ return 0 }
      memcpy(ptr, pcm, n)
      return make_memory_source(ptr, n, ch, rate, bits, sf, fmt)
   }
   if(is_list(pcm)){
      def ptr = _float_list_to_ptr(pcm)
      if(!ptr){ return 0 }
      return make_memory_source(ptr, len(pcm) * 4, ch, rate, 32, SAMPLE_FMT_F32, 3)
   }
   0
}

fn _ptr_to_blob(ptr, n){
   "Internal helper for `ptr_to_blob`."
   if(!ptr || n <= 0){ return "" }
   def out = init_str(malloc(n + 1), n)
   memcpy(out, ptr, n)
   out
}

fn get_info(buf){
   "Parses Ogg pages and returns container/codec metadata."
   if(!is_str(buf) || len(buf) < 27){ return 0 }
   if(!_is_oggs(buf, 0)){ return 0 }
   mut off = 0
   mut pages = 0
   mut serial0 = -1
   mut serial_changes = 0
   mut gran_last = 0.0
   while(off < len(buf)){
      def pg = _parse_page(buf, off)
      if(!pg){ return 0 }
      def s = dict_get(pg, "serial")
      if(serial0 < 0){ serial0 = s }
      elif(s != serial0){ serial_changes += 1 }
      gran_last = dict_get(pg, "granule", gran_last)
      pages += 1
      off = dict_get(pg, "next")
   }
   if(pages <= 0){ return 0 }
   def packets = _collect_packets(buf, 2)
   if(!packets || len(packets) <= 0){ return 0 }
   def codec = _codec_from_packet0(get(packets, 0))
   mut info = dict(16)
   info = dict_set(info, "container", "ogg")
   info = dict_set(info, "codec", codec)
   info = dict_set(info, "pages", pages)
   info = dict_set(info, "serial", serial0)
   info = dict_set(info, "granule_last", gran_last)
   info = dict_set(info, "chained", serial_changes > 0)
   if(codec == "vorbis"){
      def vi = vorbis.get_info(buf)
      if(vi){
         info = dict_set(info, "channels", dict_get(vi, "channels", 0))
         info = dict_set(info, "sample_rate", dict_get(vi, "sample_rate", dict_get(vi, "rate", 0)))
         info = dict_set(info, "bitrate_nominal", dict_get(vi, "bitrate_nominal", 0))
      }
   } elif(codec == "opus"){
      def oi = opus.get_info(buf)
      if(oi){
         info = dict_set(info, "channels", dict_get(oi, "channels", 0))
         info = dict_set(info, "sample_rate", dict_get(oi, "sample_rate", 48000))
         info = dict_set(info, "pre_skip", dict_get(oi, "pre_skip", 0))
         info = dict_set(info, "duration", dict_get(oi, "duration", 0.0))
         info = dict_set(info, "total_samples", dict_get(oi, "total_samples", 0))
      }
   }
   info
}

fn decode(buf){
   "Decodes Ogg Vorbis/Opus data into a memory sound source."
   def info = get_info(buf)
   if(!info){ return 0 }
   def codec = dict_get(info, "codec", "unknown")
   if(codec == "vorbis"){
      return _as_source(vorbis.decode(buf))
   }
   if(codec == "opus"){
      return _as_source(opus.decode(buf))
   }
   0
}

fn encode(pcm_ptr, byte_len, channels, rate, bits){
   "Encodes PCM into Ogg bytes. Default codec is Vorbis; set NY_OGG_CODEC=opus for Opus."
   mut codec = env("NY_OGG_CODEC")
   if(!is_str(codec) || len(codec) == 0){ codec = "vorbis" }
   mut pcm = pcm_ptr
   if(is_int(pcm_ptr) && pcm_ptr != 0 && byte_len > 0){
      pcm = _ptr_to_blob(pcm_ptr, byte_len)
   }
   if(codec == "opus"){
      def o = opus.encode(pcm, channels, rate, bits)
      if(o){ return o }
   }
   vorbis.encode(pcm, channels, rate, bits)
}

if(comptime{__main()}){
   use std.core.error *

   def n = 47
   def b = init_str(malloc(n + 1), n)
   memset(b, 0, n)
   store8(b, 79, 0) store8(b, 103, 1) store8(b, 103, 2) store8(b, 83, 3)
   store8(b, 0, 4)
   store8(b, 2, 5)
   store32(b, 1, 14)
   store8(b, 1, 26)
   store8(b, 19, 27)
   def p = 28
   store8(b, 79, p + 0) store8(b, 112, p + 1) store8(b, 117, p + 2) store8(b, 115, p + 3)
   store8(b, 72, p + 4) store8(b, 101, p + 5) store8(b, 97, p + 6) store8(b, 100, p + 7)
   store8(b, 1, p + 8)
   store8(b, 2, p + 9)
   store16(b, 312, p + 10)
   store32(b, 48000, p + 12)
   store16(b, 0, p + 16)
   store8(b, 0, p + 18)

   def info = get_info(b)
   assert(info != 0, "ogg info parse")
   assert(dict_get(info, "codec", "") == "opus", "ogg codec detect")
   assert(dict_get(info, "channels", 0) == 2, "ogg/opus channels")
   print("✓ std.audio.formats.ogg tests passed")
}
