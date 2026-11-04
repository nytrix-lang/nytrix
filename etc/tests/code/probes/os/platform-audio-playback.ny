use core
use core.os as os
use core.os.sound as sound
use core.os.sound.source.synth as synth

if !sound.init(false) {
   assert(sound.get_backend_name() == "none",
      "failed hosted initialization leaves no active backend")
   sound.shutdown()
   print("HOSTED_AUDIO_PLAYBACK_SKIPPED: no audio device")
} else if sound.get_backend_name() != "pulse" {
   sound.shutdown()
   print("HOSTED_AUDIO_PLAYBACK_SKIPPED: PulseAudio backend unavailable")
} else {
   def tone = synth.make_sine_source(440, 0.1, 44100, 0.25)
   assert(tone, "generated PCM source")
   def path = os.temp_dir() + "/ny-hosted-audio-" + to_str(os.pid()) + ".wav"
   assert(synth.write_synth_wav(tone, path), "real WAV resource is written")
   def resource = sound.load(path)
   assert(resource, "WAV resource imports")
   def instance = sound.play(resource)
   assert(instance, "imported resource reaches the host output stream")
   assert(!sound.is_playing(instance),
      "non-looping playback finishes before synchronous play returns")
   sound.shutdown()
   assert(sound.get_backend_name() == "none",
      "backend identity resets on shutdown")
   unwrap(os.file_remove(path))
   print("HOSTED_AUDIO_PLAYBACK_OK")
}

0
