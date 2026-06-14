;; Keywords: sound source synth os
;; Synthetic sound source generation for tones, tests, and procedural audio.
;; References:
;; - std.os.sound.source
;; - std.os
module std.os.sound.source.synth(make_sine_source, make_sine_loop_source, make_triangle_source, make_square_source, make_saw_source, make_pulse_source, write_synth_wav)
use std.core
use std.math
use std.os.sound.source as source
use std.os.sound.source.memory as memory

def WAVE_SINE = 1
def WAVE_TRIANGLE = 2
def WAVE_SQUARE = 3
def WAVE_SAW = 4
def WAVE_PULSE = 5
def SYNTH_TAU = 6.2831853071795864769

fn _clamp_unit(any x) f64 {
   mut v = x + 0.0
   if v > 1.0 { v = 1.0 }
   if v < (0.0 - 1.0) { v = 0.0 - 1.0 }
   return v
}

fn _clamp_duty(any d) f64 {
   mut v = d + 0.0
   if v < 0.01 { v = 0.01 }
   if v > 0.99 { v = 0.99 }
   return v
}

fn _phase_wrap01(any x) f64 {
   mut v = x + 0.0
   while v >= 1.0 { v = v - 1.0 }
   while v < 0.0 { v += 1.0 }
   return v
}

fn _poly_blep(any t, any dt) f64 {
   mut x, d = t + 0.0, dt + 0.0
   if d <= 0.0 { return 0.0 }
   if d > 0.5 { d = 0.5 }
   x = _phase_wrap01(x)
   if x < d {
      x = x / d
      return x + x - (x * x) - 1.0
   }
   if x > 1.0 - d {
      x = (x - 1.0) / d
      return(x * x) + x + x + 1.0
   }
   return 0.0
}

fn _wave_sample(int kind, f64 phase, f64 duty, f64 step) f64 {
   def p = phase / SYNTH_TAU
   mut dt = step / SYNTH_TAU
   if dt < 0.0 { dt = 0.0 - dt }
   case kind {
      1 -> sin(phase)
      2 -> 1.0 - (4.0 * abs(p - 0.5))
      3 -> {
         mut v = (p < 0.5) ? 1.0 : (0.0 - 1.0)
         v = v + _poly_blep(p, dt)
         v - _poly_blep(_phase_wrap01(p + 0.5), dt)
      }
      4 -> {
         mut v = (2.0 * p) - 1.0
         v - _poly_blep(p, dt)
      }
      _ -> {
         mut v = (p < duty) ? 1.0 : (0.0 - 1.0)
         v = v + _poly_blep(p, dt)
         v = v - _poly_blep(_phase_wrap01(p - duty), dt)
         def dc = (2.0 * duty) - 1.0
         v = v - dc
         def n1, n2 = abs(1.0 - dc), abs((0.0 - 1.0) - dc)
         def nmax = (n1 > n2) ? n1 : n2
         if nmax > 0.000001 { v = v / nmax }
         v
      }
   }
}

fn _pack_wave_s16_mono(int kind, int total_frames, f64 step, int rate, f64 amp, bool loop_safe, f64 duty) any {
   def channels = 1
   def bits = 16
   def buf_len = total_frames * 2
   def ptr = malloc(buf_len)
   if !ptr || total_frames <= 0 { return 0 }
   mut phase = 0.0
   mut fade = rate / 50
   if fade < 1 { fade = 1 }
   if fade > total_frames / 2 { fade = total_frames / 2 }
   mut i = 0
   while i < total_frames {
      mut env = 1.0
      if !loop_safe {
         if i < fade { env = i / (fade + 0.0) }
         elif i >= total_frames - fade { env = (total_frames - i) / (fade + 0.0) }
      }
      def raw = _wave_sample(kind, phase, duty, step)
      mut val = raw * (amp + 0.0)
      val = val * env
      val = _clamp_unit(val)
      def scaled = val * 32767.0
      def s16 = __flt_to_int(scaled + ((scaled >= 0.0) ? 0.5 : (0.0 - 0.5)))
      store16(ptr, s16, i * 2)
      phase = phase + step
      while phase >= SYNTH_TAU { phase = phase - SYNTH_TAU }
      i += 1
   }
   return memory.make(ptr, buf_len, channels, rate, bits, 1, 1)
}

fn _safe_cycles(any freq, any dur_secs, int def_cycles) int {
   mut c = def_cycles
   if freq > 0.0 && dur_secs > 0.0 { c = __flt_to_int((dur_secs + 0.0) * (freq + 0.0) + 0.5) }
   if c < 1 { c = 1 }
   return c
}

fn _safe_total_frames(any freq, int rate, int cycles, int def_frames) int {
   mut tf = def_frames
   if freq > 0.0 && rate > 0 && cycles > 0 { tf = __flt_to_int(((cycles + 0.0) * (rate + 0.0) / (freq + 0.0)) + 0.5) }
   if tf < 1 { tf = 1 }
   return tf
}

fn _safe_rate(any rate) int {
   mut r = int(rate)
   if r <= 0 { r = 44100 }
   r
}

fn _make_wave_source(int kind, any freq, any dur_secs, any rate=44100, any amp=0.5, bool loop_safe=false, any duty=0.5) any {
   def r = _safe_rate(rate)
   mut a, d = _clamp_unit(amp), _clamp_duty(duty)
   mut total_frames = __flt_to_int((dur_secs + 0.0) * (r + 0.0))
   if total_frames < 1 { total_frames = 1 }
   mut step = (SYNTH_TAU * (freq + 0.0)) / (r + 0.0)
   if loop_safe {
      def cycles = _safe_cycles(freq, dur_secs, 1)
      total_frames = _safe_total_frames(freq, r, cycles, total_frames)
      step = SYNTH_TAU * (cycles + 0.0) / (total_frames + 0.0)
   }
   return _pack_wave_s16_mono(kind, total_frames, step, r, a, loop_safe, d)
}

fn _make_wave_loop_source(int kind, any freq, any rate=44100, any amp=0.5, int cycles=128, any duty=0.5) any {
   def r = _safe_rate(rate)
   mut a, d = _clamp_unit(amp), _clamp_duty(duty)
   mut c = cycles
   if c < 1 { c = 1 }
   def total_frames = _safe_total_frames(freq, r, c, r)
   def step = SYNTH_TAU * (c + 0.0) / (total_frames + 0.0)
   return _pack_wave_s16_mono(kind, total_frames, step, r, a, true, d)
}

fn make_sine_source(any freq, any dur_secs, any rate=44100, any amp=0.5, bool loop_safe=false) any {
   "Creates a procedural mono sine source. Use loop_safe=true for gapless looping."
   return _make_wave_source(WAVE_SINE, freq, dur_secs, rate, amp, loop_safe, 0.5)
}

fn make_sine_loop_source(any freq, any rate=44100, any amp=0.5, int cycles=128) any {
   "Creates a gapless looping mono sine source(whole-number cycles, no fade edges)."
   return _make_wave_loop_source(WAVE_SINE, freq, rate, amp, cycles, 0.5)
}

fn make_triangle_source(any freq, any dur_secs, any rate=44100, any amp=0.5, bool loop_safe=false) any {
   "Creates a mono triangle source."
   return _make_wave_source(WAVE_TRIANGLE, freq, dur_secs, rate, amp, loop_safe, 0.5)
}

fn make_square_source(any freq, any dur_secs, any rate=44100, any amp=0.5, bool loop_safe=false) any {
   "Creates a mono square source."
   return _make_wave_source(WAVE_SQUARE, freq, dur_secs, rate, amp, loop_safe, 0.5)
}

fn make_saw_source(any freq, any dur_secs, any rate=44100, any amp=0.5, bool loop_safe=false) any {
   "Creates a mono ramp-up saw source."
   return _make_wave_source(WAVE_SAW, freq, dur_secs, rate, amp, loop_safe, 0.5)
}

fn make_pulse_source(any freq, any dur_secs, any rate=44100, any amp=0.5, any duty=0.2, bool loop_safe=false) any {
   "Creates a mono pulse source. duty is clamped to [0.01, 0.99]."
   return _make_wave_source(WAVE_PULSE, freq, dur_secs, rate, amp, loop_safe, duty)
}

fn write_synth_wav(any src, any file) bool {
   "Writes a synthesized source to `file` as WAV."
   source.write_wav(src, file)
}

fn _sound_synth_selftest_source(any src, int expected_rate, int expected_frames) bool {
   if !is_list(src) || src.get(0) != "SOUND_SOURCE" { return false }
   if source.source_channels(src) != 1 || source.source_rate(src) != expected_rate { return false }
   if source.source_bits(src) != 16 || source.sample_format(src) != source.SAMPLE_FMT_S16 { return false }
   if source.source_length(src) != expected_frames { return false }
   def data = src.get(1)
   is_dict(data) && data.get("len") == expected_frames * 2 && data.get("ptr") != 0
}

#main {
   def sine = make_sine_source(10.0, 0.1, 1000, 0.5, false)
   assert(_sound_synth_selftest_source(sine, 1000, 100), "sound synth sine")
   assert(load16(sine.get(1).get("ptr"), 0) == 0, "sound synth sine fade")
   assert(_sound_synth_selftest_source(make_sine_loop_source(4.0, 64, 0.8, 8), 64, 128), "sound synth loop sine")
   assert(_sound_synth_selftest_source(make_triangle_source(20.0, 0.05, 1000), 1000, 50), "sound synth triangle")
   assert(_sound_synth_selftest_source(make_square_source(20.0, 0.05, 1000, 0.25, true), 1000, 50), "sound synth square")
   assert(_sound_synth_selftest_source(make_saw_source(20.0, 0.05, 1000), 1000, 50), "sound synth saw")
   assert(_sound_synth_selftest_source(make_pulse_source(20.0, 0.05, 1000, 4.0, -0.5, true), 1000, 50), "sound synth pulse")
   print("✓ std.os.sound.source.synth self-test passed")
}
