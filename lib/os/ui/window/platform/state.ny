;; Keywords: platform window backend state os ui input
;; Shared mutable state for the backend-neutral window platform layer.
;; References:
;; - core.os.ui.window.platform
;; - core.os.ui.window
;; - core.os.ui.window.consts
module core.os.ui.window.platform.state(_ensure_platform_state, _get_platform_val, _get_platform_dict, _set_platform_val, _set_platform_dict)
use core

mut _platform_state = dict(8)

fn _ensure_platform_state() dict {
   mut needs_init = true
   if is_dict(_platform_state) { needs_init = _platform_state.len == 0 }
   if needs_init {
      _platform_state = {
         "initialized": false,
         "backend_name": "",
         "native_windows": dict(16),
         "should_close_flags": dict(8),
         "window_attribs": dict(8),
         "window_contexts": dict(8),
         "window_user_pointers": dict(8),
         "window_size_limits": dict(8),
         "window_aspect_ratios": dict(8),
         "window_callbacks": dict(8),
         "pending_native_events": dict(8),
         "window_hints": dict(8),
         "init_hints": dict(8),
         "error_callback": 0,
         "monitor_callback": 0,
         "joystick_callback": 0,
         "timer_offset": -1,
         "windows": [],
         "monitors": [],
         "platform": dict(8),
      }
   }
   _platform_state
}

fn _get_platform_val(any key, any default=0) any {
   def state = _ensure_platform_state()
   state.get(key, default)
}

fn _get_platform_dict(str key, dict default) dict {
   def state = _ensure_platform_state()
   def value = state.get(key, default)
   if is_dict(value) { value } else { default }
}

fn _set_platform_dict(str key, dict value) dict {
   mut state = _ensure_platform_state()
   state[key] = value
   _platform_state = state
   value
}

fn _set_platform_val(any key, any val) any {
   mut state = _ensure_platform_state()
   state[key] = val
   _platform_state = state
   val
}
