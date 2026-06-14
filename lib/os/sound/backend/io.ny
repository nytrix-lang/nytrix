;; Keywords: sound backend io input output stream os
;; Shared sound backend I/O contracts and sample-buffer transfer operations.
;; References:
;; - std.os.sound.backend
;; - std.os
module std.os.sound.backend.io(create, connect, disconnect, get_output_device_count, get_output_device, get_default_output_device_index, outstream_create, outstream_open, outstream_start, outstream_stop, outstream_write, outstream_write_frames, outstream_destroy, get_backend_name, FORMAT_S16LE, FORMAT_FLOAT32LE)
use std.core
use std.core.dict_mod
use std.os
use std.core.common as common
use std.os.sound.diag as sound_debug
use std.os.sound.backend.alsa as audio_alsa
use std.os.sound.backend.pulse as audio_pulse
use std.os.sound.backend.jack as audio_jack
use std.os.sound.backend.winmm as audio_winmm
use std.os.sound.backend.shared as shared

def FORMAT_S16LE = 1
def FORMAT_FLOAT32LE = 2

fn _forced_backend() str { common.env_lower("NY_AUDIO_BACKEND") }

fn _set_backend(dict ctx, int id, str name) dict {
   ctx = ctx.set("backend", id)
   ctx = ctx.set("backend_name", name)
   ctx
}

fn _connect_backend(dict ctx, str backend) any {
   mut id = 0
   mut name = ""
   mut new_ctx = 0
   if eq(backend, "pulse") {
      id = 1
      name = "pulse"
      if audio_pulse.is_available() { new_ctx = audio_pulse.init(ctx) }
   } elif eq(backend, "alsa") {
      id = 2
      name = "alsa"
      if audio_alsa.is_available() { new_ctx = audio_alsa.init(ctx) }
   } elif eq(backend, "winmm") {
      id = 3
      name = "winmm"
      if audio_winmm.is_available() { new_ctx = audio_winmm.init(ctx) }
   } elif eq(backend, "jack") {
      id = 4
      name = "jack"
      if audio_jack.is_available() { new_ctx = audio_jack.init(ctx) }
   }
   if new_ctx { return _set_backend(new_ctx, id, name) }
   0
}

fn _connect_linux_desktop(dict ctx, bool allow_jack=false) any {
   mut nctx = _connect_backend(ctx, "pulse")
   if nctx { return nctx }
   nctx = _connect_backend(ctx, "alsa")
   if nctx { return nctx }
   if allow_jack {
      nctx = _connect_backend(ctx, "jack")
      if nctx { return nctx }
   }
   0
}

fn _stream_backend(any stream) any {
   if !stream { return 0 }
   def device = stream.get("device")
   if !device { return 0 }
   def ctx = device.get("ctx")
   if !ctx { return 0 }
   ctx.get("backend")
}

fn _stream_frame_bytes(dict stream) int {
   def channels = stream.get("channels")
   def bps = stream.get("bits_per_sample", 16)
   mut sample_bytes = bps / 8
   if sample_bytes <= 0 { sample_bytes = 2 }
   channels * sample_bytes
}

fn create() dict {
   "Creates a new AudioIO context."
   mut ctx = dict(16)
   ctx = ctx.set("backend", 0)
   ctx = ctx.set("backend_name", "none")
   ctx = ctx.set("devices", list())
   ctx
}

fn connect(any ctx) any {
   "Connects to the best available audio backend."
   if !ctx { return 0 }
   def forced = _forced_backend()
   if forced == "pulse" { return _connect_backend(ctx, "pulse") } elif forced == "alsa" {
      return _connect_backend(ctx, "alsa")
   } elif forced == "jack" {
      return _connect_backend(ctx, "jack")
   } elif forced == "winmm" {
      return _connect_backend(ctx, "winmm")
   } elif forced == "portaudio" || forced == "pa" || forced == "miniaudio" {
      def name = os()
      if eq(name, "linux") { return _connect_linux_desktop(ctx, true) }
      if eq(name, "windows") { return _connect_backend(ctx, "winmm") }
      return 0
   } elif forced == "libsoundio" || forced == "soundio" {
      def name = os()
      if eq(name, "linux") { return _connect_linux_desktop(ctx, true) }
      if eq(name, "windows") { return _connect_backend(ctx, "winmm") }
      return 0
   }
   def name = os()
   if eq(name, "linux") { return _connect_linux_desktop(ctx) } elif eq(name, "windows") { return _connect_backend(ctx, "winmm") }
   ctx
}

fn disconnect(any ctx) any {
   "Implements `disconnect`."
   if !ctx { return nil }
   def b = ctx.get("backend")
   if b == 1 { audio_pulse.shutdown(ctx) }
   elif b == 2 { audio_alsa.shutdown(ctx) }
   elif b == 4 { audio_jack.shutdown(ctx) }
   elif b == 3 { audio_winmm.shutdown(ctx) }
   ctx = ctx.set("backend", 0)
   ctx = ctx.set("backend_name", "none")
}

fn get_output_device_count(any ctx) int {
   "Gets output device count."
   if !ctx { return 0 }
   def devices = ctx.get("devices", 0)
   if devices == 0 { return 0 }
   def n = devices.len
   if sound_debug.enabled() { print("Sound: get_output_device_count: count=" + to_str(n)) }
   n
}

fn get_output_device(any ctx, int index) any {
   "Gets output device."
   if !ctx { return 0 }
   def devices = ctx.get("devices", list())
   if index < 0 || index >= devices.len { return 0 }
   devices.get(index)
}

fn get_default_output_device_index(any ctx) int {
   "Gets default output device index."
   0
}

fn outstream_create(any device) dict {
   "Creates an output stream for a specific device."
   if sound_debug.enabled() { print("Sound: outstream_create: dev=" + to_str(device)) }
   mut stream = dict(16)
   stream = stream.set("device", device)
   stream = stream.set("format", FORMAT_S16LE)
   stream = stream.set("sample_rate", 44100)
   stream = stream.set("channels", 2)
   stream = stream.set("callback", nil)
   stream = stream.set("running", false)
   if sound_debug.enabled() { print("Sound: outstream_create: stream keys=" + to_str(dict_keys(stream))) }
   stream
}

fn outstream_open(any stream, int format, int sample_rate, int channels, ?fnptr callback) any {
   "Implements `outstream_open`."
   if sound_debug.enabled() {
      print("Sound: Entering outstream_open.")
      print("Sound: outstream_open: stream keys before set=" + to_str(dict_keys(stream)))
   }
   if !stream { return false }
   stream = stream.set("format", format)
   stream = stream.set("sample_rate", sample_rate)
   stream = stream.set("channels", channels)
   stream = stream.set("callback", callback)
   stream = stream.set("bits_per_sample", (format == FORMAT_FLOAT32LE) ? 32 : 16)
   def device = stream.get("device", 0)
   if sound_debug.enabled() { print("Sound: outstream_open: device=" + to_str(device)) }
   if !device { return false }
   def ctx = device.get("ctx", 0)
   if sound_debug.enabled() { print("Sound: outstream_open: ctx=" + to_str(ctx)) }
   if !ctx { return false }
   def backend = ctx.get("backend", 0)
   if sound_debug.enabled() { print("Sound: outstream_open: backend=" + to_str(backend)) }
   if backend == 1 { if audio_pulse.stream_open(stream) { return stream } else { return 0 } }
   if backend == 2 { if audio_alsa.stream_open(stream) { return stream } else { return 0 } }
   if backend == 4 { if audio_jack.stream_open(stream) { return stream } else { return 0 } }
   if backend == 3 { if audio_winmm.stream_open(stream) { return stream } else { return 0 } }
   0
}

fn outstream_start(any stream) bool {
   "Implements `outstream_start`."
   if !stream { return false }
   def b = _stream_backend(stream)
   if !b { return false }
   mut ok = false
   if b == 1 { ok = audio_pulse.stream_start(stream) }
   elif b == 2 { ok = audio_alsa.stream_start(stream) }
   elif b == 4 { ok = audio_jack.stream_start(stream) }
   elif b == 3 { ok = audio_winmm.stream_start(stream) }
   if ok { stream = stream.set("running", true) }
   ok
}

fn outstream_stop(any stream) any {
   "Implements `outstream_stop`."
   if !stream { return nil }
   def b = _stream_backend(stream)
   if !b { return nil }
   if b == 1 { audio_pulse.stream_stop(stream) }
   elif b == 2 { audio_alsa.stream_stop(stream) }
   elif b == 4 { audio_jack.stream_stop(stream) }
   elif b == 3 { audio_winmm.stream_stop(stream) }
   stream = stream.set("running", false)
}

fn outstream_write(any stream, any buf, int bytes) bool {
   "Writes raw bytes to the output stream."
   if !stream { return false }
   def b = _stream_backend(stream)
   if !b { return false }
   def handle = stream.get("handle")
   if !handle { return false }
   def nbytes = bytes
   if nbytes <= 0 { return true }
   if b == 1 { return audio_pulse.write(handle, buf, nbytes) }
   elif b == 2 {
      def frame_bytes = _stream_frame_bytes(stream)
      if frame_bytes <= 0 { return false }
      return audio_alsa.write(handle, buf, nbytes / frame_bytes, frame_bytes)
   }
   elif b == 4 {
      def frame_bytes = _stream_frame_bytes(stream)
      if frame_bytes <= 0 { return false }
      return audio_jack.write(handle, buf, nbytes / frame_bytes, frame_bytes)
   }
   elif b == 3 { return audio_winmm.write(handle, buf, nbytes) }
   false
}

fn outstream_write_frames(any stream, any buf, int frames) bool {
   "Writes frames to the output stream."
   if !stream { return false }
   def b = _stream_backend(stream)
   if !b { return false }
   def handle = stream.get("handle")
   if !handle { return false }
   def frame_bytes = _stream_frame_bytes(stream)
   if frame_bytes <= 0 { return false }
   def bytes = frames * frame_bytes
   if b == 1 { return audio_pulse.write(handle, buf, bytes) }
   elif b == 2 { return audio_alsa.write(handle, buf, frames, frame_bytes) }
   elif b == 4 { return audio_jack.write(handle, buf, frames, frame_bytes) }
   elif b == 3 { return audio_winmm.write(handle, buf, bytes) }
   false
}

fn outstream_destroy(any stream) any {
   "Implements `outstream_destroy`."
   outstream_stop(stream)
}

fn get_backend_name(any ctx) str {
   "Gets backend name."
   if !ctx { return "none" }
   return ctx.get("backend_name", "none")
}

#main {
   def ctx = create()
   assert(is_dict(ctx) && get_backend_name(ctx) == "none", "sound backend io ctx")
   assert(get_output_device_count(ctx) == 0 && get_default_output_device_index(ctx) == 0 && get_output_device(ctx, 0) == 0, "sound backend io empty devices")
   def ctx2 = shared.append_output_device(ctx, "Probe Device", "probe")
   assert(is_dict(ctx2) && get_output_device_count(ctx2) == 1, "sound backend io append device")
   def dev = get_output_device(ctx2, 0)
   assert(is_dict(dev) && dev.get("name", "") == "Probe Device", "sound backend io device")
   def stream = outstream_create(dev)
   assert(is_dict(stream) && stream.get("format", 0) == FORMAT_S16LE, "sound backend io stream")
   assert(!outstream_open(stream, FORMAT_S16LE, 44100, 2, nil) && !outstream_start(stream), "sound backend io unopened stream")
   def buf = malloc(32)
   memset(buf, 0, 32)
   assert(!outstream_write(stream, buf, 32) && !outstream_write_frames(stream, buf, 8), "sound backend io write without backend")
   outstream_destroy(stream)
   disconnect(ctx2)
   free(buf)
   assert(shared.init_output_device(create(), false, "No Device", "none") == 0, "sound backend io init false")
   assert(is_dict(shared.init_output_device(create(), true, "Ready Device", "ready")), "sound backend io init true")
   print("✓ std.os.sound.backend.io self-test passed")
}
