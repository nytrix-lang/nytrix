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
use std.core.dict_mod *
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
    if(audio_pulse.is_available()){
        def new_ctx = audio_pulse.init(ctx)
        if(new_ctx){ return _set_backend(new_ctx, 1, "pulse") }
    }
    0
}



fn _connect_alsa(ctx){
    if(audio_alsa.is_available()){
        def new_ctx = audio_alsa.init(ctx)
        if(new_ctx){ return _set_backend(new_ctx, 2, "alsa") }
    }
    0
}



fn _connect_jack(ctx){
   "Internal helper for `connect_jack`."
    if(audio_jack.is_available()){
        def new_ctx = audio_jack.init(ctx)
        if(new_ctx){ return _set_backend(new_ctx, 4, "jack") }
    }
    0
}


fn _connect_winmm(ctx){
   "Internal helper for `connect_winmm`."
    if(audio_winmm.is_available()){
        def new_ctx = audio_winmm.init(ctx)
        if(new_ctx){ return _set_backend(new_ctx, 3, "winmm") }
    }
    0
}


fn _connect_linux_desktop(ctx, allow_jack=false){
    mut nctx = _connect_pulse(ctx)
    if(nctx){ return nctx }
    nctx = _connect_alsa(ctx)
    if(nctx){ return nctx }
    if(allow_jack){
        nctx = _connect_jack(ctx)
        if(nctx){ return nctx }
    }
    0
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
    if(!ctx){ return 0 }
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
        return 0
    } elif(forced == "libsoundio" || forced == "soundio"){
        def name = os()
        if(eq(name, "linux")){ return _connect_linux_desktop(ctx, true) }
        if(eq(name, "windows")){ return _connect_winmm(ctx) }
        return 0
    }
    def name = os()
    if(eq(name, "linux")){
        return _connect_linux_desktop(ctx)
    } elif(eq(name, "windows")){
        return _connect_winmm(ctx)
    }
    ctx
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
    def devices = dict_get(ctx, "devices", 0)
    if(devices == 0){ return 0 }
    def n = len(devices)
    if(env("NY_AUDIO_DEBUG")){ print("Audio: get_output_device_count: count=" + to_str(n)) }
    n
}


fn get_output_device(ctx, index){
   "Gets output device."
    if(!ctx){ return 0 }
    def devices = core.get(ctx, "devices", list())
    if(index < 0 || index >= len(devices)){ return 0 }
    core.get(devices, index)
}


fn get_default_output_device_index(ctx){
   "Gets default output device index."
    _touch(ctx)
    0
}

fn outstream_create(device){
    "Creates an output stream for a specific device."
    if(env("NY_AUDIO_DEBUG")){ print("Audio: outstream_create: dev=" + to_str(device)) }
    mut stream = dict(16)
    stream = dict_set(stream, "device", device)
    stream = dict_set(stream, "format", FORMAT_S16LE)
    stream = dict_set(stream, "sample_rate", 44100)
    stream = dict_set(stream, "channels", 2)
    stream = dict_set(stream, "callback", 0)
    stream = dict_set(stream, "running", false)
    if(env("NY_AUDIO_DEBUG")){ print("Audio: outstream_create: stream keys=" + to_str(dict_keys(stream))) }
    stream
}

fn outstream_open(stream, format, sample_rate, channels, callback){
   "Implements `outstream_open`."
    if(env("NY_AUDIO_DEBUG")){ 
        print("Audio: Entering outstream_open.")
        print("Audio: outstream_open: stream keys before set=" + to_str(dict_keys(stream)))
    }
    if(!stream){ return false }



    stream = dict_set(stream, "format", format)
    stream = dict_set(stream, "sample_rate", sample_rate)
    stream = dict_set(stream, "channels", channels)
    stream = dict_set(stream, "callback", callback)
    stream = dict_set(stream, "bits_per_sample", (format == FORMAT_FLOAT32LE) ? 32 : 16)
    
    def device = dict_get(stream, "device", 0)
    if(env("NY_AUDIO_DEBUG")){ print("Audio: outstream_open: device=" + to_str(device)) }
    if(!device){ return false }
    
    def ctx = dict_get(device, "ctx", 0)
    if(env("NY_AUDIO_DEBUG")){ print("Audio: outstream_open: ctx=" + to_str(ctx)) }
    if(!ctx){ return false }
    
    def backend = dict_get(ctx, "backend", 0)
    if(env("NY_AUDIO_DEBUG")){ print("Audio: outstream_open: backend=" + to_str(backend)) }
    
    if(backend == 1){ if(audio_pulse.stream_open(stream)){ return stream } else { return 0 } }
    if(backend == 2){ if(audio_alsa.stream_open(stream)){ return stream } else { return 0 } }
    if(backend == 4){ if(audio_jack.stream_open(stream)){ return stream } else { return 0 } }
    if(backend == 3){ if(audio_winmm.stream_open(stream)){ return stream } else { return 0 } }
    0
}


fn outstream_start(stream){
   "Implements `outstream_start`."
    if(!stream){ return false }
    def device = core.get(stream, "device")
    if(!device){ return false }
    def ctx = core.get(device, "ctx")
    if(!ctx){ return false }
    def b = core.get(ctx, "backend")
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
    def device = core.get(stream, "device")
    if(!device){ return }
    def ctx = core.get(device, "ctx")
    if(!ctx){ return }
    def b = core.get(ctx, "backend")
    if(b == 1){ audio_pulse.stream_stop(stream) }
    elif(b == 2){ audio_alsa.stream_stop(stream) }
    elif(b == 4){ audio_jack.stream_stop(stream) }
    elif(b == 3){ audio_winmm.stream_stop(stream) }
    stream = dict_set(stream, "running", false)
}

fn outstream_write(stream, buf, bytes){
    "Writes raw bytes to the output stream."
    if(!stream){ return false }
    def device = core.get(stream, "device")
    if(!device){ return false }
    def ctx = core.get(device, "ctx")
    if(!ctx){ return false }
    def b = core.get(ctx, "backend")
    def handle = core.get(stream, "handle")
    if(!handle){ return false }
    def nbytes = bytes
    if(nbytes <= 0){ return true }
    if(b == 1){ return audio_pulse.write(handle, buf, nbytes) }
    elif(b == 2){
        def channels = core.get(stream, "channels")
        def bps = core.get(stream, "bits_per_sample", 16)
        def frame_bytes = (bps / 8) * channels
        if(frame_bytes <= 0){ return false }
        return audio_alsa.write(handle, buf, nbytes / frame_bytes, frame_bytes)
    }
    elif(b == 4){
        def channels = core.get(stream, "channels")
        def bps = core.get(stream, "bits_per_sample", 16)
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
    def device = core.get(stream, "device")
    if(!device){ return false }
    def ctx = core.get(device, "ctx")
    if(!ctx){ return false }
    def b = core.get(ctx, "backend")
    def handle = core.get(stream, "handle")
    if(!handle){ return false }
    def channels = core.get(stream, "channels")
    def bps = core.get(stream, "bits_per_sample", 16)
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
    return core.get(ctx, "backend_name", "none")
}

if(comptime{__main()}){
    use std.core.error *

    def ctx = create()
    assert(get_backend_name(ctx) == "none", "audio io initial backend")

    def nctx = connect(ctx)
    if(nctx){
        ctx = nctx
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
