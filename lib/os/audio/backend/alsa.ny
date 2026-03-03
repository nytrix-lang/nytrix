;; Keywords: audio alsa linux io

module std.os.audio.backend.alsa (
   is_available, init, shutdown,
   stream_open, stream_start, stream_stop,
   write
)

use std.core *
use std.core.dict_mod *
use std.os.ffi *
use std.os.audio.backend.shared as backend_shared

if(comptime{ __os_name() == "linux" }){
   #link "asound"

   extern fn snd_pcm_open(pcm: ptr, name: ptr, stream: i32, mode: i32): i32 as "snd_pcm_open"
   extern fn snd_pcm_set_params(pcm: ptr, format: i32, access: i32, channels: i32, rate: i32, soft_resample: i32, latency: i32): i32 as "snd_pcm_set_params"
   extern fn snd_pcm_writei(pcm: ptr, buffer: ptr, size: i64): i64 as "snd_pcm_writei"
   extern fn snd_pcm_close(pcm: ptr): i32 as "snd_pcm_close"
   extern fn snd_pcm_recover(pcm: ptr, err: i32, silent: i32): i32 as "snd_pcm_recover"
   extern fn snd_pcm_prepare(pcm: ptr): i32 as "snd_pcm_prepare"
   extern fn snd_pcm_drain(pcm: ptr): i32 as "snd_pcm_drain"
}

def SND_PCM_STREAM_PLAYBACK = 0
def SND_PCM_FORMAT_S16_LE = 2
def SND_PCM_FORMAT_FLOAT_LE = 14
def SND_PCM_ACCESS_RW_INTERLEAVED = 3

mut _lib = 0
mut _avail = -1

fn is_available(){
   "Returns whether the ALSA backend is available on this host."
   def state = backend_shared.probe_linux_library_once(_avail, _lib, "asound", "snd_pcm_open")
   _avail = get(state, 0)
   _lib = get(state, 1)
   _avail == 1
}

fn _push_unique(lst, item){
   "Appends `item` only if it is a non-empty string not already present in `lst`."
   if(!is_str(item) || len(item) == 0){ return lst }
   mut i = 0
   while(i < len(lst)){
      if(eq(get(lst, i), item)){ return lst }
      i += 1
   }
   append(lst, item)
}

fn _build_candidates(dev_id){
   "Builds the ordered list of ALSA device names to probe."
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

fn _latency_us(){
   "Returns the configured ALSA target latency in microseconds."
   mut us = 120000
   def v = env("NY_AUDIO_ALSA_LATENCY_MS")
   if(v){
      def ms = atoi(v)
      if(ms >= 20 && ms <= 2000){ us = ms * 1000 }
   }
   us
}

fn init(ctx){
   "Registers the ALSA backend in the shared audio context."
   backend_shared.init_output_device(ctx, is_available(), "ALSA Default", "default")
}

fn shutdown(ctx){
   "Shuts down ALSA backend state stored in `ctx`."
   if(ctx){ return ctx }
   0
}

fn stream_open(stream){
   "Opens an ALSA playback stream for the provided stream dictionary."
   if(!is_available()){ return false }
   def device = core.get(stream, "device")
   mut dev_id = env("NY_AUDIO_ALSA_DEVICE")
   if(!dev_id || len(dev_id) == 0){
      dev_id = core.get(device, "id", "")
      if(!is_str(dev_id) || len(dev_id) == 0 || eq(dev_id, "default")){
         dev_id = ""
      }
   }
   def format = core.get(stream, "format", 1)
   mut alsa_fmt = SND_PCM_FORMAT_S16_LE
   mut bits = 16
   if(format != 1){
      alsa_fmt = SND_PCM_FORMAT_FLOAT_LE
      bits = 32
   }
   def rate = core.get(stream, "sample_rate")
   def channels = core.get(stream, "channels")
   def pcm_ptr = malloc(8)
   mut opened = 0
   mut chosen = ""
   mut tries = _build_candidates(dev_id)
   mut i = 0
   while(i < len(tries) && opened == 0){
      def cand = get(tries, i)
      def cand_c = cstr(cand)
      if(env("NY_AUDIO_DEBUG")){ print(f"ALSA: trying device '{cand}' (ptr={pcm_ptr})") }
      def rc = snd_pcm_open(pcm_ptr, cand_c, SND_PCM_STREAM_PLAYBACK, 0)
      if(env("NY_AUDIO_DEBUG")){ print(f"ALSA: snd_pcm_open returned {rc}") }
      if(rc >= 0){
         def pcm = load64(pcm_ptr, 0)
         if(env("NY_AUDIO_DEBUG")){ print(f"ALSA: pcm handle = {pcm}") }
         if(pcm != 0){
         def serr = snd_pcm_set_params(pcm, alsa_fmt, SND_PCM_ACCESS_RW_INTERLEAVED, channels, rate, 1, _latency_us())
         if(serr >= 0){
               opened = pcm
               chosen = cand
         } else {
               if(env("NY_AUDIO_DEBUG")){ print(f"ALSA: snd_pcm_set_params failed for '{cand}': {serr}") }
               snd_pcm_close(pcm)
         }
         }
      } else {
         if(env("NY_AUDIO_DEBUG")){ print(f"ALSA: snd_pcm_open failed for '{cand}': {rc}") }
      }
      i += 1
   }
   free(pcm_ptr)
   if(opened == 0){ return false }
   if(env("NY_AUDIO_DEBUG")){
      print(f"ALSA: opened '{chosen}' @ {rate}Hz ch={channels}")
   }
   stream = dict_set(stream, "handle", opened)
   stream = dict_set(stream, "alsa_device", chosen)
   stream = dict_set(stream, "bits_per_sample", bits)
   stream
}

fn stream_start(stream){
   "Prepares an ALSA playback stream for writes."
   def pcm = get(stream, "handle")
   if(!pcm){ return false }
   snd_pcm_prepare(pcm) >= 0
}

fn stream_stop(stream){
   "Drains and closes an ALSA playback stream."
   def pcm = get(stream, "handle")
   if(pcm){ snd_pcm_drain(pcm) }
   if(pcm){ snd_pcm_close(pcm) }
   stream = dict_set(stream, "handle", 0)
}

fn write(pcm, buf, frames, frame_bytes=4){
   "Writes interleaved audio frames to ALSA, recovering transient write errors when possible."
   if(!pcm){ return false }
   mut ptr = buf
   mut left = frames
   mut fb = frame_bytes
   if(fb <= 0){ fb = 4 }
   while(left > 0){
      def written = snd_pcm_writei(pcm, ptr, left)
      if(written < 0){
         def rec = snd_pcm_recover(pcm, written, 0)
         if(rec < 0){ return false }
         continue
      }
      if(written == 0){ return false }
      left = left - written
      if(left > 0){
         ptr = ptr_add(ptr, written * fb)
      }
   }
   true
}
