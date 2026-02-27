;; Keywords: sound source

module std.audio.source (
   make_memory_source,
   read, seek, tell, length, format, sample_format,
   SAMPLE_FMT_S16, SAMPLE_FMT_U8, SAMPLE_FMT_S24, SAMPLE_FMT_S32, SAMPLE_FMT_F32
)

use std.core *
use std.core.dict *
use std.audio.source.memory as memory

def SAMPLE_FMT_S16 = 1
def SAMPLE_FMT_U8 = 2
def SAMPLE_FMT_S24 = 3
def SAMPLE_FMT_S32 = 4
def SAMPLE_FMT_F32 = 5

fn make_memory_source(ptr, byte_len, channels, rate, bits, sample_fmt=0, format_tag=1){
   "Implements `make_memory_source`."
   memory.make(ptr, byte_len, channels, rate, bits, sample_fmt, format_tag)
}

fn read(src, mix_buf, frames){
   "Reads up to 'frames' from the source into 'mix_buf'."
   if(!is_list(src) || len(src) < 3){ return 0 }
   mut data = get(src, 1)
   def cursor = __flt_to_int(get_item(data, "cursor", 0.0) + 0.0)
   def total = get(data, "total_frames")
   def frame_size = get(data, "frame_size")
   def ptr = get(data, "ptr")
   mut to_read = frames
   if(cursor + to_read > total){ to_read = total - cursor }
   if(to_read <= 0){ return 0 }
   memcpy(mix_buf, ptr + (cursor * frame_size), to_read * frame_size)
   data = dict_set(data, "cursor", cursor + to_read)
   set_idx(src, 1, data)
   to_read
}

fn seek(src, frame){
   "Implements `seek`."
   if(!is_list(src)){ return false }
   mut data = get(src, 1)
   def total = get(data, "total_frames")
   mut f = frame
   if(f < 0){ f = 0 }
   if(f > total){ f = total }
   data = dict_set(data, "cursor", f)
   set_idx(src, 1, data)
   true
}

fn tell(src){
   "Implements `tell`."
   if(!is_list(src)){ return 0 }
   __flt_to_int(get_item(get(src, 1), "cursor", 0.0) + 0.0)
}

fn length(src){
   "Implements `length`."
   if(!is_list(src)){ return 0 }
   get(get(src, 1), "total_frames")
}

fn format(src){
   "Returns list of channels, rate, bits"
   if(!is_list(src)){ 
      mut empty_fmt = list()
      empty_fmt = append(empty_fmt, 0)
      empty_fmt = append(empty_fmt, 0)
      empty_fmt = append(empty_fmt, 0)
      return empty_fmt
   }
   def d = get(src, 1)
   mut fmt = list()
   fmt = append(fmt, get(d, "channels"))
   fmt = append(fmt, get(d, "rate"))
   fmt = append(fmt, get(d, "bits"))
   fmt
}

fn sample_format(src){
   "Returns source sample format enum."
   if(!is_list(src)){ return 0 }
   def d = get(src, 1)
   def sf = get(d, "sample_fmt", 0)
   if(sf != 0){ return sf }
   def bits = get(d, "bits", 16)
   def tag = get(d, "format_tag", 1)
   if(bits == 16){ return SAMPLE_FMT_S16 }
   if(bits == 8){ return SAMPLE_FMT_U8 }
   if(bits == 24){ return SAMPLE_FMT_S24 }
   if(bits == 32){
      if(tag == 3){ return SAMPLE_FMT_F32 }
      return SAMPLE_FMT_S32
   }
   SAMPLE_FMT_S16
}

fn get_item(d, key, default){
   "Gets item."
   def v = dict_get(d, key)
   if(v == 0){ return default }
   v
}
