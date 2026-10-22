use core
use core.os.sound as sound

if sound.init(false) {
   assert(sound.get_backend_name() != "none", "successful audio initialization exposes its active backend")
   sound.shutdown()
   print("AUDIO_AVAILABLE")
} else {
   assert(sound.get_backend_name() == "none", "failed audio initialization leaves no active backend")
   print("NO_AUDIO_CLEAN_FAILURE")
}

def shutdown_ok = sound.shutdown()
assert(shutdown_ok, "audio shutdown succeeds after initialization probe")
