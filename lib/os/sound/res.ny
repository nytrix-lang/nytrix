;; Keywords: sound res os
;; Unified resource loader for sound and image files.
;; Uses libsndfile for sound (optional, bound via dlopen).
;; Pure-Ny WAV/AU fallback when libsndfile is unavailable.
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
use core.os.ffi as ffi

;; libsndfile is optional: bound through dlopen so hosts without it can still
;; decode WAV and AU files via the pure-Ny fallback.
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
   def debug = sound_debug.enabled()
   def res = file_read(filepath)
   if is_err(res) { return 0 }
   def data = unwrap(res)
   if data.len < 44 { return 0 }
   if load8(data, 0) != 82 || load8(data, 1) != 73 || load8(data, 2) != 70 || load8(data, 3) != 70 { return 0 }
   def file_size = load32(data, 4)
   if load8(data, 8) != 87 || load8(data, 9) != 65 || load8(data, 10) != 86 || load8(data, 11) != 69 { return 0 }
   if load8(data, 12) != 102 || load8(data, 13) != 109 || load8(data, 14) != 116 || load8(data, 15) != 32 { return 0 }
   def fmt_chunk_size = load32(data, 16)
   if fmt_chunk_size < 16 { return 0 }
   def audio_fmt = load16(data, 20)
   def channels = load16(data, 22)
   def sample_rate = load32(data, 24)
   def byte_rate = load32(data, 28)
   def block_align = load16(data, 32)
   def bits_per_sample = load16(data, 34)
   def data_offset = 20 + fmt_chunk_size
   mut pos = data_offset
   while pos + 8 <= data.len {
      def chunk_id = load32(data, pos)
      def chunk_size = load32(data, pos + 4)
      if chunk_id == 0x61746164 { break } ;; "data"
      pos += 8 + chunk_size
      if pos > data.len { return 0 }
   }
   if pos + 8 > data.len { return 0 }
   def data_size = load32(data, pos + 4)
   pos += 8
   if pos + data_size > data.len { return 0 }
   ;; WAV payloads are binary: copy raw bytes at `pos`. Do not route them
   ;; through `slice`, which applies UTF-8 code-point semantics and would
   ;; re-encode bytes >= 0x80 while changing the decoded sample count.
   if audio_fmt == 3 { ;; IEEE float
      if bits_per_sample != 32 { return 0 }
      def out_ptr = malloc(data_size)
      memcpy(out_ptr, ptr_add(data, pos), data_size)
      return make_memory_source(out_ptr, data_size, channels, sample_rate, 32, SAMPLE_FMT_F32, 3)
   }
   if audio_fmt == 1 { ;; PCM
      if bits_per_sample == 8 {
         mut out = malloc(data_size)
         mut i = 0
         while i < data_size {
            def v = load8(data, pos + i)
            store8(out, (v + 128) & 0xff, i)
            i += 1
         }
         make_memory_source(out, data_size, channels, sample_rate, 8, SAMPLE_FMT_U8, 1)
      } elif bits_per_sample == 16 {
         def sample_count = data_size / 2
         def out_ptr = malloc(sample_count * 2)
         mut i = 0
         while i < sample_count {
            def v = load16(data, pos + i * 2)
            store16(out_ptr, v, i * 2)
            i += 1
         }
         make_memory_source(out_ptr, sample_count * 2, channels, sample_rate, 16, SAMPLE_FMT_S16, 1)
      } elif bits_per_sample == 24 {
         def sample_count = data_size / 3
         def out_ptr = malloc(data_size)
         memcpy(out_ptr, ptr_add(data, pos), data_size)
         make_memory_source(out_ptr, sample_count * 3, channels, sample_rate, 24, SAMPLE_FMT_S24, 1)
      } elif bits_per_sample == 32 {
         def sample_count = data_size / 4
         def out_ptr = malloc(sample_count * 4)
         mut i = 0
         while i < sample_count {
            def v = load32(data, pos + i * 4)
            store32(out_ptr, v, i * 4)
            i += 1
         }
         make_memory_source(out_ptr, sample_count * 4, channels, sample_rate, 32, SAMPLE_FMT_S32, 1)
      } else { return 0 }
   } else { return 0 }
}

;; Reads a big-endian 16-bit value at offset `off`.
fn load16_be(any p, int off) int {
   (load8(p, off) << 8) | load8(p, off + 1)
}

;; Reads a big-endian 24-bit value at offset `off`.
fn load24_be(any p, int off) int {
   (load8(p, off) << 16) | (load8(p, off + 1) << 8) | load8(p, off + 2)
}

;; Reads a big-endian 32-bit value at offset `off`.
fn load32_be(any p, int off) int {
   (load8(p, off) << 24) | (load8(p, off + 1) << 16) | (load8(p, off + 2) << 8) | load8(p, off + 3)
}

;; Pure-Ny AU decoder (SND/AU format - big endian)
fn _au_decode(str filepath) any {
   def res = file_read(filepath)
   if is_err(res) { return 0 }
   def data = unwrap(res)
   if data.len < 24 { return 0 }
   if load8(data, 0) != 46 || load8(data, 1) != 115 || load8(data, 2) != 110 || load8(data, 3) != 100 { return 0 } ;; ".snd"
   def data_offset = load32_be(data, 4)
   def data_size = load32_be(data, 8)
   def encoding = load32_be(data, 12)
   def sample_rate = load32_be(data, 16)
   def channels = load32_be(data, 20)
   if data_offset < 24 || data_offset + data_size > data.len { return 0 }
   ;; AU payloads are binary. Copy them with load/store; `slice` is a text
   ;; operation and rejects the file buffer.
   if encoding == 3 { ;; 16-bit linear PCM
      if data_size % 2 != 0 { return 0 }
      def sample_count = data_size / 2
      def out_ptr = malloc(data_size)
      mut i = 0
      while i < sample_count {
         def v = load16_be(data, data_offset + i * 2)
         store16(out_ptr, v, i * 2)
         i += 1
      }
      make_memory_source(out_ptr, data_size, channels, sample_rate, 16, SAMPLE_FMT_S16, 1)
   } elif encoding == 4 { ;; 24-bit linear PCM
      if data_size % 3 != 0 { return 0 }
      def sample_count = data_size / 3
      def out_ptr = malloc(data_size)
      mut i = 0
      while i < sample_count {
         def v = load24_be(data, data_offset + i * 3)
         store8(out_ptr, v & 255, i * 3)
         store8(out_ptr, (v >> 8) & 255, i * 3 + 1)
         store8(out_ptr, (v >> 16) & 255, i * 3 + 2)
         i += 1
      }
      make_memory_source(out_ptr, data_size, channels, sample_rate, 24, SAMPLE_FMT_S24, 1)
   } elif encoding == 5 { ;; 32-bit linear PCM
      if data_size % 4 != 0 { return 0 }
      def sample_count = data_size / 4
      def out_ptr = malloc(data_size)
      mut i = 0
      while i < sample_count {
         def v = load32_be(data, data_offset + i * 4)
         store32(out_ptr, v, i * 4)
         i += 1
      }
      make_memory_source(out_ptr, data_size, channels, sample_rate, 32, SAMPLE_FMT_S32, 1)
   } elif encoding == 6 { ;; 32-bit float
      def out_ptr = malloc(data_size)
      memcpy(out_ptr, ptr_add(data, data_offset), data_size)
      make_memory_source(out_ptr, data_size, channels, sample_rate, 32, SAMPLE_FMT_F32, 3)
   } elif encoding == 7 { ;; 64-bit float
      return 0
   } else { return 0 }
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
   print("sound res: " + filepath + " requires libsndfile; pure-Ny covers WAV PCM/float(8/16/24/32 and f32) and AU")
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
   if ext == ".wav" || ext == ".au" || ext == ".snd" || ext == ".ogg" || ext == ".flac" || ext == ".mp3" || ext == ".aiff" || ext == ".aif" || ext == ".opus" {
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
