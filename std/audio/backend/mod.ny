;; Keywords: audio backend

module std.audio.backend (
   init, shutdown,
   play, stop, is_playing,
   set_master_volume, get_master_volume,
   get_backend_name
)

use std.core *
use std.os *
use std.os.thread *
use std.text *
use std.audio.backend.io (
   create, connect, disconnect,
   get_output_device_count, get_output_device, get_default_output_device_index,
   outstream_create, outstream_open, outstream_start, outstream_write_frames, outstream_destroy,
   FORMAT_S16LE, FORMAT_FLOAT32LE, get_backend_name as io_backend_name
)
use std.audio.mixer as mixer

mut _ctx = 0
mut _stream = 0
mut _running = false
mut _thread = 0
mut _mtx = 0
mut _active_sounds = []
mut _master_vol = 1.0
mut _debug = -1
mut _debug_mix = -1
mut _async_mode = -1
mut _period_frames = 0
mut _out_rate = 0
mut _out_format = FORMAT_S16LE
mut _prime_periods = -1
mut _idle_sleep_ms = 0

fn _touch(...args){
   "Internal helper for `touch`."
   len(args)
}

fn _is_debug(){
   "Internal helper for `is_debug`."
   if(_debug == -1){
      def v = env("NY_AUDIO_DEBUG")
      _debug = (v && (eq(v, "1") || eq(v, "true"))) ? 1 : 0
   }
   _debug
}

fn _is_mix_debug(){
   "Internal helper for `is_mix_debug`."
   if(_debug_mix == -1){
      def v = env("NY_AUDIO_DEBUG_MIX")
      _debug_mix = (v && (eq(v, "1") || eq(v, "true"))) ? 1 : 0
   }
   _debug_mix
}

fn _env_enabled(name){
   "Internal helper for `env_enabled`."
   def v = env(name)
   if(!v){ return false }
   eq(v, "1") || eq(v, "true")
}

fn _is_async_mode(){
   "Internal helper for `is_async_mode`."
   if(_async_mode == -1){
      def v = env("NY_AUDIO_ASYNC")
      if(!v){
         if(_ctx && eq(io_backend_name(_ctx), "pulse")){
            _async_mode = 0
         } else {
            _async_mode = 1
         }
      } elif(eq(v, "1") || eq(v, "true") || eq(v, "on") || eq(v, "yes")){
         _async_mode = 1
      } else {
         _async_mode = 0
      }
   }
   _async_mode
}

fn _get_period_frames(){
   "Internal helper for `get_period_frames`."
   if(_period_frames > 0){ return _period_frames }
   mut pf = 1024
   if(_ctx && eq(io_backend_name(_ctx), "alsa")){
      pf = 2048
   } elif(_ctx && eq(io_backend_name(_ctx), "pulse")){
      pf = 1024
   }
   def v = env("NY_AUDIO_PERIOD_FRAMES")
   if(v){
      def n = atoi(v)
      if(n >= 128 && n <= 4096){ pf = n }
   }
   _period_frames = pf
   _period_frames
}

fn _get_output_rate(){
   "Internal helper for `get_output_rate`."
   if(_out_rate > 0){ return _out_rate }
   mut r = 48000
   mut v = env("NY_AUDIO_SAMPLE_RATE")
   if(!v){ v = env("NY_AUDIO_RATE") }
   if(v){
      def n = atoi(v)
      if(n >= 8000 && n <= 192000){ r = n }
   }
   _out_rate = r
   _out_rate
}

fn _get_output_format_pref(){
   "Internal helper for `get_output_format_pref`."
   def v = env("NY_AUDIO_FORMAT")
   if(v){
      def s = lower(strip(v))
      if(s == "s16" || s == "s16le" || s == "int16"){ return FORMAT_S16LE }
      if(s == "f32" || s == "float32" || s == "float32le"){ return FORMAT_FLOAT32LE }
   }
   FORMAT_S16LE
}

fn _frame_bytes(){
   "Internal helper for `frame_bytes`."
   if(_out_format == FORMAT_FLOAT32LE){ return 8 }
   4
}

fn _get_prime_periods(){
   "Internal helper for `get_prime_periods`."
   if(_prime_periods >= 0){ return _prime_periods }
   mut p = 0
   def v = env("NY_AUDIO_PRIME_PERIODS")
   if(v){
      def n = atoi(v)
      if(n >= 0 && n <= 8){ p = n }
   }
   _prime_periods = p
   _prime_periods
}

fn _get_idle_sleep_ms(){
   "Internal helper for `get_idle_sleep_ms`."
   if(_idle_sleep_ms > 0){ return _idle_sleep_ms }
   mut ms = 2
   def v = env("NY_AUDIO_IDLE_SLEEP_MS")
   if(v){
      def n = atoi(v)
      if(n >= 1 && n <= 50){ ms = n }
   }
   _idle_sleep_ms = ms
   _idle_sleep_ms
}

fn _prime_stream(){
   "Internal helper for `prime_stream`."
   def primes = _get_prime_periods()
   if(primes <= 0){ return }
   def period_frames = _get_period_frames()
   def buf_size = period_frames * _frame_bytes()
   def mix_buf = malloc(buf_size)
   if(!mix_buf){ return }
   memset(mix_buf, 0, buf_size)
   mut i = 0
   while(i < primes){
      if(!outstream_write_frames(_stream, mix_buf, period_frames)){ break }
      i += 1
   }
   free(mix_buf)
}

fn _start_mixer_thread(){
   "Internal helper for `start_mixer_thread`."
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

fn _cleanup_partial(){
   "Internal helper for `cleanup_partial`."
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

fn init(){
   "Initializes module state."
   if(_ctx != 0){ return true }
   if(_is_debug()){ print("Audio: Initializing context...") }
   def ctx = create()
   if(!connect(ctx)){
      if(_is_debug()){ print("Audio: Failed to connect to any backend.") }
      return false
   }
   def count = get_output_device_count(ctx)
   if(count == 0){
      if(_is_debug()){ print("Audio: No output devices found.") }
      disconnect(ctx)
      return false
   }
   def dev = get_output_device(ctx, get_default_output_device_index(ctx))
   def stream = outstream_create(dev)
   mut out_format = _get_output_format_pref()
   def out_rate = _get_output_rate()
   if(!outstream_open(stream, out_format, out_rate, 2, 0)){
      out_format = FORMAT_S16LE
      if(!outstream_open(stream, out_format, out_rate, 2, 0)){
         if(_is_debug()){ print("Audio: Failed to open output stream.") }
         outstream_destroy(stream)
         disconnect(ctx)
         return false
      }
   }
   _out_format = out_format
   if(!outstream_start(stream)){
      if(_is_debug()){ print("Audio: Failed to start output stream.") }
      outstream_destroy(stream)
      disconnect(ctx)
      return false
   }
   _ctx = ctx
   _stream = stream
   _prime_stream()
   if(_is_async_mode()){
      if(!_start_mixer_thread()){
         if(_is_debug()){
            print("Audio: Failed to spawn mixer thread; falling back to sync mode.")
         }
      }
   }
   if(_is_debug()){
      def fmt = (_out_format == FORMAT_FLOAT32LE) ? "f32" : "s16"
      print(f"Audio: Backend '{io_backend_name(_ctx)}' ready @ {_get_output_rate()}Hz format={fmt} period={_get_period_frames()} prime={_get_prime_periods()}.")
   }
   true
}

fn _make_inst(sound, pitch, vol, looping, pan){
   "Internal helper for `make_inst`."
   mut inst = list()
   inst = append(inst, sound)
   inst = append(inst, 0.0)
   inst = append(inst, pitch + 0.0)
   inst = append(inst, vol + 0.0)
   inst = append(inst, looping)
   inst = append(inst, pan + 0.0)
   inst
}

fn _play_sync(sound, pitch=1.0, vol=1.0, looping=false, pan=0.0){
   "Internal helper for `play_sync`."
   if(looping && _is_debug()){
      print("Audio: sync mode ignores looping=true")
   }
   def period_frames = _get_period_frames()
   def buf_size = period_frames * _frame_bytes()
   def mix_buf = malloc(buf_size)
   if(!mix_buf){ return 0 }
   mut actives = list()
   def inst = _make_inst(sound, pitch, vol, false, pan)
   actives = append(actives, inst)
   while(len(actives) > 0){
      memset(mix_buf, 0, buf_size)
      actives = mixer.mix_sounds(mix_buf, buf_size, actives, _master_vol, 0, _get_output_rate(), _out_format)
      if(!outstream_write_frames(_stream, mix_buf, period_frames)){ break }
   }
   free(mix_buf)
   inst
}

fn shutdown(){
   "Shuts down module state."
   if(_is_debug()){ print("Audio: Shutting down...") }
   _cleanup_partial()
}

fn _audio_thread(arg){
   "Internal helper for `audio_thread`."
   _touch(arg)
   def period_frames = _get_period_frames()
   def buf_size = period_frames * _frame_bytes()
   def out_rate = _get_output_rate()
   def idle_sleep = _get_idle_sleep_ms()
   def mix_buf = malloc(buf_size)
   if(_is_debug()){ print("Audio: Background thread started.") }
   while(_running){
      memset(mix_buf, 0, buf_size)
      mutex_lock(_mtx)
      mut actives = _active_sounds
      if(len(actives) > 0){
         _active_sounds = mixer.mix_sounds(mix_buf, buf_size, actives, _master_vol, 0, out_rate, _out_format)
         actives = _active_sounds
      }
      mutex_unlock(_mtx)
      if(len(actives) == 0){
         sleep_ms(idle_sleep)
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
               def ax = (x < 0) ? (0 - x) : x
               def fx = ax + 0.0
               if(fx > peak){ peak = fx }
               if(x != 0){ has_signal = true }
               probe += 2
            }
         }
         if(peak > 0.0){
            print(f"Audio: mix probe active={len(actives)} signal={has_signal} peak={peak}")
         }
      }
      if(!outstream_write_frames(_stream, mix_buf, period_frames)){
         if(_is_debug()){ print("Audio: Write error, device might be busy or lost.") }
         sleep_ms(10)
      }
   }
   free(mix_buf)
   if(_is_debug()){ print("Audio: Background thread exiting.") }
   0
}

fn play(sound, pitch=1.0, vol=1.0, looping=false, pan=0.0){
   "Implements `play`."
   if(!_ctx && !init()){ return 0 }
   if(!_thread && looping){
      if(!_start_mixer_thread() && _is_debug()){
         print("Audio: looping requested but mixer thread start failed; using sync mode.")
      }
   }
   if(!_thread){ return _play_sync(sound, pitch, vol, looping, pan) }
   mutex_lock(_mtx)
   def inst = _make_inst(sound, pitch, vol, looping, pan)
   _active_sounds = append(_active_sounds, inst)
   if(_is_debug()){ print("Audio: queued sound") }
   mutex_unlock(_mtx)
   inst
}

fn stop(inst_or_sound){
   "Implements `stop`."
   if(!_mtx || !_thread){ return }
   mutex_lock(_mtx)
   mut new_list = list()
   mut i = 0
   while(i < len(_active_sounds)){
      def inst = get(_active_sounds, i)
      if(inst != inst_or_sound && get(inst, 0) != inst_or_sound){
         new_list = append(new_list, inst)
      }
      i += 1
   }
   _active_sounds = new_list
   mutex_unlock(_mtx)
}

fn is_playing(inst){
   "Returns whether playing."
   if(!_mtx || !_thread){ return false }
   mutex_lock(_mtx)
   mut i = 0 mut playing = false
   while(i < len(_active_sounds)){
      if(get(_active_sounds, i) == inst){ playing = true break }
      i += 1
   }
   mutex_unlock(_mtx)
   playing
}

fn set_master_volume(vol){
   "Sets master volume."
   _master_vol = vol + 0.0
}
fn get_master_volume(){
   "Gets master volume."
   _master_vol
}
fn get_backend_name(){
   "Gets backend name."
   io_backend_name(_ctx)
}
