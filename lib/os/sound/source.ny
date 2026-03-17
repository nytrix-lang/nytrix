;; Keywords: sound source
;; Sound source facade for memory-backed and synthesized audio.
module std.os.sound.source(make_memory_source, read, seek, tell, length, format, sample_format, source_channels, source_rate, source_bits, source_length, SAMPLE_FMT_S16, SAMPLE_FMT_U8, SAMPLE_FMT_S24, SAMPLE_FMT_S32, SAMPLE_FMT_F32)
use std.core
use std.core.dict_mod
use std.os.sound.source.memory as memory

def SAMPLE_FMT_S16 = 1
def SAMPLE_FMT_U8 = 2
def SAMPLE_FMT_S24 = 3
def SAMPLE_FMT_S32 = 4
def SAMPLE_FMT_F32 = 5

fn make_memory_source(any: data_ptr, any: byte_len, any: channels, any: rate, any: bits, any: sample_fmt=0, any: format_tag=1): list {
   "Implements `make_memory_source`."
   memory.make(data_ptr, byte_len, channels, rate, bits, sample_fmt, format_tag)
}

fn read(any: src, any: mix_buf, any: frames): int {
   "Reads up to 'frames' from the source into 'mix_buf'."
   if(!is_list(src) || src.len < 3){ return 0 }
   mut data = src.get(1)
   def cursor = __flt_to_int(get_item(data, "cursor", 0.0) + 0.0)
   def total = data.get("total_frames")
   def frame_size = data.get("frame_size")
   def ptr = data.get("ptr")
   mut to_read = int(frames)
   if(cursor + to_read > total){ to_read = total - cursor }
   if(to_read <= 0){ return 0 }
   memcpy(mix_buf, ptr + (cursor * frame_size), to_read * frame_size)
   data = data.set("cursor", cursor + to_read)
   src.set(1, data)
   to_read
}

fn seek(any: src, any: frame): bool {
   "Implements `seek`."
   if(!is_list(src)){ return false }
   mut data = src.get(1)
   def total = data.get("total_frames")
   mut f = frame
   if(f < 0){ f = 0 }
   if(f > total){ f = total }
   data = data.set("cursor", f)
   src.set(1, data)
   true
}

fn tell(any: src): int {
   "Implements `tell`."
   if(!is_list(src)){ return 0 }
   __flt_to_int(get_item(src.get(1), "cursor", 0.0) + 0.0)
}

fn length(any: src): any {
   "Implements `length`."
   _source_meta(src, "total_frames")
}

fn _source_meta(any: src, str: key): any {
   if(!is_list(src)){ return 0 }
   def data = src.get(1, 0)
   if(!is_dict(data)){ return 0 }
   data.get(key, 0)
}

fn format(any: src): list {
   "Returns list of channels, rate, bits"
   if(!is_list(src)){ return [0, 0, 0] }
   def d = src.get(1)
   [d.get("channels"), d.get("rate"), d.get("bits")]
}

fn source_channels(any: src): any {
   "Returns the channel count for `src`."
   _source_meta(src, "channels")
}

fn source_rate(any: src): any {
   "Returns the sample rate for `src`."
   _source_meta(src, "rate")
}

fn source_bits(any: src): any {
   "Returns the sample bit depth for `src`."
   _source_meta(src, "bits")
}

fn source_length(any: src): any {
   "Returns the total frame count for `src`."
   length(src)
}

fn sample_format(any: src): int {
   "Returns source sample format enum."
   if(!is_list(src)){ return 0 }
   def d = src.get(1)
   def sf = d.get("sample_fmt", 0)
   if(sf != 0){ return sf }
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

fn get_item(dict: d, any: key, any: default): any {
   "Returns `d[key]` or `default` when the dictionary lookup yields 0."
   def v = d.get(key)
   if(v == 0){ return default }
   v
}
