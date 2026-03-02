;; Keywords: audio alsa linux io

module std.os.audio.backend.alsa (
    is_available, init, shutdown,
    stream_open, stream_start, stream_stop,
    write
)

use std.core *
use std.core.dict *
use std.os.ffi *
use std.os.thread *

fn _touch(...args){
   "Internal helper for `touch`."
    len(args)
}

mut _lib = 0
mut _snd_pcm_open = 0
mut _snd_pcm_set_params = 0
mut _snd_pcm_writei = 0
mut _snd_pcm_close = 0
mut _snd_pcm_recover = 0
mut _snd_pcm_prepare = 0
mut _snd_pcm_drain = 0

def SND_PCM_STREAM_PLAYBACK = 0
def SND_PCM_FORMAT_S16_LE = 2
def SND_PCM_FORMAT_FLOAT_LE = 14
def SND_PCM_ACCESS_RW_INTERLEAVED = 3

fn is_available(){
   "Returns whether available."
    def lib = dlopen_any("asound", RTLD_NOW())
    if(lib != 0){ dlclose(lib) return true }
    false
}

fn _push_unique(lst, item){
   "Internal helper for `push_unique`."
    if(!is_str(item) || len(item) == 0){ return lst }
    mut i = 0
    while(i < len(lst)){
        if(eq(get(lst, i), item)){ return lst }
        i += 1
    }
    append(lst, item)
}

fn _build_candidates(dev_id){
   "Internal helper for `build_candidates`."
    mut out = list()
    out = _push_unique(out, dev_id)
    out = _push_unique(out, "pipewire")
    out = _push_unique(out, "pulse")
    out = _push_unique(out, "default")
    out = _push_unique(out, "sysdefault")
    out = _push_unique(out, "front")
    out = _push_unique(out, "plughw:0,0")
    out = _push_unique(out, "hw:0,0")
    out
}

fn _latency_us(){
   "Internal helper for `latency_us`."
    mut us = 120000
    def v = env("NY_AUDIO_ALSA_LATENCY_MS")
    if(v){
        def ms = atoi(v)
        if(ms >= 20 && ms <= 2000){ us = ms * 1000 }
    }
    us
}

fn init(ctx){
   "Initializes module state."
    if(_lib != 0){ return true }
    _lib = dlopen_any("asound", RTLD_NOW())
    if(_lib == 0){ return false }
    _snd_pcm_open = dlsym(_lib, "snd_pcm_open")
    _snd_pcm_set_params = dlsym(_lib, "snd_pcm_set_params")
    _snd_pcm_writei = dlsym(_lib, "snd_pcm_writei")
    _snd_pcm_close = dlsym(_lib, "snd_pcm_close")
    _snd_pcm_recover = dlsym(_lib, "snd_pcm_recover")
    _snd_pcm_prepare = dlsym(_lib, "snd_pcm_prepare")
    _snd_pcm_drain = dlsym(_lib, "snd_pcm_drain")
    if(_snd_pcm_open == 0 || _snd_pcm_set_params == 0 || _snd_pcm_writei == 0 || _snd_pcm_close == 0 || _snd_pcm_recover == 0){
        dlclose(_lib)
        _lib = 0
        _snd_pcm_open = 0
        _snd_pcm_set_params = 0
        _snd_pcm_writei = 0
        _snd_pcm_close = 0
        _snd_pcm_recover = 0
        _snd_pcm_prepare = 0
        _snd_pcm_drain = 0
        return false
    }
    mut dev = dict(8)
    dev = dict_set(dev, "name", "ALSA Default")
    dev = dict_set(dev, "id", "default")
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
    _snd_pcm_open = 0
    _snd_pcm_set_params = 0
    _snd_pcm_writei = 0
    _snd_pcm_close = 0
    _snd_pcm_recover = 0
    _snd_pcm_prepare = 0
    _snd_pcm_drain = 0
}

fn stream_open(stream){
   "Implements `stream_open`."
    if(_snd_pcm_open == 0 || _snd_pcm_set_params == 0 || _snd_pcm_close == 0){ return false }
    def device = get(stream, "device")
    mut dev_id = env("NY_AUDIO_ALSA_DEVICE")
    if(!dev_id || len(dev_id) == 0){
        dev_id = get(device, "id", "")
        if(!is_str(dev_id) || len(dev_id) == 0 || eq(dev_id, "default")){
            dev_id = ""
        }
    }
    def format = get(stream, "format", 1)
    mut alsa_fmt = SND_PCM_FORMAT_S16_LE
    mut bits = 16
    if(format != 1){
        alsa_fmt = SND_PCM_FORMAT_FLOAT_LE
        bits = 32
    }
    def rate = get(stream, "sample_rate")
    def channels = get(stream, "channels")
    def pcm_ptr = malloc(8)
    mut opened = 0
    mut chosen = ""
    mut tries = _build_candidates(dev_id)
    mut i = 0
    while(i < len(tries) && opened == 0){
        def cand = get(tries, i)
        if(call4(_snd_pcm_open, pcm_ptr, cand, SND_PCM_STREAM_PLAYBACK, 0) >= 0){
            def pcm = load64(pcm_ptr, 0)
            if(pcm != 0){
                def serr = call7(_snd_pcm_set_params, pcm, alsa_fmt, SND_PCM_ACCESS_RW_INTERLEAVED, channels, rate, 1, _latency_us())
                if(serr >= 0){
                    opened = pcm
                    chosen = cand
                } else {
                    call1_void(_snd_pcm_close, pcm)
                }
            }
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
    true
}

fn stream_start(stream){
   "Implements `stream_start`."
    def pcm = get(stream, "handle")
    if(!pcm){ return false }
    if(_snd_pcm_prepare != 0){
        return call1(_snd_pcm_prepare, pcm) >= 0
    }
    true
}

fn stream_stop(stream){
   "Implements `stream_stop`."
    def pcm = get(stream, "handle")
    if(pcm && _snd_pcm_drain != 0){ call1(_snd_pcm_drain, pcm) }
    if(pcm && _snd_pcm_close != 0){ call1_void(_snd_pcm_close, pcm) }
    stream = dict_set(stream, "handle", 0)
}

fn write(pcm, buf, frames, frame_bytes=4){
   "Implements `write`."
    if(!pcm || _snd_pcm_writei == 0){ return false }
    mut ptr = buf
    mut left = frames
    mut fb = frame_bytes
    if(fb <= 0){ fb = 4 }
    while(left > 0){
        def written = call3(_snd_pcm_writei, pcm, ptr, left)
        if(written < 0){
            if(_snd_pcm_recover == 0){ return false }
            def rec = call3(_snd_pcm_recover, pcm, written, 0)
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
