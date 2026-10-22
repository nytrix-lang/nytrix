;; Keywords: sound backend pulse os
;; PulseAudio simple-client backend for Linux audio playback.
;; References:
;; - core.os.sound.backend
;; - core.os
module core.os.sound.backend.pulse(is_available, init, shutdown, stream_open, stream_start, stream_stop, write)
use core
use core.dict_mod
use core.mem
use core.str
use core.common as common
use core.os.sound.diag as sound_debug
use core.os.sound.backend.shared as backend_shared

#linux {
   #link "libpulse-simple.so"
   #include <pulse/simple.h> as "pa_"
   #link "libpulse.so"
   #include <pulse/error.h> as "pa_"
   extern "pulse-simple" {
      fn pa_simple_new(i32 server, ptr app, i32 stream, ptr dev, ptr name, ptr ss, ptr map, ptr attr, ptr err) ptr as "pa_simple_new"
      fn pa_simple_drain(ptr pa, ptr err) i32 as "pa_simple_drain"
      fn pa_simple_flush(ptr pa, ptr err) i32 as "pa_simple_flush"
      fn pa_simple_free(ptr pa) as "pa_simple_free"
      fn pa_simple_write(ptr pa, ptr data, u64 bytes, ptr err) i32 as "pa_simple_write"
   }
   extern "pulse" {
      fn pa_strerror(i32 err) ptr as "pa_strerror"
   }
} #else {
   fn pa_simple_new(..._args) any { 0 }
   fn pa_strerror(any _err) any { 0 }
   fn pa_simple_drain(..._args) int { -1 }
   fn pa_simple_flush(..._args) int { -1 }
   fn pa_simple_free(any _pa) any { 0 }
   fn pa_simple_write(..._args) int { -1 }
}

#endif

fn _get_latency_ms() int { common.env_int_clamped("NY_AUDIO_LATENCY_MS", 40, 10, 500) }
def PA_STREAM_PLAYBACK = 1
def PA_SAMPLE_S16LE = 3
def PA_SAMPLE_FLOAT32LE = 5

fn is_available() bool {
   "Returns whether the PulseAudio backend is available on this host."
   backend_shared.probe_linux_library("libpulse-simple.so", "pa_simple_new") == 1
}

fn init(any ctx) any {
   "Registers the PulseAudio backend in the shared audio context."
   if !is_available() { return 0 }
   if sound_debug.enabled() { print("Sound: Pulse: init") }
   backend_shared.append_output_device(ctx, "PulseAudio Default", "pulse_default")
}

fn shutdown(any ctx) any { "Shuts down PulseAudio backend state stored in `ctx`." }

fn stream_open(any stream) any {
   "Opens a PulseAudio playback stream for the provided stream dictionary."
   if !is_available() { return false }
   def ss = malloc(12)
   def format = stream.get("format", 1)
   def bits = (format == 2) ? 32 : 16
   def rate = stream.get("sample_rate")
   def channels = stream.get("channels")
   def pa_fmt = (format == 2) ? PA_SAMPLE_FLOAT32LE : PA_SAMPLE_S16LE
   store32(ss, pa_fmt, 0)
   store32(ss, rate, 4)
   store8(ss, channels, 8)
   store8(ss, 0, 9)
   store8(ss, 0, 10)
   store8(ss, 0, 11)
   mut attr = 0
   if common.env_truthy("NY_AUDIO_PULSE_TUNE") {
      mut sample_bytes = bits / 8
      if sample_bytes <= 0 { sample_bytes = 2 }
      mut frame_bytes = sample_bytes * channels
      if frame_bytes <= 0 { frame_bytes = 4 }
      mut tlength = (rate * frame_bytes * _get_latency_ms()) / 1000
      mut min_tlength = frame_bytes * 128
      if min_tlength < frame_bytes * 32 { min_tlength = frame_bytes * 32 }
      if tlength < min_tlength { tlength = min_tlength }
      mut minreq = tlength / 4
      mut min_minreq = frame_bytes * 32
      if minreq < min_minreq { minreq = min_minreq }
      if minreq > tlength { minreq = tlength }
      attr = malloc(20)
      store32(attr, 4294967295, 0)
      store32(attr, tlength, 4)
      store32(attr, 0, 8)
      store32(attr, minreq, 12)
      store32(attr, 4294967295, 16)
   }
   def err_ptr = malloc(4)
   store32(err_ptr, 0, 0)
   mut out_dev = common.env_trim("NY_AUDIO_PULSE_DEVICE")
   if !out_dev { out_dev = "" }
   mut ?ptr out_dev_c = 0
   if out_dev.len > 0 { out_dev_c = cstr_dup(out_dev) }
   mut app_name = common.env_trim("NY_AUDIO_PULSE_APP")
   if !app_name || app_name.len == 0 { app_name = "Nytrix Audio" }
   mut stream_name = common.env_trim("NY_AUDIO_PULSE_STREAM")
   if !stream_name || stream_name.len == 0 { stream_name = "Nytrix Output" }
   def app_name_c = cstr_dup(app_name)
   def stream_name_c = cstr_dup(stream_name)
   if sound_debug.enabled() { print("Sound: Pulse: Opening stream: rate=" + to_str(rate) + " chan=" + to_str(channels) + " fmt=" + to_str(format) + " device=" + out_dev) }
   if sound_debug.enabled() { print(f"Audio: Pulse: Calling pa_simple_new(ss={ss}, app={app_name_c}, stream={stream_name_c}, dev={out_dev_c}, err={err_ptr})...") }
   def pa = pa_simple_new(0, app_name_c, PA_STREAM_PLAYBACK, out_dev_c, stream_name_c, ss, 0, attr, err_ptr)
   if sound_debug.enabled() { print("Sound: Pulse: pa_simple_new returned " + to_str(pa)) }
   if pa == 0 {
      if sound_debug.enabled() {
         def errno = load32(err_ptr, 0)
         mut msg = f"pa_simple_new failed(error {errno})"
         def err_msg = pa_strerror(errno)
         if err_msg != 0 { msg = msg + ": " + str.cstr_to_str(err_msg) }
         print("Sound: Pulse: " + msg)
      }
      if app_name_c { free(app_name_c) }
      if stream_name_c { free(stream_name_c) }
      if out_dev_c { free(out_dev_c) }
      free(ss)
      if attr { free(attr) }
      free(err_ptr)
      return false
   }
   if sound_debug.enabled() { print("Sound: Pulse: Stream opened successfully.") }
   if app_name_c { free(app_name_c) }
   if stream_name_c { free(stream_name_c) }
   if out_dev_c { free(out_dev_c) }
   free(ss)
   if attr { free(attr) }
   free(err_ptr)
   stream = stream.set("handle", pa)
   stream = stream.set("bits_per_sample", bits)
   stream
}

fn stream_start(any stream) bool {
   "PulseAudio streams begin writing immediately after open."
   true
}

fn stream_stop(any stream) any {
   "Flushes, drains, and frees a PulseAudio playback stream."
   mut ?ptr pa = stream.get("handle")
   if pa {
      def err_ptr = malloc(4)
      store32(err_ptr, 0, 0)
      pa_simple_drain(pa, err_ptr)
      pa_simple_flush(pa, err_ptr)
      free(err_ptr)
      pa_simple_free(pa)
   }
   stream = stream.set("handle", 0)
}

fn write(any pa, any buf, int size) bool {
   "Writes `size` bytes to a PulseAudio stream."
   if !pa { return false }
   mut ?ptr stream_handle = pa
   mut ?ptr data = buf
   def n = size
   if n <= 0 { return true }
   def err_ptr = malloc(4)
   store32(err_ptr, 0, 0)
   def rc = pa_simple_write(stream_handle, data, n, err_ptr)
   if rc < 0 && sound_debug.enabled() {
      def err = load32(err_ptr, 0)
      def msg = pa_strerror(err)
      if msg != 0 { print(f"Pulse: write failed: {str.cstr_to_str(msg)}") }
   }
   free(err_ptr)
   rc >= 0
}
