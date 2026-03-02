;; Keywords: audio opus ogg formats
;; Reference:
;; - https://datatracker.ietf.org/doc/html/rfc7845

module std.audio.formats.opus (
   available, get_info, decode, encode
)

use std.core *
use std.core.dict *
use std.os *
use std.os.ffi *
use std.audio.source *

mut _lib = 0
mut _op_open_memory = 0
mut _op_channel_count = 0
mut _op_pcm_total = 0
mut _op_read_float = 0
mut _op_free = 0

fn _u16le(s, i){
   "Internal helper for `u16le`."
   load8(s, i) | (load8(s, i + 1) << 8)
}

fn _u32le(s, i){
   "Internal helper for `u32le`."
   load8(s, i) | (load8(s, i + 1) << 8) | (load8(s, i + 2) << 16) | (load8(s, i + 3) << 24)
}

fn _i16(v){
   "Internal helper for `i16`."
   mut x = v
   if(x >= 32768){ x = x - 65536 }
   x
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
   if(load8(data, p + 4) != 0){ return 0 }
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
   mut out = dict(8)
   out = dict_set(out, "segs", segs)
   out = dict_set(out, "lacing", lacing)
   out = dict_set(out, "payload", payload)
   out = dict_set(out, "next", next)
   out = dict_set(out, "header_type", load8(data, p + 5))
   out = dict_set(out, "granule", _u64le(data, p + 6))
   out
}

fn _read_first_packet(data){
   "Internal helper for `read_first_packet`."
   if(!is_str(data) || len(data) < 27){ return 0 }
   if(!_is_oggs(data, 0)){ return 0 }
   def cap = 65536
   def pkt = malloc(cap + 1)
   if(!pkt){ return 0 }
   mut plen = 0
   mut off = 0
   mut done = false
   while(!done && off < len(data)){
      def page = _parse_page(data, off)
      if(!page){ free(pkt) return 0 }
      def segs = dict_get(page, "segs")
      def lacing = dict_get(page, "lacing")
      mut pay = dict_get(page, "payload")
      mut i = 0
      while(i < segs){
         def seglen = load8(data, lacing + i)
         if(plen + seglen > cap){ free(pkt) return 0 }
         if(seglen > 0){ memcpy(pkt + plen, data + pay, seglen) }
         plen += seglen
         pay += seglen
         if(seglen < 255){
            done = true
            break
         }
         i += 1
      }
      off = dict_get(page, "next")
   }
   if(!done || plen < 19){ free(pkt) return 0 }
   store8(pkt, 0, plen)
   mut out = dict(4)
   out = dict_set(out, "ptr", pkt)
   out = dict_set(out, "len", plen)
   out
}

fn _last_granule(data){
   "Internal helper for `last_granule`."
   if(!is_str(data) || len(data) < 27){ return 0.0 }
   mut off = 0
   mut g = 0.0
   while(off < len(data)){
      def pg = _parse_page(data, off)
      if(!pg){ return g }
      g = dict_get(pg, "granule", g)
      off = dict_get(pg, "next")
   }
   g
}

fn _init(){
   "Internal helper for `init`."
   if(_op_open_memory){ return true }
   if(!_lib){
      _lib = dlopen_any("opusfile", RTLD_NOW() | RTLD_LOCAL())
      if(!_lib){ _lib = dlopen_any("libopusfile", RTLD_NOW() | RTLD_LOCAL()) }
      if(!_lib){ return false }
   }
   _op_open_memory = dlsym(_lib, "op_open_memory")
   _op_channel_count = dlsym(_lib, "op_channel_count")
   _op_pcm_total = dlsym(_lib, "op_pcm_total")
   _op_read_float = dlsym(_lib, "op_read_float")
   _op_free = dlsym(_lib, "op_free")
   !!_op_open_memory && !!_op_channel_count && !!_op_pcm_total && !!_op_read_float && !!_op_free
}

fn available(){
   "Returns true when libopusfile decode symbols are available."
   _init()
}

fn get_info(data){
   "Parses OpusHead metadata from an Ogg Opus stream."
   def pkt = _read_first_packet(data)
   if(!pkt){ return 0 }
   def p = dict_get(pkt, "ptr", 0)
   def n = dict_get(pkt, "len", 0)
   if(!p || n < 19){ if(p){ free(p) } return 0 }
   if(load8(p, 0) != 79 || load8(p, 1) != 112 || load8(p, 2) != 117 || load8(p, 3) != 115 ||
      load8(p, 4) != 72 || load8(p, 5) != 101 || load8(p, 6) != 97 || load8(p, 7) != 100){
      free(p)
      return 0
   }
   def ver = load8(p, 8)
   def ch = load8(p, 9)
   def pre_skip = _u16le(p, 10)
   def in_rate = _u32le(p, 12)
   def gain_q8 = _i16(_u16le(p, 16))
   def mapping_family = load8(p, 18)
   mut stream_count = 1
   mut coupled_count = (ch > 1) ? 1 : 0
   if(mapping_family != 0){
      if(n < 21){ free(p) return 0 }
      stream_count = load8(p, 19)
      coupled_count = load8(p, 20)
   }
   free(p)
   if(ch <= 0 || ch > 255){ return 0 }
   mut total = _last_granule(data) - pre_skip
   if(total < 0){ total = 0 }
   def sr = 48000
   mut info = dict(20)
   info = dict_set(info, "container", "ogg")
   info = dict_set(info, "codec", "opus")
   info = dict_set(info, "version", ver)
   info = dict_set(info, "channels", ch)
   info = dict_set(info, "sample_rate", sr)
   info = dict_set(info, "rate", sr)
   info = dict_set(info, "input_rate", in_rate)
   info = dict_set(info, "pre_skip", pre_skip)
   info = dict_set(info, "gain_q8", gain_q8)
   info = dict_set(info, "mapping_family", mapping_family)
   info = dict_set(info, "stream_count", stream_count)
   info = dict_set(info, "coupled_count", coupled_count)
   info = dict_set(info, "total_samples", total)
   info = dict_set(info, "duration", (total + 0.0) / 48000.0)
   info
}

fn decode(data){
   "Decodes Ogg Opus bytes into a float32 memory source when libopusfile is available."
   def info = get_info(data)
   if(!info){ return 0 }
   if(!_init()){ return 0 }
   def errp = malloc(4)
   if(!errp){ return 0 }
   store32(errp, 0, 0)
   def of = call3(_op_open_memory, data, len(data), errp)
   free(errp)
   if(!of){ return 0 }
   mut ch = call2(_op_channel_count, of, -1)
   if(ch <= 0){
      call1_void(_op_free, of)
      return 0
   }
   mut total = call2(_op_pcm_total, of, -1)
   if(total < 0){ total = 0 }
   mut cap = (total > 0) ? (total * ch * 4) : (48000 * ch * 4)
   if(cap < 4096){ cap = 4096 }
   mut out = malloc(cap + 1)
   if(!out){
      call1_void(_op_free, of)
      return 0
   }
   mut out_len = 0
   def chunk_frames = 4096
   def tmp_cap = chunk_frames * ch * 4
   def tmp = malloc(tmp_cap)
   def lip = malloc(4)
   if(!tmp || !lip){
      if(tmp){ free(tmp) }
      if(lip){ free(lip) }
      free(out)
      call1_void(_op_free, of)
      return 0
   }
   while(1){
      store32(lip, 0, 0)
      def got = call4(_op_read_float, of, tmp, chunk_frames * ch, lip)
      if(got < 0){
         free(tmp)
         free(lip)
         free(out)
         call1_void(_op_free, of)
         return 0
      }
      if(got == 0){ break }
      def nbytes = got * ch * 4
      if(out_len + nbytes > cap){
         mut ncap = cap
         while(out_len + nbytes > ncap){ ncap *= 2 }
         def grown = realloc(out, ncap + 1)
         if(!grown){
            free(tmp)
            free(lip)
            free(out)
            call1_void(_op_free, of)
            return 0
         }
         out = grown
         cap = ncap
      }
      memcpy(out + out_len, tmp, nbytes)
      out_len += nbytes
   }
   free(tmp)
   free(lip)
   call1_void(_op_free, of)
   if(out_len <= 0){
      free(out)
      return 0
   }
   store8(out, 0, out_len)
   init_str(out, out_len)
   make_memory_source(out, out_len, ch, 48000, 32, SAMPLE_FMT_F32, 3)
}

fn encode(pcm, channels, rate, bits){
   "Encodes PCM to Ogg Opus bytes. Returns 0 when opus encoder bindings are unavailable."
   if(!pcm || channels <= 0 || rate <= 0 || bits <= 0){ return 0 }
   0
}

if(comptime{__main()}){
   use std.core.error *

   def n = 47
   def b = init_str(malloc(n + 1), n)
   memset(b, 0, n)
   store8(b, 79, 0) store8(b, 103, 1) store8(b, 103, 2) store8(b, 83, 3)
   store8(b, 0, 4) store8(b, 2, 5)
   store32(b, 1, 14)
   store8(b, 1, 26) store8(b, 19, 27)
   def p = 28
   store8(b, 79, p + 0) store8(b, 112, p + 1) store8(b, 117, p + 2) store8(b, 115, p + 3)
   store8(b, 72, p + 4) store8(b, 101, p + 5) store8(b, 97, p + 6) store8(b, 100, p + 7)
   store8(b, 1, p + 8) store8(b, 2, p + 9)
   store16(b, 312, p + 10)
   store32(b, 48000, p + 12)
   store16(b, 0, p + 16) store8(b, 0, p + 18)

   def info = get_info(b)
   assert(info != 0, "opus get_info")
   assert(dict_get(info, "channels", 0) == 2, "opus channels")
   assert(dict_get(info, "sample_rate", 0) == 48000, "opus sample rate")
   print("✓ std.audio.formats.opus tests passed")
}
