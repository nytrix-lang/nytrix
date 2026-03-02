#!/bin/ny
;; Sound Engine Test

use std.core *
use std.text.io *
use std.os.audio *
use std.os.audio.source.synth *
use std.os.time *
use std.os *
use std.os.path as path
use std.os.fs as fs

if(env("CI") || env("NYTRIX_TEST_MODE") == "1"){
   print("Skipping sound test in CI/Test Mode")
   __exit(0)
}

print("Sound Engine Test starting...")
if(!init()) {
    print("Failed to initialize audio backend")
    __exit(1)
}

print(f"Using audio backend: {get_backend_name()}")

;; 1. Procedural Sine Test
print("\n--- 1. Procedural Sine Test ---")
def proc_src = make_sine_source(440.0, 1.0, 48000)
if(proc_src != 0){
    print("Playing procedural sine wave (440Hz) 1s...")
    def inst = play(proc_src)
    if(inst != 0){
        mut waited = 0
        while(is_playing(inst) && waited < 1500) {
            msleep(50)
            waited += 50
        }
    }
} else {
    print("Failed to create procedural sine source")
}

;; 2. Asset Loading & Playback Test
print("\n--- 2. Asset Loading & Playback Test ---")
def asset_dir = "etc/assets/audio"
def files = [
    "sound.wav",
    "sound.ogg",
    "sound.flac",
    "sound.mp3",
    "sound.wav"
]

mut i = 0
while(i < len(files)){
    def filename = get(files, i)
    def filepath = path.join(asset_dir, filename)
    print(f"Loading asset: {filepath} ...")
    print(f"  - normalized: {path.normalize(filepath)}")
    print(f"  - exists: {file_exists(filepath)}")
    print(f"  - is_file: {fs.is_file(filepath)}")
    
    def sound = load(filepath)
    if(sound != 0){
        print(f"Playing asset: {filename} (1s) ...")
        def inst = play(sound)
        if(inst != 0){
            mut waited = 0
            while(is_playing(inst) && waited < 1000) {
                msleep(50)
                waited += 50
            }
            if(is_playing(inst)){ stop(inst) }
        } else {
            print(f"  FAILED: Could not play {filename}")
        }
    } else {
        print(f"  SKIPPED: Could not load {filepath} (decoder library missing?)")
    }
    i += 1
}

shutdown()
print("\nSound Engine Test finished")
