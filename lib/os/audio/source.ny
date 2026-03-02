;; Keywords: sound source

module std.os.audio.source (
    make_memory_source,
    read, seek, tell, length, format, sample_format,
    source_channels, source_rate, source_bits, source_length,
    SAMPLE_FMT_S16, SAMPLE_FMT_U8, SAMPLE_FMT_S24, SAMPLE_FMT_S32, SAMPLE_FMT_F32
)

use std.core *
use std.core.dict *
use std.os.audio.source.memory as memory

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

fn source_channels(src){
   if(!is_list(src)){ return 0 }
   get(get(src, 1), "channels", 0)
}

fn source_rate(src){
   if(!is_list(src)){ return 0 }
   get(get(src, 1), "rate", 0)
}

fn source_bits(src){
   if(!is_list(src)){ return 0 }
   get(get(src, 1), "bits", 0)
}

fn source_length(src){
   length(src)
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

if(comptime{__main()}){
   use std.core.test *
   use std.core.mem *

   print("Running std.os.audio.source tests...")

   fn create_test_data(size){
       def ptr = malloc(size)
       mut i = 0
       while(i < size){
           store8(ptr, i % 255, i)
           i += 1
       }
       ptr
   }

   ; Test 1: Basic Read
   {
       print("Test 1: Basic Read")
       def data_size = 100
       def ptr = create_test_data(data_size)
       ; 1 channel, 44100 rate, 8 bits (1 byte per frame)
       def src = make_memory_source(ptr, data_size, 1, 44100, 8)
       def buf = malloc(100)

       def read_count = read(src, buf, 10)
       t_assert_eq(read_count, 10, "read 10 frames")

       t_assert(memcmp(ptr, buf, 10) == 0, "buffer content matches source")

       free(buf)
       free(ptr)
   }

   ; Test 2: Partial Read (near end)
   {
       print("Test 2: Partial Read")
       def data_size = 15
       def ptr = create_test_data(data_size)
       def src = make_memory_source(ptr, data_size, 1, 44100, 8)
       def buf = malloc(20)

       read(src, buf, 10) ; Read 10, cursor at 10
       def read_count = read(src, buf, 10) ; Try to read 10 more, only 5 left

       t_assert_eq(read_count, 5, "read remaining 5 frames")

       ; verify content of last 5 bytes
       t_assert(memcmp(ptr + 10, buf, 5) == 0, "last 5 bytes match")

       free(buf)
       free(ptr)
   }

   ; Test 3: Empty Read (at end)
   {
       print("Test 3: Empty Read")
       def data_size = 10
       def ptr = create_test_data(data_size)
       def src = make_memory_source(ptr, data_size, 1, 44100, 8)
       def buf = malloc(10)

       read(src, buf, 10) ; Read all
       def read_count = read(src, buf, 10) ; Try to read more

       t_assert_eq(read_count, 0, "read 0 frames at end")

       free(buf)
       free(ptr)
   }

   ; Test 4: Invalid Source
   {
       print("Test 4: Invalid Source")
       def buf = malloc(10)
       t_assert_eq(read(list(), buf, 10), 0, "read from empty list returns 0")
       t_assert_eq(read(0, buf, 10), 0, "read from 0 returns 0")
       free(buf)
   }

   print("✓ std.os.audio.source tests passed")
}
