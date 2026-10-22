;; Keywords: ui debug environment logging
;; Lightweight window/backend diagnostics without importing the renderer.
;; The full render dump module depends on the render facade, which in turn
;; pulls every graphics and image module into a window-only program.  Keep the
;; small logging contract used by window backends in this dependency-free
;; module so capability probes can compile and fail boundedly.
module core.os.ui.debug(
   env_truthy_cached, env_present_cached, env_enabled_cached, env_toggle_cached,
   env_int_cached, elapsed_ms, debug_enabled, debug_verbose_enabled,
   event_trace_enabled, print_text, print_line, eprint_text,
   env_trim_cached, env_lower_cached, debug_deep_enabled, gfx_frame_trace_enabled,
   enabled, dump_trace_enabled, text_immediate_enabled, headless_gui_enabled,
   now, mark, dump_path, counter_add, set_framebuffer_hash_line,
   trace_process_enabled, trace_process_sample, apply_verbose_argv
)

use core
use core.common as common
use core.os.path as path

mut _profile_counters, _framebuffer_hash_line = dict(32), ""

fn env_truthy_cached(any name) bool { common.env_truthy(to_str(name)) }

fn env_present_cached(any name) bool { common.env_present(to_str(name)) }

fn env_enabled_cached(any name) bool { common.env_enabled(to_str(name)) }

fn env_toggle_cached(any name, bool default_value=false) bool {
   common.env_toggle(to_str(name), default_value)
}

fn env_int_cached(any name, int default_value=0, int min_value=-2147483648, int max_value=2147483647) int {
   common.env_int_clamped(to_str(name), default_value, min_value, max_value)
}

fn env_trim_cached(any name) str { common.env_trim(to_str(name)) }

fn env_lower_cached(any name) str { common.env_lower(to_str(name)) }

fn elapsed_ms(int t0) f64 { float(__ticks_ns() - t0) / 1e6 }

fn debug_enabled() bool { env_truthy_cached("NY_DEBUG") }

fn debug_verbose_enabled() bool { env_truthy_cached("NY_DEBUG_VERBOSE") }

fn debug_deep_enabled() bool { env_truthy_cached("NY_DEBUG_DEEP") }

fn gfx_frame_trace_enabled() bool { env_truthy_cached("NY_GFX_FRAME_TRACE") }

fn enabled() bool { env_truthy_cached("NY_GUI_PROFILE") || env_truthy_cached("NY_UI_PROFILE") }

fn dump_trace_enabled() bool { env_truthy_cached("NY_UI_GUI_DUMP_TRACE") }

fn text_immediate_enabled() bool {
   env_truthy_cached("NY_GUI_TEXT_IMMEDIATE") || env_truthy_cached("NY_UI_TEXT_IMMEDIATE")
}

fn headless_gui_enabled() bool { env_truthy_cached("NY_UI_HEADLESS_GUI") }

fn now() int { __ticks_ns() }

fn mark(any label, int t0, any detail="") bool {
   if !enabled() || t0 <= 0 { return false }
   print_line("gui:prof", to_str(label) + "=" + to_str(elapsed_ms(t0)) + "ms" + to_str(detail))
   true
}

fn dump_path(str path_env, str fallback_name) str {
   def configured = env_trim_cached(path_env)
   configured.len > 0 ? configured : path.join(path.temp_dir(), fallback_name)
}

fn counter_add(str name, f64 value=1.0) f64 {
   def next = float(_profile_counters.get(name, 0.0)) + value
   _profile_counters[name] = next
   next
}

fn set_framebuffer_hash_line(any line) bool {
   _framebuffer_hash_line = to_str(line)
   true
}

fn trace_process_enabled() bool { env_truthy_cached("NY_TRACE_PROC") }

fn trace_process_sample(int frame=0, str label="") bool { false }

fn apply_verbose_argv(int start_index=1) bool {
   mut i = max(0, start_index)
   mut found = false
   while i < argc() {
      def arg = to_str(argv(i))
      if arg == "-v" || arg == "-vv" || arg == "-vvv" || arg == "--verbose" || arg == "--debug" || arg == "--trace" {
         found = true
      }
      i += 1
   }
   found
}

fn event_trace_enabled() bool { env_truthy_cached("NY_UI_EVENT_TRACE") }

fn print_text(any line) bool {
   print(line)
   true
}

fn print_line(any tag, any line="") bool {
   print("[" + to_str(tag) + "] " + to_str(line))
   true
}

fn eprint_text(any line) bool {
   _eprint_raw(to_str(line))
   _eprint_raw("\n")
   true
}
