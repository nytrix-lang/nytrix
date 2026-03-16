;; Keywords: sound mixer
;; Sound mixer state and channel-level mixing operations.
module std.os.sound.mixer(mix_sounds, clamp_s16, load16_s)
use std.core
use std.math
use std.os (mutex_lock, mutex_unlock)
use std.os.sound.diag as sound_debug

mut _debug = -1
mut _acc_buf = 0
mut _acc_frames = 0
def SAMPLE_FMT_S16 = 1
def SAMPLE_FMT_U8 = 2
def SAMPLE_FMT_S24 = 3
def SAMPLE_FMT_S32 = 4
def SAMPLE_FMT_F32 = 5

fn _lock(any: mtx): any { if(mtx){ mutex_lock(mtx) } }

fn _unlock(any: mtx): any { if(mtx){ mutex_unlock(mtx) } }

fn _ensure_acc(any: frames): any {
   if(frames <= 0){ return 0 }
   if(_acc_buf != 0 && _acc_frames >= frames){ return _acc_buf }
   def bytes = frames * 2 * 4
   if(_acc_buf == 0){ _acc_buf = malloc(bytes) }
   else { _acc_buf = realloc(_acc_buf, bytes) }
   if(_acc_buf != 0){ _acc_frames = frames }
   _acc_buf
}

fn _fmt_from_meta(any: bits, any: format_tag, any: sample_fmt): int {
   if(sample_fmt != 0){ return sample_fmt }
   if(format_tag == 3 && bits == 32){ return SAMPLE_FMT_F32 }
   if(bits == 16){ return SAMPLE_FMT_S16 }
   if(bits == 8){ return SAMPLE_FMT_U8 }
   if(bits == 24){ return SAMPLE_FMT_S24 }
   if(bits == 32){ return SAMPLE_FMT_S32 }
   SAMPLE_FMT_S16
}

fn _load24_s(any: p, int: off): int {
   def b0, b1 = load8(p, off), load8(p, off + 1)
   def b2 = load8(p, off + 2)
   mut v = b0 | (b1 << 8) | (b2 << 16)
   if((v & 8388608) != 0){ v = v - 16777216 }
   v
}

fn _load32_s(any: p, int: off): any {
   def v = load32(p, off)
   if(v > 2147483647){ return v - 4294967296 }
   v
}

fn _sample_at(any: p, any: frame_idx, any: channel_idx, any: channels, any: sample_fmt, any: bits): f64 {
   def sb = _sample_bytes(sample_fmt, bits)
   def off = ((frame_idx * channels) + channel_idx) * sb
   if(sample_fmt == SAMPLE_FMT_U8){ return(load8(p, off) - 128) / 128.0 }
   if(sample_fmt == SAMPLE_FMT_S16){ return load16_s(p, off) / 32768.0 }
   if(sample_fmt == SAMPLE_FMT_S24){ return _load24_s(p, off) / 8388608.0 }
   if(sample_fmt == SAMPLE_FMT_S32){ return _load32_s(p, off) / 2147483648.0 }
   if(sample_fmt == SAMPLE_FMT_F32){ return load32_f32(p, off) }
   load16_s(p, off) / 32768.0
}

fn _sample_bytes(any: fmt, any: bits): int {
   if(fmt == SAMPLE_FMT_U8){ return 1 }
   if(fmt == SAMPLE_FMT_S16){ return 2 }
   if(fmt == SAMPLE_FMT_S24){ return 3 }
   if(fmt == SAMPLE_FMT_S32 || fmt == SAMPLE_FMT_F32){ return 4 }
   mut b = bits / 8
   if(b <= 0){ b = 2 }
   b
}

fn _read_cursor(any: inst): int {
   def raw = inst.get(1)
   __flt_to_int(raw + 0.0)
}

fn _write_cursor(any: inst, any: val): any { inst[1] = val + 0.0 }

fn _mix_one_s16(any: acc, any: s_ptr, any: s_channels, any: total_frames, any: cursor_start, any: period_frames, any: looping, any: gain_l, any: gain_r, any: unit_gain): int {
   mut idx = cursor_start
   mut f = 0
   while(f < period_frames){
      if(idx >= total_frames){ if(looping){ while(idx >= total_frames){ idx = idx - total_frames } } else { break } }
      def ai = f * 2
      def al_off = ai * 4
      def ar_off = (ai + 1) * 4
      mut s_l, s_r = load16_s(s_ptr, idx * s_channels * 2), s_l
      if(s_channels > 1){ s_r = load16_s(s_ptr, (idx * s_channels + 1) * 2) }
      mut add_l, add_r = s_l, s_r
      if(!unit_gain){ add_l, add_r = __flt_to_int((s_l + 0.0) * gain_l), __flt_to_int((s_r + 0.0) * gain_r) }
      store32(acc, _load32_s(acc, al_off) + add_l, al_off)
      store32(acc, _load32_s(acc, ar_off) + add_r, ar_off)
      idx += 1
      f += 1
   }
   idx
}

fn _mix_one_f32(any: acc, any: s_ptr, any: s_channels, any: total_frames, any: cursor_start, any: period_frames, any: looping, any: gain_l, any: gain_r): int {
   mut idx = cursor_start
   mut f = 0
   while(f < period_frames){
      if(idx >= total_frames){ if(looping){ while(idx >= total_frames){ idx = idx - total_frames } } else { break } }
      def ai = f * 2
      def al_off = ai * 4
      def ar_off = (ai + 1) * 4
      mut s_l, s_r = load32_f32(s_ptr, idx * s_channels * 4), s_l
      if(s_channels > 1){ s_r = load32_f32(s_ptr, (idx * s_channels + 1) * 4) }
      def add_l, add_r = __flt_to_int(s_l * gain_l * 32767.0), __flt_to_int(s_r * gain_r * 32767.0)
      store32(acc, _load32_s(acc, al_off) + add_l, al_off)
      store32(acc, _load32_s(acc, ar_off) + add_r, ar_off)
      idx += 1
      f += 1
   }
   idx
}

fn _mix_one_generic(any: acc, any: s_ptr, any: s_channels, any: s_fmt, any: s_bits, any: total_frames, any: cursor_start, any: period_frames, any: looping, any: step, any: gain_l, any: gain_r): int {
   mut current = cursor_start + 0.0
   def total_f = total_frames + 0.0
   mut f = 0
   while(f < period_frames){
      if(current >= total_f){ if(looping){ while(current >= total_f){ current = current - total_f } } else { break } }
      def idx = __flt_to_int(current)
      mut idx_next = idx + 1
      if(idx_next >= total_frames){ idx_next = looping ? 0 : (total_frames - 1) }
      def frac = current - (idx + 0.0)
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
      current = current + step
      f += 1
   }
   __flt_to_int(current)
}

fn mix_sounds(any: mix_buf, any: buf_size, any: active_sounds, any: master_vol, any: mtx, any: out_rate=44100, any: out_format=1): list {
   "Implements `mix_sounds`."
   if(_debug == -1){ _debug = sound_debug.enabled() ? 1 : 0 }
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
   while(i < active_sounds.len){
      def inst = active_sounds.get(i)
      def sound = inst.get(0)
      def cursor_i = _read_cursor(inst)
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
      if(is_list(sound) && eq(sound.get(0), "SOUND_SOURCE")){
         def data = sound.get(1)
         s_ptr, total_frames = data.get("ptr"), data.get("total_frames")
         s_channels, s_bits = data.get("channels"), data.get("bits")
         s_rate = data.get("rate", 44100) + 0.0
         s_fmt = _fmt_from_meta(s_bits, data.get("format_tag", 1), data.get("sample_fmt", 0))
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
      mut gain_l, gain_r = gain, gain
      if(abs(pan) > 0.000001){
         def pan_norm = (pan + 1.0) * 0.5
         gain_l, gain_r = gain * cos(pan_norm * PI_HALF), gain * sin(pan_norm * PI_HALF)
      }
      mut step = (pitch + 0.0) * (s_rate / out_rate_f)
      if(step <= 0.000001){ step = 0.000001 }
      def fast_s16 = (s_fmt == SAMPLE_FMT_S16) && (s_channels <= 2) && (abs(step - 1.0) < 0.000001)
      def fast_f32 = (s_fmt == SAMPLE_FMT_F32) && (s_channels <= 2) && (abs(step - 1.0) < 0.000001)
      def unit_gain = (abs(gain_l - 1.0) < 0.000001) && (abs(gain_r - 1.0) < 0.000001)
      mut new_cursor = cursor_i
      if(fast_s16){ new_cursor = _mix_one_s16(acc, s_ptr, s_channels, total_frames, cursor_i, period_frames, looping, gain_l, gain_r, unit_gain) } elif(fast_f32){
         new_cursor = _mix_one_f32(acc, s_ptr, s_channels, total_frames, cursor_i, period_frames, looping, gain_l, gain_r)
      } else {
         new_cursor = _mix_one_generic(acc, s_ptr, s_channels, s_fmt, s_bits, total_frames, cursor_i, period_frames, looping, step, gain_l, gain_r)
      }
      if(looping || new_cursor < total_frames){
         _write_cursor(inst, new_cursor)
         new_actives = new_actives.append(inst)
      }
      i += 1
   }
   _unlock(mtx)
   mut s = 0
   def total_samples = period_frames * 2
   while(s < total_samples){
      def off = s * 4
      mut x = _load32_s(acc, off)
      if(out_format == 2){
         mut fv = x / 32768.0
         if(fv > 1.0){ fv = 1.0 }
         if(fv < -1.0){ fv = -1.0 }
         store32_f32(mix_buf, fv, s * 4)
      } else {
         store16(mix_buf, clamp_s16(x), s * 2)
      }
      s += 1
   }
   new_actives
}

fn get_item(any: lst, int: idx, any: defval): any {
   "Gets item."
   if(is_list(lst) && lst.len > idx){ return lst.get(idx) }
   defval
}

fn clamp_s16(any: v): any {
   "Implements `clamp_s16`."
   if(v > 32767){ return 32767 }
   if(v < -32768){ return -32768 }
   v
}

fn load16_s(any: p, int: off): int {
   "Implements `load16_s`."
   def v = load16(p, off)
   if(v > 32767){ return v - 65536 }
   v
}
