;; Keywords: sound backend alsa os
;; ALSA sound backend for Linux audio playback.
;; References:
;; - std.os.sound.backend
;; - std.os
module std.os.sound.backend.alsa(is_available, init, shutdown, stream_open, stream_start, stream_stop, write)
use std.core
use std.core.mem (cstr)
use std.core.dict_mod
use std.core.common as common
use std.os.sound.diag as sound_debug
use std.os.sound.backend.shared as backend_shared

#linux {
   #link "libasound.so"
   extern "asound" {
      fn snd_pcm_open(ptr pcmp, ptr name, i32 stream, i32 mode) i32
      fn snd_pcm_set_params(ptr pcm, i32 format, i32 access, u32 channels, u32 rate, i32 soft_resample, u32 latency) i32
      fn snd_pcm_close(ptr pcm) i32
      fn snd_pcm_prepare(ptr pcm) i32
      fn snd_pcm_drain(ptr pcm) i32
      fn snd_pcm_writei(ptr pcm, ptr buffer, u64 size) i64
      fn snd_pcm_recover(ptr pcm, i32 err, i32 silent) i32
   }
} #else {
   fn snd_pcm_open(..._args) int {
      "Runs the snd pcm open operation."
      -1
   }
   fn snd_pcm_set_params(..._args) int {
      "Runs the snd pcm set params operation."
      -1
   }
   fn snd_pcm_close(any _pcm) int {
      "Runs the snd pcm close operation."
      0
   }
   fn snd_pcm_prepare(any _pcm) int {
      "Runs the snd pcm prepare operation."
      -1
   }
   fn snd_pcm_drain(any _pcm) int {
      "Runs the snd pcm drain operation."
      0
   }
   fn snd_pcm_writei(..._args) int {
      "Runs the snd pcm writei operation."
      -1
   }
   fn snd_pcm_recover(..._args) int {
      "Runs the snd pcm recover operation."
      -1
   }
} #endif
def SND_PCM_STREAM_PLAYBACK = 0
def SND_PCM_FORMAT_S16_LE = 2
def SND_PCM_FORMAT_FLOAT_LE = 14
def SND_PCM_ACCESS_RW_INTERLEAVED = 3
mut _lib = 0
mut _avail = -1

fn is_available() bool {
   "Returns whether the ALSA backend is available on this host."
   def state = backend_shared.probe_linux_library_once(_avail, _lib, "asound", "snd_pcm_open")
   _avail, _lib = state.get(0), state.get(1)
   _avail == 1
}

fn _push_unique(list lst, any item) list {
   if !is_str(item) || item.len == 0 { return lst }
   mut i = 0
   while i < lst.len {
      if eq(lst.get(i), item) { return lst }
      i += 1
   }
   lst.append(item)
}

fn _build_candidates(any dev_id) list {
   mut out = list()
   out = _push_unique(out, dev_id)
   out = _push_unique(out, "pipewire")
   out = _push_unique(out, "pulse")
   out = _push_unique(out, "default")
   out = _push_unique(out, "sysdefault")
   out = _push_unique(out, "front")
   out = _push_unique(out, "plughw:0,0")
   out = _push_unique(out, "hw:0,0")
   out = _push_unique(out, "null")
   out
}

fn _latency_us() int { common.env_int_clamped("NY_AUDIO_ALSA_LATENCY_MS", 120, 20, 2000) * 1000 }

fn init(any ctx) any {
   "Registers the ALSA backend in the shared audio context."
   backend_shared.init_output_device(ctx, is_available(), "ALSA Default", "default")
}

fn shutdown(any ctx) any {
   "Shuts down ALSA backend state stored in `ctx`."
   if ctx { return ctx }
   0
}

fn stream_open(any stream) any {
   "Opens an ALSA playback stream for the provided stream dictionary."
   if !is_available() { return false }
   def device = stream.get("device")
   mut dev_id = common.env_trim("NY_AUDIO_ALSA_DEVICE")
   if !dev_id { dev_id = "" }
   if dev_id.len == 0 {
      dev_id = is_dict(device) ? device.get("id", "") : ""
      if !is_str(dev_id) || dev_id.len == 0 || eq(dev_id, "default") { dev_id = "" }
   }
   def format = stream.get("format", 1)
   mut alsa_fmt = SND_PCM_FORMAT_S16_LE
   mut bits = 16
   if format != 1 {
      alsa_fmt = SND_PCM_FORMAT_FLOAT_LE
      bits = 32
   }
   def rate = stream.get("sample_rate")
   def channels = stream.get("channels")
   def pcm_ptr = malloc(8)
   mut opened = 0
   mut chosen = ""
   mut tries = _build_candidates(dev_id)
   mut i = 0
   while i < tries.len && opened == 0 {
      def cand = tries.get(i)
      def cand_c = cstr(cand)
      if sound_debug.enabled() { print(f"ALSA: trying device '{cand}' (ptr={pcm_ptr})") }
      def rc = snd_pcm_open(pcm_ptr, cand_c, SND_PCM_STREAM_PLAYBACK, 0)
      if sound_debug.enabled() { print(f"ALSA: snd_pcm_open returned {rc}") }
      if rc >= 0 {
         def pcm = load64(pcm_ptr, 0)
         if sound_debug.enabled() { print(f"ALSA: pcm handle = {pcm}") }
         if pcm != 0 {
            def serr = snd_pcm_set_params(pcm, alsa_fmt, SND_PCM_ACCESS_RW_INTERLEAVED, channels, rate, 1, _latency_us())
            if serr >= 0 {
               opened = pcm
               chosen = cand
            } else {
               if sound_debug.enabled() { print(f"ALSA: snd_pcm_set_params failed for '{cand}': {serr}") }
               snd_pcm_close(pcm)
            }
         }
      } else {
         if sound_debug.enabled() { print(f"ALSA: snd_pcm_open failed for '{cand}': {rc}") }
      }
      i += 1
   }
   free(pcm_ptr)
   if opened == 0 { return false }
   if sound_debug.enabled() { print(f"ALSA: opened '{chosen}' @ {rate}Hz ch={channels}") }
   stream = stream.set("handle", opened)
   stream = stream.set("alsa_device", chosen)
   stream = stream.set("bits_per_sample", bits)
   stream
}

fn stream_start(any stream) bool {
   "Prepares an ALSA playback stream for writes."
   def pcm = stream.get("handle")
   if !pcm { return false }
   snd_pcm_prepare(pcm) >= 0
}

fn stream_stop(any stream) any {
   "Drains and closes an ALSA playback stream."
   def pcm = stream.get("handle")
   if pcm { snd_pcm_drain(pcm) }
   if pcm { snd_pcm_close(pcm) }
   stream = stream.set("handle", 0)
}

fn write(any pcm, any buf, int frames, int frame_bytes=4) bool {
   "Writes interleaved audio frames to ALSA, recovering transient write errors when possible."
   if !pcm { return false }
   mut ptr = buf
   mut left = frames
   mut fb = frame_bytes
   if fb <= 0 { fb = 4 }
   while left > 0 {
      def written = snd_pcm_writei(pcm, ptr, left)
      if written < 0 {
         def rec = snd_pcm_recover(pcm, int(written), 0)
         if rec < 0 { return false }
         continue
      }
      if written == 0 { return false }
      left = left - written
      if left > 0 { ptr = ptr_add(ptr, written * fb) }
   }
   true
}
