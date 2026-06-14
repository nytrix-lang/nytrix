;; Keywords: sound diag mixer source res os
;; Sound facade for sources, mixers, backend selection, and one-shot playback.
;; References:
;; - std.os
module std.os.sound(init, shutdown, load, play, stop, is_playing, set_volume, get_volume, set_pitch, get_pitch, set_pan, get_pan, set_master_volume, get_master_volume, play_oneshot, get_backend_name, diag, probe, probe_text, print_probe)
use std.core
use std.os.sound.backend as snd_backend
use std.os.sound.res as res

fn init() any {
   "Initializes module state."
   res.init()
   return snd_backend.init(false)
}

fn shutdown() any {
   "Shuts down module state."
   snd_backend.shutdown()
   res.shutdown()
}

fn load(any path) any {
   "Loads a sound from file(cached)."
   res.load(path)
}

fn play(any sound, any pitch=1.0, any vol=1.0, any looping=false, any pan=0.0) any {
   "Plays a sound object. Returns a sound instance handle."
   return snd_backend.play(sound, pitch, vol, looping, pan)
}

fn stop(any inst) any {
   "Stops a sound instance or all instances of a sound."
   snd_backend.stop(inst)
}

fn is_playing(any inst) bool {
   "Returns true if the sound instance is currently playing."
   return snd_backend.is_playing(inst)
}

fn set_volume(any inst_or_sound, any vol) any {
   "Sets the volume of a sound instance or the default volume of a sound object."
   if is_list(inst_or_sound) {
      if inst_or_sound.len > 3 { inst_or_sound[3] = vol + 0.0 }
      else { inst_or_sound[6] = vol + 0.0 }
   }
}

fn get_volume(any inst_or_sound) any {
   "Returns the volume of a sound instance or a sound object."
   if is_list(inst_or_sound) {
      if inst_or_sound.len > 3 { return inst_or_sound.get(3) }
      return inst_or_sound.get(6, 1.0)
   }
   1.0
}

fn set_pitch(any inst_or_sound, any pitch) any {
   "Sets the playback pitch of a sound instance."
   if is_list(inst_or_sound) && inst_or_sound.len > 2 { inst_or_sound[2] = pitch + 0.0 }
}

fn get_pitch(any inst_or_sound) any {
   "Returns the playback pitch of a sound instance."
   if is_list(inst_or_sound) && inst_or_sound.len > 2 { return inst_or_sound.get(2) }
   1.0
}

fn set_pan(any inst, any pan) any {
   "Sets the stereo panning of a sound instance(-1.0 to 1.0)."
   if is_list(inst) && inst.len > 5 { inst[5] = pan + 0.0 }
}

fn get_pan(any inst) any {
   "Returns the stereo panning of a sound instance."
   if is_list(inst) && inst.len > 5 { return inst.get(5) }
   0.0
}

fn set_master_volume(any vol) any {
   "Sets the global master volume."
   snd_backend.set_master_volume(vol)
}

fn get_master_volume() any {
   "Returns the global master volume."
   return snd_backend.get_master_volume()
}

fn get_backend_name() str {
   "Returns the name of the active audio backend."
   return snd_backend.get_backend_name()
}

use std.os.sound.diag as diag

fn probe() any {
   "Implements `probe`."
   diag.probe()
}

fn probe_text() any {
   "Implements `probe_text`."
   diag.probe_text()
}

fn print_probe() any {
   "Implements `print_probe`."
   diag.print_probe()
}

fn play_oneshot(any path, any pitch=1.0, any vol=1.0, any pan=0.0) any {
   "Loads and plays a sound once. Useful for simple effects."
   def s = load(path)
   if s { play(s, pitch, vol, false, pan) }
}
