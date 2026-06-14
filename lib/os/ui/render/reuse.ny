;; Keywords: render reuse cache scratch allocation os ui
;; Reusable render scratch storage for reducing per-frame allocations in UI drawing paths.
;; References:
;; - std.os.ui.render
;; - std.os.ui.render.viewer.batch
module std.os.ui.render.reuse(
   make, reset, events_seen, ready_frames, reused_frames,
   note_events_seen, mark_event_seen, abs_gt, size_stable,
   input_active, capture_active, dynamic_active,
   candidate, should_present, note_full_draw, try_present
)

use std.core
use std.os.ui.render.dump as ui_profile
use std.os.ui.render as gfx

fn make() dict {
   "Creates frame-reuse controller state."
   {
      "events_seen": false,
      "ready_frames": 0,
      "reused_frames": 0,
      "last_w": 0,
      "last_h": 0,
      "trace_state": -1
   }
}

fn events_seen(dict st) bool { bool(st.get("events_seen", false)) }

fn ready_frames(dict st) int { int(st.get("ready_frames", 0)) }

fn reused_frames(dict st) int { int(st.get("reused_frames", 0)) }

fn _opt_bool(any opts, str key, bool fallback=false) bool {
   is_dict(opts) ? bool(opts.get(key, fallback)) : fallback
}

fn _opt_int(any opts, str key, int fallback=0) int {
   is_dict(opts) ? int(opts.get(key, fallback)) : fallback
}

fn _opt_f64(any opts, str key, f64 fallback=0.0) f64 {
   is_dict(opts) ? float(opts.get(key, fallback)) : fallback
}

fn _any_opt_bool(any opts, list keys) bool {
   mut i = 0
   while i < keys.len { if _opt_bool(opts, keys.get(i, ""), false) { return true } i += 1 }
   false
}

fn _any_opt_abs_gt(any opts, list keys, f64 eps) bool {
   mut i = 0
   while i < keys.len { if abs_gt(_opt_f64(opts, keys.get(i, ""), 0.0), eps) { return true } i += 1 }
   false
}

fn _enabled(any opts) bool {
   if is_dict(opts) && opts.contains("enabled") { return bool(opts.get("enabled", false)) }
   ui_profile.env_toggle_cached("NY_UI_GUI_IDLE_REUSE", true) &&
   !ui_profile.env_truthy_cached("NY_UI_DISABLE_GUI_IDLE_REUSE")
}

fn _warmup(any opts) int {
   if is_dict(opts) && opts.contains("warmup") {
      return _opt_int(opts, "warmup", 1)
   }
   ui_profile.env_int_cached("NY_UI_GUI_IDLE_REUSE_WARMUP", 1, 0, 128)
}

fn _redraw_interval(any opts) int {
   if is_dict(opts) && opts.contains("redraw_interval") {
      return _opt_int(opts, "redraw_interval", 300)
   }
   ui_profile.env_int_cached("NY_UI_GUI_IDLE_REUSE_REDRAW_INTERVAL", 300, 0, 1000000)
}

fn _trace_enabled(any opts) bool {
   if is_dict(opts) && opts.contains("trace") {
      return _opt_bool(opts, "trace", false)
   }
   ui_profile.env_truthy_cached("NY_UI_GUI_IDLE_REUSE_TRACE")
}

fn _trace(dict st, bool active, str msg, any opts=0) dict {
   if !_trace_enabled(opts) { return st }
   def next_state = active ? 1 : 0
   if int(st.get("trace_state", -1)) == next_state { return st }
   st["trace_state"] = next_state
   ui_profile.print_text(
      "[ui:reuse] " + msg +
      " warm=" + to_str(ready_frames(st)) +
      " reused=" + to_str(reused_frames(st))
   )
   st
}

fn reset(dict st, str reason="", any opts=0) dict {
   "Resets reuse warmup and reuse counters."
   def had_state = ready_frames(st) > 0 || reused_frames(st) > 0
   st["ready_frames"] = 0
   st["reused_frames"] = 0
   if had_state && reason.len > 0 {
      st = _trace(st, false, "reset reason=" + reason, opts)
   }
   st
}

fn note_events_seen(dict st, any seen) dict {
   "Stores whether input/window events were seen this frame."
   st["events_seen"] = bool(seen)
   st
}

fn mark_event_seen(dict st) dict {
   "Marks the current frame as event-active."
   st["events_seen"] = true
   st
}

fn abs_gt(any x, any eps) bool {
   "Returns true when absolute value is greater than an epsilon."
   def v, e = float(x), float(eps)
   v > e || v < -e
}

fn size_stable(dict st, any opts) bool {
   "Returns whether the window size is known and unchanged."
   def w, h = _opt_int(opts, "win_w", 0), _opt_int(opts, "win_h", 0)
   if w <= 0 || h <= 0 { return false }
   if _opt_bool(opts, "size_changed", false) {
      st["last_w"] = w
      st["last_h"] = h
      reset(st, "resize", opts)
      return false
   }
   def last_w, last_h = int(st.get("last_w", 0)), int(st.get("last_h", 0))
   if last_w == 0 || last_h == 0 {
      st["last_w"] = w
      st["last_h"] = h
      return true
   }
   if w != last_w || h != last_h {
      st["last_w"] = w
      st["last_h"] = h
      reset(st, "resize", opts)
      return false
   }
   true
}

fn input_active(dict st, any opts) bool {
   "Returns whether input or scroll movement invalidates reuse."
   events_seen(st) || _any_opt_bool(opts, ["input_active", "ui_active"]) ||
   _any_opt_abs_gt(opts, ["mouse_dx", "mouse_dy", "scroll_dx", "scroll_dy", "scroll_z"], _opt_f64(opts, "input_epsilon", 0.001))
}

fn capture_active(any opts) bool {
   "Returns whether capture/dump work must force a full frame."
   _any_opt_bool(opts, ["capture_active", "capture_request", "pending_capture", "auto_capture", "dump_suite", "batch_dump"])
}

fn dynamic_active(any opts) bool {
   "Returns whether animation or loading must force a full frame."
   def static_pose = _opt_bool(opts, "static_pose_ready", false) && !_opt_bool(opts, "animated", false)
   if static_pose {
      return _any_opt_bool(opts, ["dynamic_active", "async_load", "startup_load", "animated"])
   }
   _any_opt_bool(opts, ["dynamic_active", "async_load", "startup_load", "animated"]) ||
   _opt_int(opts, "animation_count", 0) > 0 ||
   _opt_int(opts, "skin_count", 0) > 0 ||
   _opt_int(opts, "morph_target_count", 0) > 0
}

fn candidate(dict st, any opts) bool {
   "Returns whether the frame is eligible for present-only reuse."
   if !_enabled(opts) { return false }
   if !_opt_bool(opts, "gui_frame", false) { return false }
   if !_opt_bool(opts, "gui_visible", true) { return false }
   if !_opt_bool(opts, "first_frame_done", true) { return false }
   if !_opt_bool(opts, "scene_active", true) { return false }
   if _opt_bool(opts, "bench_active", false) || _opt_bool(opts, "proof_dump", false) { return false }
   if capture_active(opts) { return false }
   if _opt_bool(opts, "layout_dirty", false) || _opt_int(opts, "layout_warm_frames", 0) > 0 { return false }
   if _opt_bool(opts, "projection_dirty", false) { return false }
   if dynamic_active(opts) || input_active(st, opts) { return false }
   size_stable(st, opts)
}

fn should_present(dict st, any opts) bool {
   "Returns whether the current frame can skip redraw and present."
   if !candidate(st, opts) {
      reset(st, "", opts)
      return false
   }
   if ready_frames(st) < _warmup(opts) { return false }
   def interval = _redraw_interval(opts)
   if interval > 0 && reused_frames(st) >= interval { return false }
   true
}

fn note_full_draw(dict st, any opts) dict {
   "Updates reuse state after a full frame draw."
   if !candidate(st, opts) {
      reset(st, "", opts)
      return st
   }
   if ready_frames(st) < _warmup(opts) {
      st["ready_frames"] = ready_frames(st) + 1
      if ready_frames(st) >= _warmup(opts) { st = _trace(st, true, "armed", opts) }
   }
   st["reused_frames"] = 0
   st
}

fn try_present(dict st, any opts) bool {
   "Attempts a present-only frame when reuse is allowed."
   ;; OpenGL has no color-load resume path here; do not reset the reuse
   ;; controller every frame. Just skip present-only reuse on GL.
   if gfx.get_active_backend_name() == "opengl" { return false }
   if !should_present(st, opts) { return false }
   gfx.set_next_frame_load_color(true)
   if !gfx.begin_frame() {
      gfx.set_next_frame_load_color(false)
      return false
   }
   if !gfx.end_frame() {
      reset(st, "end_frame", opts)
      return false
   }
   st["reused_frames"] = reused_frames(st) + 1
   ui_profile.counter_add("gui_idle_reuse_frames", 1.0)
   true
}

#main {
   mut st = make()
   def opts = {"enabled": true, "gui_frame": true, "win_w": 800, "win_h": 450, "warmup": 1}
   assert(candidate(st, opts), "viewer idle candidate")
   st = note_full_draw(st, opts)
   assert(ready_frames(st) == 1 && should_present(st, opts), "viewer idle warmup")
   st = mark_event_seen(st)
   assert(input_active(st, opts) && !candidate(st, opts), "viewer idle input blocks reuse")
   st = make()
   def posed = {"enabled": true, "gui_frame": true, "win_w": 800, "win_h": 450, "static_pose_ready": true, "skin_count": 1, "animation_count": 1}
   assert(candidate(st, posed), "viewer idle static skinned pose")
   print("✓ std.os.ui.render.reuse self-test passed")
}
