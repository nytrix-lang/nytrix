;; Keywords: sound backend jack
;; JACK sound backend probing and playback integration.
module std.os.sound.backend.jack(is_available, init, shutdown, stream_open, stream_start, stream_stop, write)
use std.core
use std.core.mem (cstr)
use std.core.dict_mod
use std.os.time
use std.core.common as common
use std.os.sound.backend.shared as backend_shared

#linux {
   #link "libjack.so"
   #include <jack/jack.h> as "jack_"
   #include <jack/ringbuffer.h> as "jack_"
   extern "jack" {
      fn _ny_jack_set_process_callback(ptr: client, fnptr: cb, ptr: arg): i32 as "jack_set_process_callback"
   }
} #else {
   fn _ny_jack_set_process_callback(..._args): i32 { -1 }
   fn jack_ringbuffer_free(any: _rb): any { 0 }
   fn jack_get_buffer_size(any: _client): int { 0 }
   fn jack_ringbuffer_create(any: _bytes): any { 0 }
   fn jack_get_ports(..._args): any { 0 }
   fn jack_port_name(any: _port): any { 0 }
   fn jack_connect(..._args): int { -1 }
   fn jack_free(any: _ptr): any { 0 }
   fn jack_ringbuffer_read_space(any: _rb): int { 0 }
   fn jack_ringbuffer_read(..._args): int { 0 }
   fn jack_port_get_buffer(..._args): any { 0 }
   fn jack_client_open(..._args): any { 0 }
   fn jack_client_close(any: _client): int { 0 }
   fn jack_port_register(..._args): any { 0 }
   fn jack_activate(any: _client): int { -1 }
   fn jack_deactivate(any: _client): int { 0 }
   fn jack_ringbuffer_write_space(any: _rb): int { 0 }
   fn jack_ringbuffer_write(..._args): int { 0 }
} #endif
def JACK_NO_START_SERVER = 0x01
def JACK_PORT_IS_INPUT = 0x1
def JACK_PORT_IS_OUTPUT = 0x2
def JACK_PORT_IS_PHYSICAL = 0x4
def JACK_DEFAULT_AUDIO_TYPE = "32 bit float mono audio"
def FORMAT_S16LE = 1
def FORMAT_FLOAT32LE = 2
def _ST_CLIENT = 0
def _ST_RB = 8
def _ST_PORTS = 16
def _ST_CHANNELS = 24
def _ST_FORMAT = 32
def _ST_SCRATCH = 40
def _ST_SCRATCH_SAMPLES = 48
def _ST_WRITE_TMP = 56
def _ST_WRITE_TMP_SAMPLES = 64
def _ST_SIZE = 72
def _WRITE_TMP_SAMPLES = 4096
def _RB_MIN_BYTES = 16384
mut _streams = dict(8)
mut _lib = 0
mut _avail = -1

fn _state_free(any: st): any {
   if(st == 0){ return nil }
   def rb = load64(st, _ST_RB)
   if(rb != 0){ jack_ringbuffer_free(rb) }
   def ports = load64(st, _ST_PORTS)
   if(ports != 0){ free(ports) }
   def scratch = load64(st, _ST_SCRATCH)
   if(scratch != 0){ free(scratch) }
   def write_tmp = load64(st, _ST_WRITE_TMP)
   if(write_tmp != 0){ free(write_tmp) }
   free(st)
}

fn _state_new(any: client, int: channels, int: format): any {
   def st = malloc(_ST_SIZE)
   if(st == 0){ return 0 }
   memset(st, 0, _ST_SIZE)
   store64(st, client, _ST_CLIENT)
   store64(st, channels, _ST_CHANNELS)
   store64(st, format, _ST_FORMAT)
   def ports = malloc(channels * 8)
   if(ports == 0){
      free(st)
      return 0
   }
   memset(ports, 0, channels * 8)
   store64(st, ports, _ST_PORTS)
   mut period = jack_get_buffer_size(client)
   if(period < 64){ period = 1024 }
   mut rb_bytes = period * channels * 4 * 16
   if(rb_bytes < _RB_MIN_BYTES){ rb_bytes = _RB_MIN_BYTES }
   def rb = jack_ringbuffer_create(rb_bytes)
   if(rb == 0){
      _state_free(st)
      return 0
   }
   store64(st, rb, _ST_RB)
   def scratch_samples = period * channels
   def scratch = malloc(scratch_samples * 4)
   if(scratch == 0){
      _state_free(st)
      return 0
   }
   store64(st, scratch, _ST_SCRATCH)
   store64(st, scratch_samples, _ST_SCRATCH_SAMPLES)
   def write_tmp = malloc(_WRITE_TMP_SAMPLES * 4)
   if(write_tmp == 0){
      _state_free(st)
      return 0
   }
   store64(st, write_tmp, _ST_WRITE_TMP)
   store64(st, _WRITE_TMP_SAMPLES, _ST_WRITE_TMP_SAMPLES)
   st
}

fn _connect_outputs(any: client, any: st): any {
   if(client == 0 || st == 0){ return nil }
   mut targets = jack_get_ports(client, 0, 0, JACK_PORT_IS_PHYSICAL | JACK_PORT_IS_INPUT)
   if(targets == 0){ targets = jack_get_ports(client, 0, 0, JACK_PORT_IS_INPUT) }
   if(targets == 0){ return nil }
   def channels = load64(st, _ST_CHANNELS)
   def ports = load64(st, _ST_PORTS)
   mut ch = 0
   while(ch < channels){
      def dst = load64(targets, ch * 8)
      if(dst == 0){ break }
      def port = load64(ports, ch * 8)
      if(port == 0){ break }
      def src_name = jack_port_name(port)
      if(src_name != 0){ jack_connect(client, cstr(src_name), dst) }
      ch += 1
   }
   jack_free(targets)
}

fn _process_cb(any: nframes, any: arg): int {
   if(arg == 0){ return 0 }
   def st = arg
   def channels = load64(st, _ST_CHANNELS)
   if(channels <= 0){ return 0 }
   def ports = load64(st, _ST_PORTS)
   if(ports == 0){ return 0 }
   def frames = nframes
   if(frames <= 0){ return 0 }
   def rb = load64(st, _ST_RB)
   def scratch = load64(st, _ST_SCRATCH)
   def total_samples = frames * channels
   def want_bytes = total_samples * 4
   mut read_bytes = 0
   if(rb != 0 && scratch != 0){
      read_bytes = jack_ringbuffer_read_space(rb)
      if(read_bytes > want_bytes){ read_bytes = want_bytes }
      if(read_bytes > 0){ jack_ringbuffer_read(rb, scratch, read_bytes) }
      if(read_bytes < want_bytes){ memset(ptr_add(scratch, read_bytes), 0, want_bytes - read_bytes) }
   }
   mut ch = 0
   while(ch < channels){
      def port = load64(ports, ch * 8)
      if(port != 0){
         def out = jack_port_get_buffer(port, frames)
         if(out != 0){
            if(scratch == 0 || read_bytes == 0){ memset(out, 0, frames * 4) } else {
               mut f = 0
               while(f < frames){
                  def in_off = ((f * channels) + ch) * 4
                  store32_f32(out, load32_f32(scratch, in_off), f * 4)
                  f += 1
               }
            }
         }
      }
      ch += 1
   }
   0
}

fn is_available(): bool {
   "Returns whether the JACK backend is available on this host."
   def state = backend_shared.probe_linux_library_once(_avail, _lib, "jack", "jack_client_open")
   _avail, _lib = state.get(0), state.get(1)
   _avail == 1
}

fn init(any: ctx): any {
   "Initializes JACK backend and appends default JACK device."
   backend_shared.init_output_device(ctx, is_available(), "JACK Default", "jack")
}

fn shutdown(any: ctx): any {
   "Shuts down JACK backend global state."
   if(ctx){ return 0 }
   0
}

fn stream_open(any: stream): any {
   "Opens JACK client, registers output ports, and installs process callback."
   if(!is_available()){ return false }
   mut channels = stream.get("channels", 2)
   if(channels <= 0){ channels = 2 }
   if(channels > 8){ channels = 8 }
   mut format = stream.get("format", FORMAT_S16LE)
   if(format != FORMAT_FLOAT32LE){ format = FORMAT_S16LE }
   def rate = stream.get("sample_rate", 48000)
   mut name = common.env_trim("NY_AUDIO_JACK_CLIENT")
   if(name.len == 0){ name = "nytrix" }
   def status_ptr = malloc(8)
   if(status_ptr == 0){ return false }
   store32(status_ptr, 0, 0)
   def client = jack_client_open(cstr(name), JACK_NO_START_SERVER, status_ptr, 0)
   free(status_ptr)
   if(client == 0){ return false }
   def st = _state_new(client, channels, format)
   if(st == 0){
      jack_client_close(client)
      return false
   }
   if(_ny_jack_set_process_callback(client, _process_cb, st) != 0){
      _state_free(st)
      jack_client_close(client)
      return false
   }
   def ports = load64(st, _ST_PORTS)
   mut i = 0
   while(i < channels){
      def pname = f"out_{i + 1}"
      def port = jack_port_register(client, cstr(pname), cstr(JACK_DEFAULT_AUDIO_TYPE), JACK_PORT_IS_OUTPUT, 0)
      if(port == 0){
         _state_free(st)
         jack_client_close(client)
         return false
      }
      store64(ports, port, i * 8)
      i += 1
   }
   _streams = _streams.set(client, st)
   stream = stream.set("handle", client)
   stream = stream.set("bits_per_sample", (format == FORMAT_FLOAT32LE) ? 32 : 16)
   stream = stream.set("sample_rate", rate)
   stream
}

fn stream_start(any: stream): bool {
   "Activates JACK processing and auto-connects output ports."
   def client = stream.get("handle")
   if(client == 0){ return false }
   if(jack_activate(client) != 0){ return false }
   def st = _streams.get(client)
   if(st != 0){ _connect_outputs(client, st) }
   true
}

fn stream_stop(any: stream): any {
   "Deactivates and closes JACK stream resources."
   def client = stream.get("handle")
   if(client == 0){ return nil }
   jack_deactivate(client)
   def st = _streams.get(client)
   if(st != 0){
      _streams = _streams.delete(client)
      _state_free(st)
   }
   jack_client_close(client)
   stream = stream.set("handle", 0)
}

fn write(any: h, any: buf, int: frames, int: frame_bytes=4): bool {
   "Converts interleaved S16/F32 frames to float and writes into JACK ringbuffer."
   if(h == 0){ return false }
   if(frames <= 0){ return true }
   def st = _streams.get(h)
   if(st == 0){ return false }
   def rb = load64(st, _ST_RB)
   def channels = load64(st, _ST_CHANNELS)
   def format = load64(st, _ST_FORMAT)
   def tmp = load64(st, _ST_WRITE_TMP)
   def tmp_samples = load64(st, _ST_WRITE_TMP_SAMPLES)
   if(rb == 0 || channels <= 0 || tmp == 0 || tmp_samples <= 0){ return false }
   def total_samples = frames * channels
   mut done = 0
   mut stalls = 0
   while(done < total_samples){
      mut writable = jack_ringbuffer_write_space(rb) / 4
      if(writable <= 0){
         stalls += 1
         if(stalls > 5000){ return false }
         msleep(1)
         continue
      }
      stalls = 0
      mut chunk = total_samples - done
      if(chunk > writable){ chunk = writable }
      if(chunk > tmp_samples){ chunk = tmp_samples }
      mut i = 0
      if(format == FORMAT_FLOAT32LE){
         while(i < chunk){
            def x = load32_f32(buf, (done + i) * 4)
            store32_f32(tmp, x, i * 4)
            i += 1
         }
      } else {
         while(i < chunk){
            mut s = load16(buf, (done + i) * 2)
            if(s > 32767){ s = s - 65536 }
            store32_f32(tmp, (s + 0.0) / 32768.0, i * 4)
            i += 1
         }
      }
      def need = chunk * 4
      def wrote = jack_ringbuffer_write(rb, tmp, need)
      if(wrote <= 0){
         msleep(1)
         continue
      }
      done += wrote / 4
   }
   true
}
