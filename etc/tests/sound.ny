use std.core *
use std.text.io *
use std.audio *
use std.audio.source.synth *
use std.os.time *

print("Sound Example starting...")
if(!init()) {
    print("Failed to initialize audio backend")
    __exit(1)
}

print(f"Using audio backend: {get_backend_name()}")

;; Create a clearly audible sine wave sound source (440Hz = A4)
def src = make_sine_source(440.0, 2.0, 48000)
assert(src != 0, "Failed to create sine source")

;; Play it
print("Playing sine wave (440Hz) 2s...")
def inst = play(src)
assert(inst != 0, "Failed to play sound")

;; Wait until playback actually finishes (or timeout), to avoid backend-latency
;; differences from looking like cut tails.
mut waited_ms = 0
while(is_playing(inst) && waited_ms < 5000) {
    msleep(20)
    waited_ms += 20
}

if(is_playing(inst)) { stop(inst) }
shutdown()
print("Sound Example finished")
