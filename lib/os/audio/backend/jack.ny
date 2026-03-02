;; Keywords: audio jack linux io

module std.audio.backend.jack (
    is_available, init, shutdown,
    stream_open, stream_start, stream_stop,
    write
)

use std.core *
use std.core.dict *
use std.os *
use std.os.ffi *

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

mut _lib = 0
mut _jack_client_open = 0
mut _jack_client_close = 0
mut _jack_activate = 0
mut _jack_deactivate = 0
mut _jack_set_process_callback = 0
mut _jack_get_buffer_size = 0
mut _jack_port_register = 0
mut _jack_port_get_buffer = 0
mut _jack_get_ports = 0
mut _jack_port_name = 0
mut _jack_connect = 0
mut _jack_free = 0
mut _jack_ringbuffer_create = 0
mut _jack_ringbuffer_free = 0
mut _jack_ringbuffer_read = 0
mut _jack_ringbuffer_read_space = 0
mut _jack_ringbuffer_write = 0
mut _jack_ringbuffer_write_space = 0

mut _streams = dict(8)

fn _touch(...args){
   "Consumes arguments intentionally."
   len(args)
}

fn _reset_symbols(){
   "Clears loaded JACK symbol pointers."
   _jack_client_open = 0
   _jack_client_close = 0
   _jack_activate = 0
   _jack_deactivate = 0
   _jack_set_process_callback = 0
   _jack_get_buffer_size = 0
   _jack_port_register = 0
   _jack_port_get_buffer = 0
   _jack_get_ports = 0
   _jack_port_name = 0
   _jack_connect = 0
   _jack_free = 0
   _jack_ringbuffer_create = 0
   _jack_ringbuffer_free = 0
   _jack_ringbuffer_read = 0
   _jack_ringbuffer_read_space = 0
   _jack_ringbuffer_write = 0
   _jack_ringbuffer_write_space = 0
}

fn _close_lib(){
   "Closes libjack and clears global pointers."
   if(_lib != 0){ dlclose(_lib) }
   _lib = 0
   _reset_symbols()
}

fn _load_lib(){
   "Loads libjack and binds required symbols."
   if(_lib != 0){ return true }
   _lib = dlopen_any("jack", RTLD_NOW())
   if(_lib == 0){ return false }
   _jack_client_open = dlsym(_lib, "jack_client_open")
   _jack_client_close = dlsym(_lib, "jack_client_close")
   _jack_activate = dlsym(_lib, "jack_activate")
   _jack_deactivate = dlsym(_lib, "jack_deactivate")
   _jack_set_process_callback = dlsym(_lib, "jack_set_process_callback")
   _jack_get_buffer_size = dlsym(_lib, "jack_get_buffer_size")
   _jack_port_register = dlsym(_lib, "jack_port_register")
   _jack_port_get_buffer = dlsym(_lib, "jack_port_get_buffer")
   _jack_get_ports = dlsym(_lib, "jack_get_ports")
   _jack_port_name = dlsym(_lib, "jack_port_name")
   _jack_connect = dlsym(_lib, "jack_connect")
   _jack_free = dlsym(_lib, "jack_free")
   _jack_ringbuffer_create = dlsym(_lib, "jack_ringbuffer_create")
   _jack_ringbuffer_free = dlsym(_lib, "jack_ringbuffer_free")
   _jack_ringbuffer_read = dlsym(_lib, "jack_ringbuffer_read")
   _jack_ringbuffer_read_space = dlsym(_lib, "jack_ringbuffer_read_space")
   _jack_ringbuffer_write = dlsym(_lib, "jack_ringbuffer_write")
   _jack_ringbuffer_write_space = dlsym(_lib, "jack_ringbuffer_write_space")
   if(
      _jack_client_open == 0 || _jack_client_close == 0 ||
      _jack_activate == 0 || _jack_deactivate == 0 ||
      _jack_set_process_callback == 0 || _jack_get_buffer_size == 0 ||
      _jack_port_register == 0 || _jack_port_get_buffer == 0 ||
      _jack_ringbuffer_create == 0 || _jack_ringbuffer_free == 0 ||
      _jack_ringbuffer_read == 0 || _jack_ringbuffer_read_space == 0 ||
      _jack_ringbuffer_write == 0 || _jack_ringbuffer_write_space == 0
   ){
      _close_lib()
      return false
   }
   true
}

fn _state_free(st){
   "Frees all memory owned by a JACK stream state block."
   if(st == 0){ return }
   def rb = load64(st, _ST_RB)
   if(rb != 0 && _jack_ringbuffer_free != 0){ call1_void(_jack_ringbuffer_free, rb) }
   def ports = load64(st, _ST_PORTS)
   if(ports != 0){ free(ports) }
   def scratch = load64(st, _ST_SCRATCH)
   if(scratch != 0){ free(scratch) }
   def write_tmp = load64(st, _ST_WRITE_TMP)
   if(write_tmp != 0){ free(write_tmp) }
   free(st)
}

fn _state_new(client, channels, format){
   "Allocates stream state and JACK ringbuffer storage."
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

   mut period = call1(_jack_get_buffer_size, client)
   if(period < 64){ period = 1024 }

   mut rb_bytes = period * channels * 4 * 16
   if(rb_bytes < _RB_MIN_BYTES){ rb_bytes = _RB_MIN_BYTES }
   def rb = call1(_jack_ringbuffer_create, rb_bytes)
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

fn _connect_outputs(client, st){
   "Connects client output ports to physical/system inputs."
   if(client == 0 || st == 0){ return }
   if(_jack_get_ports == 0 || _jack_port_name == 0 || _jack_connect == 0 || _jack_free == 0){ return }

   mut targets = call4(_jack_get_ports, client, 0, 0, JACK_PORT_IS_PHYSICAL | JACK_PORT_IS_INPUT)
   if(targets == 0){
      targets = call4(_jack_get_ports, client, 0, 0, JACK_PORT_IS_INPUT)
   }
   if(targets == 0){ return }

   def channels = load64(st, _ST_CHANNELS)
   def ports = load64(st, _ST_PORTS)
   mut ch = 0
   while(ch < channels){
      def dst = load64(targets, ch * 8)
      if(dst == 0){ break }
      def port = load64(ports, ch * 8)
      if(port == 0){ break }
      def src_name = call1(_jack_port_name, port)
      if(src_name != 0){
         call3(_jack_connect, client, src_name, dst)
      }
      ch += 1
   }
   call1_void(_jack_free, targets)
}

fn _process_cb(nframes, arg){
   "JACK process callback: drains ringbuffer and writes deinterleaved float output."
   if(arg == 0 || _jack_port_get_buffer == 0){ return 0 }
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
      read_bytes = call1(_jack_ringbuffer_read_space, rb)
      if(read_bytes > want_bytes){ read_bytes = want_bytes }
      if(read_bytes > 0){
         call3(_jack_ringbuffer_read, rb, scratch, read_bytes)
      }
      if(read_bytes < want_bytes){
         memset(ptr_add(scratch, read_bytes), 0, want_bytes - read_bytes)
      }
   }

   mut ch = 0
   while(ch < channels){
      def port = load64(ports, ch * 8)
      if(port != 0){
         def out = call2(_jack_port_get_buffer, port, frames)
         if(out != 0){
            if(scratch == 0 || read_bytes == 0){
               memset(out, 0, frames * 4)
            } else {
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

fn is_available(){
   "Returns whether JACK shared library is available."
   def h = dlopen_any("jack", RTLD_NOW())
   if(h != 0){
      dlclose(h)
      return true
   }
   false
}

fn init(ctx){
   "Initializes JACK backend and appends default JACK device."
   if(!_load_lib()){ return false }
   mut dev = dict(8)
   dev = dict_set(dev, "name", "JACK Default")
   dev = dict_set(dev, "id", "jack")
   dev = dict_set(dev, "ctx", ctx)
   mut devices = get(ctx, "devices", list())
   devices = append(devices, dev)
   ctx = dict_set(ctx, "devices", devices)
   true
}

fn shutdown(ctx){
   "Shuts down JACK backend global state."
   if(ctx){ 0 }
   _close_lib()
}

fn stream_open(stream){
   "Opens JACK client, registers output ports, and installs process callback."
   if(!_load_lib()){ return false }

   mut channels = get(stream, "channels", 2)
   if(channels <= 0){ channels = 2 }
   if(channels > 8){ channels = 8 }

   mut format = get(stream, "format", FORMAT_S16LE)
   if(format != FORMAT_FLOAT32LE){ format = FORMAT_S16LE }

   def rate = get(stream, "sample_rate", 48000)
   mut name = env("NY_AUDIO_JACK_CLIENT")
   if(!is_str(name) || len(name) == 0){ name = "nytrix" }

   def status_ptr = malloc(8)
   if(status_ptr == 0){ return false }
   store32(status_ptr, 0, 0)
   def client = call4(_jack_client_open, name, JACK_NO_START_SERVER, status_ptr, 0)
   free(status_ptr)
   if(client == 0){ return false }

   def st = _state_new(client, channels, format)
   if(st == 0){
      call1_void(_jack_client_close, client)
      return false
   }

   if(call3(_jack_set_process_callback, client, fn_ptr(_process_cb), st) != 0){
      _state_free(st)
      call1_void(_jack_client_close, client)
      return false
   }

   def ports = load64(st, _ST_PORTS)
   mut i = 0
   while(i < channels){
      def pname = f"out_{i + 1}"
      def port = call5(_jack_port_register, client, pname, JACK_DEFAULT_AUDIO_TYPE, JACK_PORT_IS_OUTPUT, 0)
      if(port == 0){
         _state_free(st)
         call1_void(_jack_client_close, client)
         return false
      }
      store64(ports, port, i * 8)
      i += 1
   }

   _streams = dict_set(_streams, client, st)
   stream = dict_set(stream, "handle", client)
   stream = dict_set(stream, "bits_per_sample", (format == FORMAT_FLOAT32LE) ? 32 : 16)
   stream = dict_set(stream, "sample_rate", rate)
   true
}

fn stream_start(stream){
   "Activates JACK processing and auto-connects output ports."
   def client = get(stream, "handle")
   if(client == 0){ return false }
   if(call1(_jack_activate, client) != 0){ return false }
   def st = dict_get(_streams, client)
   if(st != 0){ _connect_outputs(client, st) }
   true
}

fn stream_stop(stream){
   "Deactivates and closes JACK stream resources."
   def client = get(stream, "handle")
   if(client == 0){ return }
   if(_jack_deactivate != 0){ call1(_jack_deactivate, client) }
   def st = dict_get(_streams, client)
   if(st != 0){
      _streams = dict_del(_streams, client)
      _state_free(st)
   }
   if(_jack_client_close != 0){ call1_void(_jack_client_close, client) }
   stream = dict_set(stream, "handle", 0)
}

fn write(handle, buf, frames, frame_bytes=4){
   "Converts interleaved S16/F32 frames to float and writes into JACK ringbuffer."
   _touch(frame_bytes)
   if(handle == 0){ return false }
   if(frames <= 0){ return true }

   def st = dict_get(_streams, handle)
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
      mut writable = call1(_jack_ringbuffer_write_space, rb) / 4
      if(writable <= 0){
         stalls += 1
         if(stalls > 5000){ return false }
         sleep_ms(1)
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
      def wrote = call3(_jack_ringbuffer_write, rb, tmp, need)
      if(wrote <= 0){
         sleep_ms(1)
         continue
      }
      done += wrote / 4
   }
   true
}
