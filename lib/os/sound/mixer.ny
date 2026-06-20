;; Keywords: sound mixer os
;; Sound mixer state and channel-level mixing operations.
;; References:
;; - std.os.sound
;; - std.os
module std.os.sound.mixer(mix_sounds, clamp_s16, load16_s, set_fx_enabled, fx_enabled, set_fx_param, get_fx_param, reset_fx, fx_preset)
use std.core
use std.math
use std.os (mutex_lock, mutex_unlock)
use std.os.sound.diag as sound_debug

mut _debug = -1
mut _acc_buf = 0
mut _acc_frames = 0
mut _fx_enabled = true
mut _fx_drive = 0.08
mut _fx_comp = 0.20
mut _fx_hpf = 28.0
mut _fx_lpf = 16000.0
mut _fx_delay = 0.0
mut _fx_delay_time = 0.22
mut _fx_delay_fb = 0.18
mut _fx_reverb = 0.0
mut _fx_chorus = 0.12
mut _fx_flanger = 0.0
mut _fx_crush = 0.0
mut _fx_trem = 0.0
mut _fx_pan = 0.0
mut _fx_tone = 0.58

mut _fx_lp_l = 0.0
mut _fx_lp_r = 0.0
mut _fx_hp_l = 0.0
mut _fx_hp_r = 0.0
mut _fx_delay_buf = 0
mut _fx_delay_frames = 0
mut _fx_delay_pos = 0
mut _fx_mod_buf = 0
mut _fx_mod_frames = 0
mut _fx_mod_pos = 0
mut _fx_mod_phase = 0.0
mut _fx_motion_phase = 0.0
mut _fx_comp_env_l = 0.0
mut _fx_comp_env_r = 0.0
mut _fx_delay_lp_l = 0.0
mut _fx_delay_lp_r = 0.0
mut _fx_hold_l = 0.0
mut _fx_hold_r = 0.0
mut _fx_hold_i = 0
mut _fx_delay_active = false
mut _fx_mod_active = false
def SAMPLE_FMT_S16 = 1
def SAMPLE_FMT_U8 = 2
def SAMPLE_FMT_S24 = 3
def SAMPLE_FMT_S32 = 4
def SAMPLE_FMT_F32 = 5

fn _lock(any mtx) any { if mtx { mutex_lock(mtx) } }

fn _unlock(any mtx) any { if mtx { mutex_unlock(mtx) } }

fn _ensure_acc(any frames) any {
   if frames <= 0 { return 0 }
   if _acc_buf != 0 && _acc_frames >= frames { return _acc_buf }
   def bytes = frames * 2 * 4
   if _acc_buf == 0 { _acc_buf = malloc(bytes) }
   else { _acc_buf = realloc(_acc_buf, bytes) }
   if _acc_buf != 0 { _acc_frames = frames }
   _acc_buf
}

fn _fmt_from_meta(any bits, any format_tag, any sample_fmt) int {
   if sample_fmt != 0 { return sample_fmt }
   if format_tag == 3 && bits == 32 { return SAMPLE_FMT_F32 }
   if bits == 16 { return SAMPLE_FMT_S16 }
   if bits == 8 { return SAMPLE_FMT_U8 }
   if bits == 24 { return SAMPLE_FMT_S24 }
   if bits == 32 { return SAMPLE_FMT_S32 }
   SAMPLE_FMT_S16
}

fn _load24_s(any p, int off) int {
   def b0, b1 = load8(p, off), load8(p, off + 1)
   def b2 = load8(p, off + 2)
   mut v = b0 | (b1 << 8) | (b2 << 16)
   if (v & 8388608) != 0 { v = v - 16777216 }
   v
}

fn _load32_s(any p, int off) any {
   def v = load32(p, off)
   if v > 2147483647 { return v - 4294967296 }
   v
}

fn _sample_at(any p, any frame_idx, any channel_idx, any channels, any sample_fmt, any bits) f64 {
   def sb = _sample_bytes(sample_fmt, bits)
   def off = ((frame_idx * channels) + channel_idx) * sb
   if sample_fmt == SAMPLE_FMT_U8 { return(load8(p, off) - 128) / 128.0 }
   if sample_fmt == SAMPLE_FMT_S16 { return load16_s(p, off) / 32768.0 }
   if sample_fmt == SAMPLE_FMT_S24 { return _load24_s(p, off) / 8388608.0 }
   if sample_fmt == SAMPLE_FMT_S32 { return _load32_s(p, off) / 2147483648.0 }
   if sample_fmt == SAMPLE_FMT_F32 { return load32_f32(p, off) }
   load16_s(p, off) / 32768.0
}

fn _sample_bytes(any fmt, any bits) int {
   if fmt == SAMPLE_FMT_U8 { return 1 }
   if fmt == SAMPLE_FMT_S16 { return 2 }
   if fmt == SAMPLE_FMT_S24 { return 3 }
   if fmt == SAMPLE_FMT_S32 || fmt == SAMPLE_FMT_F32 { return 4 }
   mut b = bits / 8
   if b <= 0 { b = 2 }
   b
}

fn _read_cursor(any inst) int {
   def raw = inst.get(1)
   __flt_to_int(raw + 0.0)
}

fn _write_cursor(any inst, any val) any { inst[1] = val + 0.0 }

fn _mix_one_s16(any acc, any s_ptr, any s_channels, any total_frames, any cursor_start, any period_frames, any looping, any gain_l, any gain_r, any unit_gain) int {
   mut idx = cursor_start
   mut f = 0
   while f < period_frames {
      if idx >= total_frames { if looping { while idx >= total_frames { idx = idx - total_frames } } else { break } }
      def ai = f * 2
      def al_off = ai * 4
      def ar_off = (ai + 1) * 4
      mut s_l, s_r = load16_s(s_ptr, idx * s_channels * 2), s_l
      if s_channels > 1 { s_r = load16_s(s_ptr, (idx * s_channels + 1) * 2) }
      mut add_l, add_r = s_l, s_r
      if !unit_gain { add_l, add_r = __flt_to_int((s_l + 0.0) * gain_l), __flt_to_int((s_r + 0.0) * gain_r) }
      store32(acc, _load32_s(acc, al_off) + add_l, al_off)
      store32(acc, _load32_s(acc, ar_off) + add_r, ar_off)
      idx += 1
      f += 1
   }
   idx
}

fn _mix_one_f32(any acc, any s_ptr, any s_channels, any total_frames, any cursor_start, any period_frames, any looping, any gain_l, any gain_r) int {
   mut idx = cursor_start
   mut f = 0
   while f < period_frames {
      if idx >= total_frames { if looping { while idx >= total_frames { idx = idx - total_frames } } else { break } }
      def ai = f * 2
      def al_off = ai * 4
      def ar_off = (ai + 1) * 4
      mut s_l, s_r = load32_f32(s_ptr, idx * s_channels * 4), s_l
      if s_channels > 1 { s_r = load32_f32(s_ptr, (idx * s_channels + 1) * 4) }
      def add_l, add_r = __flt_to_int(s_l * gain_l * 32767.0), __flt_to_int(s_r * gain_r * 32767.0)
      store32(acc, _load32_s(acc, al_off) + add_l, al_off)
      store32(acc, _load32_s(acc, ar_off) + add_r, ar_off)
      idx += 1
      f += 1
   }
   idx
}

fn _mix_one_generic(any acc, any s_ptr, any s_channels, any s_fmt, any s_bits, any total_frames, any cursor_start, any period_frames, any looping, any step, any gain_l, any gain_r) int {
   mut current = cursor_start + 0.0
   def total_f = total_frames + 0.0
   mut f = 0
   while f < period_frames {
      if current >= total_f { if looping { while current >= total_f { current = current - total_f } } else { break } }
      def idx = __flt_to_int(current)
      mut idx_next = idx + 1
      if idx_next >= total_frames { idx_next = looping ? 0 : (total_frames - 1) }
      def frac = current - (idx + 0.0)
      mut l0 = _sample_at(s_ptr, idx, 0, s_channels, s_fmt, s_bits)
      mut l1 = _sample_at(s_ptr, idx_next, 0, s_channels, s_fmt, s_bits)
      mut left = l0 + (l1 - l0) * frac
      mut right = left
      if s_channels > 1 {
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

fn set_fx_enabled(bool enabled=true) any { _fx_enabled = enabled }
fn fx_enabled() bool { _fx_enabled }

fn reset_fx() any {
   _fx_enabled = true
   _fx_drive = 0.08
   _fx_comp = 0.20
   _fx_hpf = 28.0
   _fx_lpf = 16000.0
   _fx_delay = 0.0
   _fx_delay_time = 0.22
   _fx_delay_fb = 0.18
   _fx_reverb = 0.0
   _fx_chorus = 0.12
   _fx_flanger = 0.0
   _fx_crush = 0.0
   _fx_trem = 0.0
   _fx_pan = 0.0
   _fx_tone = 0.58
   _fx_lp_l = 0.0
   _fx_lp_r = 0.0
   _fx_hp_l = 0.0
   _fx_hp_r = 0.0
   _fx_mod_phase = 0.0
   _fx_motion_phase = 0.0
   _fx_comp_env_l = 0.0
   _fx_comp_env_r = 0.0
   _fx_delay_lp_l = 0.0
   _fx_delay_lp_r = 0.0
   _fx_hold_i = 0
   _fx_delay_active = false
   _fx_mod_active = false
}

fn set_fx_param(str name, any value) any {
   def v = value + 0.0
   case name {
      "drive", "saturation", "distortion" -> { _fx_drive = clamp01(v) }
      "compress", "compression", "comp" -> { _fx_comp = clamp01(v) }
      "hpf", "highpass", "high_pass" -> { _fx_hpf = clamp(v, 10.0, 12000.0) }
      "lpf", "lowpass", "low_pass" -> { _fx_lpf = clamp(v, 200.0, 22000.0) }
      "delay", "echo" -> { _fx_delay = clamp01(v) }
      "delay_time", "echo_time" -> { _fx_delay_time = clamp(v, 0.025, 1.50) }
      "delay_fb", "feedback" -> { _fx_delay_fb = clamp(v, 0.0, 0.92) }
      "reverb", "space" -> { _fx_reverb = clamp01(v) }
      "chorus" -> { _fx_chorus = clamp01(v) }
      "flanger", "flange" -> { _fx_flanger = clamp01(v) }
      "bitcrush", "crush", "bitcrusher" -> { _fx_crush = clamp01(v) }
      "tremolo", "trem" -> { _fx_trem = clamp01(v) }
      "autopan", "pan_lfo" -> { _fx_pan = clamp01(v) }
      "tone", "air" -> { _fx_tone = clamp01(v) }
      _ -> nil
   }
}

fn get_fx_param(str name) f64 {
   case name {
      "drive", "saturation", "distortion" -> _fx_drive
      "compress", "compression", "comp" -> _fx_comp
      "hpf", "highpass", "high_pass" -> _fx_hpf
      "lpf", "lowpass", "low_pass" -> _fx_lpf
      "delay", "echo" -> _fx_delay
      "delay_time", "echo_time" -> _fx_delay_time
      "delay_fb", "feedback" -> _fx_delay_fb
      "reverb", "space" -> _fx_reverb
      "chorus" -> _fx_chorus
      "flanger", "flange" -> _fx_flanger
      "bitcrush", "crush", "bitcrusher" -> _fx_crush
      "tremolo", "trem" -> _fx_trem
      "autopan", "pan_lfo" -> _fx_pan
      "tone", "air" -> _fx_tone
      _ -> 0.0
   }
}

fn fx_preset(str name) any {
   case name {
      "clean" -> {
_fx_drive = 0.04
         _fx_comp = 0.14
         _fx_hpf = 24.0
         _fx_lpf = 18000.0
_fx_delay = 0.0
         _fx_reverb = 0.0
         _fx_chorus = 0.04
         _fx_flanger = 0.0
         _fx_crush = 0.0
         _fx_trem = 0.0
         _fx_pan = 0.0
      }
      "wide" -> {
_fx_drive = 0.08
         _fx_comp = 0.18
         _fx_hpf = 34.0
         _fx_lpf = 15500.0
_fx_delay = 0.04
         _fx_delay_time = 0.18
         _fx_delay_fb = 0.16
         _fx_reverb = 0.08
         _fx_chorus = 0.28
         _fx_flanger = 0.0
         _fx_crush = 0.0
         _fx_trem = 0.0
         _fx_pan = 0.12
      }
      "echo" -> {
_fx_drive = 0.07
         _fx_comp = 0.22
         _fx_hpf = 42.0
         _fx_lpf = 13000.0
_fx_delay = 0.32
         _fx_delay_time = 0.26
         _fx_delay_fb = 0.36
         _fx_reverb = 0.05
         _fx_chorus = 0.14
         _fx_flanger = 0.0
         _fx_crush = 0.0
         _fx_trem = 0.0
         _fx_pan = 0.06
      }
      "space" -> {
_fx_drive = 0.05
         _fx_comp = 0.18
         _fx_hpf = 55.0
         _fx_lpf = 11500.0
_fx_delay = 0.18
         _fx_delay_time = 0.34
         _fx_delay_fb = 0.30
         _fx_reverb = 0.34
         _fx_chorus = 0.22
         _fx_flanger = 0.0
         _fx_crush = 0.0
         _fx_trem = 0.0
         _fx_pan = 0.12
      }
      "lofi" -> {
_fx_drive = 0.20
         _fx_comp = 0.36
         _fx_hpf = 85.0
         _fx_lpf = 5200.0
_fx_delay = 0.08
         _fx_delay_time = 0.18
         _fx_delay_fb = 0.22
         _fx_reverb = 0.08
         _fx_chorus = 0.10
         _fx_flanger = 0.0
         _fx_crush = 0.36
         _fx_trem = 0.08
         _fx_pan = 0.0
      }
      "crush" -> {
_fx_drive = 0.28
         _fx_comp = 0.42
         _fx_hpf = 120.0
         _fx_lpf = 4200.0
_fx_delay = 0.0
         _fx_reverb = 0.0
         _fx_chorus = 0.02
         _fx_flanger = 0.0
         _fx_crush = 0.62
         _fx_trem = 0.0
         _fx_pan = 0.0
      }
      "flange" -> {
_fx_drive = 0.10
         _fx_comp = 0.22
         _fx_hpf = 40.0
         _fx_lpf = 13500.0
_fx_delay = 0.06
         _fx_delay_time = 0.16
         _fx_delay_fb = 0.18
         _fx_reverb = 0.04
         _fx_chorus = 0.06
         _fx_flanger = 0.50
         _fx_crush = 0.0
         _fx_trem = 0.0
         _fx_pan = 0.10
      }
      _ -> reset_fx()
   }
}

fn _fx_ensure_delay(int rate) any {
   mut frames = rate * 3
   if frames < 4096 { frames = 4096 }
   if _fx_delay_buf != 0 && _fx_delay_frames >= frames { return nil }
   def bytes = frames * 2 * 4
   if _fx_delay_buf == 0 { _fx_delay_buf = malloc(bytes) } else { _fx_delay_buf = realloc(_fx_delay_buf, bytes) }
if _fx_delay_buf != 0 { _fx_delay_frames = frames
   _fx_delay_pos = 0
   memset(_fx_delay_buf, 0, bytes) }
}

fn _fx_ensure_mod(int rate) any {
   mut frames = rate / 8
   if frames < 2048 { frames = 2048 }
   if _fx_mod_buf != 0 && _fx_mod_frames >= frames { return nil }
   def bytes = frames * 2 * 4
   if _fx_mod_buf == 0 { _fx_mod_buf = malloc(bytes) } else { _fx_mod_buf = realloc(_fx_mod_buf, bytes) }
if _fx_mod_buf != 0 { _fx_mod_frames = frames
   _fx_mod_pos = 0
   memset(_fx_mod_buf, 0, bytes) }
}

fn _fx_alpha(f64 hz, f64 rate) f64 {
   def h = clamp(hz, 1.0, rate * 0.45)
   def rc = 1.0 / (6.283185307179586 * h)
   def dt = 1.0 / rate
   clamp01(dt / (rc + dt))
}

fn _fx_soft(f64 x) f64 { x / (1.0 + abs(x) * 0.72) }

fn _fx_quant(f64 x, f64 crush) f64 {
   if crush <= 0.0001 { return x }
   mut bits = int(16.0 - crush * 12.0)
   if bits < 4 { bits = 4 }
   if bits > 16 { bits = 16 }
   def levels = pow(2.0, bits + 0.0) * 0.5 - 1.0
   if levels <= 1.0 { return x }
   def q = x * levels
   __flt_to_int(q + ((q >= 0.0) ? 0.5 : (0.0 - 0.5))) / levels
}

fn _fx_wrap_pos(int pos, int frames) int {
   mut p = pos
   while p < 0 { p += frames }
   while p >= frames { p -= frames }
   p
}

fn _fx_read_delay(any buf, int frames, f64 pos, int ch) f64 {
   if !buf || frames <= 1 { return 0.0 }
   mut p = pos
   def ff = frames + 0.0
   while p < 0.0 { p += ff }
   while p >= ff { p -= ff }
   mut i0 = int(p)
   mut i1 = i0 + 1
   if i1 >= frames { i1 = 0 }
   def t = p - (i0 + 0.0)
   def off = ch * 4
   def a = load32_f32(buf, i0 * 8 + off)
   def b = load32_f32(buf, i1 * 8 + off)
   a + (b - a) * t
}

fn _fx_clear_delay() {
   if _fx_delay_buf && _fx_delay_frames > 0 { memset(_fx_delay_buf, 0, _fx_delay_frames * 8) }
   _fx_delay_lp_l = 0.0
   _fx_delay_lp_r = 0.0
   _fx_delay_active = false
   return
}

fn _fx_clear_mod() {
   if _fx_mod_buf && _fx_mod_frames > 0 { memset(_fx_mod_buf, 0, _fx_mod_frames * 8) }
   _fx_mod_active = false
   return
}

fn _fx_comp_gain(f64 env, f64 amt) f64 {
   if amt <= 0.0001 { return 1.0 }
   def thr = 0.78 - amt * 0.42
   if env <= thr || env <= 0.000001 { return 1.0 }
   def ratio = 1.0 + amt * 6.0
   (thr + (env - thr) / ratio) / env
}

fn _fx_apply(any acc, int period_frames, int out_rate) any {
   if !_fx_enabled { return nil }

   def rate = max(1000.0, out_rate + 0.0)
   def hp_a = _fx_alpha(_fx_hpf, rate)
   def lp_hz = clamp(_fx_lpf * (0.70 + _fx_tone * 0.45), 200.0, rate * 0.46)
   def lp_on = lp_hz < rate * 0.44
   def lp_a = _fx_alpha(lp_hz, rate)
   def use_delay = _fx_delay > 0.0001 || _fx_reverb > 0.0001
   def use_mod = _fx_chorus > 0.0001 || _fx_flanger > 0.0001
   def use_crush = _fx_crush > 0.0001

   if use_delay {
      _fx_ensure_delay(out_rate)
      _fx_delay_active = true
   } elif _fx_delay_active {
      _fx_clear_delay()
   }

   if use_mod {
      _fx_ensure_mod(out_rate)
      _fx_mod_active = true
   } elif _fx_mod_active {
      _fx_clear_mod()
   }

   if !use_crush { _fx_hold_i = 0 }

   def comp_att = 1.0 - exp((0.0 - 1.0) / (max(0.002, 0.004 + _fx_comp * 0.010) * rate))
   def comp_rel = 1.0 - exp((0.0 - 1.0) / (max(0.020, 0.060 + _fx_comp * 0.160) * rate))

   mut f = 0
   while f < period_frames {
      def loff = f * 8
      def roff = loff + 4
      mut l = _load32_s(acc, loff) / 32768.0
      mut r = _load32_s(acc, roff) / 32768.0

      ;; Cleanup EQ: DC/high-pass first, musical one-pole low-pass after.
      _fx_hp_l += hp_a * (l - _fx_hp_l)
      _fx_hp_r += hp_a * (r - _fx_hp_r)
      l = l - _fx_hp_l
      r = r - _fx_hp_r

      if lp_on {
         _fx_lp_l += lp_a * (l - _fx_lp_l)
         _fx_lp_r += lp_a * (r - _fx_lp_r)
         l = _fx_lp_l
         r = _fx_lp_r
      } else {
         _fx_lp_l = l
         _fx_lp_r = r
      }

      ;; Character stage: soft drive with makeup compensation, not harsh clipping.
      if _fx_drive > 0.0001 {
         def pre = 1.0 + _fx_drive * 3.2
         def post = 1.0 / (1.0 + _fx_drive * 1.15)
         l = _fx_soft(l * pre) * post
         r = _fx_soft(r * pre) * post
      }

      ;; Bitcrush is intentionally conservative: fewer DC jumps, sane hold times.
      if use_crush {
         mut hold = int(1.0 + _fx_crush * _fx_crush * 20.0)
         if hold < 1 { hold = 1 }
         if _fx_hold_i <= 0 {
            _fx_hold_l = _fx_quant(l, _fx_crush * 0.86)
            _fx_hold_r = _fx_quant(r, _fx_crush * 0.86)
            _fx_hold_i = hold
         }
         l = _fx_hold_l
         r = _fx_hold_r
         _fx_hold_i -= 1
      }

      _fx_mod_phase += (0.045 + _fx_chorus * 0.12 + _fx_flanger * 0.32) / rate
      _fx_motion_phase += (0.42 + _fx_trem * 4.20 + _fx_pan * 0.56) / rate
      while _fx_mod_phase >= 1.0 { _fx_mod_phase -= 1.0 }
      while _fx_motion_phase >= 1.0 { _fx_motion_phase -= 1.0 }

      ;; Chorus/flanger: fractional short delay, very damped feedback.
      if use_mod && _fx_mod_buf != 0 && _fx_mod_frames > 1 {
         def ph = _fx_mod_phase * 6.283185307179586
         def m1 = (sin(ph) + 1.0) * 0.5
         def m2 = (sin(ph + 3.141592653589793) + 1.0) * 0.5
         def base = 0.0048 + _fx_chorus * 0.010 + _fx_flanger * 0.0010
         def depth = _fx_chorus * 0.0075 + _fx_flanger * 0.0028
         def dl = _fx_read_delay(_fx_mod_buf, _fx_mod_frames, (_fx_mod_pos + 0.0) - (base + m1 * depth) * rate, 0)
         def dr = _fx_read_delay(_fx_mod_buf, _fx_mod_frames, (_fx_mod_pos + 0.0) - (base + m2 * depth) * rate, 1)
         def wet = clamp01(_fx_chorus * 0.18 + _fx_flanger * 0.24)
         def fb = clamp(_fx_flanger * 0.16, 0.0, 0.24)
         store32_f32(_fx_mod_buf, l + dl * fb, _fx_mod_pos * 8)
         store32_f32(_fx_mod_buf, r + dr * fb, _fx_mod_pos * 8 + 4)
         l = l * (1.0 - wet) + dl * wet
         r = r * (1.0 - wet) + dr * wet
         _fx_mod_pos += 1
         if _fx_mod_pos >= _fx_mod_frames { _fx_mod_pos = 0 }
      }

      ;; Echo/space: damped feedback path with crossfeed and lower wet ceiling.
      if use_delay && _fx_delay_buf != 0 && _fx_delay_frames > 1 {
         def dtime = clamp(_fx_delay_time, 0.025, 1.5)
         def dpos = (_fx_delay_pos + 0.0) - dtime * rate
         def hpos = (_fx_delay_pos + 0.0) - max(1.0, dtime * 0.43 * rate)
         def qpos = (_fx_delay_pos + 0.0) - max(1.0, dtime * 0.67 * rate)
         def el = _fx_read_delay(_fx_delay_buf, _fx_delay_frames, dpos, 0)
         def er = _fx_read_delay(_fx_delay_buf, _fx_delay_frames, dpos, 1)
         def hl = _fx_read_delay(_fx_delay_buf, _fx_delay_frames, hpos, 0)
         def hr = _fx_read_delay(_fx_delay_buf, _fx_delay_frames, hpos, 1)
         def ql = _fx_read_delay(_fx_delay_buf, _fx_delay_frames, qpos, 0)
         def qr = _fx_read_delay(_fx_delay_buf, _fx_delay_frames, qpos, 1)
         def wet = clamp01(_fx_delay) * 0.55
         def rv = clamp01(_fx_reverb) * 0.62
         def fb = clamp(_fx_delay_fb * 0.55 + rv * 0.16, 0.0, 0.62)
         _fx_delay_lp_l += 0.11 * ((el * 0.68 + hl * 0.20 + qr * 0.12) - _fx_delay_lp_l)
         _fx_delay_lp_r += 0.11 * ((er * 0.68 + hr * 0.20 + ql * 0.12) - _fx_delay_lp_r)
         def feed_l = _fx_soft(_fx_delay_lp_l * fb)
         def feed_r = _fx_soft(_fx_delay_lp_r * fb)
         store32_f32(_fx_delay_buf, l + feed_l, _fx_delay_pos * 8)
         store32_f32(_fx_delay_buf, r + feed_r, _fx_delay_pos * 8 + 4)
         l = l + el * wet + (hl * 0.24 + qr * 0.14) * rv
         r = r + er * wet + (hr * 0.24 + ql * 0.14) * rv
         _fx_delay_pos += 1
         if _fx_delay_pos >= _fx_delay_frames { _fx_delay_pos = 0 }
      }

      ;; Motion after spatial FX, equal-power-ish autopan and gentle tremolo.
      if _fx_trem > 0.0001 || _fx_pan > 0.0001 {
         def ph = _fx_motion_phase * 6.283185307179586
         def trem = (sin(ph) + 1.0) * 0.5
         def amp = 1.0 - _fx_trem * trem * 0.36
         def pan = sin(ph * 0.73) * _fx_pan * 0.48
         def lg = sqrt(clamp01(0.5 - pan * 0.5)) * 1.41421356
         def rg = sqrt(clamp01(0.5 + pan * 0.5)) * 1.41421356
         l = l * amp * lg
         r = r * amp * rg
      }

      ;; Smooth compressor and safety limiter.
      if _fx_comp > 0.0001 {
         def al = abs(l)
         def ar = abs(r)
         _fx_comp_env_l += (al > _fx_comp_env_l ? comp_att : comp_rel) * (al - _fx_comp_env_l)
         _fx_comp_env_r += (ar > _fx_comp_env_r ? comp_att : comp_rel) * (ar - _fx_comp_env_r)
         l = l * _fx_comp_gain(_fx_comp_env_l, _fx_comp)
         r = r * _fx_comp_gain(_fx_comp_env_r, _fx_comp)
      }

      l = _fx_soft(l * 1.025)
      r = _fx_soft(r * 1.025)
      l = clamp(l, -0.985, 0.985)
      r = clamp(r, -0.985, 0.985)
      store32(acc, __flt_to_int(l * 32767.0), loff)
      store32(acc, __flt_to_int(r * 32767.0), roff)
      f += 1
   }
}

fn mix_sounds(any mix_buf, any buf_size, any active_sounds, any master_vol, any mtx, any out_rate=44100, any out_format=1) list {
   "Implements `mix_sounds`."
   if _debug == -1 { _debug = sound_debug.enabled() ? 1 : 0 }
   def frame_bytes = (out_format == 2) ? 8 : 4
   def period_frames = buf_size / frame_bytes
   if period_frames <= 0 { return [] }
   def acc = _ensure_acc(period_frames)
   if acc == 0 {
      memset(mix_buf, 0, buf_size)
      return []
   }
   memset(acc, 0, period_frames * 2 * 4)
   _lock(mtx)
   mut new_actives = list()
   mut i = 0
   mut out_rate_f = out_rate + 0.0
   if out_rate_f < 1000.0 { out_rate_f = 44100.0 }
   def PI_HALF = 1.570796
   while i < active_sounds.len {
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
      if is_list(sound) && eq(sound.get(0), "SOUND_SOURCE") {
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
      if s_ptr == 0 || total_frames <= 0 { i += 1 continue }
      if s_channels <= 0 { s_channels = 1 }
      def gain = vol * master_vol
      mut gain_l, gain_r = gain, gain
      if abs(pan) > 0.000001 {
         def pan_norm = (pan + 1.0) * 0.5
         gain_l, gain_r = gain * cos(pan_norm * PI_HALF), gain * sin(pan_norm * PI_HALF)
      }
      mut step = (pitch + 0.0) * (s_rate / out_rate_f)
      if step <= 0.000001 { step = 0.000001 }
      def fast_s16 = (s_fmt == SAMPLE_FMT_S16) && (s_channels <= 2) && (abs(step - 1.0) < 0.000001)
      def fast_f32 = (s_fmt == SAMPLE_FMT_F32) && (s_channels <= 2) && (abs(step - 1.0) < 0.000001)
      def unit_gain = (abs(gain_l - 1.0) < 0.000001) && (abs(gain_r - 1.0) < 0.000001)
      mut new_cursor = cursor_i
      if fast_s16 { new_cursor = _mix_one_s16(acc, s_ptr, s_channels, total_frames, cursor_i, period_frames, looping, gain_l, gain_r, unit_gain) } elif fast_f32 {
         new_cursor = _mix_one_f32(acc, s_ptr, s_channels, total_frames, cursor_i, period_frames, looping, gain_l, gain_r)
      } else {
         new_cursor = _mix_one_generic(acc, s_ptr, s_channels, s_fmt, s_bits, total_frames, cursor_i, period_frames, looping, step, gain_l, gain_r)
      }
      if looping || new_cursor < total_frames {
         _write_cursor(inst, new_cursor)
         new_actives = new_actives.append(inst)
      }
      i += 1
   }
   _unlock(mtx)
   _fx_apply(acc, period_frames, int(out_rate_f))
   mut s = 0
   def total_samples = period_frames * 2
   while s < total_samples {
      def off = s * 4
      mut x = _load32_s(acc, off)
      if out_format == 2 {
         mut fv = x / 32768.0
         if fv > 1.0 { fv = 1.0 }
         if fv < -1.0 { fv = -1.0 }
         store32_f32(mix_buf, fv, s * 4)
      } else {
         store16(mix_buf, clamp_s16(x), s * 2)
      }
      s += 1
   }
   new_actives
}

fn get_item(any lst, int idx, any defval) any {
   "Gets item."
   if is_list(lst) && lst.len > idx { return lst.get(idx) }
   defval
}

fn clamp_s16(any v) any {
   "Implements `clamp_s16`."
   if v > 32767 { return 32767 }
   if v < -32768 { return -32768 }
   v
}

fn load16_s(any p, int off) int {
   "Implements `load16_s`."
   def v = load16(p, off)
   if v > 32767 { return v - 65536 }
   v
}
