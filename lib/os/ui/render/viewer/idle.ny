;; Keywords: viewer idle timing throttle frame os ui render
;; Idle timing and frame pacing helpers for reducing unnecessary viewer work.
;; References:
;; - std.os.ui.render.viewer.runtime
module std.os.ui.render.viewer.idle(enabled, note_events_seen, mark_event_seen, note_full_draw, try_present)
use std.core
use std.os.ui.render.dump as ui_profile
use std.os.ui.render.reuse

mut _state = reuse.make()
mut _mode = -1

fn enabled() bool {
   "Returns whether viewer idle-frame reuse is enabled."
   if(_mode == -1){
      _mode = (ui_profile.env_toggle_cached("NY_UI_GUI_IDLE_REUSE", true) &&
      !ui_profile.env_truthy_cached("NY_UI_DISABLE_GUI_IDLE_REUSE")) ? 1 : 0
   }
   _mode == 1
}

fn note_events_seen(seen) bool {
   "Records event activity in the viewer reuse state."
   if(!enabled()){ return bool(seen) }
   _state = reuse.note_events_seen(_state, seen)
   reuse.events_seen(_state)
}

fn mark_event_seen() bool {
   "Marks the viewer reuse state as event-active."
   if(!enabled()){ return true }
   _state = reuse.mark_event_seen(_state)
   true
}

fn note_full_draw(opts) bool {
   "Updates viewer reuse state after a full draw."
   if(!enabled()){ return false }
   _state = reuse.note_full_draw(_state, opts)
   true
}

fn try_present(opts) bool {
   "Attempts a present-only frame through the viewer reuse state."
   enabled() && reuse.try_present(_state, opts)
}

#main {
   assert(is_bool(enabled()), "viewer idle enabled")
   assert(mark_event_seen(), "viewer idle mark")
   print("✓ std.os.ui.render.viewer.idle self-test passed")
}
