;; Keywords: sound

module std.os.sound (
   init, shutdown,
   load, play, stop, is_playing,
   set_volume, get_volume,
   set_pitch, get_pitch,
   set_pan, get_pan,
   set_master_volume, get_master_volume,
   play_oneshot,
   get_backend_name,
   diag, probe, probe_text, print_probe
)

use std.core *
use std.os.sound.backend as snd_backend
use std.os.sound.res as res

fn init(){
   "Initializes module state."
   res.init()
   return snd_backend.init(false)
}

fn shutdown(){
   "Shuts down module state."
   snd_backend.shutdown()
   res.shutdown()
}

fn load(path){
   "Loads a sound from file (cached)."
   res.load(path)
}

fn play(sound, pitch=1.0, vol=1.0, looping=false, pan=0.0){
   "Plays a sound object. Returns a sound instance handle."
   return snd_backend.play(sound, pitch, vol, looping, pan)
}

fn stop(inst){
   "Stops a sound instance or all instances of a sound."
   snd_backend.stop(inst)
}

fn is_playing(inst){
   "Returns true if the sound instance is currently playing."
   return snd_backend.is_playing(inst)
}

fn set_volume(inst_or_sound, vol){
   "Sets the volume of a sound instance or the default volume of a sound object."
   if(is_list(inst_or_sound)){
      if(len(inst_or_sound) > 3){ store_item(inst_or_sound, 3, vol + 0.0) }
      else { store_item(inst_or_sound, 6, vol + 0.0) }
   }
}

fn get_volume(inst_or_sound){
   "Returns the volume of a sound instance or a sound object."
   if(is_list(inst_or_sound)){
      if(len(inst_or_sound) > 3){ return get(inst_or_sound, 3) }
      return get(inst_or_sound, 6, 1.0)
   }
   1.0
}

fn set_pitch(inst_or_sound, pitch){
   "Sets the playback pitch of a sound instance."
   if(is_list(inst_or_sound) && len(inst_or_sound) > 2){
      store_item(inst_or_sound, 2, pitch + 0.0)
   }
}

fn get_pitch(inst_or_sound){
   "Returns the playback pitch of a sound instance."
   if(is_list(inst_or_sound) && len(inst_or_sound) > 2){
      return get(inst_or_sound, 2)
   }
   1.0
}

fn set_pan(inst, pan){
   "Sets the stereo panning of a sound instance (-1.0 to 1.0)."
   if(is_list(inst) && len(inst) > 5){
      store_item(inst, 5, pan + 0.0)
   }
}

fn get_pan(inst){
   "Returns the stereo panning of a sound instance."
   if(is_list(inst) && len(inst) > 5){
      return get(inst, 5)
   }
   0.0
}

fn set_master_volume(vol){
   "Sets the global master volume."
   snd_backend.set_master_volume(vol)
}

fn get_master_volume(){
   "Returns the global master volume."
   return snd_backend.get_master_volume()
}

fn get_backend_name(){
   "Returns the name of the active audio backend."
   return snd_backend.get_backend_name()
}

use std.os.sound.diag as diag

fn probe(){
   "Implements `probe`."
   diag.probe()
}
fn probe_text(){
   "Implements `probe_text`."
   diag.probe_text()
}
fn print_probe(){
   "Implements `print_probe`."
   diag.print_probe()
}

fn play_oneshot(path, pitch=1.0, vol=1.0, pan=0.0){
   "Loads and plays a sound once. Useful for simple effects."
   def s = load(path)
   if(s){ play(s, pitch, vol, false, pan) }
}
