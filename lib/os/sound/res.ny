;; Keywords: sound res os
;; Unified resource loader for sound and image files.
;; Uses libsndfile for sound.
;; References:
;; - core.os.sound
;; - core.os
module core.os.sound.res(init, shutdown, load, get_sound_info, get_image_info, clear_cache, is_cached)
use core
use core.error
use core.dict_mod
use core.mem (cstr)
use core.os (file_read, mutex_free, mutex_lock, mutex_new, mutex_unlock)
use core.os.path as path
use core.os.fs as fs
use core.str as str
use core.math.parse.img as image
use core.os.sound.diag as sound_debug
use core.os.sound.source

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

fn _decode_sf(str filepath) any {
   def debug = sound_debug.enabled()
   if !_sf_available() {
      if debug { print("RES: Sndfile backend unavailable") }
      return 0
   }
   def info_ptr = malloc(64)
   memset(info_ptr, 0, 64)
   def path_cstr = cstr(filepath)
   def sf = sf_open(path_cstr, SFM_READ, info_ptr)
   if !sf {
      if debug {
         def err_code = sf_error(0)
         def err_msg = sf_error_number(err_code)
         print("RES: Sndfile sf_open failed: " + str.cstr_to_str(err_msg))
      }
      free(info_ptr)
      return 0
   }
   def frames   = load64_h(info_ptr, 0)
   def rate     = load32(info_ptr, 8)
   def channels = load32(info_ptr, 12)
   if debug { print("RES: Sndfile opened " + filepath + " frames=" + to_str(frames) + " rate=" + to_str(rate) + " channels=" + to_str(channels)) }
   if frames <= 0 || channels <= 0 || channels > 32 {
      sf_close(sf)
      free(info_ptr)
      return 0
   }
   def n_samples = frames * channels
   def out_ptr = malloc(n_samples * 2)
   def got = sf_read_short(sf, out_ptr, n_samples)
   sf_close(sf)
   free(info_ptr)
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
   def ext = str.lower(path.extname(full))
   mut any asset = 0
   if ext == ".wav" || ext == ".ogg" || ext == ".flac" || ext == ".mp3" || ext == ".aiff" || ext == ".aif" || ext == ".opus" {
      asset = _decode_sf(full)
   } else {
      def res = file_read(full)
      if is_err(res) { return 0 }
      asset = image.decode(unwrap(res), ext)
   }
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
   source.sound_info(sound)
}

fn get_image_info(any img) any {
   "Returns normalized metadata for a loaded image."
   image.get_info(img)
}
