;; Keywords: sound source synth

module std.os.sound.source.synth (
   make_sine_source,
   make_sine_loop_source,
   make_triangle_source,
   make_square_source,
   make_saw_source,
   make_pulse_source
)

use std.core *
use std.math *
use std.os.sound.source.memory as memory

def WAVE_SINE = 1
def WAVE_TRIANGLE = 2
def WAVE_SQUARE = 3
def WAVE_SAW = 4
def WAVE_PULSE = 5

fn _clamp_unit(x){
   "Internal helper for `clamp_unit`."
   mut v = x + 0.0
   if(v > 1.0){ v = 1.0 }
   if(v < -1.0){ v = -1.0 }
   v
}

fn _clamp_duty(d){
   "Internal helper for `clamp_duty`."
   mut v = d + 0.0
   if(v < 0.01){ v = 0.01 }
   if(v > 0.99){ v = 0.99 }
   v
}

fn _phase_wrap01(x){
   "Internal helper for `phase_wrap01`."
   mut v = x + 0.0
   while(v >= 1.0){ v = v - 1.0 }
   while(v < 0.0){ v = v + 1.0 }
   v
}

fn _poly_blep(t, dt){
   "Internal helper for `poly_blep`."
   mut x = t + 0.0
   mut d = dt + 0.0
   if(d <= 0.0){ return 0.0 }
   if(d > 0.5){ d = 0.5 }
   x = _phase_wrap01(x)
   if(x < d){
      x = x / d
      return x + x - (x * x) - 1.0
   }
   if(x > 1.0 - d){
      x = (x - 1.0) / d
      return (x * x) + x + x + 1.0
   }
   0.0
}

fn _wave_sample(kind, phase, duty, step){
   "Internal helper for `wave_sample`."
   def p = phase / TAU
   mut dt = step / TAU
   if(dt < 0.0){ dt = 0.0 - dt }
   if(kind == WAVE_SINE){ return sin(phase) }
   if(kind == WAVE_TRIANGLE){ return 1.0 - (4.0 * abs(p - 0.5)) }
   if(kind == WAVE_SQUARE){
      mut v = (p < 0.5) ? 1.0 : -1.0
      v = v + _poly_blep(p, dt)
      v = v - _poly_blep(_phase_wrap01(p + 0.5), dt)
      return v
   }
   if(kind == WAVE_SAW){
      mut v = (2.0 * p) - 1.0
      v = v - _poly_blep(p, dt)
      return v
   }
   mut v = (p < duty) ? 1.0 : -1.0
   v = v + _poly_blep(p, dt)
   v = v - _poly_blep(_phase_wrap01(p - duty), dt)
   def dc = (2.0 * duty) - 1.0
   v = v - dc
   def n1 = abs(1.0 - dc)
   def n2 = abs(-1.0 - dc)
   def nmax = (n1 > n2) ? n1 : n2
   if(nmax > 0.000001){ v = v / nmax }
   v
}

fn _pack_wave_s16_mono(kind, total_frames, step, rate, amp, loop_safe, duty){
   "Internal helper for `pack_wave_s16_mono`."
   def channels = 1
   def bits = 16
   def buf_len = total_frames * 2
   def ptr = malloc(buf_len)
   if(!ptr || total_frames <= 0){ return 0 }
   mut phase = 0.0
   mut fade = rate / 50
   if(fade < 1){ fade = 1 }
   if(fade > total_frames / 2){ fade = total_frames / 2 }
   mut i = 0
   while(i < total_frames){
      mut env = 1.0
      if(!loop_safe){
         if(i < fade){ env = i / (fade + 0.0) }
         elif(i >= total_frames - fade){
         env = (total_frames - i) / (fade + 0.0)
         }
      }
      mut val = _wave_sample(kind, phase, duty, step) * (amp + 0.0) * env
      val = _clamp_unit(val)
      def scaled = val * 32767.0
      def s16 = __flt_to_int(scaled + ((scaled >= 0.0) ? 0.5 : -0.5))
      store16(ptr, s16, i * 2)
      phase = phase + step
      while(phase >= TAU){ phase = phase - TAU }
      i += 1
   }
   memory.make(ptr, buf_len, channels, rate, bits, 1, 1)
}

fn _safe_cycles(freq, dur_secs, def_cycles){
   "Internal helper for `safe_cycles`."
   mut c = def_cycles
   if(freq > 0.0 && dur_secs > 0.0){
      c = __flt_to_int((dur_secs + 0.0) * (freq + 0.0) + 0.5)
   }
   if(c < 1){ c = 1 }
   c
}

fn _safe_total_frames(freq, rate, cycles, def_frames){
   "Internal helper for `safe_total_frames`."
   mut tf = def_frames
   if(freq > 0.0 && rate > 0 && cycles > 0){
      tf = __flt_to_int(((cycles + 0.0) * (rate + 0.0) / (freq + 0.0)) + 0.5)
   }
   if(tf < 1){ tf = 1 }
   tf
}

fn _make_wave_source(kind, freq, dur_secs, rate=44100, amp=0.5, loop_safe=false, duty=0.5){
   "Internal helper for `make_wave_source`."
   mut r = rate
   if(r <= 0){ r = 44100 }
   mut a = _clamp_unit(amp)
   mut d = _clamp_duty(duty)
   mut total_frames = __flt_to_int((dur_secs + 0.0) * (r + 0.0))
   if(total_frames < 1){ total_frames = 1 }
   mut step = (TAU * (freq + 0.0)) / (r + 0.0)
   if(loop_safe){
      def cycles = _safe_cycles(freq, dur_secs, 1)
      total_frames = _safe_total_frames(freq, r, cycles, total_frames)
      step = TAU * (cycles + 0.0) / (total_frames + 0.0)
   }
   _pack_wave_s16_mono(kind, total_frames, step, r, a, loop_safe, d)
}

fn _make_wave_loop_source(kind, freq, rate=44100, amp=0.5, cycles=128, duty=0.5){
   "Internal helper for `make_wave_loop_source`."
   mut r = rate
   if(r <= 0){ r = 44100 }
   mut a = _clamp_unit(amp)
   mut d = _clamp_duty(duty)
   mut c = cycles
   if(c < 1){ c = 1 }
   def total_frames = _safe_total_frames(freq, r, c, r)
   def step = TAU * (c + 0.0) / (total_frames + 0.0)
   _pack_wave_s16_mono(kind, total_frames, step, r, a, true, d)
}

fn make_sine_source(freq, dur_secs, rate=44100, amp=0.5, loop_safe=false){
   "Creates a procedural mono sine source. Use loop_safe=true for gapless looping."
   _make_wave_source(WAVE_SINE, freq, dur_secs, rate, amp, loop_safe, 0.5)
}

fn make_sine_loop_source(freq, rate=44100, amp=0.5, cycles=128){
   "Creates a gapless looping mono sine source (whole-number cycles, no fade edges)."
   _make_wave_loop_source(WAVE_SINE, freq, rate, amp, cycles, 0.5)
}

fn make_triangle_source(freq, dur_secs, rate=44100, amp=0.5, loop_safe=false){
   "Creates a mono triangle source."
   _make_wave_source(WAVE_TRIANGLE, freq, dur_secs, rate, amp, loop_safe, 0.5)
}

fn make_square_source(freq, dur_secs, rate=44100, amp=0.5, loop_safe=false){
   "Creates a mono square source."
   _make_wave_source(WAVE_SQUARE, freq, dur_secs, rate, amp, loop_safe, 0.5)
}

fn make_saw_source(freq, dur_secs, rate=44100, amp=0.5, loop_safe=false){
   "Creates a mono ramp-up saw source."
   _make_wave_source(WAVE_SAW, freq, dur_secs, rate, amp, loop_safe, 0.5)
}

fn make_pulse_source(freq, dur_secs, rate=44100, amp=0.5, duty=0.2, loop_safe=false){
   "Creates a mono pulse source. duty is clamped to [0.01, 0.99]."
   _make_wave_source(WAVE_PULSE, freq, dur_secs, rate, amp, loop_safe, duty)
}
