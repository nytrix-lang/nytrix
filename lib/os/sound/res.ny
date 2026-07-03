;; Keywords: sound res os
;; Unified resource loader for sound and image files.
;; Uses libsndfile for sound.
;; References:
;; - std.os.sound
;; - std.os
module std.os.sound.res(init, shutdown, load, get_sound_info, get_image_info, clear_cache, is_cached)
use std.core
use std.core.error
use std.core.dict_mod
use std.core.mem (cstr)
use std.os (env, file_read, file_remove, file_write, mutex_free, mutex_lock, mutex_new, mutex_unlock, temp_dir)
use std.os.clock (ticks)
use std.os.path as path
use std.os.fs as fs
use std.core.str as str
use std.math.parse.img as image
use std.os.sound.diag as sound_debug
use std.os.sound.source

#linux {
   #link "libsndfile.so"
   #include <sndfile.h> as "sf_"
   extern {
      fn sf_open(ptr path, int mode, ptr info) ptr as "sf_open"
      fn sf_error(ptr sf) int as "sf_error"
      fn sf_error_number(int errnum) ptr as "sf_error_number"
      fn sf_close(ptr sf) int as "sf_close"
      fn sf_read_short(ptr sf, ptr out, int items) int as "sf_read_short"
   }
} #else {
   fn sf_open(any _path, int _mode, any _info) any {
      "Runs the sf open operation."
      0
   }
   fn sf_error(any _sf) int {
      "Runs the sf error operation."
      0
   }
   fn sf_error_number(int _errnum) any {
      "Runs the sf error number operation."
      0
   }
   fn sf_close(any _sf) int {
      "Runs the sf close operation."
      0
   }
   fn sf_read_short(any _sf, any _ptr, int _items) int {
      "Runs the sf read short operation."
      0
   }
} #endif
def SFM_READ = 0x10

fn _sf_available() bool {
   "Checks whether libsndfile is available(linked via #include)."
   #linux { true } #else { false } #endif
}

fn _decode_sf(any data) any {
   if !_sf_available() { return 0 }
   if !is_str(data) || data.len < 16 { return 0 }
   def debug = sound_debug.enabled()
   def tmp_fn = "ny_snd_" + to_str(ticks()) + "_" + to_str(malloc(1)) + ".tmp"
   def tmp_path = path.normalize(temp_dir() + "/" + tmp_fn)
   match file_write(tmp_path, data) {
      err(e) -> {
         if debug { print("RES: Sndfile temp write failed: " + to_str(e)) }
         return 0
      }
      ok(ignoredok)  -> { ignoredok }
   }
   def info_ptr = malloc(64)
   memset(info_ptr, 0, 64)
   def path_cstr = cstr(tmp_path)
   def sf = sf_open(path_cstr, SFM_READ, info_ptr)
   if !sf {
      if debug {
         def err_code = sf_error(0)
         def err_msg = sf_error_number(err_code)
         print("RES: Sndfile sf_open failed: " + str.cstr_to_str(err_msg))
      }
      free(info_ptr)
      match file_remove(tmp_path) { _ -> {} }
      return 0
   }
   def frames   = load64(info_ptr, 0)
   def rate     = load32(info_ptr, 8)
   def channels = load32(info_ptr, 12)
   if debug { print("RES: Sndfile opened " + tmp_path + " frames=" + to_str(frames) + " rate=" + to_str(rate) + " channels=" + to_str(channels)) }
   if frames <= 0 || channels <= 0 || channels > 32 {
      sf_close(sf)
      free(info_ptr)
      match file_remove(tmp_path) { _ -> {} }
      return 0
   }
   def n_samples = frames * channels
   def out_ptr = malloc(n_samples * 2)
   def got = sf_read_short(sf, out_ptr, n_samples)
   sf_close(sf)
   free(info_ptr)
   match file_remove(tmp_path) { _ -> {} }
   if got <= 0 {
      free(out_ptr)
      return 0
   }
   make_memory_source(out_ptr, got * 2, channels, rate, 16, SAMPLE_FMT_S16, 1)
}

mut _cache = dict(64)
mut _mtx = 0

fn init() bool {
   "Initializes the shared resource cache."
   if _mtx == 0 { _mtx = mutex_new() }
   true
}

fn shutdown() bool {
   "Clears cached resources and releases the cache mutex."
   clear_cache()
   if _mtx != 0 { mutex_free(_mtx) _mtx = 0 }
   true
}

fn clear_cache() bool {
   "Drops all cached resource entries."
   if _mtx == 0 { return true }
   mutex_lock(_mtx)
   _cache = dict(64)
   mutex_unlock(_mtx)
   true
}

fn is_cached(any filepath) bool {
   "Returns whether `filepath` is already present in the resource cache."
   if _mtx == 0 { return false }
   def full = path.normalize(filepath)
   mutex_lock(_mtx)
   def exists = _cache.contains(full)
   mutex_unlock(_mtx)
   exists
}

fn load(any filepath) any {
   "Loads a sound or image asset from disk, caching successful results by normalized path."
   if _mtx == 0 { init() }
   def full = path.normalize(filepath)
   mutex_lock(_mtx)
   def cached = _cache.get(full)
   mutex_unlock(_mtx)
   if cached { return cached }
   if !fs.is_file(full) { return 0 }
   def res = file_read(full)
   if is_err(res) { return 0 }
   def data = unwrap(res)
   def ext = str.lower(path.extname(full))
   mut asset = 0
   if ext == ".wav" || ext == ".ogg" || ext == ".flac" || ext == ".mp3" || ext == ".aiff" || ext == ".aif" || ext == ".opus" { asset = _decode_sf(data) } else { asset = image.decode(data, ext) }
   if asset != 0 {
      mutex_lock(_mtx)
      _cache = _cache.set(full, asset)
      mutex_unlock(_mtx)
      if sound_debug.enabled() { print("RES: successfully loaded " + full) }
   } else {
      if sound_debug.enabled() { print("RES: FAILED to load " + full) }
   }
   asset
}

fn get_sound_info(any sound) any {
   "Returns normalized metadata for a loaded sound source."
   if !is_list(sound) { return 0 }
   def tag = sound.get(0)
   if tag != "SOUND_SOURCE" { return 0 }
   def d = sound.get(1)
   mut info = dict(8)
   info = info.set("channels", d.get("channels"))
   info = info.set("rate",     d.get("rate"))
   info = info.set("bits",     d.get("bits"))
   def frames = d.get("total_frames")
   def rate = d.get("rate")
   def duration = (frames + 0.0) / (rate + 0.0)
   info = info.set("duration", duration)
   info = info.set("frames",   frames)
   info
}

fn get_image_info(any img) any {
   "Returns normalized metadata for a loaded image."
   image.get_info(img)
}
