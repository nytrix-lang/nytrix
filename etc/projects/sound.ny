#!/usr/bin/env ny
;; Keywords: sound audio sine wav example
;; Write a quiet, smoothly enveloped sine WAV and play it through the sound backend.
use core
use core.os.sound (
   load as sound_load,
   init as sound_init,
   set_master_volume as set_sound_master_volume,
   play as play_sound,
   get_backend_name as sound_backend_name,
   shutdown as sound_shutdown
)
use core.os.sound.source.synth (make_sine_source, write_synth_wav)

def output = "tmp/workspace/artifacts/run-results/sound-generated.wav"
def tone = make_sine_source(440.0, 1.2, 48000, 0.04, false)
if !tone {
   print("tone generation failed")
   exit(1)
}

if !write_synth_wav(tone, output) {
   print("WAV write failed")
   exit(1)
}

def loaded_tone = sound_load(output)
if !loaded_tone {
   print("WAV load failed")
   sound_shutdown()
   exit(1)
}

if !sound_init(false) {
   print("audio backend initialization failed")
   sound_shutdown()
   exit(1)
}

set_sound_master_volume(1.0)
def playback = play_sound(loaded_tone, 1.0, 1.0, false, 0.0)
def backend = sound_backend_name()
sound_shutdown()
if !playback {
   print("audio playback failed")
   exit(1)
}

print("generated=", output)
print("format=PCM S16LE rate=48000Hz channels=1 duration=1.2s peak<=0.04FS")
print("played=440Hz sine backend=", backend, " PCM=1:1 master=1.0")
