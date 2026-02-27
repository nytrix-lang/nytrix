;; Keywords: audio flac formats rfc9639
;; References:
;; - https://xiph.org/downloads/
;; - https://www.rfc-editor.org/rfc/rfc9639
;; - https://github.com/xiph/flac/blob/master/src/libFLAC/include/FLAC/format.h

module std.audio.formats.flac (
   decode, encode, get_info
)

use std.core *
use std.core.dict *
use std.os *
use std.audio.source *

def FLAC_SIGNATURE = 0x664C6143

def METADATA_BLOCK_TYPE_STREAMINFO = 0
def METADATA_BLOCK_TYPE_PADDING = 1
def METADATA_BLOCK_TYPE_APPLICATION = 2
def METADATA_BLOCK_TYPE_SEEKTABLE = 3
def METADATA_BLOCK_TYPE_VORBIS_COMMENT = 4
def METADATA_BLOCK_TYPE_CUESHEET = 5
def METADATA_BLOCK_TYPE_PICTURE = 6

def FLAC_FRAME_SYNC_CODE = 0x3FFE

def SUBFRAME_TYPE_CONSTANT = 0
def SUBFRAME_TYPE_VERBATIM = 1
def SUBFRAME_TYPE_FIXED = 8
def SUBFRAME_TYPE_LPC = 32
def SUBFRAME_TYPE_RESERVED = 63

fn _bs_make(data, start_byte_offset, end_byte_offset){
   "Internal helper for `bs_make`."
   def bs = malloc(40)
   store64(bs, data, 0)
   store64(bs, mul(start_byte_offset, 8), 8)
   store64(bs, mul(end_byte_offset, 8), 16)
   store64(bs, 0, 24)
   store64(bs, 0, 32)
   bs
}

fn _bs_free(bs){
   "Internal helper for `bs_free`."
   free(bs)
}

fn _bs_get_bit(bs){
   "Internal helper for `bs_get_bit`."
   mut cache_bits = load64(bs, 32)
   mut current_bit_pos = load64(bs, 8)
   def bit_limit = load64(bs, 16)
   if(current_bit_pos >= bit_limit){ return -1 }
   if(cache_bits == 0){
      def byte_offset = div(current_bit_pos, 8)
      def byte_data = load64(bs, 0)
      store64(bs, load8(byte_data, byte_offset), 24)
      cache_bits = 8
   }
   def current_byte_value = load64(bs, 24)
   def bit = shr(current_byte_value, sub(cache_bits, 1)) band 1
   store64(bs, sub(cache_bits, 1), 32)
   store64(bs, add(current_bit_pos, 1), 8)
   bit
}

fn _bs_get_bits(bs, count){
   "Internal helper for `bs_get_bits`."
   if(count == 0){ return 0 }
   if(count > 64){ return -1 }
   mut value = 0
   mut i = 0
   while(i < count){
      def bit = _bs_get_bit(bs)
      if(bit == -1){ return -1 }
      value = shl(value, 1) bor bit
      i += 1
   }
   value
}

fn _bs_u8(bs){
   "Internal helper for `bs_u8`."
   _bs_get_bits(bs, 8)
}
fn _bs_u16(bs){
   "Internal helper for `bs_u16`."
   _bs_get_bits(bs, 16)
}
fn _bs_u24(bs){
   "Internal helper for `bs_u24`."
   _bs_get_bits(bs, 24)
}
fn _bs_u32(bs){
   "Internal helper for `bs_u32`."
   _bs_get_bits(bs, 32)
}


fn _bs_read_unary_unsigned(bs) {
   "Internal helper for `bs_read_unary_unsigned`."
    mut value = 0
    while(true){
        def bit = _bs_get_bit(bs)
        if(bit == -1){ return -1 }
        if(bit == 0){ break }
        value += 1
    }
    value
}

fn _bs_read_signed_rice(bs, parameter) {
   "Internal helper for `bs_read_signed_rice`."
    def val = _bs_read_unary_unsigned(bs)
    if(val == -1){ return -1 }
    def rest = _bs_get_bits(bs, parameter)
    if(rest == -1){ return -1 }
    def result = shl(val, parameter) bor rest
    if(band(result, 1) == 1){
        return neg(shr(result, 1) + 1)
    }
    shr(result, 1)
}


fn parse_metadata_block_header(data, offset_ptr) {
   "Parses metadata block header."
    def current_offset = load64(offset_ptr)
    if(add(current_offset, 4) > len(data)){ return 0 }
    def head = load8(data, current_offset)
    def is_last = (head & 0x80) != 0
    def block_type = band(head, 0x7F)
    def block_len = (load8(data, add(current_offset, 1)) << 16) | (load8(data, add(current_offset, 2)) << 8) | load8(data, add(current_offset, 3))
    store64(offset_ptr, add(current_offset, 4))
    def header_info = dict(3)
    dict_set(header_info, "is_last", is_last)
    dict_set(header_info, "block_type", block_type)
    dict_set(header_info, "block_len", block_len)
    header_info
}

fn parse_flac_streaminfo(bs) {
   "Parses flac streaminfo."
    def min_blocksize = _bs_get_bits(bs, 16)
    def max_blocksize = _bs_get_bits(bs, 16)
    def min_frame_size = _bs_get_bits(bs, 24)
    def max_frame_size = _bs_get_bits(bs, 24)
    def sample_rate_bits = _bs_get_bits(bs, 20)
    def channels_bits = _bs_get_bits(bs, 3)
    def bits_per_sample_bits = _bs_get_bits(bs, 5)
    def total_samples_bits = _bs_get_bits(bs, 36)
    def channels = add(channels_bits, 1)
    def bits_per_sample_map = list(8)
    list_append(bits_per_sample_map, 0)
    list_append(bits_per_sample_map, 8)
    list_append(bits_per_sample_map, 12)
    list_append(bits_per_sample_map, 16)
    list_append(bits_per_sample_map, 20)
    list_append(bits_per_sample_map, 24)
    list_append(bits_per_sample_map, 0)
    list_append(bits_per_sample_map, 0)
    def bits_per_sample = get(bits_per_sample_map, bits_per_sample_bits)
    if(min_blocksize == -1 || max_blocksize == -1 || sample_rate_bits == -1){ return 0 }
    def info = dict(10)
    dict_set(info, "min_blocksize", min_blocksize)
    dict_set(info, "max_blocksize", max_blocksize)
    dict_set(info, "min_frame_size", min_frame_size)
    dict_set(info, "max_frame_size", max_frame_size)
    dict_set(info, "sample_rate", sample_rate_bits)
    dict_set(info, "channels", channels)
    dict_set(info, "bits_per_sample", bits_per_sample)
    dict_set(info, "total_samples", total_samples_bits)
    info
}


fn parse_flac_frame_header(bs, stream_info) {
   "Parses flac frame header."
    def sync_code = _bs_get_bits(bs, 14)
    if(sync_code != FLAC_FRAME_SYNC_CODE){ return 0 }
    def blocking_strategy = _bs_get_bit(bs)
    def block_size_code = _bs_get_bits(bs, 4)
    def sample_rate_code = _bs_get_bits(bs, 4)
    def channel_assignment_code = _bs_get_bits(bs, 4)
    def sample_size_code = _bs_get_bits(bs, 3)
    def is_k = _bs_get_bit(bs)
    if(blocking_strategy == -1 || block_size_code == -1 || sample_rate_code == -1 || channel_assignment_code == -1 || sample_size_code == -1 || is_k == -1){ return 0 }
    mut block_size = 0
    if(block_size_code == 0){ block_size = 0 }
    elif(block_size_code == 1){ block_size = 192 }
    elif(block_size_code >= 2 && block_size_code <= 5){ block_size = mul(576, shl(1, sub(block_size_code, 2))) }
    elif(block_size_code == 6){ block_size = add(_bs_get_bits(bs, 8), 1) }
    elif(block_size_code == 7){ block_size = add(_bs_get_bits(bs, 16), 1) }
    elif(block_size_code >= 8 && block_size_code <= 15){ block_size = mul(256, shl(1, sub(block_size_code, 8))) }
    mut sample_rate = 0
    def stream_info_rate = dict_get(stream_info, "sample_rate", 0)
    if(sample_rate_code == 0){ sample_rate = stream_info_rate }
    elif(sample_rate_code == 1){ sample_rate = 88200 }
    elif(sample_rate_code == 2){ sample_rate = 176400 }
    elif(sample_rate_code == 3){ sample_rate = 192000 }
    elif(sample_rate_code == 4){ sample_rate = 8000 }
    elif(sample_rate_code == 5){ sample_rate = 16000 }
    elif(sample_rate_code == 6){ sample_rate = 22050 }
    elif(sample_rate_code == 7){ sample_rate = 24000 }
    elif(sample_rate_code == 8){ sample_rate = 32000 }
    elif(sample_rate_code == 9){ sample_rate = 44100 }
    elif(sample_rate_code == 10){ sample_rate = 48000 }
    elif(sample_rate_code == 11){ sample_rate = 96000 }
    elif(sample_rate_code == 12){ sample_rate = add(_bs_get_bits(bs, 8), 1) }
    elif(sample_rate_code == 13){ sample_rate = add(_bs_get_bits(bs, 16), 1) }
    elif(sample_rate_code == 14){ sample_rate = add(_bs_get_bits(bs, 16), 10) }
    else { sample_rate = 0 }
    mut channels = 0
    def stream_info_channels = dict_get(stream_info, "channels", 0)
    if(channel_assignment_code >= 0 && channel_assignment_code <= 7){ channels = add(channel_assignment_code, 1) }
    elif(channel_assignment_code >= 8 && channel_assignment_code <= 10){ channels = 2 }
    else { channels = stream_info_channels }
    mut bits_per_sample = 0
    def stream_info_bits_per_sample = dict_get(stream_info, "bits_per_sample", 0)
    if(sample_size_code == 0){ bits_per_sample = stream_info_bits_per_sample }
    elif(sample_size_code == 1){ bits_per_sample = 8 }
    elif(sample_size_code == 2){ bits_per_sample = 12 }
    elif(sample_size_code == 3){ bits_per_sample = 16 }
    elif(sample_size_code == 4){ bits_per_sample = 20 }
    elif(sample_size_code == 5){ bits_per_sample = 24 }
    else { bits_per_sample = 0 }
    def header_info = dict(5)
    dict_set(header_info, "block_size", block_size)
    dict_set(header_info, "sample_rate", sample_rate)
    dict_set(header_info, "channels", channels)
    dict_set(header_info, "bits_per_sample", bits_per_sample)
    dict_set(header_info, "blocking_strategy", blocking_strategy)
    header_info
}

fn parse_flac_subframe(bs, stream_info, frame_header, channel_idx) {
   "Parses flac subframe."
    def subframe_type_code = _bs_get_bits(bs, 6)
    def wasted_bits_per_sample = _bs_read_unary_unsigned(bs)
    print("Subframe " + to_str(channel_idx) + ": Type Code=" + to_str(subframe_type_code) + ", Wasted=" + to_str(wasted_bits_per_sample))
    mut subframe_type = 0
    mut predictor_order = 0
    if(subframe_type_code == 0){ subframe_type = SUBFRAME_TYPE_CONSTANT }
    elif(subframe_type_code == 1){ subframe_type = SUBFRAME_TYPE_VERBATIM }
    elif(subframe_type_code >= 8 && subframe_type_code <= 31){
        subframe_type = SUBFRAME_TYPE_FIXED
        predictor_order = band(subframe_type_code, 7)
    }
    elif(subframe_type_code >= 32 && subframe_type_code <= 63){
        subframe_type = SUBFRAME_TYPE_LPC
        predictor_order = add(band(subframe_type_code, 31), 1)
    }
    def block_size = dict_get(frame_header, "block_size", 0)
    def bits_per_sample = dict_get(frame_header, "bits_per_sample", 0)
    if(subframe_type == SUBFRAME_TYPE_CONSTANT){
        def sample = _bs_get_bits(bs, bits_per_sample)
        print("Constant Subframe Sample: " + to_str(sample))
    }
    def subframe_info = dict(3)
    dict_set(subframe_info, "type", subframe_type)
    dict_set(subframe_info, "predictor_order", predictor_order)
    dict_set(subframe_info, "wasted_bits", wasted_bits_per_sample)
    subframe_info
}


fn get_info(data){
   "Parses FLAC STREAMINFO metadata. Now native."
   if(!is_str(data)){ return 0 }
   if(len(data) < 4){ return 0 }
   def sig = load32(data, 0)
   if(sig != FLAC_SIGNATURE){ return 0 }
   mut offset_ptr = malloc(8)
   store64(offset_ptr, 4)
   def n = len(data)
   mut stream_info = 0
   while(load64(offset_ptr) < n){
      def header_res = parse_metadata_block_header(data, offset_ptr)
      if(!header_res){ break }
      def is_last = dict_get(header_res, "is_last", false)
      def block_type = dict_get(header_res, "block_type", -1)
      def block_len = dict_get(header_res, "block_len", 0)
      def current_offset = load64(offset_ptr)
      if(add(current_offset, block_len) > n){
          free(offset_ptr)
          return 0
      }
      if(block_type == METADATA_BLOCK_TYPE_STREAMINFO){
         if(block_len < 34){ free(offset_ptr) return 0 }
         def bs = _bs_make(data, current_offset, add(current_offset, block_len))
         stream_info = parse_flac_streaminfo(bs)
         _bs_free(bs)
         if(!stream_info){ free(offset_ptr) return 0 }
      }
      store64(offset_ptr, add(current_offset, block_len))
      if(is_last){ break }
   }
   def data_offset = load64(offset_ptr)
   free(offset_ptr)
   if(stream_info){
      dict_set(stream_info, "data_offset", data_offset)
   }
   stream_info
}

fn decode(data){
   "Decodes FLAC bytes into float32 PCM image dict. (Native Implementation Required)"
   def info = get_info(data)
   if(!info){ return 0 }
   print("FLAC STREAMINFO: " + to_str(dict_get(info, "sample_rate", 0)) + "Hz, " + to_str(dict_get(info, "channels", 0)) + "ch, " + to_str(dict_get(info, "bits_per_sample", 0)) + "bps")
   def bs_audio = _bs_make(data, dict_get(info, "data_offset", 0), len(data))
   def frame_header = parse_flac_frame_header(bs_audio, info)
   if(frame_header){
       print("FLAC Frame Header: Block Size=" + to_str(dict_get(frame_header, "block_size", 0)) +
             ", Sample Rate=" + to_str(dict_get(frame_header, "sample_rate", 0)) + "Hz" +
             ", Channels=" + to_str(dict_get(frame_header, "channels", 0)) +
             ", BPS=" + to_str(dict_get(frame_header, "bits_per_sample", 0)))
       def channels = dict_get(frame_header, "channels", 0)
       mut ch_idx = 0
       while(ch_idx < channels){
           def subframe_data = parse_flac_subframe(bs_audio, info, frame_header, ch_idx)
           ch_idx += 1
       }
   }
   _bs_free(bs_audio)
   make_memory_source(0, 0, dict_get(info, "channels", 0), dict_get(info, "sample_rate", 0), dict_get(info, "bits_per_sample", 0), SAMPLE_FMT_S16, 0)
}

fn encode(pcm, channels, rate, bits){
   "Encodes PCM (bytes/source/dict) to FLAC bytes. (Native Implementation Required)"
   0
}

if(comptime{__main()}){
   use std.audio.formats.flac *
   
   print("Testing std.audio.formats.flac (native metadata & frame header parsing)...")
   assert(get_info("not flac") == 0)

   def dummy_flac_data = bytes(100)
   store32(dummy_flac_data, FLAC_SIGNATURE, 0)

   store8(dummy_flac_data, 0x80 | METADATA_BLOCK_TYPE_STREAMINFO, 4)
   store8(dummy_flac_data, 0x00, 5)
   store8(dummy_flac_data, 0x00, 6)
   store8(dummy_flac_data, 34, 7)

   store8(dummy_flac_data, 0x00, 8)
   store8(dummy_flac_data, 0xC0, 9)
   store8(dummy_flac_data, 0x00, 10)
   store8(dummy_flac_data, 0xC0, 11)
   store8(dummy_flac_data, 0x00, 12)
   store8(dummy_flac_data, 0x00, 13)
   store8(dummy_flac_data, 0x00, 14)
   store8(dummy_flac_data, 0x00, 15)
   store8(dummy_flac_data, 0x00, 16)
   store8(dummy_flac_data, 0x00, 17)
   store8(dummy_flac_data, 0x00, 18)
   store8(dummy_flac_data, 0x00, 19)
   store8(dummy_flac_data, 0x00, 20)
   store8(dummy_flac_data, 0x00, 21)
   store8(dummy_flac_data, 0x00, 22)
   store8(dummy_flac_data, 0x00, 23)
   store8(dummy_flac_data, 0xAC, 24)
   store8(dummy_flac_data, 0x44, 25)
   store8(dummy_flac_data, 0x11, 26)
   store8(dummy_flac_data, 0x00, 27)
   store8(dummy_flac_data, 0x00, 28)
   store8(dummy_flac_data, 0x00, 29)
   store8(dummy_flac_data, 0x00, 30)
   store8(dummy_flac_data, 0x00, 31)
   store8(dummy_flac_data, 0x00, 32)
   store8(dummy_flac_data, 0x00, 33)
   store8(dummy_flac_data, 0x00, 34)
   store8(dummy_flac_data, 0x00, 35)
   store8(dummy_flac_data, 0x00, 36)
   store8(dummy_flac_data, 0x00, 37)
   store8(dummy_flac_data, 0x00, 38)
   store8(dummy_flac_data, 0x00, 39)
   store8(dummy_flac_data, 0x00, 40)
   store8(dummy_flac_data, 0x00, 41)

   def streaminfo_bytes = bytes(34)
   store8(streaminfo_bytes, 0x12, 0)
   store8(streaminfo_bytes, 0x00, 1)
   store8(streaminfo_bytes, 0x12, 2)
   store8(streaminfo_bytes, 0x00, 3)
   store8(streaminfo_bytes, 0x00, 4)
   store8(streaminfo_bytes, 0x00, 5)
   store8(streaminfo_bytes, 0x00, 6)
   store8(streaminfo_bytes, 0x00, 7)
   store8(streaminfo_bytes, 0xAC, 8)
   store8(streaminfo_bytes, 0x44, 9)
   store8(streaminfo_bytes, 0xF0, 10)
   store8(streaminfo_bytes, 0x00, 11)
   store8(streaminfo_bytes, 0x00, 12)
   store8(streaminfo_bytes, 0x00, 13)
   store8(streaminfo_bytes, 0x00, 14)
   store8(streaminfo_bytes, 0x00, 15)
   store8(streaminfo_bytes, 0x00, 16)
   store8(streaminfo_bytes, 0x00, 17)
   store8(streaminfo_bytes, 0x00, 18)
   store8(streaminfo_bytes, 0x00, 19)
   store8(streaminfo_bytes, 0x00, 20)
   store8(streaminfo_bytes, 0x00, 21)
   store8(streaminfo_bytes, 0x00, 22)
   store8(streaminfo_bytes, 0x00, 23)
   store8(streaminfo_bytes, 0x00, 24)
   store8(streaminfo_bytes, 0x00, 25)
   store8(streaminfo_bytes, 0x00, 26)
   store8(streaminfo_bytes, 0x00, 27)
   store8(streaminfo_bytes, 0x00, 28)
   store8(streaminfo_bytes, 0x00, 29)
   store8(streaminfo_bytes, 0x00, 30)
   store8(streaminfo_bytes, 0x00, 31)
   store8(streaminfo_bytes, 0x00, 32)
   store8(streaminfo_bytes, 0x00, 33)

   memcpy(ptr_add(dummy_flac_data, 8), streaminfo_bytes, 34)

   def info = get_info(dummy_flac_data)
   assert(info != 0, "get_info should parse valid FLAC header")
   assert(dict_get(info, "sample_rate", 0) == 44100, "sample_rate should be 44100")
   assert(dict_get(info, "channels", 0) == 2, "channels should be 2")
   assert(dict_get(info, "bits_per_sample", 0) == 16, "bits_per_sample should be 16")
   assert(dict_get(info, "data_offset", 0) == 42, "data_offset should be 42")


   def dummy_frame_header_bytes = bytes(4)
   store8(dummy_frame_header_bytes, 0xFF, 0)
   store8(dummy_frame_header_bytes, 0xF8, 1)
   store8(dummy_frame_header_bytes, 0x81, 2)
   store8(dummy_frame_header_bytes, 0xC0, 3)

   memcpy(ptr_add(dummy_flac_data, 42), dummy_frame_header_bytes, 4)

   def info_for_frame_test = get_info(dummy_flac_data)
   def bs_test_frame = _bs_make(dummy_flac_data, dict_get(info_for_frame_test, "data_offset", 0), len(dummy_flac_data))
   def frame_header = parse_flac_frame_header(bs_test_frame, info_for_frame_test)
   _bs_free(bs_test_frame)

   assert(frame_header != 0, "parse_flac_frame_header should parse valid frame header")
   assert(dict_get(frame_header, "block_size", 0) == 192, "frame_header block_size correct")
   assert(dict_get(frame_header, "sample_rate", 0) == 44100, "frame_header sample_rate correct")
   assert(dict_get(frame_header, "channels", 0) == 2, "frame_header channels correct")
   assert(dict_get(frame_header, "bits_per_sample", 0) == 16, "frame_header bits_per_sample correct")


   def dummy_bs_data = bytes(4)
   store8(dummy_bs_data, 0xE0, 0)
   store8(dummy_bs_data, 0x05, 1)

   def bs_rice = _bs_make(dummy_bs_data, 0, len(dummy_bs_data))
   def unary_val = _bs_read_unary_unsigned(bs_rice)
   assert(unary_val == 3, "read_unary_unsigned should be 3")
   _bs_free(bs_rice)

   def bs_signed_rice = _bs_make(dummy_bs_data, 0, len(dummy_bs_data))
   def signed_rice_val = _bs_read_signed_rice(bs_signed_rice, 2)
   assert(signed_rice_val == 6, "read_signed_rice should be 6")
   _bs_free(bs_signed_rice)

   def dummy_bs_data_neg = bytes(4)
   store8(dummy_bs_data_neg, 0x78, 0)
   store8(dummy_bs_data_neg, 0x0A, 1)

   def bs_signed_rice_neg = _bs_make(dummy_bs_data_neg, 0, len(dummy_bs_data_neg))
   def signed_rice_neg_val = _bs_read_signed_rice(bs_signed_rice_neg, 3)
   assert(signed_rice_neg_val == -8, "read_signed_rice negative value correct")
   _bs_free(bs_signed_rice_neg)

   assert(decode("dummy_flac_data") == 0, "Decode with dummy data should fail (placeholder)")
   assert(encode(0, 0, 0, 0) == 0, "Encode with dummy data should fail (placeholder)")
   
   print("✓ std.audio.formats.flac tests passed")
}
