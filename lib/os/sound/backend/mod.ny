;; Keywords: sound backend pulse jack io input output stream alsa shared winmm
;; Sound backend facade for ALSA, PulseAudio, JACK, WinMM, and shared backend selection.
module std.os.sound.backend(init, shutdown, play, stop, is_playing, set_master_volume, get_master_volume, get_backend_name)
use std.core
use std.math (abs)
use std.os
use std.os.thread
use std.os.time
use std.core.str
use std.core.common as common
use std.os.sound.diag as sound_debug
use std.os.sound.backend.io (
   create, connect, disconnect, get_output_device_count, get_output_device, get_default_output_device_index,
   outstream_create, outstream_open, outstream_start, outstream_write_frames, outstream_destroy,
   FORMAT_S16LE, FORMAT_FLOAT32LE, get_backend_name as io_backend_name,
)

use std.os.sound.mixer as mixer

mut _ctx = 0
mut _stream = 0
mut _running = false
mut _thread = 0
mut _mtx = 0
mut _active_sounds = []
mut _master_vol = 1.0
mut _debug_mix = -1
mut _async_mode = -1
mut _period_frames = 0
mut _out_rate = 0
mut _out_format = FORMAT_S16LE
mut _prime_periods = -1
mut _idle_sleep_ms = 0

fn _is_debug(): bool { sound_debug.enabled() }

fn _is_mix_debug(): bool {
   _debug_mix = common.cached_env_present(_debug_mix, "NY_AUDIO_DEBUG_MIX")
   _debug_mix == 1
}

fn _is_async_mode(): bool {
   if(_async_mode == -1){
      def v = common.env_lower("NY_AUDIO_ASYNC")
      if(v.len == 0){
         if(_ctx && eq(io_backend_name(_ctx), "pulse")){ _async_mode = 0 } else { _async_mode = 1 }
      } else {
         case v {
            "0", "false", "off", "no" -> { _async_mode = 0 }
            _ -> { _async_mode = 1 }
         }
      }
   }
   _async_mode == 1
}

fn _get_period_frames(): int {
   if(_period_frames > 0){ return _period_frames }
   mut pf = 1024
   if(_ctx && eq(io_backend_name(_ctx), "alsa")){ pf = 2048 } elif(_ctx && eq(io_backend_name(_ctx), "pulse")){ pf = 1024 }
   pf = common.env_int_clamped("NY_AUDIO_PERIOD_FRAMES", pf, 128, 4096)
   _period_frames = pf
   _period_frames
}

fn _get_output_rate(): int {
   if(_out_rate > 0){ return _out_rate }
   mut r, v = 48000, common.env_trim("NY_AUDIO_SAMPLE_RATE")
   if(v.len == 0){ v = common.env_trim("NY_AUDIO_RATE") }
   if(v.len > 0){
      def n = atoi(v)
      if(n >= 8000 && n <= 192000){ r = n }
   }
   _out_rate = r
   _out_rate
}

fn _get_output_format_pref(): int {
   def s = common.env_lower("NY_AUDIO_FORMAT")
   if(s == "s16" || s == "s16le" || s == "int16"){ return FORMAT_S16LE }
   if(s == "f32" || s == "float32" || s == "float32le"){ return FORMAT_FLOAT32LE }
   FORMAT_S16LE
}

fn _frame_bytes(): int {
   if(_out_format == FORMAT_FLOAT32LE){ return 8 }
   4
}

fn _get_prime_periods(): int {
   if(_prime_periods >= 0){ return _prime_periods }
   _prime_periods = common.env_int_clamped("NY_AUDIO_PRIME_PERIODS", 0, 0, 8)
   _prime_periods
}

fn _get_idle_sleep_ms(): int {
   if(_idle_sleep_ms > 0){ return _idle_sleep_ms }
   _idle_sleep_ms = common.env_int_clamped("NY_AUDIO_IDLE_SLEEP_MS", 2, 1, 50)
   _idle_sleep_ms
}

fn _prime_stream(): any {
   def primes = _get_prime_periods()
   if(primes <= 0){ return nil }
   def period_frames = _get_period_frames()
   def buf_size = period_frames * _frame_bytes()
   def mix_buf = malloc(buf_size)
   if(!mix_buf){ return nil }
   memset(mix_buf, 0, buf_size)
   mut i = 0
   while(i < primes){
      if(!outstream_write_frames(_stream, mix_buf, period_frames)){ break }
      i += 1
   }
   free(mix_buf)
}

fn _start_mixer_thread(): bool {
   if(_thread){ return true }
   if(_mtx == 0){
      _mtx = mutex_new()
      if(!_mtx){ return false }
      _active_sounds = []
   }
   _running = true
   _thread = thread_spawn(_audio_thread, 0)
   if(!_thread){
      _running = false
      if(_mtx){ mutex_free(_mtx) _mtx = 0 }
      _active_sounds = []
      return false
   }
   true
}

fn _cleanup_partial(): any {
   _running = false
   if(_thread){ thread_join(_thread) _thread = 0 }
   if(_stream){ outstream_destroy(_stream) _stream = 0 }
   if(_ctx){ disconnect(_ctx) _ctx = 0 }
   if(_mtx){ mutex_free(_mtx) _mtx = 0 }
   _active_sounds = []
   _period_frames = 0
   _out_rate = 0
   _out_format = FORMAT_S16LE
   _prime_periods = -1
   _idle_sleep_ms = 0
}

fn _do_init(bool: force_async=false): bool {
   if(_ctx != 0){ return true }
   if(_is_debug()){ print("Sound: Initializing context...") }
   def ctx = create()
   def nctx = connect(ctx)
   if(!nctx){
      if(_is_debug()){ print("Sound: Failed to connect to any backend.") }
      return false
   }
   _ctx = nctx
   if(_is_debug()){ print("Sound: Context connected to backend: " + get_backend_name()) }
   def count = get_output_device_count(_ctx)
   if(_is_debug()){ print("Sound: Found " + to_str(count) + " output devices.") }
   if(count == 0){
      if(_is_debug()){ print("Sound: No output devices found.") }
      disconnect(_ctx)
      _ctx = 0
      return false
   }
   def dev = get_output_device(_ctx, get_default_output_device_index(_ctx))
   mut stream = outstream_create(dev)
   mut out_format = _get_output_format_pref()
   def out_rate = _get_output_rate()
   def nstream = outstream_open(stream, out_format, out_rate, 2, nil)
   if(!nstream){
      out_format = FORMAT_S16LE
      def nstream2 = outstream_open(stream, out_format, out_rate, 2, nil)
      if(!nstream2){
         if(_is_debug()){ print("Sound: Failed to open output stream.") }
         outstream_destroy(stream)
         disconnect(nctx)
         return false
      }
      stream = nstream2
   } else {
      stream = nstream
   }
   _out_format = out_format
   if(!outstream_start(stream)){
      if(_is_debug()){ print("Sound: Failed to start output stream.") }
      outstream_destroy(stream)
      disconnect(nctx)
      return false
   }
   _ctx = nctx
   _stream = stream
   _prime_stream()
   if(force_async || _is_async_mode()){
      if(!_start_mixer_thread()){ if(_is_debug()){ print("Sound: Failed to spawn mixer thread; falling back to sync mode.") } }
   }
   if(_is_debug()){
      def fmt = (_out_format == FORMAT_FLOAT32LE) ? "f32" : "s16"
      print(f"Sound: Backend '{io_backend_name(_ctx)}' ready @ {_get_output_rate()}Hz format={fmt} period={_get_period_frames()} prime={_get_prime_periods()}.")
   }
   true
}

fn init(bool: force_async=false): bool {
   "Initializes module state."
   _do_init(force_async)
}

fn _make_inst(any: sound, any: pitch, any: vol, bool: looping, any: pan): list {
   mut inst = list()
   inst = inst.append(sound)
   inst = inst.append(0.0)
   inst = inst.append(pitch + 0.0)
   inst = inst.append(vol + 0.0)
   inst = inst.append(looping)
   inst = inst.append(pan + 0.0)
   inst
}

fn _play_sync(any: sound, any: pitch=1.0, any: vol=1.0, bool: looping=false, any: pan=0.0): any {
   if(looping && _is_debug()){ print("Sound: sync mode ignores looping=true") }
   def period_frames = _get_period_frames()
   def buf_size = period_frames * _frame_bytes()
   def mix_buf = malloc(buf_size)
   if(!mix_buf){ return 0 }
   mut actives = list()
   def inst = _make_inst(sound, pitch, vol, false, pan)
   actives = actives.append(inst)
   while(actives.len > 0){
      memset(mix_buf, 0, buf_size)
      actives = mixer.mix_sounds(mix_buf, buf_size, actives, _master_vol, 0, _get_output_rate(), _out_format)
      if(!outstream_write_frames(_stream, mix_buf, period_frames)){ break }
   }
   free(mix_buf)
   inst
}

fn shutdown(): any {
   "Shuts down module state."
   if(_is_debug()){ print("Sound: Shutting down...") }
   _cleanup_partial()
}

fn _audio_thread(any: arg): int {
   def period_frames = _get_period_frames()
   def buf_size = period_frames * _frame_bytes()
   def out_rate = _get_output_rate()
   def idle_sleep = _get_idle_sleep_ms()
   def mix_buf = malloc(buf_size)
   if(_is_debug()){ print("Sound: Background thread started.") }
   while(_running){
      memset(mix_buf, 0, buf_size)
      mutex_lock(_mtx)
      mut actives = _active_sounds
      if(actives.len > 0){
         _active_sounds = mixer.mix_sounds(mix_buf, buf_size, actives, _master_vol, 0, out_rate, _out_format)
         actives = _active_sounds
      }
      mutex_unlock(_mtx)
      if(actives.len == 0){
         msleep(idle_sleep)
         continue
      }
      if(_is_mix_debug()){
         mut probe = 0
         mut has_signal = false
         mut peak = 0.0
         if(_out_format == FORMAT_FLOAT32LE){
            while(probe < buf_size){
               def x = load32_f32(mix_buf, probe)
               def ax = abs(x)
               if(ax > peak){ peak = ax }
               if(ax > 0.000001){ has_signal = true }
               probe += 4
            }
         } else {
            while(probe < buf_size){
               mut x = load16(mix_buf, probe)
               if(x > 32767){ x = x - 65536 }
               def ax, fx = (x < 0) ? (0 - x) : x, ax + 0.0
               if(fx > peak){ peak = fx }
               if(x != 0){ has_signal = true }
               probe += 2
            }
         }
         if(peak > 0.0){ print(f"Sound: mix probe active={actives.len} signal={has_signal} peak={peak}") }
      }
      if(!outstream_write_frames(_stream, mix_buf, period_frames)){
         if(_is_debug()){ print("Sound: Write error, device might be busy or lost.") }
         msleep(10)
      }
   }
   free(mix_buf)
   if(_is_debug()){ print("Sound: Background thread exiting.") }
   0
}

fn play(any: sound, any: pitch=1.0, any: vol=1.0, bool: looping=false, any: pan=0.0): any {
   "Implements `play`."
   if(!_ctx && !_do_init()){ return 0 }
   if(!_thread && looping){ if(!_start_mixer_thread() && _is_debug()){ print("Sound: looping requested but mixer thread start failed; using sync mode.") } }
   if(!_thread){ return _play_sync(sound, pitch, vol, looping, pan) }
   mutex_lock(_mtx)
   def inst = _make_inst(sound, pitch, vol, looping, pan)
   _active_sounds = _active_sounds.append(inst)
   if(_is_debug()){ print("Sound: queued sound") }
   mutex_unlock(_mtx)
   inst
}

fn stop(any: inst_or_sound): any {
   "Implements `stop`."
   if(!_mtx || !_thread){ return nil }
   mutex_lock(_mtx)
   mut new_list = list()
   mut i = 0
   while(i < _active_sounds.len){
      def inst = _active_sounds.get(i)
      if(inst != inst_or_sound && inst.get(0) != inst_or_sound){ new_list = new_list.append(inst) }
      i += 1
   }
   _active_sounds = new_list
   mutex_unlock(_mtx)
}

fn is_playing(any: inst): bool {
   "Returns whether playing."
   if(!_mtx || !_thread){ return false }
   mutex_lock(_mtx)
   mut i, playing = 0, false
   while(i < _active_sounds.len){
      if(_active_sounds.get(i) == inst){ playing = true break }
      i += 1
   }
   mutex_unlock(_mtx)
   playing
}

fn set_master_volume(any: vol): any {
   "Sets master volume."
   _master_vol = vol + 0.0
}

fn get_master_volume(): f64 {
   "Gets master volume."
   _master_vol
}

fn get_backend_name(): str {
   "Gets backend name."
   return io_backend_name(_ctx)
}
