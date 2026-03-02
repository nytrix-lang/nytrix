;; Keywords: audio winmm windows io

module std.os.audio.backend.winmm (
    is_available, init, shutdown,
    stream_open, stream_start, stream_stop,
    write
)

use std.core *
use std.core.dict *
use std.os *
use std.os.time *
use std.os.ffi *

fn _touch(...args){
   "Internal helper for `touch`."
    len(args)
}

mut _lib = 0
mut _waveOutOpen = 0
mut _waveOutClose = 0
mut _waveOutPrepareHeader = 0
mut _waveOutUnprepareHeader = 0
mut _waveOutWrite = 0
mut _waveOutReset = 0

def WAVE_FORMAT_PCM = 1

def MMSYSERR_NOERROR = 0
def CALLBACK_NULL = 0

fn is_available(){
   "Returns whether available."
    eq(os(), "windows")
}

fn init(ctx){
   "Initializes module state."
    if(!is_available()){ return false }
    if(_lib != 0){ return true }
    _lib = dlopen_any("winmm.dll", RTLD_NOW())
    if(_lib == 0){ return false }
    _waveOutOpen = dlsym(_lib, "waveOutOpen")
    _waveOutClose = dlsym(_lib, "waveOutClose")
    _waveOutPrepareHeader = dlsym(_lib, "waveOutPrepareHeader")
    _waveOutUnprepareHeader = dlsym(_lib, "waveOutUnprepareHeader")
    _waveOutWrite = dlsym(_lib, "waveOutWrite")
    _waveOutReset = dlsym(_lib, "waveOutReset")
    if(!_waveOutOpen || !_waveOutClose || !_waveOutPrepareHeader || !_waveOutUnprepareHeader || !_waveOutWrite || !_waveOutReset){
        dlclose(_lib)
        _lib = 0
        return false
    }
    mut dev = dict(8)
    dev = dict_set(dev, "name", "Windows Multimedia")
    dev = dict_set(dev, "id", "winmm")
    dev = dict_set(dev, "ctx", ctx)
    mut devices = get(ctx, "devices", list())
    devices = append(devices, dev)
    ctx = dict_set(ctx, "devices", devices)
    true
}

fn shutdown(ctx){
   "Shuts down module state."
    _touch(ctx)
    if(_lib != 0){
        dlclose(_lib)
        _lib = 0
    }
}

fn stream_open(stream){
   "Implements `stream_open`."
    if(!_lib){ return false }
    def hWaveOut = malloc(8)
    def wfx = malloc(18)
    def format = get(stream, "format")
    def sample_rate = get(stream, "sample_rate")
    def channels = get(stream, "channels")
    def bits_per_sample = (format == 2) ? 32 : 16
    store16(wfx, WAVE_FORMAT_PCM, 0)
    store16(wfx, channels, 2)
    store32(wfx, sample_rate, 4)
    store32(wfx, sample_rate * channels * (bits_per_sample / 8), 8)
    store16(wfx, channels * (bits_per_sample / 8), 12)
    store16(wfx, bits_per_sample, 14)
    store16(wfx, 0, 16)
    def res = call6(_waveOutOpen, hWaveOut, -1, wfx, 0, 0, CALLBACK_NULL)
    free(wfx)
    if(res != MMSYSERR_NOERROR){
        free(hWaveOut)
        return false
    }
    def handle = load64(hWaveOut, 0)
    free(hWaveOut)
    stream = dict_set(stream, "handle", handle)
    stream = dict_set(stream, "bits_per_sample", bits_per_sample)
    true
}

fn stream_start(stream){
   "Implements `stream_start`."
    _touch(stream)
    true
}

fn stream_stop(stream){
   "Implements `stream_stop`."
    def handle = get(stream, "handle")
    if(handle && _waveOutClose){
        call1_void(_waveOutReset, handle)
        call1_void(_waveOutClose, handle)
    }
    stream = dict_set(stream, "handle", 0)
    true
}

fn write(handle, buf, bytes){
   "Implements `write`."
    if(!handle || !_waveOutWrite){ return false }
    def hdr_size = 32
    def hdr = malloc(hdr_size)
    memset(hdr, 0, hdr_size)
    store_ptr(hdr, 0, buf)
    store32(hdr, bytes, 8)
    if(call2(_waveOutPrepareHeader, handle, hdr) != MMSYSERR_NOERROR){
        free(hdr)
        return false
    }
    if(call2(_waveOutWrite, handle, hdr) != MMSYSERR_NOERROR){
        call2_void(_waveOutUnprepareHeader, handle, hdr)
        free(hdr)
        return false
    }
    while((load32(hdr, 24) & 1) == 0){
        msleep(1)
    }
    call2_void(_waveOutUnprepareHeader, handle, hdr)
    free(hdr)
    true
}
