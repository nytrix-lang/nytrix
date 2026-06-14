#!/usr/bin/env ny

;; Keywords: os sound audio synth wav example
;; Play a generated synth source, write it as WAV, then load that WAV back.
use std.core
use std.os (exit, file_exists, file_remove, msleep, pid, temp_dir, ticks)
use std.os.path as path
use std.os.sound
use std.os.sound.source.synth

fn wait_playback(any inst, int limit_ms) int {
   mut waited = 0
   while is_playing(inst) && waited < limit_ms {
      msleep(50)
      waited += 50
   }
   if is_playing(inst) { stop(inst) }
   waited
}

fn play_source(str label, any sound, int limit_ms) bool {
   print(f"{label}: loaded={sound != 0}")
   if sound == 0 { return false }
   def inst = play(sound)
   if inst != 0 { wait_playback(inst, limit_ms) }
   inst != 0
}

fn play_file(str label, str file, int limit_ms) bool {
   def sound = load(file)
   print(f"{label}: exists={file_exists(file)} loaded={sound != 0}")
   if sound == 0 { return false }
   def inst = play(sound)
   if inst != 0 { wait_playback(inst, limit_ms) }
   true
}

fn temp_sound_path(ext) str {
   path.join(temp_dir(), "nytrix-sine-" + to_str(pid()) + "-" + to_str(ticks()) + ext)
}

if !init() {
   print("audio backend unavailable")
   exit(0)
}

print(f"backend={get_backend_name()}")
def tone = make_sine_source(440.0, 0.75, 48000, 0.45, false)
play_source("synth.sine", tone, 1000)
def wav_file = temp_sound_path(".wav")

if write_synth_wav(tone, wav_file) {
   play_file("generated.wav", wav_file, 1000)
   match file_remove(wav_file) { _ -> {} }
} else {
   print("generated.wav: write failed")
}

shutdown()
