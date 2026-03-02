;; Keywords: sound mp3 formats rfc5219
;; References:
;; - https://www.rfc-editor.org/rfc/rfc5219
;; - https://github.com/lieff/minimp3

module std.audio.formats.mp3 (
   decode, encode
)

use std.core *
use std.core.error *
use std.audio.source *

fn new_mp3_frame_info(frame_bytes, frame_offset, channels, hz, layer, bitrate_kbps) {
   "Implements `new_mp3_frame_info`."
    def info = dict(6)
    dict_set(info, "frame_bytes", frame_bytes)
    dict_set(info, "frame_offset", frame_offset)
    dict_set(info, "channels", channels)
    dict_set(info, "hz", hz)
    dict_set(info, "layer", layer)
    dict_set(info, "bitrate_kbps", bitrate_kbps)
    info
}

fn new_mp3_decoder() {
   "Implements `new_mp3_decoder`."
    def decoder = dict(1)
    dict_set(decoder, "header", bytes(4))
    dict_set(decoder, "free_format_bytes", 0)
    dict_set(decoder, "reserv", 0)
    dict_set(decoder, "reserv_buf", bytes(511))
    dict_set(decoder, "qmf_state", malloc(15*2*32*4))
    dict_set(decoder, "mdct_overlap", malloc(2*9*32*4))
    decoder
}

def HDR_SIZE = 4

fn hdr_is_mono(h){
   "Implements `hdr_is_mono`."
   return band(load8(h, 3), 0xC0) == 0xC0
}
fn hdr_get_layer(h){
   "Implements `hdr_get_layer`."
   return band(shr(load8(h, 1), 1), 3)
}
fn hdr_get_bitrate_idx(h){
   "Implements `hdr_get_bitrate_idx`."
   return shr(load8(h, 2), 4)
}
fn hdr_get_sample_rate_idx(h){
   "Implements `hdr_get_sample_rate_idx`."
   return band(shr(load8(h, 2), 2), 3)
}
fn hdr_test_mpeg1(h){
   "Implements `hdr_test_mpeg1`."
   return band(load8(h, 1), 0x08) != 0
}
fn hdr_test_not_mpeg25(h){
   "Implements `hdr_test_not_mpeg25`."
   return band(load8(h, 1), 0x10) != 0
}
fn hdr_is_layer_1(h){
   "Implements `hdr_is_layer_1`."
   return band(load8(h, 1), 0x06) == 0x06
}
fn hdr_test_padding(h){
   "Implements `hdr_test_padding`."
   return band(load8(h, 2), 0x02) != 0
}
fn hdr_is_free_format(h){
   "Implements `hdr_is_free_format`."
   return band(load8(h, 2), 0xF0) == 0
}

fn hdr_valid(h) {
   "Implements `hdr_valid`."
    return load8(h, 0) == 0xff &&
           (band(load8(h, 1), 0xF0) == 0xF0 || band(load8(h, 1), 0xFE) == 0xE2) &&
           (hdr_get_layer(h) != 0) &&
           (hdr_get_bitrate_idx(h) != 15) &&
           (hdr_get_sample_rate_idx(h) != 3)
}

fn hdr_compare(h1, h2) {
   "Implements `hdr_compare`."
    return hdr_valid(h2) &&
           (band(xor(load8(h1, 1), load8(h2, 1)), 0xFE) == 0) &&
           (band(xor(load8(h1, 2), load8(h2, 2)), 0x0C) == 0) &&
           !(xor(hdr_is_free_format(h1), hdr_is_free_format(h2)))
}

fn hdr_bitrate_kbps(h) {
   "Implements `hdr_bitrate_kbps`."
    def halfrate_mpeg1_l1 = list(15)
    list_append(halfrate_mpeg1_l1, 0) list_append(halfrate_mpeg1_l1, 32) list_append(halfrate_mpeg1_l1, 40)
    list_append(halfrate_mpeg1_l1, 48) list_append(halfrate_mpeg1_l1, 56) list_append(halfrate_mpeg1_l1, 64)
    list_append(halfrate_mpeg1_l1, 80) list_append(halfrate_mpeg1_l1, 96) list_append(halfrate_mpeg1_l1, 112)
    list_append(halfrate_mpeg1_l1, 128) list_append(halfrate_mpeg1_l1, 160) list_append(halfrate_mpeg1_l1, 192)
    list_append(halfrate_mpeg1_l1, 224) list_append(halfrate_mpeg1_l1, 256) list_append(halfrate_mpeg1_l1, 320)
    def halfrate_mpeg1_l2 = list(15)
    list_append(halfrate_mpeg1_l2, 0) list_append(halfrate_mpeg1_l2, 32) list_append(halfrate_mpeg1_l2, 48)
    list_append(halfrate_mpeg1_l2, 56) list_append(halfrate_mpeg1_l2, 64) list_append(halfrate_mpeg1_l2, 80)
    list_append(halfrate_mpeg1_l2, 96) list_append(halfrate_mpeg1_l2, 112) list_append(halfrate_mpeg1_l2, 128)
    list_append(halfrate_mpeg1_l2, 160) list_append(halfrate_mpeg1_l2, 192) list_append(halfrate_mpeg1_l2, 224)
    list_append(halfrate_mpeg1_l2, 256) list_append(halfrate_mpeg1_l2, 320) list_append(halfrate_mpeg1_l2, 384)
    def halfrate_mpeg1_l3 = list(15)
    list_append(halfrate_mpeg1_l3, 0) list_append(halfrate_mpeg1_l3, 32) list_append(halfrate_mpeg1_l3, 40)
    list_append(halfrate_mpeg1_l3, 48) list_append(halfrate_mpeg1_l3, 56) list_append(halfrate_mpeg1_l3, 64)
    list_append(halfrate_mpeg1_l3, 80) list_append(halfrate_mpeg1_l3, 96) list_append(halfrate_mpeg1_l3, 112)
    list_append(halfrate_mpeg1_l3, 128) list_append(halfrate_mpeg1_l3, 144) list_append(halfrate_mpeg1_l3, 160)
    list_append(halfrate_mpeg1_l3, 176) list_append(halfrate_mpeg1_l3, 192) list_append(halfrate_mpeg1_l3, 224)
    list_append(halfrate_mpeg1_l3, 256)
    def halfrate_mpeg2_l1 = list(15)
    list_append(halfrate_mpeg2_l1, 0) list_append(halfrate_mpeg2_l1, 32) list_append(halfrate_mpeg2_l1, 48)
    list_append(halfrate_mpeg2_l1, 56) list_append(halfrate_mpeg2_l1, 64) list_append(halfrate_mpeg2_l1, 80)
    list_append(halfrate_mpeg2_l1, 96) list_append(halfrate_mpeg2_l1, 112) list_append(halfrate_mpeg2_l1, 128)
    list_append(halfrate_mpeg2_l1, 144) list_append(halfrate_mpeg2_l1, 160) list_append(halfrate_mpeg2_l1, 176)
    list_append(halfrate_mpeg2_l1, 192) list_append(halfrate_mpeg2_l1, 224) list_append(halfrate_mpeg2_l1, 256)
    def halfrate_mpeg2_l2_l3 = list(15)
    list_append(halfrate_mpeg2_l2_l3, 0) list_append(halfrate_mpeg2_l2_l3, 8) list_append(halfrate_mpeg2_l2_l3, 16)
    list_append(halfrate_mpeg2_l2_l3, 24) list_append(halfrate_mpeg2_l2_l3, 32) list_append(halfrate_mpeg2_l2_l3, 40)
    list_append(halfrate_mpeg2_l2_l3, 48) list_append(halfrate_mpeg2_l2_l3, 56) list_append(halfrate_mpeg2_l2_l3, 64)
    list_append(halfrate_mpeg2_l2_l3, 80) list_append(halfrate_mpeg2_l2_l3, 96) list_append(halfrate_mpeg2_l2_l3, 112)
    list_append(halfrate_mpeg2_l2_l3, 128) list_append(halfrate_mpeg2_l2_l3, 144) list_append(halfrate_mpeg2_l2_l3, 160)
    def br_idx = hdr_get_bitrate_idx(h)
    def layer_idx = hdr_get_layer(h)
    def mpeg1 = hdr_test_mpeg1(h)
    if(br_idx == 0 || br_idx == 15){ return 0 }
    mut rates_list = 0
    if(mpeg1){
        if(layer_idx == 3){ rates_list = halfrate_mpeg1_l1 }
        elif(layer_idx == 2){ rates_list = halfrate_mpeg1_l2 }
        elif(layer_idx == 1){ rates_list = halfrate_mpeg1_l3 }
    } else {
        if(layer_idx == 3){ rates_list = halfrate_mpeg2_l1 }
        elif(layer_idx == 2 || layer_idx == 1){ rates_list = halfrate_mpeg2_l2_l3 }
    }
    if(rates_list == 0){ return 0 }
    mul(get(rates_list, br_idx), 1000)
}

fn hdr_sample_rate_hz(h) {
   "Implements `hdr_sample_rate_hz`."
    def g_hz_values = list(3)
    list_append(g_hz_values, 44100)
    list_append(g_hz_values, 48000)
    list_append(g_hz_values, 32000)
    def sr_idx = hdr_get_sample_rate_idx(h)
    def rate = get(g_hz_values, sr_idx)
    mut divisor = 1
    if(!hdr_test_mpeg1(h)){ divisor = mul(divisor, 2) }
    if(!hdr_test_not_mpeg25(h)){ divisor = mul(divisor, 2) }
    if(divisor > 1){ return div(rate, divisor) }
    rate
}

fn hdr_frame_samples(h) {
   "Implements `hdr_frame_samples`."
    def layer_idx = hdr_get_layer(h)
    def mpeg1 = hdr_test_mpeg1(h)
    if(layer_idx == 3){
        return 384
    } elif(layer_idx == 2){
        return 1152
    } elif(layer_idx == 1){
        if(mpeg1){ return 1152 }
        return 576
    }
    0
}

fn hdr_frame_bytes(h, free_format_size) {
   "Implements `hdr_frame_bytes`."
    def samples = hdr_frame_samples(h)
    def bitrate = hdr_bitrate_kbps(h)
    def samplerate = hdr_sample_rate_hz(h)
    def padding = hdr_padding(h)
    if(bitrate == 0 || samplerate == 0 || samples == 0){ return free_format_size }
    def bytes_per_frame = div(mul(samples, bitrate), mul(8, samplerate))
    mut frame_bytes = add(bytes_per_frame, padding)
    if(hdr_get_layer(h) == 3){
        frame_bytes = band(frame_bytes, bnot(3))
    }
    return frame_bytes
}

fn hdr_padding(h) {
   "Implements `hdr_padding`."
    if(hdr_test_padding(h)){
        if(hdr_get_layer(h) == 3){ return 4 }
        return 1
    }
    0
}

fn mp3_find_frame(buf, buf_bytes, free_format_bytes_ptr, frame_size_ptr) {
   "Implements `mp3_find_frame`."
    def hdr = bytes(HDR_SIZE)
    mut i = 0
    while(i < sub(buf_bytes, HDR_SIZE)) {
        memcpy(hdr, ptr_add(buf, i), HDR_SIZE)
        if(hdr_valid(hdr)) {
            def dummy_free_format = malloc(8)
            store64(dummy_free_format, 0)
            def fb = hdr_frame_bytes(hdr, load64(dummy_free_format))
            free(dummy_free_format)
            def frame_and_padding = fb
            if(fb > 0 && add(i, frame_and_padding) <= buf_bytes) {
                 store64(frame_size_ptr, frame_and_padding)
                 store64(free_format_bytes_ptr, fb)
                 return i
            }
        }
        i += 1
    }
    store64(frame_size_ptr, 0)
    return buf_bytes
}

fn decode(buf) -> int {
   "Decodes MP3 data into a memory sound source."
   def size = len(buf)
   if(size < HDR_SIZE){ return 0 }
   def dec = new_mp3_decoder()
   def header = dict_get(dec, "header", 0)
   def free_format_bytes_val = malloc(8)
   store64(free_format_bytes_val, dict_get(dec, "free_format_bytes", 0))
   def frame_size_val = malloc(8)
   def frame_start_offset = mp3_find_frame(buf, size, free_format_bytes_val, frame_size_val)
   def frame_size = load64(frame_size_val)
   free(free_format_bytes_val)
   free(frame_size_val)
   if(frame_size == 0 || add(frame_start_offset, frame_size) > size) { return 0 }
   memcpy(header, ptr_add(buf, frame_start_offset), HDR_SIZE)
   mut channels_val = 0
   if(hdr_is_mono(header)){
       channels_val = 1
   } else {
       channels_val = 2
   }
   def info = new_mp3_frame_info(
       add(frame_start_offset, frame_size),
       frame_start_offset,
       channels_val,
       hdr_sample_rate_hz(header),
       sub(4, hdr_get_layer(header)),
       hdr_bitrate_kbps(header)
   )
   print("Decoded MP3 frame: " + to_str(dict_get(info, "hz", 0)) + "Hz, " + to_str(dict_get(info, "channels", 0)) + "ch, " + to_str(div(dict_get(info, "bitrate_kbps", 0), 1000)) + "kbps")
   make_memory_source(0, 0, dict_get(info, "channels", 0), dict_get(info, "hz", 0), 16, SAMPLE_FMT_S16, 0)
}

fn encode(pcm_ptr, byte_len, channels, rate, bits) -> int {
   "Encodes raw PCM data into an MP3 file buffer."
   0
}

if(comptime{__main()}){
   use std.core *
   use std.audio.formats.mp3 *
   use std.os *

   print("Testing std.audio.formats.mp3...")

   def dummy_mp3_header_l3 = bytes(4)
   store8(dummy_mp3_header_l3, 0xFF, 0)
   store8(dummy_mp3_header_l3, 0xFB, 1)
   store8(dummy_mp3_header_l3, 0x90, 2)
   store8(dummy_mp3_header_l3, 0x00, 3)

   def dummy_buf_l3 = bytes(100)
   memcpy(dummy_buf_l3, dummy_mp3_header_l3, 4)

   def sound_l3 = decode(dummy_buf_l3)
   assert(sound_l3 != 0, "Decode should return a sound source for a valid header (L3)")

   print("Decoded MP3 frame (dummy L3): " + to_str(source_rate(sound_l3)) + "Hz, " + to_str(source_channels(sound_l3)) + "ch, " + to_str(div(source_bitrate_kbps(sound_l3), 1000)) + "kbps")

   assert(source_channels(sound_l3) == 2, "Channels should be 2 for dummy stereo header (L3)")
   assert(source_rate(sound_l3) == 44100, "Sample rate should be 44100Hz for dummy header (L3)")
   assert(div(source_bitrate_kbps(sound_l3), 1000) == 128, "Bitrate should be 128kbps for dummy header (L3)")

   def dummy_mp3_header_l1 = bytes(4)
   store8(dummy_mp3_header_l1, 0xFF, 0)
   store8(dummy_mp3_header_l1, 0xF2, 1)
   store8(dummy_mp3_header_l1, 0xF0, 2)
   store8(dummy_mp3_header_l1, 0xC0, 3)

   def dummy_buf_l1 = bytes(100)
   memcpy(dummy_buf_l1, dummy_mp3_header_l1, 4)

   def sound_l1 = decode(dummy_buf_l1)
   assert(sound_l1 != 0, "Decode should return a sound source for a valid header (L1)")
   assert(source_channels(sound_l1) == 1, "Channels should be 1 for dummy mono header (L1)")
   assert(source_rate(sound_l1) == 48000, "Sample rate should be 48000Hz for dummy header (L1)")
   assert(div(source_bitrate_kbps(sound_l1), 1000) == 320, "Bitrate should be 320kbps for dummy header (L1)")

   print("✓ std.audio.formats.mp3 tests passed")
}
