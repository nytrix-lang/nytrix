;; Keywords: sound res os
;; Unified resource loader for sound and image files.
;; Uses libsndfile for sound (optional, bound via dlopen).
;; Pure-Ny WAV/AIFF/AU fallback when libsndfile is unavailable.
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
use core.os.sound.wav as wav
use core.os.sound.au as au
use core.os.sound.aiff as aiff
use core.os.ffi as ffi

;; libsndfile is optional: bound through dlopen so hosts without it can still
;; decode WAV, AIFF, and AU files via the pure-Ny fallback.
mut _snd_checked = false
mut _snd_ok = false
mut _snd_handle = 0
mut _snd_fn_open = 0
mut _snd_fn_error = 0
mut _snd_fn_error_number = 0
mut _snd_fn_close = 0
mut _snd_fn_read_short = 0

fn _env_off(str name) bool {
   def raw = __env_get(name)
   if !is_str(raw) { return false }
   def v = lower(raw)
   v == "1" || v == "true" || v == "on" || v == "yes"
}

fn _snd_load() bool {
   ;; The disable toggle is read on every call. Only the dlopen result is cached,
   ;; so a process can flip between libsndfile and the pure-Ny route.
   if _snd_disable() { return false }
   if _snd_checked { return _snd_ok }
   _snd_checked = true
   _snd_ok = false
   mut lib = 0
   if comptime { __os_name() == "linux" } {
      lib = ffi.dlopen_checked("libsndfile.so.1", "sf_open", ffi.RTLD_NOW() | ffi.RTLD_GLOBAL())
      if !lib { lib = ffi.dlopen_checked("libsndfile.so", "sf_open", ffi.RTLD_NOW() | ffi.RTLD_GLOBAL()) }
   } elif comptime { __os_name() == "macos" } {
      lib = ffi.dlopen_checked("libsndfile.1.dylib", "sf_open", ffi.RTLD_NOW() | ffi.RTLD_GLOBAL())
      if !lib { lib = ffi.dlopen_checked("libsndfile.dylib", "sf_open", ffi.RTLD_NOW() | ffi.RTLD_GLOBAL()) }
   } else {
      lib = ffi.dlopen_checked("sndfile-1.dll", "sf_open", ffi.RTLD_NOW() | ffi.RTLD_GLOBAL())
      if !lib { lib = ffi.dlopen_checked("sndfile.dll", "sf_open", ffi.RTLD_NOW() | ffi.RTLD_GLOBAL()) }
   }
   if !lib { return false }
   _snd_fn_open = ffi.dlsym(lib, "sf_open")
   _snd_fn_error = ffi.dlsym(lib, "sf_error")
   _snd_fn_error_number = ffi.dlsym(lib, "sf_error_number")
   _snd_fn_close = ffi.dlsym(lib, "sf_close")
   _snd_fn_read_short = ffi.dlsym(lib, "sf_read_short")
   if !_snd_fn_open || !_snd_fn_error || !_snd_fn_error_number || !_snd_fn_close || !_snd_fn_read_short {
      return false
   }
   _snd_handle = lib
   _snd_ok = true
   true
}

fn _snd_available() bool {
   _snd_load()
   _snd_ok
}

fn _snd_disable() bool {
   _env_off("NY_SNDFILE_DISABLE") || _env_off("NY_SOUND_DISABLE_LIBSNDFILE")
}

;; Pure-Ny WAV decoder (PCM, float, 8/16/24/32-bit)
fn _wav_decode(str filepath) any {
   def res = file_read(filepath)
   if is_err(res) { return 0 }
   wav.decode_bytes(unwrap(res))
}

;; Pure-Ny AU decoder (SND/AU format - big endian)
fn _au_decode(str filepath) any {
   def res = file_read(filepath)
   if is_err(res) { return 0 }
   au.decode_bytes(unwrap(res))
}

fn _aiff_decode(str filepath) any {
   def res = file_read(filepath)
   if is_err(res) { return 0 }
   aiff.decode_bytes(unwrap(res))
}

fn _snd_decode(str filepath) any {
   def debug = sound_debug.enabled()
   if _snd_disable() { return 0 }
   if _snd_available() {
      def info_ptr = malloc(64)
      memset(info_ptr, 0, 64)
      def path_cstr = cstr(filepath)
      def sf = ffi.any3(_snd_fn_open, path_cstr, 0x10, info_ptr) ;; SFM_READ
      if !sf {
         if debug {
            def err_code = ffi.any0(_snd_fn_error)
            def err_msg = ffi.any0(_snd_fn_error_number)
            print("RES: Sndfile sf_open failed: " + str.cstr_to_str(err_msg))
         }
         free(info_ptr)
      } else {
         def frames = load64_h(info_ptr, 0)
         def rate = load32(info_ptr, 8)
         def channels = load32(info_ptr, 12)
         if debug { print("RES: Sndfile opened " + filepath + " frames=" + to_str(frames) + " rate=" + to_str(rate) + " channels=" + to_str(channels)) }
         free(info_ptr)
         if frames <= 0 || channels <= 0 || channels > 32 { return 0 }
         def n_samples = frames * channels
         def out_ptr = malloc(n_samples * 2)
         def got = ffi.any3(_snd_fn_read_short, sf, out_ptr, n_samples)
         ffi.any1(_snd_fn_close, sf)
         if got <= 0 {
            free(out_ptr)
            return 0
         }
         make_memory_source(out_ptr, got * 2, channels, rate, 16, SAMPLE_FMT_S16, 1)
      }
   }
   return 0
}

fn _wav_disable() bool {
   def raw = __env_get("NY_WAV_DISABLE")
   if is_str(raw) {
      def v = lower(raw)
      v == "1" || v == "true" || v == "on" || v == "yes"
   }
   false
}

fn _au_disable() bool {
   def raw = __env_get("NY_AU_DISABLE")
   if is_str(raw) {
      def v = lower(raw)
      v == "1" || v == "true" || v == "on" || v == "yes"
   }
   false
}

fn _raw_ptr(any p) any {
   if !p { return 0 }
   if is_int(p) { return to_int(p) }
   p
}

fn _decode_sf(str filepath) any {
   def debug = sound_debug.enabled()
   if _snd_disable() { return 0 }
   if _snd_available() {
      def info_ptr = malloc(64)
      memset(info_ptr, 0, 64)
      def path_cstr = cstr(filepath)
      def sf = ffi.any3(_snd_fn_open, path_cstr, 0x10, info_ptr)
      if !sf {
         if debug {
            def err_code = ffi.any0(_snd_fn_error)
            def err_msg = ffi.any0(_snd_fn_error_number)
            print("RES: Sndfile sf_open failed: " + str.cstr_to_str(err_msg))
         }
         free(info_ptr)
         return 0
      }
      def frames = load64_h(info_ptr, 0)
      def rate = load32(info_ptr, 8)
      def channels = load32(info_ptr, 12)
      if debug { print("RES: Sndfile opened " + filepath + " frames=" + to_str(frames) + " rate=" + to_str(rate) + " channels=" + to_str(channels)) }
      free(info_ptr)
      if frames <= 0 || channels <= 0 || channels > 32 { return 0 }
      def n_samples = frames * channels
      def out_ptr = malloc(n_samples * 2)
      def got = ffi.any3(_snd_fn_read_short, sf, out_ptr, n_samples)
      ffi.any1(_snd_fn_close, sf)
      if got <= 0 {
         free(out_ptr)
         return 0
      }
      make_memory_source(out_ptr, got * 2, channels, rate, 16, SAMPLE_FMT_S16, 1)
   }
   return 0
}

fn _decode_wav(str filepath) any {
   if _wav_disable() { return 0 }
   _wav_decode(filepath)
}

fn _decode_au(str filepath) any {
   if _au_disable() { return 0 }
   _au_decode(filepath)
}

fn _sndfile_required(str filepath) any {
   print("sound res: " + filepath + " requires libsndfile; pure-Ny covers WAV, AIFF/AIFC, and AU supported encodings")
   0
}

fn decode(str filepath) any {
   "Decodes the sound file at `filepath`."
   def debug = sound_debug.enabled()
   if debug { print("RES: decode " + filepath) }
   def full = path.normalize(filepath)
   if !fs.is_file(full) { return 0 }
   def ext = str(str.lower(path.extname(full)))
   if ext == ".wav" {
      def res = _decode_wav(full)
      if res != 0 { return res }
   } else if ext == ".au" || ext == ".snd" {
      def res = _decode_au(full)
      if res != 0 { return res }
   } else if ext == ".aiff" || ext == ".aif" || ext == ".aifc" {
      def res = _aiff_decode(full)
      if res != 0 { return res }
   }
   if _snd_available() && !_snd_disable() {
      def res = _decode_sf(full)
      if res != 0 { return res }
   }
   if debug { print("RES: no decoder for " + full) }
   _sndfile_required(full)
}

mut _cache = dict(64)
mut _mtx = 0

fn init() bool {
   "Initializes the sound resource cache."
   if _mtx == 0 { _mtx = mutex_new() }
   true
}

fn shutdown() bool {
   "Clears the sound cache and releases its lock."
   clear_cache()
   if _mtx != 0 { mutex_free(_mtx) _mtx = 0 }
   true
}

fn clear_cache() bool {
   "Removes every cached sound."
   if _mtx == 0 { return true }
   mutex_lock(_mtx)
   _cache = dict(64)
   mutex_unlock(_mtx)
   true
}

fn is_cached(any filepath) bool {
   "Returns whether the sound at `filepath` is cached."
   if _mtx == 0 { return false }
   def full = path.normalize(filepath)
   mutex_lock(_mtx)
   def exists = _cache.contains(full)
   mutex_unlock(_mtx)
   exists
}

fn load(any filepath) any {
   "Loads a sound through the cache."
   if _mtx == 0 { init() }
   def full = path.normalize(filepath)
   mutex_lock(_mtx)
   def cached = _cache.get(full)
   mutex_unlock(_mtx)
   if cached { return cached }
   if !fs.is_file(full) { return 0 }
   def ext = str(str.lower(path.extname(full)))
   mut any asset = 0
   if ext == ".wav" || ext == ".au" || ext == ".snd" || ext == ".aiff" || ext == ".aif" || ext == ".aifc" || ext == ".ogg" || ext == ".flac" || ext == ".mp3" || ext == ".opus" {
      asset = decode(full)
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
   "Returns information about a loaded sound."
   source.sound_info(sound)
}

fn get_image_info(any img) any {
   "Returns information about a loaded image."
   image.get_info(img)
}
