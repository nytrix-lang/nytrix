;; Keywords: audio pulse linux io

module std.os.audio.backend.pulse (
    is_available, init, shutdown,
    stream_open, stream_start, stream_stop,
    write
)

use std.core *
use std.core.dict *
use std.os.ffi *
use std.text *

fn _touch(...args){
   "Internal helper for `touch`."
    len(args)
}

fn _env_enabled(name){
   "Internal helper for `env_enabled`."
    def v = env(name)
    if(!v){ return false }
    def s = lower(strip(v))
    s == "1" || s == "true" || s == "on" || s == "yes"
}

fn _get_latency_ms(){
   "Internal helper for `get_latency_ms`."
    mut ms = 40
    def v = env("NY_AUDIO_LATENCY_MS")
    if(v){
        def n = atoi(v)
        if(n >= 10 && n <= 500){ ms = n }
    }
    ms
}

mut _lib = 0
mut _pa_simple_new = 0
mut _pa_simple_free = 0
mut _pa_simple_write = 0
mut _pa_simple_drain = 0
mut _pa_simple_flush = 0
mut _pa_strerror = 0

def PA_STREAM_PLAYBACK = 1
def PA_SAMPLE_S16LE = 3
def PA_SAMPLE_FLOAT32LE = 5

fn is_available(){
   "Returns whether available."
    def lib = dlopen_any("pulse-simple", RTLD_NOW())
    if(lib != 0){ dlclose(lib) return true }
    false
}

fn init(ctx){
   "Initializes module state."
    if(_lib != 0){ return true }
    _lib = dlopen_any("pulse-simple", RTLD_NOW())
    if(_lib == 0){ return false }
    _pa_simple_new = dlsym(_lib, "pa_simple_new")
    _pa_simple_free = dlsym(_lib, "pa_simple_free")
    _pa_simple_write = dlsym(_lib, "pa_simple_write")
    _pa_simple_drain = dlsym(_lib, "pa_simple_drain")
    _pa_simple_flush = dlsym(_lib, "pa_simple_flush")
    def lib_main = dlopen_any("pulse", RTLD_NOW())
    if(lib_main != 0){ _pa_strerror = dlsym(lib_main, "pa_strerror") }
    if(_pa_simple_new == 0 || _pa_simple_free == 0 || _pa_simple_write == 0){
        dlclose(_lib)
        _lib = 0
        _pa_simple_new = 0
        _pa_simple_free = 0
        _pa_simple_write = 0
        _pa_simple_drain = 0
        _pa_simple_flush = 0
        return false
    }
    mut dev = dict(8)
    dev = dict_set(dev, "name", "PulseAudio Default")
    dev = dict_set(dev, "id", "pulse")
    dev = dict_set(dev, "ctx", ctx)
    mut devices = get(ctx, "devices", list())
    devices = append(devices, dev)
    ctx = dict_set(ctx, "devices", devices)
    true
}

fn shutdown(ctx){
   "Shuts down module state."
    _touch(ctx)
    if(_lib != 0){ dlclose(_lib) _lib = 0 }
    _pa_simple_new = 0
    _pa_simple_free = 0
    _pa_simple_write = 0
    _pa_simple_drain = 0
    _pa_simple_flush = 0
}

fn stream_open(stream){
   "Implements `stream_open`."
    if(_pa_simple_new == 0){ return false }
    def ss = malloc(12)
    def format = get(stream, "format", 1)
    def bits = (format == 2) ? 32 : 16
    def rate = get(stream, "sample_rate")
    def channels = get(stream, "channels")
    def pa_fmt = (format == 2) ? PA_SAMPLE_FLOAT32LE : PA_SAMPLE_S16LE
    store32(ss, pa_fmt, 0)
    store32(ss, rate, 4)
    store8(ss, channels, 8)
    store8(ss, 0, 9)
    store8(ss, 0, 10)
    store8(ss, 0, 11)
    mut attr = 0
    if(_env_enabled("NY_AUDIO_PULSE_TUNE")){
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
    def pa = call9(_pa_simple_new, 0, app_name, PA_STREAM_PLAYBACK, out_dev, stream_name, ss, 0, attr, err_ptr)
    free(ss)
    if(attr){ free(attr) }
    if(pa == 0){
        if(env("NY_AUDIO_DEBUG") && _pa_strerror != 0){
            def err = load32(err_ptr, 0)
            print(f"Pulse: stream_open failed: {call1(_pa_strerror, err)}")
        }
        free(err_ptr)
        return false
    }
    free(err_ptr)
    stream = dict_set(stream, "handle", pa)
    stream = dict_set(stream, "bits_per_sample", bits)
    true
}

fn stream_start(stream){
   "Implements `stream_start`."
    _touch(stream)
    true
}

fn stream_stop(stream){
   "Implements `stream_stop`."
    def pa = get(stream, "handle")
    if(pa && _pa_simple_drain != 0){
        def err_ptr = malloc(4)
        store32(err_ptr, 0, 0)
        call2(_pa_simple_drain, pa, err_ptr)
        free(err_ptr)
    } elif(pa && _pa_simple_flush != 0){
        def err_ptr = malloc(4)
        store32(err_ptr, 0, 0)
        call2(_pa_simple_flush, pa, err_ptr)
        free(err_ptr)
    }
    if(pa && _pa_simple_free != 0){ call1_void(_pa_simple_free, pa) }
    stream = dict_set(stream, "handle", 0)
}

fn write(pa, buf, size){
   "Implements `write`."
    if(!pa || _pa_simple_write == 0){ return false }
    def n = size
    if(n <= 0){ return true }
    def err_ptr = malloc(4)
    store32(err_ptr, 0, 0)
    def rc = call4(_pa_simple_write, pa, buf, n, err_ptr)
    if(rc < 0 && env("NY_AUDIO_DEBUG") && _pa_strerror != 0){
        def err = load32(err_ptr, 0)
        print(f"Pulse: write failed: {call1(_pa_strerror, err)}")
    }
    free(err_ptr)
    rc >= 0
}
