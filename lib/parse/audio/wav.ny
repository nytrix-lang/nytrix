;; Keywords: sound wav formats riff rfc2361
;; Reference:
;; - https://en.wikipedia.org/wiki/WAV
;; - https://www.rfc-editor.org/rfc/rfc2361

module std.audio.formats.wav (
   decode, encode
)

use std.core *
use std.core.error *
use std.audio.source *

fn decode(buf){
   "Decodes PCM/float WAV data into a memory sound source."
   def size = len(buf)
   if(size < 44){ return 0 }
   if(load32(buf, 0) != 0x46464952){ return 0 }
   if(load32(buf, 8) != 0x45564157){ return 0 }
   mut off = 12
   mut fmt_found = false
   mut fmt_tag = 0
   mut fmt_subtag = 0
   mut channels = 0
   mut rate = 0
   mut bits = 0
   while(off + 8 < size){
      def id = load32(buf, off)
      def chunk_len = load32(buf, off + 4)
      def next_off = off + 8 + chunk_len
      if(next_off > size){ break }
      if(id == 0x20746D66){
         if(chunk_len < 16){ return 0 }
         fmt_tag = load16(buf, off + 8)
         channels = load16(buf, off + 10)
         rate = load32(buf, off + 12)
         bits = load16(buf, off + 22)
         fmt_subtag = 0
         if(fmt_tag == 0xFFFE && chunk_len >= 40){
            fmt_subtag = load16(buf, off + 32)
         }
         fmt_found = true
      } elif(id == 0x61746164){
         if(!fmt_found){ return 0 }
         if(channels <= 0 || rate <= 0){ return 0 }
         def samples_ptr = malloc(chunk_len)
         if(!samples_ptr){ return 0 }
         memcpy(samples_ptr, ptr_add(buf, off + 8), chunk_len)
         mut sf = 0
         if(fmt_tag == 1){
            if(bits == 8){ sf = SAMPLE_FMT_U8 }
            elif(bits == 16){ sf = SAMPLE_FMT_S16 }
            elif(bits == 24){ sf = SAMPLE_FMT_S24 }
            elif(bits == 32){ sf = SAMPLE_FMT_S32 }
            else { free(samples_ptr) return 0 }
         } elif(fmt_tag == 3){
            if(bits != 32){ free(samples_ptr) return 0 }
            sf = SAMPLE_FMT_F32
         } elif(fmt_tag == 0xFFFE){
            if(fmt_subtag == 1){
               if(bits == 8){ sf = SAMPLE_FMT_U8 }
               elif(bits == 16){ sf = SAMPLE_FMT_S16 }
               elif(bits == 24){ sf = SAMPLE_FMT_S24 }
               elif(bits == 32){ sf = SAMPLE_FMT_S32 }
               else { free(samples_ptr) return 0 }
            } elif(fmt_subtag == 3){
               if(bits != 32){ free(samples_ptr) return 0 }
               sf = SAMPLE_FMT_F32
            } else {
               free(samples_ptr)
               return 0
            }
         } else {
            free(samples_ptr)
            return 0
         }
         return make_memory_source(samples_ptr, chunk_len, channels, rate, bits, sf, fmt_tag)
      }
      off = next_off
      if((chunk_len & 1) != 0){ off += 1 }
   }
   0
}

fn encode(pcm_ptr, byte_len, channels, rate, bits){
   "Encodes raw PCM data into a WAV file buffer."
   def total_size = 44 + byte_len
   mut buf = malloc(total_size)
   if(!buf){ return 0 }
   store32(buf, 0x46464952, 0)
   store32(buf, total_size - 8, 4)
   store32(buf, 0x45564157, 8)
   store32(buf, 0x20746D66, 12)
   store32(buf, 16, 16)
   store16(buf, 1, 20)
   store16(buf, channels, 22)
   store32(buf, rate, 24)
   store32(buf, rate * channels * (bits / 8), 28)
   store16(buf, channels * (bits / 8), 32)
   store16(buf, bits, 34)
   store32(buf, 0x61746164, 36)
   store32(buf, byte_len, 40)
   memcpy(ptr_add(buf, 44), pcm_ptr, byte_len)
   def b = bytes(total_size)
   memcpy(b, buf, total_size)
   free(buf)
   b
}

if(comptime{__main()}){
   use std.core *
   use std.audio.formats.wav *
   print("Testing std.audio.formats.wav...")
   def rate = 44100
   def channels = 2
   def bits = 16
   def byte_len = 1024
   def pcm = malloc(byte_len)
   memset(pcm, 0, byte_len)

   def encoded = encode(pcm, byte_len, channels, rate, bits)
   assert(len(encoded) > 44, "WAV header + data")

   def s = decode(encoded)
   assert(s != 0, "Decode successful")
   free(pcm)
   print("✓ std.audio.formats.wav tests passed")
}
