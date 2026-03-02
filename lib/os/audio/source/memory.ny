;; Keywords: sound source memory

module std.audio.source.memory (
   make
)

use std.core *
use std.core.dict *

fn make(ptr, byte_len, channels, rate, bits, sample_fmt=0, format_tag=1){
   "Creates a data source from a memory buffer."
   mut data = dict(8)
   data = dict_set(data, "ptr", ptr)
   data = dict_set(data, "len", byte_len)
   data = dict_set(data, "channels", channels)
   data = dict_set(data, "rate", rate)
   data = dict_set(data, "bits", bits)
   if(sample_fmt == 0){
      if(bits == 16){ sample_fmt = 1 }
      elif(bits == 8){ sample_fmt = 2 }
      elif(bits == 24){ sample_fmt = 3 }
      elif(bits == 32){ sample_fmt = (format_tag == 3) ? 5 : 4 }
   }
   data = dict_set(data, "sample_fmt", sample_fmt)
   data = dict_set(data, "format_tag", format_tag)
   data = dict_set(data, "cursor", 0.0)
   mut sample_bytes = bits / 8
   if(sample_bytes <= 0){ sample_bytes = 1 }
   mut frame_size = sample_bytes * channels
   if(frame_size <= 0){ frame_size = channels }
   def total_frames = byte_len / frame_size
   if(env("NY_AUDIO_DEBUG")){
      print("SourceMemory: byte_len:", byte_len, "frame_size:", frame_size, "total_frames:", total_frames, "sample_fmt:", sample_fmt)
   }
   data = dict_set(data, "sample_bytes", sample_bytes)
   data = dict_set(data, "frame_size", frame_size)
   data = dict_set(data, "total_frames", total_frames)
   def vtable = dict(8)
   mut src = list()
   src = append(src, "SOUND_SOURCE")
   src = append(src, data)
   src = append(src, vtable)
   src
}
