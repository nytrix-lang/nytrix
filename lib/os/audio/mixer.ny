;; Keywords: sound mixer

module std.os.audio.mixer (
   mix_sounds, clamp_s16, load16_s
)

use std.core *
use std.math *

mut _debug = -1
mut _acc_buf = 0
mut _acc_frames = 0

def SAMPLE_FMT_S16 = 1
def SAMPLE_FMT_U8 = 2
def SAMPLE_FMT_S24 = 3
def SAMPLE_FMT_S32 = 4
def SAMPLE_FMT_F32 = 5

fn _lock(mtx){
   "Internal helper for `lock`."
   if(mtx){ mutex_lock(mtx) }
}

fn _unlock(mtx){
   "Internal helper for `unlock`."
   if(mtx){ mutex_unlock(mtx) }
}

fn _ensure_acc(frames){
   "Internal helper for `ensure_acc`."
   if(frames <= 0){ return 0 }
   if(_acc_buf != 0 && _acc_frames >= frames){ return _acc_buf }
   def bytes = frames * 2 * 4
   if(_acc_buf == 0){ _acc_buf = malloc(bytes) }
   else { _acc_buf = realloc(_acc_buf, bytes) }
   if(_acc_buf != 0){ _acc_frames = frames }
   _acc_buf
}

fn _fmt_from_meta(bits, format_tag, sample_fmt){
   "Internal helper for `fmt_from_meta`."
   if(sample_fmt != 0){ return sample_fmt }
   if(format_tag == 3 && bits == 32){ return SAMPLE_FMT_F32 }
   if(bits == 16){ return SAMPLE_FMT_S16 }
   if(bits == 8){ return SAMPLE_FMT_U8 }
   if(bits == 24){ return SAMPLE_FMT_S24 }
   if(bits == 32){ return SAMPLE_FMT_S32 }
   SAMPLE_FMT_S16
}

fn _sample_bytes(fmt, bits){
   "Internal helper for `sample_bytes`."
   if(fmt == SAMPLE_FMT_U8){ return 1 }
   if(fmt == SAMPLE_FMT_S16){ return 2 }
   if(fmt == SAMPLE_FMT_S24){ return 3 }
   if(fmt == SAMPLE_FMT_S32 || fmt == SAMPLE_FMT_F32){ return 4 }
   mut b = bits / 8
   if(b <= 0){ b = 2 }
   b
}

fn _load24_s(ptr, off){
   "Internal helper for `load24_s`."
   def b0 = load8(ptr, off)
   def b1 = load8(ptr, off + 1)
   def b2 = load8(ptr, off + 2)
   mut v = b0 | (b1 << 8) | (b2 << 16)
   if((v & 8388608) != 0){ v = v - 16777216 }
   v
}

fn _load32_s(ptr, off){
   "Internal helper for `load32_s`."
   def v = load32(ptr, off)
   if(v > 2147483647){ return v - 4294967296 }
   v
}

fn _sample_at(ptr, frame_idx, channel_idx, channels, sample_fmt, bits){
   "Internal helper for `sample_at`."
   def sb = _sample_bytes(sample_fmt, bits)
   def off = ((frame_idx * channels) + channel_idx) * sb
   if(sample_fmt == SAMPLE_FMT_U8){
      return (load8(ptr, off) - 128) / 128.0
   }
   if(sample_fmt == SAMPLE_FMT_S16){
      return load16_s(ptr, off) / 32768.0
   }
   if(sample_fmt == SAMPLE_FMT_S24){
      return _load24_s(ptr, off) / 8388608.0
   }
   if(sample_fmt == SAMPLE_FMT_S32){
      return _load32_s(ptr, off) / 2147483648.0
   }
   if(sample_fmt == SAMPLE_FMT_F32){
      return load32_f32(ptr, off)
   }
   load16_s(ptr, off) / 32768.0
}

fn mix_sounds(mix_buf, buf_size, active_sounds, master_vol, mtx, out_rate=44100, out_format=1){
   "Implements `mix_sounds`."
   if(_debug == -1){
      def env_val = env("NY_AUDIO_DEBUG")
      _debug = (env_val && (eq(env_val, "1") || eq(env_val, "true"))) ? 1 : 0
   }
   def frame_bytes = (out_format == 2) ? 8 : 4
   def period_frames = buf_size / frame_bytes
   if(period_frames <= 0){ return [] }
   def acc = _ensure_acc(period_frames)
   if(acc == 0){
      memset(mix_buf, 0, buf_size)
      return []
   }
   memset(acc, 0, period_frames * 2 * 4)
   _lock(mtx)
   mut new_actives = list()
   mut i = 0
   mut out_rate_f = out_rate + 0.0
   if(out_rate_f < 1000.0){ out_rate_f = 44100.0 }
   def PI_HALF = 1.570796
   while(i < len(active_sounds)){
      def inst = get(active_sounds, i)
      def sound = get(inst, 0)
      mut cursor = get_item(inst, 1, 0.0)
      def pitch = get_item(inst, 2, 1.0)
      def vol = get_item(inst, 3, 1.0)
      def looping = get_item(inst, 4, false)
      def pan = get_item(inst, 5, 0.0)
      mut s_ptr = 0
      mut total_frames = 0
      mut s_channels = 2
      mut s_bits = 16
      mut s_rate = 44100.0
      mut s_fmt = SAMPLE_FMT_S16
      if(is_list(sound) && eq(get(sound, 0), "SOUND_SOURCE")){
         def data = get(sound, 1)
         s_ptr = get(data, "ptr")
         total_frames = get(data, "total_frames")
         s_channels = get(data, "channels")
         s_bits = get(data, "bits")
         s_rate = get(data, "rate", 44100) + 0.0
         s_fmt = _fmt_from_meta(s_bits, get(data, "format_tag", 1), get(data, "sample_fmt", 0))
      } else {
         s_ptr = get_item(sound, 4, 0)
         total_frames = get_item(sound, 5, 0) / 4
         s_channels = 2
         s_bits = 16
         s_rate = 44100.0
         s_fmt = SAMPLE_FMT_S16
      }
      if(s_ptr == 0 || total_frames <= 0){ i += 1 continue }
      if(s_channels <= 0){ s_channels = 1 }
      def gain = vol * master_vol
      mut gain_l = gain
      mut gain_r = gain
      if(abs(pan) > 0.000001){
         def pan_norm = (pan + 1.0) * 0.5
         gain_l = gain * cos(pan_norm * PI_HALF)
         gain_r = gain * sin(pan_norm * PI_HALF)
      }
      mut step = (pitch + 0.0) * (s_rate / out_rate_f)
      if(step <= 0.000001){ step = 0.000001 }
      def fast_s16 = (s_fmt == SAMPLE_FMT_S16) && (s_channels <= 2) && (abs(step - 1.0) < 0.000001)
      def fast_f32 = (s_fmt == SAMPLE_FMT_F32) && (s_channels <= 2) && (abs(step - 1.0) < 0.000001)
      def unit_gain = (abs(gain_l - 1.0) < 0.000001) && (abs(gain_r - 1.0) < 0.000001)
      mut f = 0
      mut current_cursor = cursor + 0.0
      def total_frames_f = total_frames + 0.0
      if(fast_s16){
         mut idx_i = __flt_to_int(current_cursor)
         while(f < period_frames){
            if(idx_i >= total_frames){
               if(looping){
                  while(idx_i >= total_frames){ idx_i = idx_i - total_frames }
               } else { break }
            }
            def ai = f * 2
            def al_off = ai * 4
            def ar_off = (ai + 1) * 4
            mut s_l = load16_s(s_ptr, idx_i * s_channels * 2)
            mut s_r = s_l
            if(s_channels > 1){
               s_r = load16_s(s_ptr, (idx_i * s_channels + 1) * 2)
            }
            mut add_l = s_l
            mut add_r = s_r
            if(!unit_gain){
               add_l = __flt_to_int((s_l + 0.0) * gain_l)
               add_r = __flt_to_int((s_r + 0.0) * gain_r)
            }
            store32(acc, _load32_s(acc, al_off) + add_l, al_off)
            store32(acc, _load32_s(acc, ar_off) + add_r, ar_off)
            idx_i += 1
            f += 1
         }
         current_cursor = idx_i + 0.0
      } elif(fast_f32){
         mut idx_i = __flt_to_int(current_cursor)
         while(f < period_frames){
            if(idx_i >= total_frames){
               if(looping){
                  while(idx_i >= total_frames){ idx_i = idx_i - total_frames }
               } else { break }
            }
            def ai = f * 2
            def al_off = ai * 4
            def ar_off = (ai + 1) * 4
            mut s_l = load32_f32(s_ptr, idx_i * s_channels * 4)
            mut s_r = s_l
            if(s_channels > 1){
               s_r = load32_f32(s_ptr, (idx_i * s_channels + 1) * 4)
            }
            def add_l = __flt_to_int(s_l * gain_l * 32767.0)
            def add_r = __flt_to_int(s_r * gain_r * 32767.0)
            store32(acc, _load32_s(acc, al_off) + add_l, al_off)
            store32(acc, _load32_s(acc, ar_off) + add_r, ar_off)
            idx_i += 1
            f += 1
         }
         current_cursor = idx_i + 0.0
      } else {
         while(f < period_frames){
            if(current_cursor >= total_frames_f){
               if(looping){
                  while(current_cursor >= total_frames_f){ current_cursor = current_cursor - total_frames_f }
               } else { break }
            }
            def idx = __flt_to_int(current_cursor)
            mut idx_next = idx + 1
            if(idx_next >= total_frames){
               idx_next = looping ? 0 : (total_frames - 1)
            }
            def frac = current_cursor - (idx + 0.0)
            mut l0 = _sample_at(s_ptr, idx, 0, s_channels, s_fmt, s_bits)
            mut l1 = _sample_at(s_ptr, idx_next, 0, s_channels, s_fmt, s_bits)
            mut left = l0 + (l1 - l0) * frac
            mut right = left
            if(s_channels > 1){
               mut r0 = _sample_at(s_ptr, idx, 1, s_channels, s_fmt, s_bits)
               mut r1 = _sample_at(s_ptr, idx_next, 1, s_channels, s_fmt, s_bits)
               right = r0 + (r1 - r0) * frac
            }
            def ai = f * 2
            def al_off = ai * 4
            def ar_off = (ai + 1) * 4
            def add_l = __flt_to_int(left * gain_l * 32767.0)
            def add_r = __flt_to_int(right * gain_r * 32767.0)
            store32(acc, _load32_s(acc, al_off) + add_l, al_off)
            store32(acc, _load32_s(acc, ar_off) + add_r, ar_off)
            current_cursor = current_cursor + step
            f += 1
         }
      }
      if(looping || current_cursor < total_frames_f){
         store_item(inst, 1, current_cursor)
         new_actives = append(new_actives, inst)
      }
      i += 1
   }
   _unlock(mtx)
   mut peak_l = 0
   mut peak_r = 0
   mut s = 0
   def total_samples = period_frames * 2
   while(s < total_samples){
      def off = s * 4
      mut x = _load32_s(acc, off)
      if((s & 1) == 0){
         if(abs(x) > peak_l){ peak_l = abs(x) }
      } else {
         if(abs(x) > peak_r){ peak_r = abs(x) }
      }
      if(out_format == 2){
         mut f = x / 32768.0
         if(f > 1.0){ f = 1.0 }
         if(f < -1.0){ f = -1.0 }
         store32_f32(mix_buf, f, s * 4)
      } else {
         store16(mix_buf, clamp_s16(x), s * 2)
      }
      s += 1
   }
   if(_debug && (peak_l > 32000 || peak_r > 32000)){
      print(f"MIXER: high peak L={peak_l} R={peak_r}")
   }
   new_actives
}

fn get_item(lst, idx, defval){
   "Gets item."
   if(is_list(lst) && len(lst) > idx){ return get(lst, idx) }
   defval
}

fn clamp_s16(v){
   "Implements `clamp_s16`."
   if(v > 32767){ return 32767 }
   if(v < -32768){ return -32768 }
   v
}

fn load16_s(ptr, off) {
   "Implements `load16_s`."
   def v = load16(ptr, off)
   if(v > 32767){ return v - 65536 }
   v
}
