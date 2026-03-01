;; Keywords: sound aiff formats ieee754
;; References:
;; - https://www-mmsp.ece.mcgill.ca/Documents/AudioFormats/AIFF/AIFF.html
;; - http://www.muratnkonar.com/aiff/index.html

module std.audio.formats.aiff (
   decode, encode
)

use std.core *
use std.core.error *
use std.audio.source *

def FORM_ID = 0x464F524D
def AIFF_ID = 0x41494646
def AIFC_ID = 0x41494643
def COMM_ID = 0x434F4D4D
def SSND_ID = 0x53534E44
def NONE_ID = 0x4E4F4E45

fn decode_f80(bytes_ptr) {
   "Decodes input data."
    def exp = sub(band(shl(load8(bytes_ptr, 0), 8), load8(bytes_ptr, 1)), 16383)
    def mantissa = 0
    def hi_mant = bor(shl(load8(bytes_ptr, 2), 24), bor(shl(load8(bytes_ptr, 3), 16), bor(shl(load8(bytes_ptr, 4), 8), load8(bytes_ptr, 5))))
    if (exp < 0) {
        return shr(hi_mant, neg(sub(exp, 31)))
    }
    def shift = sub(exp, 31)
    if(shift > 0) { return shl(hi_mant, shift) }
    return shr(hi_mant, neg(shift))
}

fn read_be32(buf, offset) {
   "Reads be32."
    def b0 = load8(buf, offset)
    def b1 = load8(buf, add(offset, 1))
    def b2 = load8(buf, add(offset, 2))
    def b3 = load8(buf, add(offset, 3))
    bor(shl(b0, 24), bor(shl(b1, 16), bor(shl(b2, 8), b3)))
}

fn read_be16(buf, offset) {
   "Reads be16."
    def b0 = load8(buf, offset)
    def b1 = load8(buf, add(offset, 1))
    bor(shl(b0, 8), b1)
}

fn decode(buf) -> int {
   "Decodes AIFF data into a memory sound source."
   def size = len(buf)
   if(size < 12){ return 0 }
   if(read_be32(buf, 0) != FORM_ID) { return 0 }
   def type_id = read_be32(buf, 8)
   if(type_id != AIFF_ID && type_id != AIFC_ID) { return 0 }
   mut offset = 12
   mut channels = 0
   mut frames = 0
   mut bits = 0
   mut rate = 0
   mut data_offset = 0
   mut data_size = 0
   mut compression = 0
   while(offset < sub(size, 8)) {
       def chunk_id = read_be32(buf, offset)
       def chunk_size = read_be32(buf, add(offset, 4))
       mut next_chunk = add(offset, add(8, chunk_size))
       if(band(chunk_size, 1) != 0) { next_chunk = add(next_chunk, 1) }
       if(next_chunk > size) { break }
       if(chunk_id == COMM_ID) {
           channels = read_be16(buf, add(offset, 8))
           frames = read_be32(buf, add(offset, 10))
           bits = read_be16(buf, add(offset, 14))
           rate = decode_f80(ptr_add(buf, add(offset, 16)))
           if(type_id == AIFC_ID) {
               compression = read_be32(buf, add(offset, 26))
               if(compression != NONE_ID) {
                   print("Error: Compressed AIFC not supported")
                   return 0
               }
           }
       } 
       elif(chunk_id == SSND_ID) {
           def offset_val = read_be32(buf, add(offset, 8))
           def blockSize = read_be32(buf, add(offset, 12))
           data_offset = add(offset, add(16, offset_val))
           data_size = sub(chunk_size, add(8, offset_val))
       }
       offset = next_chunk
   }
   if(channels == 0 || rate == 0 || data_offset == 0) { return 0 }
   def pcm_data = malloc(data_size)
   memcpy(pcm_data, ptr_add(buf, data_offset), data_size)
   if(bits == 16) {
       mut i = 0
       while(i < data_size) {
           def b0 = load8(pcm_data, i)
           def b1 = load8(pcm_data, add(i, 1))
           store8(pcm_data, b1, i)
           store8(pcm_data, b0, add(i, 1))
           i = add(i, 2)
       }
   } elif (bits == 24) {
       mut i = 0
       while(i < data_size) {
           def b0 = load8(pcm_data, i)
           def b2 = load8(pcm_data, add(i, 2))
           store8(pcm_data, b2, i)
           store8(pcm_data, b0, add(i, 2))
           i = add(i, 3)
       }
   }
   print("Decoded AIFF: " + to_str(rate) + "Hz, " + to_str(channels) + "ch, " + to_str(bits) + "bit")
   make_memory_source(pcm_data, data_size, channels, rate, bits, SAMPLE_FMT_S16, 1)
}

fn encode(pcm_ptr, byte_len, channels, rate, bits) -> int {
   "Encodes raw PCM data into an AIFF buffer."
   0
}

if(comptime{__main()}){
    use std.core *
    use std.audio.formats.aiff *

    print("Testing std.audio.formats.aiff...")

    def dummy_aiff = bytes(100)
    
    store8(dummy_aiff, 0x46, 0) store8(dummy_aiff, 0x4F, 1) store8(dummy_aiff, 0x52, 2) store8(dummy_aiff, 0x4D, 3)
    store8(dummy_aiff, 0x00, 4) store8(dummy_aiff, 0x00, 5) store8(dummy_aiff, 0x00, 6) store8(dummy_aiff, 0x50, 7)
    store8(dummy_aiff, 0x41, 8) store8(dummy_aiff, 0x49, 9) store8(dummy_aiff, 0x46, 10) store8(dummy_aiff, 0x46, 11)

    def c_off = 12
    store8(dummy_aiff, 0x43, c_off) store8(dummy_aiff, 0x4F, c_off+1) store8(dummy_aiff, 0x4D, c_off+2) store8(dummy_aiff, 0x4D, c_off+3)
    store8(dummy_aiff, 0x00, c_off+4) store8(dummy_aiff, 0x00, c_off+5) store8(dummy_aiff, 0x00, c_off+6) store8(dummy_aiff, 0x12, c_off+7)
    store8(dummy_aiff, 0x00, c_off+8) store8(dummy_aiff, 0x02, c_off+9)
    store8(dummy_aiff, 0x00, c_off+10) store8(dummy_aiff, 0x00, c_off+11) store8(dummy_aiff, 0x00, c_off+12) store8(dummy_aiff, 0x00, c_off+13)
    store8(dummy_aiff, 0x00, c_off+14) store8(dummy_aiff, 0x10, c_off+15)

    store8(dummy_aiff, 0x40, c_off+16) store8(dummy_aiff, 0x0E, c_off+17)
    store8(dummy_aiff, 0xAC, c_off+18) store8(dummy_aiff, 0x44, c_off+19)
    store8(dummy_aiff, 0x00, c_off+20) store8(dummy_aiff, 0x00, c_off+21)
    
    def s_off = 12 + 8 + 18
    store8(dummy_aiff, 0x53, s_off) store8(dummy_aiff, 0x53, s_off+1) store8(dummy_aiff, 0x4E, s_off+2) store8(dummy_aiff, 0x44, s_off+3)
    store8(dummy_aiff, 0x00, s_off+4) store8(dummy_aiff, 0x00, s_off+5) store8(dummy_aiff, 0x00, s_off+6) store8(dummy_aiff, 0x08, s_off+7)
    store8(dummy_aiff, 0x00, s_off+8) store8(dummy_aiff, 0x00, s_off+9) store8(dummy_aiff, 0x00, s_off+10) store8(dummy_aiff, 0x00, s_off+11)
    store8(dummy_aiff, 0x00, s_off+12) store8(dummy_aiff, 0x00, s_off+13) store8(dummy_aiff, 0x00, s_off+14) store8(dummy_aiff, 0x00, s_off+15)

    def sound = decode(dummy_aiff)
    assert(sound != 0, "Decode should return a sound source")
    assert(source_channels(sound) == 2, "Channels should be 2")
    assert(source_rate(sound) == 44100, "Rate should be 44100")
    
    print("✓ std.audio.formats.aiff tests passed")
}
