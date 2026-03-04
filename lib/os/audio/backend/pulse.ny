;; Keywords: audio pulse linux io

module std.os.audio.backend.pulse (
   is_available, init, shutdown,
   stream_open, stream_start, stream_stop,
   write
)

use std.core *
use std.core.dict_mod *
use std.core.mem *
use std.str *
use std.util.common as common
use std.os.audio.backend.shared as backend_shared

if(comptime{ __os_name() == "linux" }){
   #link "libpulse-simple.so"
   #include <pulse/simple.h> as "pa_"
   #link "libpulse.so"
   #include <pulse/error.h> as "pa_"
}

fn _get_latency_ms(){
   "Returns the configured PulseAudio latency target in milliseconds."
   mut ms = 40
   def v = env("NY_AUDIO_LATENCY_MS")
   if(v){
      def n = atoi(v)
      if(n >= 10 && n <= 500){ ms = n }
   }
   ms
}

def PA_STREAM_PLAYBACK = 1
def PA_SAMPLE_S16LE = 3
def PA_SAMPLE_FLOAT32LE = 5

mut _lib = 0
mut _avail = -1

fn is_available(){
   "Returns whether the PulseAudio backend is available on this host."
   def state = backend_shared.probe_linux_library_once(_avail, _lib, "pulse-simple", "pa_simple_new")
   _avail = get(state, 0)
   _lib = get(state, 1)
   _avail == 1
}

fn init(ctx){
   "Registers the PulseAudio backend in the shared audio context."
   if(!is_available()){ return 0 }
   if(env("NY_AUDIO_DEBUG")){ print("Audio: Pulse: init") }
   backend_shared.append_output_device(ctx, "PulseAudio Default", "pulse_default")
}

fn shutdown(ctx){
   "Shuts down PulseAudio backend state stored in `ctx`."
   common.touch(ctx)
}

fn stream_open(stream){
   "Opens a PulseAudio playback stream for the provided stream dictionary."
   if(!is_available()){ return false }
   def ss = malloc(12)
   def format = core.get(stream, "format", 1)
   def bits = (format == 2) ? 32 : 16
   def rate = core.get(stream, "sample_rate")
   def channels = core.get(stream, "channels")
   def pa_fmt = (format == 2) ? PA_SAMPLE_FLOAT32LE : PA_SAMPLE_S16LE
   store32(ss, pa_fmt, 0)
   store32(ss, rate, 4)
   store8(ss, channels, 8)
   store8(ss, 0, 9)
   store8(ss, 0, 10)
   store8(ss, 0, 11)

   mut attr = 0
   if(common.env_truthy("NY_AUDIO_PULSE_TUNE")){
      mut sample_bytes = bits / 8
      if(sample_bytes <= 0){ sample_bytes = 2 }
      mut frame_bytes = sample_bytes * channels
      if(frame_bytes <= 0){ frame_bytes = 4 }
      mut tlength = (rate * frame_bytes * _get_latency_ms()) / 1000
      mut min_tlength = frame_bytes * 128
      if(min_tlength < frame_bytes * 32){ min_tlength = frame_bytes * 32 }
      if(tlength < min_tlength){ tlength = min_tlength }
      mut minreq = tlength / 4
      mut min_minreq = frame_bytes * 32
      if(minreq < min_minreq){ minreq = min_minreq }
      if(minreq > tlength){ minreq = tlength }
      attr = malloc(20)
      store32(attr, 4294967295, 0)
      store32(attr, tlength, 4)
      store32(attr, 0, 8)
      store32(attr, minreq, 12)
      store32(attr, 4294967295, 16)
   }

   def err_ptr = malloc(4)
   store32(err_ptr, 0, 0)

   mut out_dev = env("NY_AUDIO_PULSE_DEVICE")
   if(!out_dev || len(out_dev) == 0){ out_dev = 0 }
   mut app_name = env("NY_AUDIO_PULSE_APP")
   if(!app_name || len(app_name) == 0){ app_name = "Nytrix Audio" }
   mut stream_name = env("NY_AUDIO_PULSE_STREAM")
   if(!stream_name || len(stream_name) == 0){ stream_name = "Nytrix Output" }
   def out_dev_c = out_dev ? cstr_dup(out_dev) : 0
   def app_name_c = cstr_dup(app_name)
   def stream_name_c = cstr_dup(stream_name)

   if(env("NY_AUDIO_DEBUG")){
      print("Audio: Pulse: Opening stream: rate=" + to_str(rate) + " chan=" + to_str(channels) + " fmt=" + to_str(format))
   }

   if(env("NY_AUDIO_DEBUG")){
      print(f"Audio: Pulse: Calling pa_simple_new(ss={ss}, app={app_name_c}, stream={stream_name_c}, dev={out_dev_c}, err={err_ptr})...")
   }
   def pa = pa_simple_new(0, app_name_c, PA_STREAM_PLAYBACK, out_dev_c, stream_name_c, ss, 0, attr, err_ptr)
   if(env("NY_AUDIO_DEBUG")){
      print("Audio: Pulse: pa_simple_new returned " + to_str(pa))
   }
   if(pa == 0){
      if(env("NY_AUDIO_DEBUG")){
         def errno = load32(err_ptr, 0)
         mut msg = f"pa_simple_new failed (error {errno})"
         def err_msg = pa_strerror(errno)
         if(err_msg != 0){ msg = msg + ": " + cstr_to_str(err_msg) }
         print("Audio: Pulse: " + msg)
      }
      if(app_name_c){ free(app_name_c) }
      if(stream_name_c){ free(stream_name_c) }
      if(out_dev_c){ free(out_dev_c) }
      free(ss)
      if(attr){ free(attr) }
      free(err_ptr)
      return false
   }

   if(env("NY_AUDIO_DEBUG")){ print("Audio: Pulse: Stream opened successfully.") }

   if(app_name_c){ free(app_name_c) }
   if(stream_name_c){ free(stream_name_c) }
   if(out_dev_c){ free(out_dev_c) }
   free(ss)
   if(attr){ free(attr) }
   free(err_ptr)

   stream = dict_set(stream, "handle", pa)
   stream = dict_set(stream, "bits_per_sample", bits)
   stream
}

fn stream_start(stream){
   "PulseAudio streams begin writing immediately after open."
   common.touch(stream)
   true
}

fn stream_stop(stream){
   "Flushes, drains, and frees a PulseAudio playback stream."
   def pa = get(stream, "handle")
   if(pa){
      def err_ptr = malloc(4)
      store32(err_ptr, 0, 0)
      pa_simple_drain(pa, err_ptr)
      pa_simple_flush(pa, err_ptr)
      free(err_ptr)
      pa_simple_free(pa)
   }
   stream = dict_set(stream, "handle", 0)
}

fn write(pa, buf, size){
   "Writes `size` bytes to a PulseAudio stream."
   if(!pa){ return false }
   def n = size
   if(n <= 0){ return true }
   def err_ptr = malloc(4)
   store32(err_ptr, 0, 0)
   def rc = pa_simple_write(pa, buf, n, err_ptr)
   if(rc < 0 && env("NY_AUDIO_DEBUG")){
      def err = load32(err_ptr, 0)
      def msg = pa_strerror(err)
      if(msg != 0){ print(f"Pulse: write failed: {cstr_to_str(msg)}") }
   }
   free(err_ptr)
   rc >= 0
}
