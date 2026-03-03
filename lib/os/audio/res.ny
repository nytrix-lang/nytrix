;; Keywords: resource manager
;; Unified resource loader for audio and image files.
;; Uses libsndfile for audio and std.image for images.

module std.os.audio.res (
   init, shutdown,
   load, get_sound_info, get_image_info,
   clear_cache,
   is_cached
)

use std.core *
use std.core.error *
use std.core.dict_mod *
use std.os *
use std.os.path as path
use std.os.fs as fs
use std.text as str
use std.os.ffi *
use std.image as image
use std.os.audio.source *

if(comptime{ __os_name() == "linux" }){
    #link "sndfile"
} else if(comptime{ __os_name() == "macos" }){
    #link "sndfile"
} else if(comptime{ __os_name() == "windows" }){
    #link "sndfile"
}

extern fn sf_open(path: ptr, mode: i32, sfinfo: ptr): ptr as "sf_open"
extern fn sf_close(sndfile: ptr): i32 as "sf_close"
extern fn sf_read_short(sndfile: ptr, buf: ptr, items: i64): i64 as "sf_read_short"
extern fn sf_strerror(sndfile: ptr): ptr as "sf_strerror"
extern fn sf_error(sndfile: ptr): i32 as "sf_error"

def SFM_READ = 0x10

mut _sf_avail = -1

fn _sf_available(){
    "Checks whether libsndfile is available for extended audio decoding."
    if(_sf_avail != -1){ return _sf_avail == 1 }
    def h = dlopen_checked("sndfile", "sf_open")
    if(h == 0){ _sf_avail = 0 return false }
    dlclose(h)
    _sf_avail = 1
    true
}

fn _decode_sf(data){
    "Decodes audio bytes through libsndfile by routing them through a temporary file."
    if(!_sf_available()){ return 0 }
    if(!is_str(data) || len(data) < 16){ return 0 }
    def debug = env("NY_AUDIO_DEBUG")
    def tmp_fn = "ny_snd_" + to_str(ticks()) + "_" + to_str(malloc(1)) + ".tmp"
    def tmp_path = path.normalize(temp_dir() + "/" + tmp_fn)
    match file_write(tmp_path, data){
        err(e) -> {
            if(debug){ print("RES: Sndfile temp write failed: " + to_str(e)) }
            return 0
        }
        ok(_)  -> {}
    }
    def info_ptr = malloc(64)
    memset(info_ptr, 0, 64)
    def path_cstr = cstr(tmp_path)
    def sf = sf_open(path_cstr, SFM_READ, info_ptr)
    if(!sf){
        if(debug){
            def err_code = sf_error(0)
            def err_msg = sf_strerror(err_code)
            print("RES: Sndfile sf_open failed: " + cstr_to_str(err_msg))
        }
        free(info_ptr)
        match file_remove(tmp_path){ _ -> {} }
        return 0
    }
    def frames   = load64(info_ptr, 0)
    def rate     = load32(info_ptr, 8)
    def channels = load32(info_ptr, 12)
    if(debug){
        print("RES: Sndfile opened " + tmp_path + " frames=" + to_str(frames) + " rate=" + to_str(rate) + " channels=" + to_str(channels))
    }
    if(frames <= 0 || channels <= 0 || channels > 32){
        sf_close(sf)
        free(info_ptr)
        match file_remove(tmp_path){ _ -> {} }
        return 0
    }
    def n_samples = frames * channels
    def out_ptr = malloc(n_samples * 2)
    def got = sf_read_short(sf, out_ptr, n_samples)
    sf_close(sf)
    free(info_ptr)
    match file_remove(tmp_path){ _ -> {} }
    if(got <= 0){
        free(out_ptr)
        return 0
    }
    make_memory_source(out_ptr, got * 2, channels, rate, 16, SAMPLE_FMT_S16, 1)
}

;; Module state
mut _cache = dict(64)
mut _mtx = 0

fn init(){
    "Initializes the shared resource cache."
    if(_mtx == 0){ _mtx = mutex_new() }
    true
}

fn shutdown(){
    "Clears cached resources and releases the cache mutex."
    clear_cache()
    if(_mtx != 0){ mutex_free(_mtx) _mtx = 0 }
}

fn clear_cache(){
    "Drops all cached resource entries."
    if(_mtx == 0){ return }
    mutex_lock(_mtx)
    _cache = dict(64)
    mutex_unlock(_mtx)
}

fn is_cached(filepath){
    "Returns whether `filepath` is already present in the resource cache."
    if(_mtx == 0){ return false }
    def full = path.normalize(filepath)
    mutex_lock(_mtx)
    def exists = dict_has(_cache, full)
    mutex_unlock(_mtx)
    exists
}

fn load(filepath){
    "Loads an audio or image asset from disk, caching successful results by normalized path."
    if(_mtx == 0){ init() }
    def full = path.normalize(filepath)
    mutex_lock(_mtx)
    def cached = dict_get(_cache, full)
    mutex_unlock(_mtx)
    if(cached){ return cached }
    if(!fs.is_file(full)){ return 0 }
    def res = file_read(full)
    if(is_err(res)){ return 0 }
    def data = unwrap(res)
    def ext = str.lower(path.extname(full))
    mut asset = 0
    if(ext == ".wav" || ext == ".ogg" || ext == ".flac" || ext == ".mp3" || ext == ".aiff" || ext == ".aif" || ext == ".opus"){
        asset = _decode_sf(data)
    } else {
        asset = image.decode(data, ext)
    }
    if(asset != 0){
        mutex_lock(_mtx)
        _cache = dict_set(_cache, full, asset)
        mutex_unlock(_mtx)
        if(env("NY_AUDIO_DEBUG")){ print("RES: successfully loaded " + full) }
    } else {
        if(env("NY_AUDIO_DEBUG")){ print("RES: FAILED to load " + full) }
    }
    asset
}

fn get_sound_info(sound){
    "Returns normalized metadata for a loaded sound source."
    if(!is_list(sound)){ return 0 }
    def tag = get(sound, 0)
    if(tag != "SOUND_SOURCE"){ return 0 }
    def d = get(sound, 1)
    mut info = dict(8)
    info = dict_set(info, "channels", get(d, "channels"))
    info = dict_set(info, "rate",     get(d, "rate"))
    info = dict_set(info, "bits",     get(d, "bits"))
    def frames = get(d, "total_frames")
    def rate = get(d, "rate")
    def duration = (frames + 0.0) / (rate + 0.0)
    info = dict_set(info, "duration", duration)
    info = dict_set(info, "frames",   frames)
    info
}

fn get_image_info(img){
    "Returns normalized metadata for a loaded image."
    image.get_info(img)
}
