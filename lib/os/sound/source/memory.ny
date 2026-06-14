;; Keywords: sound source memory os
;; In-memory sound source construction and sample access.
;; References:
;; - std.os.sound.source
;; - std.os
module std.os.sound.source.memory(make)
use std.core
use std.core.dict
use std.os.sound.diag as sound_debug

fn make(ptr data_ptr, int byte_len, int channels, int rate, int bits, int sample_fmt=0, int format_tag=1) list {
   "Creates a data source from a memory buffer."
   mut data = dict(8)
   data = data.set("ptr", data_ptr)
   data = data.set("len", byte_len)
   data = data.set("channels", channels)
   data = data.set("rate", rate)
   data = data.set("bits", bits)
   if sample_fmt == 0 {
      if bits == 16 { sample_fmt = 1 }
      elif bits == 8 { sample_fmt = 2 }
      elif bits == 24 { sample_fmt = 3 }
      elif bits == 32 { sample_fmt = (format_tag == 3) ? 5 : 4 }
   }
   data = data.set("sample_fmt", sample_fmt)
   data = data.set("format_tag", format_tag)
   data = data.set("cursor", 0.0)
   mut sample_bytes = bits / 8
   if sample_bytes <= 0 { sample_bytes = 1 }
   mut frame_size = sample_bytes * channels
   if frame_size <= 0 { frame_size = channels }
   def total_frames = byte_len / frame_size
   if sound_debug.enabled() { print("SourceMemory: byte_len:", byte_len, "frame_size:", frame_size, "total_frames:", total_frames, "sample_fmt:", sample_fmt) }
   data = data.set("sample_bytes", sample_bytes)
   data = data.set("frame_size", frame_size)
   data = data.set("total_frames", total_frames)
   def vtable = dict(8)
   mut src = list()
   src = src.append("SOUND_SOURCE")
   src = src.append(data)
   src = src.append(vtable)
   src
}
