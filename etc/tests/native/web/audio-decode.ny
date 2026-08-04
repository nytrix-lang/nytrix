;; Keywords: browser web audio decode playback portability fixture
;; The browser target must decode a packed audio asset through Web Audio and
;; start one-shot playback. This fixture requests decode+play of the packed
;; beep.wav; the host echoes the decoded buffer length, duration, and the
;; started source into data-audio-* attributes the harness greps out of
;; --dump-dom. Decoding is independent of the user gesture; audible output
;; still waits on the gesture-gated context. web_play runs at top level so the
;; decode is recorded even when the asyncify frame loop has not yet started.
use std.os.sound as sound
use std.os.ui.render as gfx
use std.os.ui.window as window

def backend = sound.init(false)
if !backend { sound.shutdown() }
def win = gfx.init_window(320, 180, "Nytrix browser audio decode", 0, true, false, 1)
sound.web_play("beep.wav")
mut n = 0
while !gfx.window_should_close() {
   sound.web_play("beep.wav")
   gfx.begin_frame_clear(gfx.BLACK)
   gfx.draw_rect(24.0, 24.0, 272.0, 132.0, gfx.WHITE)
   n += 1
   if n > 3 {
      window.set_should_close(win, true)
   }
   gfx.end_frame()
}