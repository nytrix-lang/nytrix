;; Keywords: audio io system

module std.os.audio.backend.io (
    create, connect, disconnect,
    get_output_device_count, get_output_device,
    get_default_output_device_index,

    outstream_create, outstream_open, outstream_start, outstream_stop,
    outstream_write, outstream_write_frames,
    outstream_destroy,

    get_backend_name,

    FORMAT_S16LE, FORMAT_FLOAT32LE
)

use std.core *
use std.core.dict *
use std.os *
use std.text *

use std.os.audio.backend.alsa as audio_alsa
use std.os.audio.backend.pulse as audio_pulse
use std.os.audio.backend.jack as audio_jack
use std.os.audio.backend.winmm as audio_winmm

def FORMAT_S16LE = 1
def FORMAT_FLOAT32LE = 2

fn _touch(...args){
   "Internal helper for `touch`."
    len(args)
}

fn _forced_backend(){
   "Internal helper for `forced_backend`."
    def b = env("NY_AUDIO_BACKEND")
    if(!b){ return "" }
    lower(strip(b))
}

fn _set_backend(ctx, id, name){
   "Internal helper for `set_backend`."
    ctx = dict_set(ctx, "backend", id)
    ctx = dict_set(ctx, "backend_name", name)
    ctx
}

fn _connect_pulse(ctx){
   "Internal helper for `connect_pulse`."
    if(audio_pulse.is_available() && audio_pulse.init(ctx)){ ctx = _set_backend(ctx, 1, "pulse") return true }
    false
}

fn _connect_alsa(ctx){
   "Internal helper for `connect_alsa`."
    if(audio_alsa.is_available() && audio_alsa.init(ctx)){ ctx = _set_backend(ctx, 2, "alsa") return true }
    false
}

fn _connect_jack(ctx){
   "Internal helper for `connect_jack`."
    if(audio_jack.is_available() && audio_jack.init(ctx)){ ctx = _set_backend(ctx, 4, "jack") return true }
    false
}

fn _connect_winmm(ctx){
   "Internal helper for `connect_winmm`."
    if(audio_winmm.is_available() && audio_winmm.init(ctx)){ ctx = _set_backend(ctx, 3, "winmm") return true }
    false
}

fn _connect_linux_desktop(ctx, allow_jack=false){
   "Internal helper for `connect_linux_desktop`."
    if(_connect_pulse(ctx)){ return true }
    if(_connect_alsa(ctx)){ return true }
    if(allow_jack && _connect_jack(ctx)){ return true }
    false
}

fn create(){
    "Creates a new AudioIO context."
    mut ctx = dict(16)
    ctx = dict_set(ctx, "backend", 0)
    ctx = dict_set(ctx, "backend_name", "none")
    ctx = dict_set(ctx, "devices", list())
    ctx
}

fn connect(ctx){
    "Connects to the best available audio backend."
    if(!ctx){ return false }
    def forced = _forced_backend()
    if(forced == "pulse"){
        return _connect_pulse(ctx)
    } elif(forced == "alsa"){
        return _connect_alsa(ctx)
    } elif(forced == "jack"){
        return _connect_jack(ctx)
    } elif(forced == "winmm"){
        return _connect_winmm(ctx)
    } elif(forced == "portaudio" || forced == "pa" || forced == "miniaudio"){
        def name = os()
        if(eq(name, "linux")){ return _connect_linux_desktop(ctx, true) }
        if(eq(name, "windows")){ return _connect_winmm(ctx) }
        false
    } elif(forced == "libsoundio" || forced == "soundio"){
        def name = os()
        if(eq(name, "linux")){ return _connect_linux_desktop(ctx, true) }
        if(eq(name, "windows")){ return _connect_winmm(ctx) }
        false
    }
    def name = os()
    mut connected = false
    if(eq(name, "linux")){
        connected = _connect_linux_desktop(ctx)
    } elif(eq(name, "windows")){
        connected = _connect_winmm(ctx)
    }
    connected
}

fn disconnect(ctx){
   "Implements `disconnect`."
    if(!ctx){ return }
    def b = get(ctx, "backend")
    if(b == 1){ audio_pulse.shutdown(ctx) }
    elif(b == 2){ audio_alsa.shutdown(ctx) }
    elif(b == 4){ audio_jack.shutdown(ctx) }
    elif(b == 3){ audio_winmm.shutdown(ctx) }
    ctx = dict_set(ctx, "backend", 0)
    ctx = dict_set(ctx, "backend_name", "none")
}

fn get_output_device_count(ctx){
   "Gets output device count."
    if(!ctx){ return 0 }
    len(get(ctx, "devices", list()))
}

fn get_output_device(ctx, index){
   "Gets output device."
    if(!ctx){ return 0 }
    def devices = get(ctx, "devices", list())
    if(index < 0 || index >= len(devices)){ return 0 }
    get(devices, index)
}

fn get_default_output_device_index(ctx){
   "Gets default output device index."
    _touch(ctx)
    0
}

fn outstream_create(device){
    "Creates an output stream for a specific device."
    mut stream = dict(16)
    stream = dict_set(stream, "device", device)
    stream = dict_set(stream, "format", FORMAT_S16LE)
    stream = dict_set(stream, "sample_rate", 44100)
    stream = dict_set(stream, "channels", 2)
    stream = dict_set(stream, "callback", 0)
    stream = dict_set(stream, "running", false)
    stream
}

fn outstream_open(stream, format, sample_rate, channels, callback){
   "Implements `outstream_open`."
    if(!stream){ return false }
    stream = dict_set(stream, "format", format)
    stream = dict_set(stream, "sample_rate", sample_rate)
    stream = dict_set(stream, "channels", channels)
    stream = dict_set(stream, "callback", callback)
    stream = dict_set(stream, "bits_per_sample", (format == FORMAT_FLOAT32LE) ? 32 : 16)
    def device = get(stream, "device")
    if(!device){ return false }
    def ctx = get(device, "ctx")
    if(!ctx){ return false }
    def b = get(ctx, "backend")
    mut ok = false
    if(b == 1){ ok = audio_pulse.stream_open(stream) }
    elif(b == 2){ ok = audio_alsa.stream_open(stream) }
    elif(b == 4){ ok = audio_jack.stream_open(stream) }
    elif(b == 3){ ok = audio_winmm.stream_open(stream) }
    ok
}

fn outstream_start(stream){
   "Implements `outstream_start`."
    if(!stream){ return false }
    def device = get(stream, "device")
    if(!device){ return false }
    def ctx = get(device, "ctx")
    if(!ctx){ return false }
    def b = get(ctx, "backend")
    mut ok = false
    if(b == 1){ ok = audio_pulse.stream_start(stream) }
    elif(b == 2){ ok = audio_alsa.stream_start(stream) }
    elif(b == 4){ ok = audio_jack.stream_start(stream) }
    elif(b == 3){ ok = audio_winmm.stream_start(stream) }
    if(ok){ stream = dict_set(stream, "running", true) }
    ok
}

fn outstream_stop(stream){
   "Implements `outstream_stop`."
    if(!stream){ return }
    def device = get(stream, "device")
    if(!device){ return }
    def ctx = get(device, "ctx")
    if(!ctx){ return }
    def b = get(ctx, "backend")
    if(b == 1){ audio_pulse.stream_stop(stream) }
    elif(b == 2){ audio_alsa.stream_stop(stream) }
    elif(b == 4){ audio_jack.stream_stop(stream) }
    elif(b == 3){ audio_winmm.stream_stop(stream) }
    stream = dict_set(stream, "running", false)
}

fn outstream_write(stream, buf, bytes){
    "Writes raw bytes to the output stream."
    if(!stream){ return false }
    def device = get(stream, "device")
    if(!device){ return false }
    def ctx = get(device, "ctx")
    if(!ctx){ return false }
    def b = get(ctx, "backend")
    def handle = get(stream, "handle")
    if(!handle){ return false }
    def nbytes = bytes
    if(nbytes <= 0){ return true }
    if(b == 1){ return audio_pulse.write(handle, buf, nbytes) }
    elif(b == 2){
        def channels = get(stream, "channels")
        def bps = get(stream, "bits_per_sample", 16)
        def frame_bytes = (bps / 8) * channels
        if(frame_bytes <= 0){ return false }
        return audio_alsa.write(handle, buf, nbytes / frame_bytes, frame_bytes)
    }
    elif(b == 4){
        def channels = get(stream, "channels")
        def bps = get(stream, "bits_per_sample", 16)
        def frame_bytes = (bps / 8) * channels
        if(frame_bytes <= 0){ return false }
        return audio_jack.write(handle, buf, nbytes / frame_bytes, frame_bytes)
    }
    elif(b == 3){ return audio_winmm.write(handle, buf, nbytes) }
    false
}

fn outstream_write_frames(stream, buf, frames){
    "Writes frames to the output stream."
    if(!stream){ return false }
    def device = get(stream, "device")
    if(!device){ return false }
    def ctx = get(device, "ctx")
    if(!ctx){ return false }
    def b = get(ctx, "backend")
    def handle = get(stream, "handle")
    if(!handle){ return false }
    def channels = get(stream, "channels")
    def bps = get(stream, "bits_per_sample", 16)
    mut sample_bytes = bps / 8
    if(sample_bytes <= 0){ sample_bytes = 2 }
    def frame_bytes = channels * sample_bytes
    def bytes = frames * channels * sample_bytes
    if(b == 1){ return audio_pulse.write(handle, buf, bytes) }
    elif(b == 2){ return audio_alsa.write(handle, buf, frames, frame_bytes) }
    elif(b == 4){ return audio_jack.write(handle, buf, frames, frame_bytes) }
    elif(b == 3){ return audio_winmm.write(handle, buf, bytes) }
    false
}

fn outstream_destroy(stream){
   "Implements `outstream_destroy`."
    outstream_stop(stream)
}

fn get_backend_name(ctx){
   "Gets backend name."
    if(!ctx){ return "none" }
    get(ctx, "backend_name", "none")
}

if(comptime{__main()}){
    use std.core.error *

    def ctx = create()
    assert(get_backend_name(ctx) == "none", "audio io initial backend")

    if(connect(ctx)){
        assert(get_output_device_count(ctx) > 0, "audio io device count")
        def dev = get_output_device(ctx, 0)
        assert(dev != 0, "audio io get device")
        def stream = outstream_create(dev)
        assert(outstream_open(stream, FORMAT_S16LE, 44100, 2, 0), "audio io open")
        assert(outstream_start(stream), "audio io start")
        def buf = malloc(64)
        memset(buf, 0, 64)
        assert(outstream_write_frames(stream, buf, 8), "audio io write frames")
        free(buf)
        outstream_destroy(stream)
        disconnect(ctx)
        assert(get_backend_name(ctx) == "none", "audio io shutdown backend")
    } else {
        assert(get_output_device_count(ctx) == 0, "audio io no devices when not connected")
    }
    print("✓ std.os.audio.backend.io tests passed")
}
