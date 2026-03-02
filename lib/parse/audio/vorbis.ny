;; Keywords: audio vorbis ogg formats
;; Reference:
;; - https://xiph.org/vorbis/
;; - https://github.com/xiph/vorbis
;; - https://xiph.org/ogg/doc/libogg/reference.html

module std.audio.formats.vorbis (
   decode, encode, get_info
)

use std.core *
use std.core.dict *
use std.os *
use std.os.ffi *
use std.os.dirs *
use std.os.path as path
use std.os.time *
use std.audio.source *

mut _tmp_seq = 0

mut _ptr_bytes = 0
mut _long_bytes = 0
mut _og_page_off_header = 0
mut _og_page_off_header_len = 0
mut _og_page_off_body = 0
mut _og_page_off_body_len = 0

def _OV_FILE_SIZE = 65536
def _VORBIS_INFO_SIZE = 1024
def _VORBIS_COMMENT_SIZE = 2048
def _VORBIS_DSP_SIZE = 16384
def _VORBIS_BLOCK_SIZE = 4096
def _OGG_STREAM_SIZE = 4096
def _OGG_PACKET_SIZE = 512
def _OGG_PAGE_SIZE = 128

def _OGG_FLAG_PHYSICAL_INPUT = 1

def _SAMPLE_FMT_S16 = 1
def _SAMPLE_FMT_U8 = 2
def _SAMPLE_FMT_S24 = 3
def _SAMPLE_FMT_S32 = 4
def _SAMPLE_FMT_F32 = 5

mut _lib_vorbisfile = 0
mut _lib_vorbis = 0
mut _lib_vorbisenc = 0
mut _lib_ogg = 0

mut _ov_fopen = 0
mut _ov_read_float = 0
mut _ov_clear = 0

mut _vorbis_info_init = 0
mut _vorbis_info_clear = 0
mut _vorbis_encode_init = 0
mut _vorbis_comment_init = 0
mut _vorbis_comment_add_tag = 0
mut _vorbis_comment_clear = 0
mut _vorbis_analysis_init = 0
mut _vorbis_block_init = 0
mut _vorbis_block_clear = 0
mut _vorbis_dsp_clear = 0
mut _vorbis_analysis_headerout = 0
mut _vorbis_analysis_buffer = 0
mut _vorbis_analysis_wrote = 0
mut _vorbis_analysis_blockout = 0
mut _vorbis_analysis = 0
mut _vorbis_bitrate_addblock = 0
mut _vorbis_bitrate_flushpacket = 0

mut _ogg_stream_init = 0
mut _ogg_stream_clear = 0
mut _ogg_stream_packetin = 0
mut _ogg_stream_flush = 0
mut _ogg_stream_pageout = 0
mut _ogg_page_eos = 0

fn _touch(...args){
   "Consumes arguments intentionally."
   len(args)
}

fn _dbg(...args){
   "Prints debug traces when NY_AUDIO_DEBUG is set."
   if(env("NY_AUDIO_DEBUG")){ print("vorbis:", args) }
}

fn _tmp_path(prefix, ext){
   "Returns a unique temporary path."
   _tmp_seq += 1
   mut base = temp_dir()
   if(!is_str(base) || len(base) == 0){ base = "." }
   path.normalize(base + sep() + prefix + "_" + to_str(pid()) + "_" + to_str(ticks()) + "_" + to_str(_tmp_seq) + ext)
}

fn _cleanup(path_s){
   "Removes a temporary file path if it exists."
   if(!is_str(path_s) || len(path_s) == 0){ return }
   match file_remove(path_s){ _ -> {} }
}

fn _sizes_init(){
   "Initializes pointer/long size and ogg_page field offsets."
   if(_ptr_bytes != 0){ return }
   if(IS_X86_64 || IS_AARCH64){ _ptr_bytes = 8 }
   else { _ptr_bytes = 4 }
   if(IS_WINDOWS){ _long_bytes = 4 }
   else { _long_bytes = _ptr_bytes }

   _og_page_off_header = 0
   _og_page_off_header_len = _ptr_bytes
   mut after_hlen = _og_page_off_header_len + _long_bytes
   if(_ptr_bytes == 8 && (after_hlen % 8) != 0){
      after_hlen += 8 - (after_hlen % 8)
   }
   _og_page_off_body = after_hlen
   _og_page_off_body_len = _og_page_off_body + _ptr_bytes
}

fn _ptr_load(p, off=0){
   "Loads a native pointer-sized field from memory."
   _sizes_init()
   if(_ptr_bytes == 8){ return load64(p, off) }
   load32(p, off)
}

fn _long_load(p, off=0){
   "Loads a native long-sized field from memory."
   _sizes_init()
   if(_long_bytes == 8){ return from_int(load64(p, off)) }
   from_int(load32(p, off))
}

fn _close_libs(){
   "Closes loaded Vorbis/Ogg libraries and clears symbol state."
   if(_lib_vorbisfile != 0){ dlclose(_lib_vorbisfile) }
   if(_lib_vorbisenc != 0 && _lib_vorbisenc != _lib_vorbisfile){ dlclose(_lib_vorbisenc) }
   if(_lib_vorbis != 0 && _lib_vorbis != _lib_vorbisfile && _lib_vorbis != _lib_vorbisenc){ dlclose(_lib_vorbis) }
   if(_lib_ogg != 0 && _lib_ogg != _lib_vorbisfile && _lib_ogg != _lib_vorbisenc && _lib_ogg != _lib_vorbis){ dlclose(_lib_ogg) }

   _lib_vorbisfile = 0
   _lib_vorbis = 0
   _lib_vorbisenc = 0
   _lib_ogg = 0

   _ov_fopen = 0
   _ov_read_float = 0
   _ov_clear = 0

   _vorbis_info_init = 0
   _vorbis_info_clear = 0
   _vorbis_encode_init = 0
   _vorbis_comment_init = 0
   _vorbis_comment_add_tag = 0
   _vorbis_comment_clear = 0
   _vorbis_analysis_init = 0
   _vorbis_block_init = 0
   _vorbis_block_clear = 0
   _vorbis_dsp_clear = 0
   _vorbis_analysis_headerout = 0
   _vorbis_analysis_buffer = 0
   _vorbis_analysis_wrote = 0
   _vorbis_analysis_blockout = 0
   _vorbis_analysis = 0
   _vorbis_bitrate_addblock = 0
   _vorbis_bitrate_flushpacket = 0

   _ogg_stream_init = 0
   _ogg_stream_clear = 0
   _ogg_stream_packetin = 0
   _ogg_stream_flush = 0
   _ogg_stream_pageout = 0
   _ogg_page_eos = 0
}

fn _load_libs(){
   "Loads required Vorbis/Ogg shared libraries."
   if(_lib_vorbisfile == 0){
      _lib_vorbisfile = dlopen_any("vorbisfile", RTLD_NOW() | RTLD_LOCAL())
      if(_lib_vorbisfile == 0){ _lib_vorbisfile = dlopen_any("libvorbisfile", RTLD_NOW() | RTLD_LOCAL()) }
   }
   if(_lib_vorbis == 0){
      _lib_vorbis = dlopen_any("vorbis", RTLD_NOW() | RTLD_LOCAL())
      if(_lib_vorbis == 0){ _lib_vorbis = dlopen_any("libvorbis", RTLD_NOW() | RTLD_LOCAL()) }
   }
   if(_lib_vorbisenc == 0){
      _lib_vorbisenc = dlopen_any("vorbisenc", RTLD_NOW() | RTLD_LOCAL())
      if(_lib_vorbisenc == 0){ _lib_vorbisenc = dlopen_any("libvorbisenc", RTLD_NOW() | RTLD_LOCAL()) }
   }
   if(_lib_ogg == 0){
      _lib_ogg = dlopen_any("ogg", RTLD_NOW() | RTLD_LOCAL())
      if(_lib_ogg == 0){ _lib_ogg = dlopen_any("libogg", RTLD_NOW() | RTLD_LOCAL()) }
   }
   true
}

fn _dlsym_any(name){
   "Resolves a symbol from any loaded Vorbis/Ogg library."
   def a = (_lib_vorbisfile != 0) ? dlsym(_lib_vorbisfile, name) : 0
   if(a != 0){ return a }
   def b = (_lib_vorbisenc != 0) ? dlsym(_lib_vorbisenc, name) : 0
   if(b != 0){ return b }
   def c = (_lib_vorbis != 0) ? dlsym(_lib_vorbis, name) : 0
   if(c != 0){ return c }
   (_lib_ogg != 0) ? dlsym(_lib_ogg, name) : 0
}

fn _init_decode(){
   "Loads vorbisfile decode symbols."
   if(_ov_fopen != 0 && _ov_read_float != 0 && _ov_clear != 0){ return true }
   if(!_load_libs()){ return false }
   _ov_fopen = _dlsym_any("ov_fopen")
   _ov_read_float = _dlsym_any("ov_read_float")
   _ov_clear = _dlsym_any("ov_clear")
   if(_ov_fopen == 0 || _ov_read_float == 0 || _ov_clear == 0){
      _close_libs()
      return false
   }
   true
}

fn _init_encode(){
   "Loads libvorbisenc/libogg encode symbols."
   if(_vorbis_encode_init != 0 && _ogg_stream_init != 0){ return true }
   if(!_load_libs()){ return false }

   _vorbis_info_init = _dlsym_any("vorbis_info_init")
   _vorbis_info_clear = _dlsym_any("vorbis_info_clear")
   _vorbis_encode_init = _dlsym_any("vorbis_encode_init")
   _vorbis_comment_init = _dlsym_any("vorbis_comment_init")
   _vorbis_comment_add_tag = _dlsym_any("vorbis_comment_add_tag")
   _vorbis_comment_clear = _dlsym_any("vorbis_comment_clear")
   _vorbis_analysis_init = _dlsym_any("vorbis_analysis_init")
   _vorbis_block_init = _dlsym_any("vorbis_block_init")
   _vorbis_block_clear = _dlsym_any("vorbis_block_clear")
   _vorbis_dsp_clear = _dlsym_any("vorbis_dsp_clear")
   _vorbis_analysis_headerout = _dlsym_any("vorbis_analysis_headerout")
   _vorbis_analysis_buffer = _dlsym_any("vorbis_analysis_buffer")
   _vorbis_analysis_wrote = _dlsym_any("vorbis_analysis_wrote")
   _vorbis_analysis_blockout = _dlsym_any("vorbis_analysis_blockout")
   _vorbis_analysis = _dlsym_any("vorbis_analysis")
   _vorbis_bitrate_addblock = _dlsym_any("vorbis_bitrate_addblock")
   _vorbis_bitrate_flushpacket = _dlsym_any("vorbis_bitrate_flushpacket")

   _ogg_stream_init = _dlsym_any("ogg_stream_init")
   _ogg_stream_clear = _dlsym_any("ogg_stream_clear")
   _ogg_stream_packetin = _dlsym_any("ogg_stream_packetin")
   _ogg_stream_flush = _dlsym_any("ogg_stream_flush")
   _ogg_stream_pageout = _dlsym_any("ogg_stream_pageout")
   _ogg_page_eos = _dlsym_any("ogg_page_eos")

   if(
      _vorbis_info_init == 0 || _vorbis_info_clear == 0 || _vorbis_encode_init == 0 ||
      _vorbis_comment_init == 0 || _vorbis_comment_add_tag == 0 || _vorbis_comment_clear == 0 ||
      _vorbis_analysis_init == 0 || _vorbis_block_init == 0 || _vorbis_block_clear == 0 ||
      _vorbis_dsp_clear == 0 || _vorbis_analysis_headerout == 0 || _vorbis_analysis_buffer == 0 ||
      _vorbis_analysis_wrote == 0 || _vorbis_analysis_blockout == 0 || _vorbis_analysis == 0 ||
      _vorbis_bitrate_addblock == 0 || _vorbis_bitrate_flushpacket == 0 ||
      _ogg_stream_init == 0 || _ogg_stream_clear == 0 || _ogg_stream_packetin == 0 ||
      _ogg_stream_flush == 0 || _ogg_stream_pageout == 0 || _ogg_page_eos == 0
   ){
      _close_libs()
      return false
   }
   true
}

fn _ptr_to_blob(ptr, n){
   "Converts raw memory to Ny string bytes."
   if(!ptr || n <= 0){ return "" }
   def out = init_str(malloc(n + 1), n)
   memcpy(out, ptr, n)
   out
}

fn _next_page(data, offset, end){
   "Finds and parses the next Ogg page from `offset`."
   mut p = offset
   while(p + 27 <= end){
      if(load8(data, p) == 79 && load8(data, p + 1) == 103 && load8(data, p + 2) == 103 && load8(data, p + 3) == 83){
         def segs = load8(data, p + 26)
         def lacing = p + 27
         def payload = lacing + segs
         if(payload > end){ return 0 }
         mut total = 0
         mut i = 0
         while(i < segs){
            total += load8(data, lacing + i)
            i += 1
         }
         def next_p = payload + total
         if(next_p > end){ return 0 }
         mut d = dict(8)
         d = dict_set(d, "start", p)
         d = dict_set(d, "lacing", lacing)
         d = dict_set(d, "payload", payload)
         d = dict_set(d, "segs", segs)
         d = dict_set(d, "next", next_p)
         return d
      }
      p += 1
   }
   0
}

fn _read_ident_packet(data){
   "Reads and assembles the first logical packet (Vorbis ident header)."
   def end = len(data)
   if(end < 32){ return 0 }
   def cap = 65536
   def pkt = malloc(cap)
   if(!pkt){ return 0 }
   mut plen = 0
   mut off = 0
   mut done = false
   while(!done){
      def page = _next_page(data, off, end)
      if(!page){ free(pkt) return 0 }
      def segs = dict_get(page, "segs")
      def lacing = dict_get(page, "lacing")
      mut pay = dict_get(page, "payload")
      mut i = 0
      while(i < segs){
         def seglen = load8(data, lacing + i)
         if(plen + seglen > cap){ free(pkt) return 0 }
         if(seglen > 0){ memcpy(ptr_add(pkt, plen), ptr_add(data, pay), seglen) }
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
   mut out = dict(4)
   out = dict_set(out, "ptr", pkt)
   out = dict_set(out, "len", plen)
   out
}

fn get_info(data){
   "Parses Vorbis identification header and returns stream metadata."
   if(!is_str(data) || len(data) < 32){ return 0 }
   def pkt = _read_ident_packet(data)
   if(!pkt){ return 0 }
   def p = dict_get(pkt, "ptr", 0)
   def n = dict_get(pkt, "len", 0)
   if(!p || n < 30){ free(p) return 0 }
   if(load8(p, 0) != 1){ free(p) return 0 }
   if(load8(p, 1) != 118 || load8(p, 2) != 111 || load8(p, 3) != 114 || load8(p, 4) != 98 || load8(p, 5) != 105 || load8(p, 6) != 115){
      free(p)
      return 0
   }

   def ver = load32(p, 7)
   def channels = load8(p, 11)
   def rate = load32(p, 12)
   def br_max = load32(p, 16)
   def br_nom = load32(p, 20)
   def br_min = load32(p, 24)
   def blocks = load8(p, 28)
   def framing = load8(p, 29) & 1
   free(p)

   if(ver != 0 || channels < 1 || channels > 8 || rate <= 0 || framing != 1){ return 0 }

   def bs0 = 1 << (blocks & 15)
   def bs1 = 1 << ((blocks >> 4) & 15)

   mut info = dict(12)
   info = dict_set(info, "channels", channels)
   info = dict_set(info, "rate", rate)
   info = dict_set(info, "sample_rate", rate)
   info = dict_set(info, "bitrate_max", br_max)
   info = dict_set(info, "bitrate_nominal", br_nom)
   info = dict_set(info, "bitrate_min", br_min)
   info = dict_set(info, "blocksize_0", bs0)
   info = dict_set(info, "blocksize_1", bs1)
   info
}

fn _sample_fmt_from_bits(bits){
   "Infers source sample format enum from bit depth."
   if(bits == 8){ return _SAMPLE_FMT_U8 }
   if(bits == 16){ return _SAMPLE_FMT_S16 }
   if(bits == 24){ return _SAMPLE_FMT_S24 }
   if(bits == 32){ return _SAMPLE_FMT_S32 }
   0
}

fn _blob_from_any(pcm){
   "Normalizes PCM input (bytes/source/dict) into a metadata dictionary."
   if(is_str(pcm)){
      mut meta = dict(8)
      meta = dict_set(meta, "blob", pcm)
      meta = dict_set(meta, "channels", 0)
      meta = dict_set(meta, "rate", 0)
      meta = dict_set(meta, "bits", 0)
      meta = dict_set(meta, "sample_fmt", 0)
      return meta
   }
   if(is_list(pcm) && len(pcm) > 1 && eq(get(pcm, 0), "SOUND_SOURCE")){
      def d = get(pcm, 1)
      if(is_dict(d)){
         def ptr = get(d, "ptr", 0)
         def n = get(d, "len", 0)
         if(ptr && n > 0){
            def blob = _ptr_to_blob(ptr, n)
            if(blob){
               mut meta = dict(8)
               meta = dict_set(meta, "blob", blob)
               meta = dict_set(meta, "channels", get(d, "channels", 0))
               meta = dict_set(meta, "rate", get(d, "rate", 0))
               meta = dict_set(meta, "bits", get(d, "bits", 0))
               meta = dict_set(meta, "sample_fmt", get(d, "sample_fmt", 0))
               return meta
            }
         }
      }
   }
   if(is_dict(pcm)){
      mut ptr = get(pcm, "ptr", 0)
      if(!ptr){ ptr = get(pcm, "data", 0) }
      def n = get(pcm, "len", get(pcm, "byte_len", 0))
      if(ptr && n > 0){
         def blob = _ptr_to_blob(ptr, n)
         if(blob){
            mut meta = dict(8)
            meta = dict_set(meta, "blob", blob)
            meta = dict_set(meta, "channels", get(pcm, "channels", 0))
            meta = dict_set(meta, "rate", get(pcm, "sample_rate", get(pcm, "rate", 0)))
            meta = dict_set(meta, "bits", get(pcm, "bits", 0))
            meta = dict_set(meta, "sample_fmt", get(pcm, "sample_fmt", 0))
            return meta
         }
      }
   }
   0
}

fn _blob_to_f32(blob, bits, sample_fmt){
   "Converts packed PCM bytes to interleaved float32 PCM bytes."
   if(!is_str(blob)){ return 0 }
   mut sf = sample_fmt
   if(sf == 0){ sf = _sample_fmt_from_bits(bits) }
   if(sf == _SAMPLE_FMT_F32){ return blob }

   mut bps = bits / 8
   if(bps <= 0){
      if(sf == _SAMPLE_FMT_U8){ bps = 1 }
      elif(sf == _SAMPLE_FMT_S16){ bps = 2 }
      elif(sf == _SAMPLE_FMT_S24){ bps = 3 }
      elif(sf == _SAMPLE_FMT_S32){ bps = 4 }
   }
   if(bps <= 0){ return 0 }

   def n = len(blob)
   if(n <= 0 || (n % bps) != 0){ return 0 }
   def count = n / bps
   def out_n = count * 4
   def out = malloc(out_n + 1)
   if(!out){ return 0 }

   mut i = 0
   while(i < count){
      mut v = 0.0
      if(sf == _SAMPLE_FMT_U8){
         def x = load8(blob, i)
         v = ((x - 128) + 0.0) / 128.0
      } elif(sf == _SAMPLE_FMT_S16){
         mut x = load16(blob, i * 2)
         if(x >= 32768){ x = x - 65536 }
         v = (x + 0.0) / 32768.0
      } elif(sf == _SAMPLE_FMT_S24){
         def off = i * 3
         mut x = load8(blob, off) | (load8(blob, off + 1) << 8) | (load8(blob, off + 2) << 16)
         if(x >= 0x800000){ x = x - 0x1000000 }
         v = (x + 0.0) / 8388608.0
      } elif(sf == _SAMPLE_FMT_S32){
         mut x = load32(blob, i * 4)
         if(x >= 2147483648){ x = x - 4294967296 }
         v = (x + 0.0) / 2147483648.0
      } else {
         free(out)
         return 0
      }

      if(v > 1.0){ v = 1.0 }
      if(v < -1.0){ v = -1.0 }
      store32_f32(out, v, i * 4)
      i += 1
   }
   store8(out, 0, out_n)
   init_str(out, out_n)
}

fn _ensure_cap(ptr, cap, need){
   "Ensures output buffer capacity. Returns dict {ok, ptr, cap}."
   mut out = ptr
   mut c = cap
   if(need > c){
      if(c < 1024){ c = 1024 }
      while(need > c){ c = c * 2 }
      def grown = realloc(out, c + 1)
      if(!grown){
         mut bad = dict(4)
         bad = dict_set(bad, "ok", false)
         bad = dict_set(bad, "ptr", out)
         bad = dict_set(bad, "cap", cap)
         return bad
      }
      out = grown
   }
   mut ok = dict(4)
   ok = dict_set(ok, "ok", true)
   ok = dict_set(ok, "ptr", out)
   ok = dict_set(ok, "cap", c)
   ok
}

fn _append_bytes(out, out_len, out_cap, src, n){
   "Appends raw bytes to a growable output buffer."
   mut st = dict(8)
   if(n <= 0){
      st = dict_set(st, "ok", true)
      st = dict_set(st, "ptr", out)
      st = dict_set(st, "len", out_len)
      st = dict_set(st, "cap", out_cap)
      return st
   }
   if(!src){
      st = dict_set(st, "ok", false)
      st = dict_set(st, "ptr", out)
      st = dict_set(st, "len", out_len)
      st = dict_set(st, "cap", out_cap)
      return st
   }

   def need = out_len + n
   def eg = _ensure_cap(out, out_cap, need)
   if(!dict_get(eg, "ok", false)){
      st = dict_set(st, "ok", false)
      st = dict_set(st, "ptr", dict_get(eg, "ptr", out))
      st = dict_set(st, "len", out_len)
      st = dict_set(st, "cap", dict_get(eg, "cap", out_cap))
      return st
   }

   def p = dict_get(eg, "ptr", out)
   def c = dict_get(eg, "cap", out_cap)
   memcpy(ptr_add(p, out_len), src, n)

   st = dict_set(st, "ok", true)
   st = dict_set(st, "ptr", p)
   st = dict_set(st, "len", need)
   st = dict_set(st, "cap", c)
   st
}

fn _emit_pages(os, og, out, out_len, out_cap, force_flush){
   "Flushes/pageouts ogg pages and appends header+body bytes."
   _sizes_init()
   mut ptr = out
   mut olen = out_len
   mut ocap = out_cap
   mut eos = false
   while(true){
      def rc = force_flush ? call2(_ogg_stream_flush, os, og) : call2(_ogg_stream_pageout, os, og)
      if(rc == 0){ break }

      def hptr = _ptr_load(og, _og_page_off_header)
      def hlen = _long_load(og, _og_page_off_header_len)
      def bptr = _ptr_load(og, _og_page_off_body)
      def blen = _long_load(og, _og_page_off_body_len)
      _dbg("emit: rc/hlen/blen", rc, hlen, blen)

      def a = _append_bytes(ptr, olen, ocap, hptr, hlen)
      if(!dict_get(a, "ok", false)){
         mut bad = dict(8)
         bad = dict_set(bad, "ok", false)
         bad = dict_set(bad, "ptr", dict_get(a, "ptr", ptr))
         bad = dict_set(bad, "len", dict_get(a, "len", olen))
         bad = dict_set(bad, "cap", dict_get(a, "cap", ocap))
         bad = dict_set(bad, "eos", eos)
         return bad
      }
      ptr = dict_get(a, "ptr", ptr)
      olen = dict_get(a, "len", olen)
      ocap = dict_get(a, "cap", ocap)

      def b = _append_bytes(ptr, olen, ocap, bptr, blen)
      if(!dict_get(b, "ok", false)){
         mut bad2 = dict(8)
         bad2 = dict_set(bad2, "ok", false)
         bad2 = dict_set(bad2, "ptr", dict_get(b, "ptr", ptr))
         bad2 = dict_set(bad2, "len", dict_get(b, "len", olen))
         bad2 = dict_set(bad2, "cap", dict_get(b, "cap", ocap))
         bad2 = dict_set(bad2, "eos", eos)
         return bad2
      }
      ptr = dict_get(b, "ptr", ptr)
      olen = dict_get(b, "len", olen)
      ocap = dict_get(b, "cap", ocap)

      if(call1(_ogg_page_eos, og) != 0){ eos = true }
   }

   mut ok = dict(8)
   ok = dict_set(ok, "ok", true)
   ok = dict_set(ok, "ptr", ptr)
   ok = dict_set(ok, "len", olen)
   ok = dict_set(ok, "cap", ocap)
   ok = dict_set(ok, "eos", eos)
   ok
}

fn _nominal_bitrate(channels, rate){
   "Selects a managed nominal bitrate for Vorbis encode."
   mut br = channels * rate * 2
   if(br < 48000){ br = 48000 }
   if(br > 256000){ br = 256000 }
   def ev = env("NY_VORBIS_BITRATE")
   if(ev){
      def n = atoi(ev)
      if(n >= 32000 && n <= 1000000){ br = n }
   }
   br
}

fn _bitrate_try_list(seed){
   "Builds a conservative bitrate fallback list for managed-mode init."
   mut xs = list()
   if(seed >= 32000){ xs = append(xs, seed) }
   if(seed != 256000){ xs = append(xs, 256000) }
   if(seed != 224000){ xs = append(xs, 224000) }
   if(seed != 192000){ xs = append(xs, 192000) }
   if(seed != 160000){ xs = append(xs, 160000) }
   if(seed != 128000){ xs = append(xs, 128000) }
   if(seed != 112000){ xs = append(xs, 112000) }
   if(seed != 96000){ xs = append(xs, 96000) }
   if(seed != 80000){ xs = append(xs, 80000) }
   if(seed != 64000){ xs = append(xs, 64000) }
   if(seed != 48000){ xs = append(xs, 48000) }
   if(seed != 32000){ xs = append(xs, 32000) }
   xs
}

fn decode(data){
   "Decodes Ogg Vorbis bytes into a float32 memory source."
   def info = get_info(data)
   if(!info){
      _dbg("decode: get_info failed")
      return 0
   }
   if(!_init_decode()){
      _dbg("decode: _init_decode failed")
      return 0
   }

   def ch = dict_get(info, "channels", 0)
   def sr = dict_get(info, "sample_rate", dict_get(info, "rate", 0))
   if(ch <= 0 || sr <= 0){
      _dbg("decode: invalid stream meta", ch, sr)
      return 0
   }

   def in_path = _tmp_path("ny_vorbis_in", ".ogg")
   match file_write(in_path, data){
      err(_) -> {
         _dbg("decode: file_write failed", in_path)
         return 0
      }
      ok(_) -> {}
   }

   def vf = malloc(_OV_FILE_SIZE)
   if(!vf){
      _dbg("decode: alloc vf failed")
      _cleanup(in_path)
      return 0
   }
   memset(vf, 0, _OV_FILE_SIZE)

   def rc = call2(_ov_fopen, in_path, vf)
   _cleanup(in_path)
   if(rc < 0){
      _dbg("decode: ov_fopen failed rc=", rc)
      free(vf)
      return 0
   }

   def pcmpp = malloc(8)
   def bsp = malloc(4)
   if(!pcmpp || !bsp){
      _dbg("decode: alloc pcmpp/bsp failed")
      if(pcmpp){ free(pcmpp) }
      if(bsp){ free(bsp) }
      call1(_ov_clear, vf)
      free(vf)
      return 0
   }

   mut cap = 65536
   def out0 = malloc(cap + 1)
   if(!out0){
      _dbg("decode: alloc output failed")
      free(pcmpp)
      free(bsp)
      call1(_ov_clear, vf)
      free(vf)
      return 0
   }
   mut out = out0
   mut out_len = 0

   while(true){
      store32(bsp, 0, 0)
      def got = call4(_ov_read_float, vf, pcmpp, 4096, bsp)
      if(got < 0){
         _dbg("decode: ov_read_float error", got)
         free(out)
         free(pcmpp)
         free(bsp)
         call1(_ov_clear, vf)
         free(vf)
         return 0
      }
      if(got == 0){ break }

      def nbytes = got * ch * 4
      def eg = _ensure_cap(out, cap, out_len + nbytes)
      if(!dict_get(eg, "ok", false)){
         _dbg("decode: grow failed need=", out_len + nbytes)
         free(out)
         free(pcmpp)
         free(bsp)
         call1(_ov_clear, vf)
         free(vf)
         return 0
      }
      out = dict_get(eg, "ptr", out)
      cap = dict_get(eg, "cap", cap)

      def pcm = _ptr_load(pcmpp, 0)
      mut i = 0
      while(i < got){
         mut c = 0
         while(c < ch){
            def plane = _ptr_load(pcm, c * _ptr_bytes)
            def v = load32_f32(plane, i * 4)
            store32_f32(out, v, out_len + ((i * ch + c) * 4))
            c += 1
         }
         i += 1
      }
      out_len += nbytes
   }

   free(pcmpp)
   free(bsp)
   call1(_ov_clear, vf)
   free(vf)

   if(out_len <= 0){
      _dbg("decode: empty output")
      free(out)
      return 0
   }

   store8(out, 0, out_len)
   init_str(out, out_len)
   make_memory_source(out, out_len, ch, sr, 32, SAMPLE_FMT_F32, 3)
}

fn encode(pcm, channels, rate, bits){
   "Encodes PCM (bytes/source/dict) into Ogg Vorbis bytes."
   if(!_init_encode()){
      _dbg("encode: _init_encode failed")
      return 0
   }

   def meta = _blob_from_any(pcm)
   if(!meta){
      _dbg("encode: _blob_from_any failed")
      return 0
   }

   def blob = dict_get(meta, "blob", "")
   if(!is_str(blob) || len(blob) == 0){
      _dbg("encode: empty blob")
      return 0
   }

   mut ch = channels
   if(ch <= 0){ ch = dict_get(meta, "channels", 0) }
   mut sr = rate
   if(sr <= 0){ sr = dict_get(meta, "rate", 0) }
   mut bt = bits
   if(bt <= 0){ bt = dict_get(meta, "bits", 0) }

   mut sf = dict_get(meta, "sample_fmt", 0)
   if(sf == 0){ sf = _sample_fmt_from_bits(bt) }

   if(ch <= 0 || ch > 8 || sr <= 0){
      _dbg("encode: invalid channels/rate", ch, sr)
      return 0
   }

   def f32_blob = _blob_to_f32(blob, bt, sf)
   if(!f32_blob){
      _dbg("encode: _blob_to_f32 failed bits/fmt=", bt, sf)
      return 0
   }

   def total_samples = len(f32_blob) / 4
   if(total_samples <= 0 || (total_samples % ch) != 0){
      _dbg("encode: invalid sample count", total_samples, "ch=", ch)
      return 0
   }
   def total_frames = total_samples / ch

   def vi = malloc(_VORBIS_INFO_SIZE)
   def vc = malloc(_VORBIS_COMMENT_SIZE)
   def vd = malloc(_VORBIS_DSP_SIZE)
   def vb = malloc(_VORBIS_BLOCK_SIZE)
   def os = malloc(_OGG_STREAM_SIZE)
   def og = malloc(_OGG_PAGE_SIZE)
   def op_h = malloc(_OGG_PACKET_SIZE)
   def op_c = malloc(_OGG_PACKET_SIZE)
   def op_k = malloc(_OGG_PACKET_SIZE)
   def op = malloc(_OGG_PACKET_SIZE)

   if(!vi || !vc || !vd || !vb || !os || !og || !op_h || !op_c || !op_k || !op){
      _dbg("encode: alloc state failed")
      if(vi){ free(vi) }
      if(vc){ free(vc) }
      if(vd){ free(vd) }
      if(vb){ free(vb) }
      if(os){ free(os) }
      if(og){ free(og) }
      if(op_h){ free(op_h) }
      if(op_c){ free(op_c) }
      if(op_k){ free(op_k) }
      if(op){ free(op) }
      return 0
   }

   memset(vi, 0, _VORBIS_INFO_SIZE)
   memset(vc, 0, _VORBIS_COMMENT_SIZE)
   memset(vd, 0, _VORBIS_DSP_SIZE)
   memset(vb, 0, _VORBIS_BLOCK_SIZE)
   memset(os, 0, _OGG_STREAM_SIZE)
   memset(og, 0, _OGG_PAGE_SIZE)
   memset(op_h, 0, _OGG_PACKET_SIZE)
   memset(op_c, 0, _OGG_PACKET_SIZE)
   memset(op_k, 0, _OGG_PACKET_SIZE)
   memset(op, 0, _OGG_PACKET_SIZE)

   mut vi_ok = false
   mut vc_ok = false
   mut vd_ok = false
   mut vb_ok = false
   mut os_ok = false

   call1_void(_vorbis_info_init, vi)
   vi_ok = true

   def nominal_seed = _nominal_bitrate(ch, sr)
   mut nominal = nominal_seed
   mut enc_rc = -1
   def tries = _bitrate_try_list(nominal_seed)
   mut ti = 0
   while(ti < len(tries)){
      nominal = get(tries, ti, nominal_seed)
      enc_rc = call6(_vorbis_encode_init, vi, ch, sr, -1, nominal, -1)
      if(enc_rc == 0){ break }
      call1_void(_vorbis_info_clear, vi)
      call1_void(_vorbis_info_init, vi)
      ti += 1
   }
   if(enc_rc != 0){
      _dbg("encode: vorbis_encode_init failed rc=", enc_rc, "ch/r/s/nom=", ch, sr, nominal)
      call1_void(_vorbis_info_clear, vi)
      free(vi) free(vc) free(vd) free(vb) free(os) free(og) free(op_h) free(op_c) free(op_k) free(op)
      return 0
   }

   call1_void(_vorbis_comment_init, vc)
   vc_ok = true
   call3_void(_vorbis_comment_add_tag, vc, "ENCODER", "nytrix-vorbis")

   def an_rc = call2(_vorbis_analysis_init, vd, vi)
   if(an_rc != 0){
      _dbg("encode: vorbis_analysis_init failed rc=", an_rc)
      call1_void(_vorbis_comment_clear, vc)
      call1_void(_vorbis_info_clear, vi)
      free(vi) free(vc) free(vd) free(vb) free(os) free(og) free(op_h) free(op_c) free(op_k) free(op)
      return 0
   }
   vd_ok = true

   def bl_rc = call2(_vorbis_block_init, vd, vb)
   if(bl_rc != 0){
      _dbg("encode: vorbis_block_init failed rc=", bl_rc)
      call1_void(_vorbis_dsp_clear, vd)
      call1_void(_vorbis_comment_clear, vc)
      call1_void(_vorbis_info_clear, vi)
      free(vi) free(vc) free(vd) free(vb) free(os) free(og) free(op_h) free(op_c) free(op_k) free(op)
      return 0
   }
   vb_ok = true

   mut serial = ((pid() & 0x7fff) << 16) | ((ticks() & 0xffff) + 1)
   if(serial == 0){ serial = 1 }
   def os_rc = call2(_ogg_stream_init, os, serial)
   if(os_rc != 0){
      _dbg("encode: ogg_stream_init failed rc=", os_rc)
      call1_void(_vorbis_block_clear, vb)
      call1_void(_vorbis_dsp_clear, vd)
      call1_void(_vorbis_comment_clear, vc)
      call1_void(_vorbis_info_clear, vi)
      free(vi) free(vc) free(vd) free(vb) free(os) free(og) free(op_h) free(op_c) free(op_k) free(op)
      return 0
   }
   os_ok = true

   def ho_rc = call5(_vorbis_analysis_headerout, vd, vc, op_h, op_c, op_k)
   if(ho_rc != 0){
      _dbg("encode: vorbis_analysis_headerout failed rc=", ho_rc)
      call1_void(_ogg_stream_clear, os)
      call1_void(_vorbis_block_clear, vb)
      call1_void(_vorbis_dsp_clear, vd)
      call1_void(_vorbis_comment_clear, vc)
      call1_void(_vorbis_info_clear, vi)
      free(vi) free(vc) free(vd) free(vb) free(os) free(og) free(op_h) free(op_c) free(op_k) free(op)
      return 0
   }

   call2(_ogg_stream_packetin, os, op_h)
   call2(_ogg_stream_packetin, os, op_c)
   call2(_ogg_stream_packetin, os, op_k)

   mut out_cap = 65536
   def out0 = malloc(out_cap + 1)
   if(!out0){
      _dbg("encode: output alloc failed")
      call1_void(_ogg_stream_clear, os)
      call1_void(_vorbis_block_clear, vb)
      call1_void(_vorbis_dsp_clear, vd)
      call1_void(_vorbis_comment_clear, vc)
      call1_void(_vorbis_info_clear, vi)
      free(vi) free(vc) free(vd) free(vb) free(os) free(og) free(op_h) free(op_c) free(op_k) free(op)
      return 0
   }

   mut out = out0
   mut out_len = 0

   def ph = _emit_pages(os, og, out, out_len, out_cap, true)
   if(!dict_get(ph, "ok", false)){
      _dbg("encode: emit header pages failed")
      free(dict_get(ph, "ptr", out))
      call1_void(_ogg_stream_clear, os)
      call1_void(_vorbis_block_clear, vb)
      call1_void(_vorbis_dsp_clear, vd)
      call1_void(_vorbis_comment_clear, vc)
      call1_void(_vorbis_info_clear, vi)
      free(vi) free(vc) free(vd) free(vb) free(os) free(og) free(op_h) free(op_c) free(op_k) free(op)
      return 0
   }
   out = dict_get(ph, "ptr", out)
   out_len = dict_get(ph, "len", out_len)
   out_cap = dict_get(ph, "cap", out_cap)

   def read_frames = 1024
   mut frame_pos = 0
   mut wrote_eos = false
   mut eos_seen = false

   while(!eos_seen){
      mut got = total_frames - frame_pos
      if(got > read_frames){ got = read_frames }
      if(got < 0){ got = 0 }

      if(got > 0){
         def planes = call2(_vorbis_analysis_buffer, vd, got)
         if(!planes){
            _dbg("encode: vorbis_analysis_buffer returned null")
            free(out)
            if(os_ok){ call1_void(_ogg_stream_clear, os) }
            if(vb_ok){ call1_void(_vorbis_block_clear, vb) }
            if(vd_ok){ call1_void(_vorbis_dsp_clear, vd) }
            if(vc_ok){ call1_void(_vorbis_comment_clear, vc) }
            if(vi_ok){ call1_void(_vorbis_info_clear, vi) }
            free(vi) free(vc) free(vd) free(vb) free(os) free(og) free(op_h) free(op_c) free(op_k) free(op)
            return 0
         }
         mut i = 0
         while(i < got){
            mut c = 0
            while(c < ch){
               def ch_ptr = _ptr_load(planes, c * _ptr_bytes)
               def in_off = ((frame_pos + i) * ch + c) * 4
               store32_f32(ch_ptr, load32_f32(f32_blob, in_off), i * 4)
               c += 1
            }
            i += 1
         }
      }

      call2(_vorbis_analysis_wrote, vd, got)
      if(got == 0){ wrote_eos = true }

      while(call2(_vorbis_analysis_blockout, vd, vb) == 1){
         call2(_vorbis_analysis, vb, 0)
         call1(_vorbis_bitrate_addblock, vb)

         while(call2(_vorbis_bitrate_flushpacket, vd, op) != 0){
            call2(_ogg_stream_packetin, os, op)
            def pg = _emit_pages(os, og, out, out_len, out_cap, false)
            if(!dict_get(pg, "ok", false)){
               _dbg("encode: emit packet pages failed")
               free(dict_get(pg, "ptr", out))
               if(os_ok){ call1_void(_ogg_stream_clear, os) }
               if(vb_ok){ call1_void(_vorbis_block_clear, vb) }
               if(vd_ok){ call1_void(_vorbis_dsp_clear, vd) }
               if(vc_ok){ call1_void(_vorbis_comment_clear, vc) }
               if(vi_ok){ call1_void(_vorbis_info_clear, vi) }
               free(vi) free(vc) free(vd) free(vb) free(os) free(og) free(op_h) free(op_c) free(op_k) free(op)
               return 0
            }
            out = dict_get(pg, "ptr", out)
            out_len = dict_get(pg, "len", out_len)
            out_cap = dict_get(pg, "cap", out_cap)
            if(dict_get(pg, "eos", false)){
               eos_seen = true
               break
            }
         }
         if(eos_seen){ break }
      }

      frame_pos += got
      if(wrote_eos && !eos_seen){
         if(frame_pos >= total_frames){
            ;; already fed end-of-stream marker and drained available packets.
            break
         }
      }

      if(frame_pos >= total_frames && !wrote_eos){
         ;; force one final iteration with got=0.
         frame_pos = total_frames
      }
   }

   if(os_ok){ call1_void(_ogg_stream_clear, os) }
   if(vb_ok){ call1_void(_vorbis_block_clear, vb) }
   if(vd_ok){ call1_void(_vorbis_dsp_clear, vd) }
   if(vc_ok){ call1_void(_vorbis_comment_clear, vc) }
   if(vi_ok){ call1_void(_vorbis_info_clear, vi) }

   free(vi) free(vc) free(vd) free(vb) free(os) free(og) free(op_h) free(op_c) free(op_k) free(op)

   if(out_len <= 0){
      _dbg("encode: empty output")
      free(out)
      return 0
   }

   store8(out, 0, out_len)
   init_str(out, out_len)
}

if(comptime{__main()}){
   use std.audio.formats.vorbis *
   assert(get_info("not ogg") == 0)
   assert(decode("not ogg") == 0)
   print("✓ std.audio.formats.vorbis tests passed")
}
