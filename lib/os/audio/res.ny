;; Keywords: audio resource manager

module std.audio.res (
   init, shutdown,
   load, get_sound_info,
   clear_cache,
   is_cached
)

use std.core *
use std.core.error *
use std.core.dict *
use std.os.path as path
use std.os.fs as fs
use std.text as str
use std.audio.formats.wav as wav
use std.audio.formats.ogg as ogg
use std.audio.formats.opus as opus
use std.audio.formats.acc as acc
use std.audio.formats.flac as flac
use std.audio.formats.mp3 as mp3
use std.audio.source *

mut _cache = dict(32)
mut _mtx = 0

fn init(){
   "Initializes module state."
   if(_mtx == 0){ _mtx = mutex_new() }
   true
}

fn shutdown(){
   "Shuts down module state."
   clear_cache()
   if(_mtx != 0){ mutex_free(_mtx) _mtx = 0 }
}

fn clear_cache(){
   "Implements `clear_cache`."
   if(_mtx == 0){ return }
   mutex_lock(_mtx)
   _cache = dict(32)
   mutex_unlock(_mtx)
}

fn is_cached(filepath){
   "Returns whether cached."
   if(_mtx == 0){ return false }
   def full = path.normalize(filepath)
   mutex_lock(_mtx)
   def exists = dict_has(_cache, full)
   mutex_unlock(_mtx)
   exists
}

fn load(filepath){
   "Loads an audio file into memory. Supports WAV, OGG (Vorbis/Opus), Opus, AAC/ACC, and FLAC."
   if(_mtx == 0){ init() }
   def full = path.normalize(filepath)
   mutex_lock(_mtx)
   def cached = dict_get(_cache, full)
   mutex_unlock(_mtx)
   if(cached){ return cached }
   if(!fs.is_file(full)){ return 0 }
   mut sound = 0
   def ext = str.lower(path.extname(full))
   def res = file_read(full)
   if(is_err(res)){ return 0 }
   def data = unwrap(res)
   if(ext == ".wav"){
      sound = wav.decode(data)
   }
   elif(ext == ".ogg" || ext == ".oga"){
      sound = ogg.decode(data)
   }
   elif(ext == ".opus"){
      sound = opus.decode(data)
   }
   elif(ext == ".acc" || ext == ".aac"){
      sound = acc.decode(data)
   }
   elif(ext == ".mp3"){
      sound = mp3.decode(data)
   }
   elif(ext == ".flac"){
      def img = flac.decode(data)
      if(img){
         def pcm = dict_get(img, "data")
         if(is_list(pcm)){
            def ptr = _float_list_to_ptr(pcm)
            if(ptr){
               sound = make_memory_source(
                  ptr,
                  len(pcm) * 4,
                  dict_get(img, "channels"),
                  dict_get(img, "sample_rate", dict_get(img, "rate")),
                  32,
                  SAMPLE_FMT_F32,
                  3
               )
            }
         } elif(is_int(pcm) && pcm != 0){
            def bytes = dict_get(img, "byte_len", 0)
            if(bytes > 0){
               sound = make_memory_source(
                  pcm,
                  bytes,
                  dict_get(img, "channels"),
                  dict_get(img, "sample_rate", dict_get(img, "rate")),
                  32,
                  SAMPLE_FMT_F32,
                  3
               )
            }
         } elif(pcm){
            sound = make_memory_source(
               pcm,
               len(pcm) * 4,
               dict_get(img, "channels"),
               dict_get(img, "sample_rate", dict_get(img, "rate")),
               32,
               SAMPLE_FMT_F32,
               3
            )
         }
      }
   }
   if(sound != 0){
      mutex_lock(_mtx)
      _cache = dict_set(_cache, full, sound)
      mutex_unlock(_mtx)
   }
   sound
}

fn _float_list_to_ptr(xs){
   "Converts list<float> to packed float32 buffer."
   if(!is_list(xs)){ return 0 }
   def n = len(xs)
   if(n <= 0){ return 0 }
   def ptr = malloc(n * 4)
   if(!ptr){ return 0 }
   mut i = 0
   while(i < n){
      store32_f32(ptr, get(xs, i, 0.0) + 0.0, i * 4)
      i += 1
   }
   ptr
}

fn get_sound_info(sound){
   "Returns a dictionary with sound metadata (channels, rate, bits, duration)."
   if(!is_list(sound)){ return 0 }
   def tag = get(sound, 0)
   if(tag != "SOUND_SOURCE"){ return 0 }
   def d = get(sound, 1)
   mut info = dict(8)
   info = dict_set(info, "channels", get(d, "channels"))
   info = dict_set(info, "rate", get(d, "rate"))
   info = dict_set(info, "bits", get(d, "bits"))
   def frames = get(d, "total_frames")
   def rate = get(d, "rate")
   def duration = (frames + 0.0) / (rate + 0.0)
   info = dict_set(info, "duration", duration)
   info = dict_set(info, "frames", frames)
   info
}
if(comptime{__main()}){
   use std.core.error *

   print("Testing std.audio.res audio loading...")

   fn verify_audio(filepath, name){
      def s = load(filepath)
      if(s == 0){
         print("  FAILED: Could not load " + filepath)
         return false
      }

      def info = get_sound_info(s)
      def rate = dict_get(info, "rate")
      def chan = dict_get(info, "channels")

      if(rate <= 0 || chan <= 0){
         print("  FAILED: " + name + " invalid info: " + to_str(rate) + "Hz, " + to_str(chan) + "ch")
         return false
      }

      print("  SUCCESS: " + name + " (" + to_str(rate) + "Hz, " + to_str(chan) + "ch)")
      true
   }

   init()
   mut ok = true
   ok = ok && verify_audio("etc/assets/audio/test_sine.wav", "WAV")
   ok = ok && verify_audio("etc/assets/audio/test_sine.ogg", "OGG")
   ok = ok && verify_audio("etc/assets/audio/test_sine.flac", "FLAC")
   ok = ok && verify_audio("etc/assets/audio/test_sine.mp3", "MP3")

   if(ok){
      print("✓ std.audio.res all audio format tests passed")
   } else {
      print("✗ SOME std.audio.res TESTS FAILED")
      __exit(1)
   }
   shutdown()
}
