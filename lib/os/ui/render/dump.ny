;; Keywords: profile profiling tracing os ui render
;; UI profiling, tracing, environment toggles, and benchmark logging.
;; References:
;; - std.os.ui.render
;; - std.os.ui.render.matrix
module std.os.ui.render.dump(
   save, safe_name, root_dir, path_named, snapshot_path, close_with_dump, snapshot_once,
   auto_dump_enabled, auto_dump_path, auto_dump_delay_frames, auto_dump_pre_frame, auto_dump_post_frame,
   gui_auto_dump_path, gui_auto_dump_delay, gui_auto_dump_exit_enabled, parse_skip_list,
   split_model_list, model_skipped, suite_field, suite_parse_line, suite_snapshot_path,
   suite_parse_text, suite_parse_env, suite_exit_enabled, gizmo_mode, framebuffer_hash_line,
   set_framebuffer_hash_line,
   env_truthy_cached, env_present_cached, env_enabled_cached, env_toggle_cached, env_int_cached,
   env_trim_cached, env_lower_cached, mode_truthy_cached, set_bool, set_bools, set_str, apply_verbose_argv, reset_env_cache,
   debug_enabled, debug_verbose_enabled, debug_deep_enabled, gfx_frame_trace_enabled, enabled,
   trace_enabled, deep_enabled, gui_trace_enabled, dump_trace_enabled, text_immediate_enabled,
   text_group_batches_enabled, min_ms, now, elapsed_ns, elapsed_ms, ms_between, elapsed_s, format_tag,
   format_line, colorize_line, print_text, eprint_text, print_line, eprint_line, log, mark, mark_next,
   mark_done, trace, dump_enabled, dump_path, append_line, event_trace_enabled, event_trace,
   stage_enabled, stage_ms_since, stage_log, profile_dump_force, profile_dump_set_path,
   profile_dump_enabled, profile_dump_file, profile_dump_row, bench_force, bench_dump_set_path,
   bench_dump_file, bench_enabled, bench_reset, bench_record, bench_flush, counter_reset, counter_add,
   counter_value, counter_json_fields, frame_reset, frame_record, frame_samples, frame_avg, frame_fps,
   trace_process_enabled, trace_process_sample,
   force_headless, force_surfaced_headless, force_frame_hash_lock, force_frame_print_every,
   batch_prefetch_enabled, batch_fast_env_enabled, frame_hash_lock_enabled, parity_lock_stats_enabled,
   dump_pose_enabled, parity_trace_enabled, editor_parity_trace_enabled, visible_skybox_default,
   startup_skybox_enabled, frame_print_every, headless_enabled, headless_gui_enabled,
   headless_sim_enabled, ui_bench_enabled, headless_visual_bench_enabled, nosurface_enabled,
   crosshair_enabled, bench_loop_ready, fast_nosurface_bench_enabled, fast_surface_bench_enabled,
   sim_nosurface_bench_enabled, world_grid_enabled, generated_env_width, gui_scale_env_present
)

use std.core
use std.core.error
use std.core.str as str
use std.core.common as common
use std.core.term as term
use std.math (min)
use std.math.crypto.encoding.bytes
use std.math.crypto.hash as hash
use std.os (file_read)
use std.os.path as path
use std.os.path as ospath
use std.os.sys as ossys
use std.os.ui.render (request_frame_capture, read_framebuffer, snapshot)
use std.os.ui.window
use std.math.parse.img as img_mod

mut _env_bool_cache, _env_int_cache, _env_str_cache = dict(64), dict(32), dict(32)
mut _dump_path_cache, _counter_values = dict(16), dict(64)
mut _profile_dump_mode, _bench_mode = -1, -1
mut _batch_prefetch_mode, _batch_fast_env_mode = -1, -1
mut _frame_hash_lock_mode, _parity_lock_stats_mode = -1, -1
mut _headless_mode, _headless_gui_mode, _headless_sim_mode = -1, -1, -1
mut _ui_bench_mode, _nosurface_mode = -1, -1
mut _fast_nosurface_bench_mode, _fast_surface_bench_mode, _sim_nosurface_bench_mode = -1, -1, -1
mut _headless_visual_bench_mode, _crosshair_mode, _world_grid_mode = -1, -1, -2
mut _frame_print_every_cache = -1
mut _profile_dump_path, _bench_dump_path = "", ""
mut _last_framebuffer_hash_line = ""
mut _bench_frames, _bench_begin_ns, _bench_draw_ns, _bench_end_ns = 0, 0, 0, 0
mut _frame_samples = 0
mut _frame_update_ms, _frame_draw_ms, _frame_world_ms, _frame_ui_ms = 0.0, 0.0, 0.0, 0.0
mut _frame_frame_ms, _frame_event_ms, _frame_gui_prep_ms, _frame_sim_ms = 0.0, 0.0, 0.0, 0.0
mut _min_ms_cache = -1
mut _proc_trace_last_ns, _proc_trace_last_rss, _proc_trace_peak_rss = 0, 0, 0
mut _proc_trace_last_cpu_ticks, _proc_trace_last_frame = 0, -1000000000
mut _proc_trace_next_frame = 0
mut _verbose_argv_mode = -1

fn _key(any name) str { to_str(name) }

fn _ny_trace_raw() bool {
   if is_dict(_env_bool_cache) && _env_bool_cache.contains("NY_TRACE") { return bool(_env_bool_cache.get("NY_TRACE", false)) }
   if is_dict(_env_str_cache) && _env_str_cache.contains("trim:NY_TRACE") {
      def v = str.lower(str.strip(to_str(_env_str_cache.get("trim:NY_TRACE", ""))))
      return !(v == "" || v == "0" || v == "false" || v == "no" || v == "off")
   }
   common.env_truthy("NY_TRACE")
}

fn _ny_trace_mode() str {
   mut v = ""
   if is_dict(_env_str_cache) && _env_str_cache.contains("lower:NY_TRACE") {
      v = str.lower(str.strip(to_str(_env_str_cache.get("lower:NY_TRACE", ""))))
   } elif common.env_present("NY_TRACE") {
      v = common.env_lower("NY_TRACE")
   } else {
      return ""
   }
   if v == "" || v == "1" || v == "true" || v == "yes" || v == "on" { return "lite" }
   v
}

fn _ny_trace_deep() bool {
   def m = _ny_trace_mode()
   common.env_truthy("NY_TRACE_DEEP") || m == "2" || m == "deep" || m == "full" || m == "spam"
}

fn _ny_trace_spam() bool {
   def m = _ny_trace_mode()
   common.env_truthy("NY_TRACE_SPAM") || m == "3" || m == "spam"
}

fn _ny_trace_perf() bool {
   def m = _ny_trace_mode()
   common.env_truthy("NY_TRACE_PERF") || m == "perf" || m == "bench" || m == "fast"
}

fn _ny_trace_value(str name) str {
   def k = _key(name)
   if !_ny_trace_raw() { return "" }
   if common.env_present(k) { return "" }
   def deep = _ny_trace_deep()
   def spam = _ny_trace_spam()
   def perf = _ny_trace_perf()
   ;; NY_TRACE=1 is intentionally low-cost. It should not silently enable
   ;; texture tracing, startup spam, profile dumps, or per-frame diagnostics.
   ;; Expensive traces stay behind their explicit env vars or deep/spam modes.
   if k == "NY_UI_RENDER_BACKEND" { return "vk" }
   if k == "NY_UI_VSYNC" { return perf ? "0" : "1" }
   if k == "NY_VK_FRAME_SLEEP_MS" { return "0" }
   if k == "NY_TEX_TRACE" { return "0" }
   if k == "NY_UI_TEX_TRACE" { return "0" }
   if k == "NY_UI_TEX_TRACE_SYNC_ALL" { return spam ? "1" : "0" }
   if k == "NY_UI_STARTUP_TRACE" { return "0" }
   if k == "NY_VK_PROFILE_DUMP" { return "0" }
   if k == "NY_VK_PROFILE_EVERY" { return perf ? "1000000" : "120" }
   if k == "NY_UI_SHADER_TRACE" { return "0" }
   if k == "NYTRIX_VK_VERTEX_MB" { return "16" }
   if k == "NYTRIX_VK_STAGING_MB" { return "96" }
   if k == "NY_UI_KEEP_ENV_CPU_IMAGES" { return "0" }
   if k == "NY_TRACE_PROC" { return "0" }
   if k == "NY_TRACE_PROC_EVERY_MS" { return "1000" }
   if k == "NY_TRACE_PROC_EVERY_FRAMES" { return spam ? "1" : (deep ? "30" : "120") }
   if k == "NY_TRACE_PROC_SMAPS" { return "0" }
   ;; Deep mode: detailed frame flow, GUI dump state, and begin diagnostics.
   if deep && k == "NY_RENDER_DEBUG" { return "1" }
   if deep && k == "NY_VK_BEGIN_TRACE" { return "1" }
   if deep && k == "NY_UI_GUI_DUMP_TRACE" { return "1" }
   if deep && k == "NY_UI_PROFILE" { return "1" }
   if deep && k == "NY_UI_PROFILE_TRACE" { return "1" }
   if deep && k == "NY_VK_PROFILE_TRACE" { return "1" }
   if deep && k == "NY_GFX_FRAME_TRACE" { return "1" }
   ;; Spam mode: stage-by-stage Vulkan breadcrumbs.  This is very verbose.
   if spam && k == "NY_VK_STAGE_TRACE" { return "1" }
   if spam && k == "NY_VK_DESCRIPTOR_TRACE" { return "1" }
   ""
}

fn _ny_trace_has_value(str name) bool {
   _ny_trace_value(name).len > 0
}

fn _ny_trace_truthy(str name) bool {
   def v = str.lower(str.strip(_ny_trace_value(name)))
   if v == "" || v == "0" || v == "false" || v == "no" || v == "off" { return false }
   true
}

fn _bool_cache() dict {
   if !is_dict(_env_bool_cache) { _env_bool_cache = dict(64) }
   _env_bool_cache
}

fn _int_cache() dict {
   if !is_dict(_env_int_cache) { _env_int_cache = dict(32) }
   _env_int_cache
}

fn _str_cache() dict {
   if !is_dict(_env_str_cache) { _env_str_cache = dict(32) }
   _env_str_cache
}

fn reset_env_cache() any {
   "Clears cached UI profile environment flags."
   _env_bool_cache, _env_int_cache, _env_str_cache = dict(64), dict(32), dict(32)
   _dump_path_cache = dict(16)
   _min_ms_cache = -1
}

fn set_bool(any name, any value) bool {
   "Overrides a cached boolean profile/env flag for this process."
   def k = _key(name)
   _bool_cache()
   _env_bool_cache[k] = bool(value)
   _env_bool_cache["present:" + k] = bool(value)
   _env_bool_cache["enabled:" + k] = bool(value)
   _env_bool_cache["toggle:" + k + ":true"] = bool(value)
   _env_bool_cache["toggle:" + k + ":false"] = bool(value)
   bool(value)
}

fn set_bools(any names, any value=true) int {
   "Overrides a list of cached boolean profile/env flags for this process."
   if !is_list(names) { return 0 }
   mut i, n = 0, names.len
   while i < n {
      set_bool(names.get(i, ""), value)
      i += 1
   }
   n
}

fn set_str(any name, any value) bool {
   "Overrides a cached string profile/env value for this process."
   def k = _key(name)
   def v = to_str(value)
   _str_cache()
   _bool_cache()
   _env_str_cache["trim:" + k] = str.strip(v)
   _env_str_cache["lower:" + k] = str.lower(str.strip(v))
   _env_bool_cache["present:" + k] = true
   true
}

fn _argv_verbose_level(any token) int {
   def t = str.lower(str.strip(to_str(token)))
   if t == "-vv" || t == "--debug-deep" || t == "--trace-deep" { return 2 }
   if t == "-vvv" || t == "--trace-spam" || t == "--debug-spam" { return 3 }
   if t == "-v" || t == "--verbose" || t == "--debug" || t == "--trace" || t == "--trace-ui" { return 1 }
   0
}

fn _argv_verbose_token(any token) bool {
   _argv_verbose_level(token) > 0
}

fn apply_verbose_argv(int start_index=1) bool {
   "Enables bounded UI/render/input diagnostics from argv; -vv and --trace-spam increase verbosity."
   mut i = int(max(0, start_index))
   mut level = 1
   while i < argc() {
      def lv = _argv_verbose_level(argv(i))
      if lv > level { level = lv }
      i += 1
   }
   if level <= 0 { return false }
   if _verbose_argv_mode >= level { return true }
   _verbose_argv_mode = level
   ;; Keep -v bounded: do not turn on generic NY_TRACE, because it can fan out
   ;; into Vulkan/profile frame spam through shared trace defaults.  -vv keeps
   ;; the cheap NY_TRACE=1 compatibility mode; --trace-spam/-vvv is explicit.
   if level >= 2 {
      set_str("NY_TRACE", level >= 3 ? "spam" : "1")
      set_bool("NY_TRACE", true)
   } else {
      set_str("NY_TRACE", "0")
      set_bool("NY_TRACE", false)
   }
   ;; Useful-by-default diagnostics stay quiet at level 1.  Deeper UI startup,
   ;; input, renderer, and Vulkan initialization traces are available with -vv
   ;; without making a normal verbose run look like a trace session.
   set_bools(["NY_UI_STARTUP_TRACE", "NY_UI_INPUT_TRACE", "NY_VK_INIT_TRACE"], level >= 2)
   if level >= 2 {
      set_bools(["NY_UI_TRACE", "NY_RENDER_DEBUG", "NY_FONT_LOAD_TRACE"], true)
   } else {
      set_bools(["NY_UI_TRACE", "NY_RENDER_DEBUG", "NY_FONT_LOAD_TRACE", "NY_TRACE_PROC"], false)
   }
   if level < 3 {
      ;; Explicitly suppress hot traces in useful/deep modes, even if a cached
      ;; previous run asked for them.  Users can still request full spam with
      ;; --trace-spam/-vvv or explicit env vars before startup.
      set_bools([
            "NY_GFX_FRAME_TRACE", "NY_VK_BEGIN_TRACE", "NY_VK_STAGE_TRACE",
            "NY_GL_TEXT_TRACE", "NY_VK_DESCRIPTOR_TRACE", "NY_UI_TEX_TRACE",
            "NY_TEX_TRACE", "NY_UI_GUI_DUMP_TRACE", "NY_TRACE_SPAM",
            "NY_TRACE_PROC", "NY_UI_PROFILE", "NY_UI_PROFILE_TRACE",
            "NY_VK_PROFILE_TRACE", "NY_VK_PROFILE_DUMP"
      ], false)
   }
   if level >= 2 {
      ;; Compact profiler summaries only.  No Vulkan stage breadcrumbs, no
      ;; per-frame begin/end tracing, and no per-glyph logs.
      set_bools(["NY_UI_PROFILE", "NY_VK_PROFILE_TRACE"], true)
      set_str("NY_VK_PROFILE_EVERY", "120")
   }
   if level >= 3 {
      ;; Last-resort spam mode: exact frame/stage breadcrumbs and hot draw logs.
      set_bools([
            "NY_DEBUG", "NY_DEBUG_VERBOSE", "NY_DEBUG_DEEP",
            "NY_GFX_FRAME_TRACE", "NY_VK_BEGIN_TRACE", "NY_VK_STAGE_TRACE",
            "NY_UI_GUI_DUMP_TRACE", "NY_GL_TEXT_TRACE", "NY_VK_DESCRIPTOR_TRACE",
            "NY_UI_TEX_TRACE", "NY_TEX_TRACE", "NY_TRACE_SPAM"
      ], true)
   }
   if level >= 2 {
      eprint_text(level >= 3 ? "[ui:verbose] enabled spam tracing by --trace-spam" :
         "[ui:verbose] enabled compact deep diagnostics by -vv")
   }
   true
}

fn env_truthy_cached(any name) bool {
   "Returns a cached truthy environment flag."
   def k = _key(name)
   if _ny_trace_has_value(k) { return _ny_trace_truthy(k) }
   _bool_cache()
   if _env_bool_cache.contains(k) { return bool(_env_bool_cache.get(k, false)) }
   def v = common.env_truthy(k)
   _env_bool_cache[k] = v
   v
}

fn env_present_cached(any name) bool {
   "Returns a cached environment-presence flag."
   def raw = _key(name)
   if _ny_trace_has_value(raw) { return true }
   def k = "present:" + raw
   _bool_cache()
   if _env_bool_cache.contains(k) { return bool(_env_bool_cache.get(k, false)) }
   def v = common.env_present(_key(name))
   _env_bool_cache[k] = v
   v
}

fn env_enabled_cached(any name) bool {
   "Returns a cached enabled environment flag."
   def raw = _key(name)
   if _ny_trace_has_value(raw) { return _ny_trace_truthy(raw) }
   def k = "enabled:" + raw
   _bool_cache()
   if _env_bool_cache.contains(k) { return bool(_env_bool_cache.get(k, false)) }
   def v = common.env_enabled(_key(name))
   _env_bool_cache[k] = v
   v
}

fn env_toggle_cached(any name, bool default_value=false) bool {
   "Returns a cached toggle environment flag with a default when unset."
   def raw = _key(name)
   if _ny_trace_has_value(raw) { return _ny_trace_truthy(raw) }
   def k = "toggle:" + raw + ":" + to_str(bool(default_value))
   _bool_cache()
   if _env_bool_cache.contains(k) { return bool(_env_bool_cache.get(k, false)) }
   def v = common.env_toggle(_key(name), bool(default_value))
   _env_bool_cache[k] = v
   v
}

fn env_int_cached(any name, int default_value=0, int min_value=-2147483648, int max_value=2147483647) int {
   "Returns a cached clamped integer environment value."
   def raw = _key(name)
   if _ny_trace_has_value(raw) {
      def tv = int(_ny_trace_value(raw))
      if tv < int(min_value) { return int(min_value) }
      if tv > int(max_value) { return int(max_value) }
      return tv
   }
   def k = "int:" + raw + ":" + to_str(int(default_value)) + ":" + to_str(int(min_value)) + ":" + to_str(int(max_value))
   _int_cache()
   if _env_int_cache.contains(k) { return int(_env_int_cache.get(k, default_value)) }
   def v = common.env_int_clamped(_key(name), int(default_value), int(min_value), int(max_value))
   _env_int_cache[k] = v
   v
}

fn env_trim_cached(any name) str {
   "Returns a cached trimmed environment value."
   def raw = _key(name)
   if _ny_trace_has_value(raw) { return _ny_trace_value(raw) }
   def k = "trim:" + raw
   _str_cache()
   if _env_str_cache.contains(k) { return to_str(_env_str_cache.get(k, "")) }
   def v = common.env_trim(_key(name))
   _env_str_cache[k] = v
   v
}

fn env_lower_cached(any name) str {
   "Returns a cached lower-cased environment value."
   def raw = _key(name)
   if _ny_trace_has_value(raw) { return str.lower(_ny_trace_value(raw)) }
   def k = "lower:" + raw
   _str_cache()
   if _env_str_cache.contains(k) { return to_str(_env_str_cache.get(k, "")) }
   def v = common.env_lower(_key(name))
   _env_str_cache[k] = v
   v
}

fn mode_truthy_cached(any mode, any name) int {
   "Returns an existing -1/0/1 mode or initializes it from a cached truthy environment flag."
   def m = int(mode)
   (m != -1) ? m : (env_truthy_cached(name) ? 1 : 0)
}

fn debug_enabled() bool {
   "Returns true when NY_DEBUG is enabled."
   env_truthy_cached("NY_DEBUG")
}

fn debug_verbose_enabled() bool {
   "Returns true when NY_DEBUG_VERBOSE is enabled."
   env_truthy_cached("NY_DEBUG_VERBOSE")
}

fn debug_deep_enabled() bool {
   "Returns true when NY_DEBUG_DEEP is enabled."
   env_truthy_cached("NY_DEBUG_DEEP")
}

fn gfx_frame_trace_enabled() bool {
   "Returns true when graphics frame tracing is enabled."
   env_truthy_cached("NY_GFX_FRAME_TRACE")
}

fn enabled() bool {
   "Returns true when general GUI profiling is enabled."
   env_truthy_cached("NY_GUI_PROFILE") || env_truthy_cached("NY_UI_PROFILE")
}

fn trace_enabled() bool {
   "Returns true when UI trace/profile trace output is enabled."
   env_truthy_cached("NY_UI_PROFILE_TRACE") || env_truthy_cached("NY_UI_TRACE")
}

fn deep_enabled() bool {
   "Returns true when verbose UI profiling is enabled."
   env_truthy_cached("NY_UI_PROFILE_DEEP")
}

fn gui_trace_enabled() bool {
   "Returns true when GUI trace output is enabled."
   env_truthy_cached("NY_UI_GUI_TRACE")
}

fn dump_trace_enabled() bool {
   "Returns true when GUI dump tracing is enabled."
   env_truthy_cached("NY_UI_GUI_DUMP_TRACE")
}

fn text_immediate_enabled() bool {
   "Returns true when text should bypass queued batching for diagnosis."
   env_truthy_cached("NY_GUI_TEXT_IMMEDIATE") || env_truthy_cached("NY_UI_TEXT_IMMEDIATE")
}

fn text_group_batches_enabled() bool {
   "Returns true when queued text runs may be grouped by font/color."
   env_toggle_cached("NY_GUI_TEXT_GROUP_BATCH", true) && env_toggle_cached("NY_UI_TEXT_GROUP_BATCH", true)
}

fn force_headless(bool nosurface=true, bool bench=false, bool sim=false) bool {
   "Forces UI headless policy flags for this process."
   _headless_mode = 1
   set_bool("NY_UI_HEADLESS", true)
   if nosurface {
      _nosurface_mode = 1
      set_bool("NYTRIX_VK_ALLOW_HEADLESS", true)
   }
   if bench {
      _ui_bench_mode = 1
      set_bool("NY_UI_BENCH", true)
   }
   if sim {
      _headless_sim_mode = 1
      set_bools(["NY_UI_HEADLESS_SIM", "NY_UI_REAL_HEADLESS_SIM"])
   }
   true
}

fn force_surfaced_headless() bool {
   "Forces a surfaced headless UI mode for this process."
   _headless_mode, _headless_gui_mode = 1, 1
   set_bools(["NY_UI_HEADLESS", "NY_UI_HEADLESS_GUI", "NY_UI_HEADLESS_MATCH_WINDOW"])
   true
}

fn force_frame_hash_lock() bool {
   "Forces deterministic frame hash policy flags for this process."
   _frame_hash_lock_mode = 1
   _parity_lock_stats_mode = 1
   set_bools(["NY_UI_FRAME_HASH_LOCK", "NY_UI_PARITY_LOCK_STATS"])
   true
}

fn force_frame_print_every(int frames) int {
   "Overrides the UI frame-print interval for this process."
   _frame_print_every_cache = frames
   _frame_print_every_cache
}

fn batch_prefetch_enabled() bool {
   "Returns true when UI batch prefetch is enabled."
   _batch_prefetch_mode = mode_truthy_cached(_batch_prefetch_mode, "NY_UI_BATCH_PREFETCH")
   _batch_prefetch_mode == 1
}

fn batch_fast_env_enabled() bool {
   "Returns true when fast batch environment generation is enabled."
   _batch_fast_env_mode = mode_truthy_cached(_batch_fast_env_mode, "NY_UI_BATCH_FAST_ENV")
   _batch_fast_env_mode == 1
}

fn frame_hash_lock_enabled() bool {
   "Returns true when deterministic frame hashing is enabled."
   _frame_hash_lock_mode = mode_truthy_cached(_frame_hash_lock_mode, "NY_UI_FRAME_HASH_LOCK")
   _frame_hash_lock_mode == 1
}

fn parity_lock_stats_enabled() bool {
   "Returns true when parity lock stats are enabled."
   if frame_hash_lock_enabled() {
      _parity_lock_stats_mode = 1
      return true
   }
   _parity_lock_stats_mode = mode_truthy_cached(_parity_lock_stats_mode, "NY_UI_PARITY_LOCK_STATS")
   _parity_lock_stats_mode == 1
}

fn dump_pose_enabled(int auto_dump_enabled, bool cli_dump_requested, bool batch_dump_enabled, bool gui_dump_suite_active, bool gui_probe_enabled) bool {
   "Returns true when the current frame should dump scene pose data."
   def auto_dump_now = (auto_dump_enabled == 1) ||
   (auto_dump_enabled == -1 && (env_truthy_cached("NYTRIX_AUTO_DUMP") || cli_dump_requested))
   batch_dump_enabled || (auto_dump_now && !gui_dump_suite_active && !gui_probe_enabled)
}

fn parity_trace_enabled() bool {
   "Returns true when UI parity tracing is enabled."
   env_truthy_cached("NY_UI_PARITY_TRACE")
}

fn editor_parity_trace_enabled() bool {
   "Returns true when editor parity tracing is enabled."
   env_truthy_cached("NY_UI_PARITY_TRACE_EDITOR") || parity_trace_enabled()
}

fn visible_skybox_default() bool {
   "Returns the default visible-skybox policy."
   if env_present_cached("NY_UI_VISIBLE_SKYBOX") {
      return env_enabled_cached("NY_UI_VISIBLE_SKYBOX")
   }
   if env_present_cached("NY_UI_SHOW_SKYBOX") {
      return env_enabled_cached("NY_UI_SHOW_SKYBOX")
   }
   if
   env_present_cached("NY_UI_SKYBOX") ||
   env_present_cached("NY_UI_SKYBOX_SOURCE") ||
   env_present_cached("NY_UI_SKYBOX_PATH") ||
   env_present_cached("NY_DEMO_SKYBOX")
   {
      return true
   }
   false
}

fn startup_skybox_enabled() bool {
   "Returns true when startup skybox loading is enabled."
   env_toggle_cached("NY_UI_STARTUP_SKYBOX", visible_skybox_default())
}

fn frame_print_every() int {
   "Returns the UI frame-print interval."
   if _frame_print_every_cache > 0 { return _frame_print_every_cache }
   _frame_print_every_cache = env_int_cached("NY_UI_FRAME_PRINT_EVERY", 120, 1, 1000000)
   _frame_print_every_cache
}

fn headless_enabled() bool {
   "Returns true when UI headless mode is enabled."
   _headless_mode = mode_truthy_cached(_headless_mode, "NY_UI_HEADLESS")
   _headless_mode == 1
}

fn headless_gui_enabled() bool {
   "Returns true when headless mode should still run GUI code."
   _headless_gui_mode = mode_truthy_cached(_headless_gui_mode, "NY_UI_HEADLESS_GUI")
   _headless_gui_mode == 1
}

fn headless_sim_enabled() bool {
   "Returns true when headless simulation mode is enabled."
   if _headless_sim_mode != -1 { return _headless_sim_mode == 1 }
   _headless_sim_mode = (env_truthy_cached("NY_UI_REAL_HEADLESS_SIM") ||
   env_truthy_cached("NY_UI_HEADLESS_SIM")) ? 1 : 0
   _headless_sim_mode == 1
}

fn ui_bench_enabled() bool {
   "Returns true when the UI benchmark loop is enabled."
   _ui_bench_mode = mode_truthy_cached(_ui_bench_mode, "NY_UI_BENCH")
   _ui_bench_mode == 1
}

fn headless_visual_bench_enabled() bool {
   "Returns true when headless benchmark mode should render visual work."
   if _headless_visual_bench_mode != -1 { return _headless_visual_bench_mode == 1 }
   if env_truthy_cached("NY_UI_HEADLESS_MODEL_ONLY") {
      _headless_visual_bench_mode = 0
   } elif env_present_cached("NY_UI_HEADLESS_VISUAL") {
      _headless_visual_bench_mode = env_truthy_cached("NY_UI_HEADLESS_VISUAL") ? 1 : 0
   } else {
      _headless_visual_bench_mode = 1
   }
   _headless_visual_bench_mode == 1
}

fn nosurface_enabled() bool {
   "Returns true when UI rendering should use a no-surface backend."
   if _nosurface_mode != -1 { return _nosurface_mode == 1 }
   def backend = env_lower_cached("NY_UI_BACKEND")
   _nosurface_mode = (backend == "none" || env_truthy_cached("NYTRIX_VK_ALLOW_HEADLESS")) ? 1 : 0
   _nosurface_mode == 1
}

fn crosshair_enabled() bool {
   "Returns true when viewer crosshair rendering is enabled."
   _crosshair_mode = mode_truthy_cached(_crosshair_mode, "NY_UI_CROSSHAIR")
   _crosshair_mode == 1
}

fn bench_loop_ready(int timeout_ns, int auto_dump_enabled, bool batch_dump_enabled) bool {
   "Returns true when the fast UI benchmark loop may be used."
   ui_bench_enabled() && headless_enabled() && timeout_ns > 0 && auto_dump_enabled != 1 && !batch_dump_enabled
}

fn fast_nosurface_bench_enabled(int timeout_ns, int auto_dump_enabled, bool batch_dump_enabled) bool {
   "Returns true when the no-surface fast benchmark path is enabled."
   if _fast_nosurface_bench_mode != -1 { return _fast_nosurface_bench_mode == 1 }
   _fast_nosurface_bench_mode = (bench_loop_ready(timeout_ns, auto_dump_enabled, batch_dump_enabled) && nosurface_enabled() && !headless_sim_enabled()) ? 1 : 0
   _fast_nosurface_bench_mode == 1
}

fn fast_surface_bench_enabled(int timeout_ns, int auto_dump_enabled, bool batch_dump_enabled, bool gui_dump_suite_active) bool {
   "Returns true when the surfaced fast benchmark path is enabled."
   if _fast_surface_bench_mode != -1 { return _fast_surface_bench_mode == 1 }
   _fast_surface_bench_mode = (bench_loop_ready(timeout_ns, auto_dump_enabled, batch_dump_enabled) &&
      !nosurface_enabled() &&
      !headless_sim_enabled() &&
   !gui_dump_suite_active) ? 1 : 0
   _fast_surface_bench_mode == 1
}

fn sim_nosurface_bench_enabled(int timeout_ns, int auto_dump_enabled, bool batch_dump_enabled) bool {
   "Returns true when the simulated no-surface benchmark path is enabled."
   if _sim_nosurface_bench_mode != -1 { return _sim_nosurface_bench_mode == 1 }
   _sim_nosurface_bench_mode = (bench_loop_ready(timeout_ns, auto_dump_enabled, batch_dump_enabled) && nosurface_enabled() && headless_sim_enabled()) ? 1 : 0
   _sim_nosurface_bench_mode == 1
}

fn world_grid_enabled(bool gui_visible, bool scene_selected, bool gui_probe_enabled) bool {
   "Returns true when the viewer world grid should be drawn."
   if _world_grid_mode == -2 {
      if env_present_cached("NY_UI_WORLD_GRID") {
         _world_grid_mode = env_truthy_cached("NY_UI_WORLD_GRID") ? 1 : 0
      } else {
         _world_grid_mode = -1
      }
   }
   if _world_grid_mode != -1 { return _world_grid_mode == 1 }
   gui_visible || scene_selected || gui_probe_enabled
}

fn generated_env_width(bool batch_dump_enabled, bool fast_env_enabled) int {
   "Returns the generated environment texture width."
   def w = env_int_cached("NY_UI_GENERATED_ENV_W", 256, 64, 1024)
   (batch_dump_enabled && fast_env_enabled) ? min(w, 128) : w
}

fn gui_scale_env_present() bool {
   "Returns true when NY_UI_SCALE is explicitly present."
   env_present_cached("NY_UI_SCALE")
}

fn min_ms() f64 {
   "Returns the minimum profile mark duration to print, in milliseconds."
   if _min_ms_cache < 0 {
      _min_ms_cache = env_int_cached("NY_GUI_PROFILE_MIN_MS", 1, 0, 1000000)
   }
   float(_min_ms_cache)
}

fn now() int {
   "Returns monotonic nanoseconds."
   __ticks_ns()
}

fn elapsed_ns(int t0) int {
   "Returns nanoseconds elapsed since `t0`."
   __ticks_ns() - int(t0)
}

fn elapsed_ms(int t0) f64 {
   "Returns milliseconds elapsed since `t0`."
   float(__ticks_ns() - int(t0)) / 1e6
}

fn ms_between(int t1, int t0) f64 {
   "Returns milliseconds between two monotonic timestamps."
   float(int(t1) - int(t0)) / 1e6
}

fn elapsed_s(int t0) f64 {
   "Returns seconds elapsed since `t0`."
   float(__ticks_ns() - int(t0)) / 1e9
}

fn stage_ms_since(int t0) f64 {
   "Returns milliseconds elapsed since `t0`, or zero for inactive timestamps."
   if int(t0) <= 0 { return 0.0 }
   float(__ticks_ns() - int(t0)) / 1e6
}

fn format_tag(any tag) str {
   "Formats a semantic colored bracket tag for terminal logs."
   term.log_tag(tag)
}

fn colorize_line(any line) str {
   "Applies semantic colors to bracketed tags in a log line."
   term.log_text(line)
}

fn format_line(any tag, any msg="") str {
   "Formats a semantic `[tag] message` line."
   format_tag(tag) + (to_str(msg).len > 0 ? (" " + to_str(msg)) : "")
}

fn print_text(any line) bool {
   "Prints a log line with colored bracket tags."
   print(colorize_line(line))
   true
}

fn eprint_text(any line) bool {
   "Prints a log line to stderr with colored bracket tags."
   eprint(colorize_line(line))
   true
}

fn print_line(any tag, any msg="") bool {
   "Prints a semantic colored `[tag] message` line."
   print(format_line(tag, msg))
   true
}

fn eprint_line(any tag, any msg="") bool {
   "Prints a semantic colored `[tag] message` line to stderr."
   eprint(format_line(tag, msg))
   true
}

fn log(any label, any ms, any detail="") bool {
   "Prints a thresholded GUI profile sample."
   if !enabled() { return false }
   def v = float(ms)
   if v < min_ms() { return false }
   def suffix = (detail == nil) ? "" : to_str(detail)
   print_line("gui:prof", to_str(label) + "=" + str.to_fixed(v, 3) + "ms" + suffix)
   true
}

fn mark(any label, int t0, any detail="") bool {
   "Prints a thresholded GUI profile sample from a start timestamp."
   if !enabled() { return false }
   def start = int(t0)
   if start <= 0 { return false }
   log(label, float(__ticks_ns() - start) / 1e6, detail)
}

fn mark_next(bool on, any label, int t0, any detail="") int {
   "Marks a profile sample and returns the next checkpoint timestamp."
   if on {
      mark(label, t0, detail)
      def next_t = __ticks_ns()
      return next_t
   }
   int(t0)
}

fn mark_done(bool on, any label, int t0, any detail="") bool {
   "Marks a terminal profile sample when enabled."
   if !on { return false }
   mark(label, t0, detail)
   true
}

fn trace(any label, any detail="") bool {
   "Prints a low-volume UI trace line."
   if !trace_enabled() && !enabled() { return false }
   print_line("ui:trace", to_str(label) + to_str(detail))
   true
}

fn event_trace_enabled() bool {
   "Returns true when raw UI event tracing is enabled."
   env_truthy_cached("NY_UI_EVENT_TRACE")
}

fn event_trace(int typ, any data, any tag="event", bool close_state=false) bool {
   "Prints one normalized UI event trace line."
   if !event_trace_enabled() { return false }
   def key, sc =
   is_dict(data) ? int(data.get("key", -1)) : -1,
   is_dict(data) ? int(data.get("scancode", data.get("raw_key", -1))) : -1
   def x, y =
   is_dict(data) ? float(data.get("x", 0.0)) : 0.0,
   is_dict(data) ? float(data.get("y", 0.0)) : 0.0
   def dx, dy =
   is_dict(data) ? float(data.get("dx", 0.0)) : 0.0,
   is_dict(data) ? float(data.get("dy", 0.0)) : 0.0
   print_line("ui:" + to_str(tag), "type=" + to_str(typ) +
      " key=" + to_str(key) +
      " sc=" + to_str(sc) +
      " x=" + to_str(x) +
      " y=" + to_str(y) +
      " dx=" + to_str(dx) +
      " dy=" + to_str(dy) +
   " close=" + to_str(bool(close_state)))
   true
}

fn stage_enabled() bool {
   "Returns true when batch/scene stage profiling is enabled."
   env_truthy_cached("NY_UI_BATCH_STAGE_TRACE") ||
   env_truthy_cached("NY_SCENE_PROFILE_TRACE") ||
   trace_enabled()
}

fn stage_log(any prefix, any model_name, any stage, int t0) bool {
   "Prints a batch/scene stage timing line."
   if !stage_enabled() { return false }
   print_line(prefix, "model=" + to_str(model_name) +
   " stage=" + to_str(stage) + " ms=" + to_str(stage_ms_since(t0)))
   true
}

fn dump_enabled(str flag_env, bool default_on=false) bool {
   "Returns a cached dump toggle."
   if env_present_cached(flag_env) {
      return env_enabled_cached(flag_env)
   }
   bool(default_on)
}

fn dump_path(str path_env, str fallback_name) str {
   "Returns a cached dump path, defaulting to the OS temp directory."
   def k = _key(path_env) + ":" + _key(fallback_name)
   if _dump_path_cache.contains(k) { return to_str(_dump_path_cache.get(k, "")) }
   def raw = env_trim_cached(path_env)
   def p = (raw.len > 0) ? raw : ospath.join(ospath.temp_dir(), _key(fallback_name))
   _dump_path_cache[k] = p
   p
}

fn append_line(str path, str line) bool {
   "Appends a single line to a profiling/debug dump."
   def p = ospath.normalize(to_str(path))
   if p.len <= 0 { return false }
   def s = to_str(line)
   def buf = s + ((s.len > 0 && load8(s, s.len - 1) == 10) ? "" : "\n")
   def open_res = ossys.sys_open(p, 1089, 420)
   if is_err(open_res) { return false }
   def fd = unwrap(open_res)
   mut off = 0
   while off < buf.len {
      def w = __write_off(fd, buf, buf.len - off, off)
      if w <= 0 {
         ossys.sys_close_quiet(fd)
         return false
      }
      off += w
   }
   ossys.sys_close_quiet(fd)
   true
}

fn profile_dump_force(bool value=true) bool {
   "Forces UI profile JSONL dumps on or off for this process."
   _profile_dump_mode = bool(value) ? 1 : 0
   bool(value)
}

fn profile_dump_set_path(str path) str {
   "Overrides the UI profile JSONL dump path for this process."
   _profile_dump_path = to_str(path)
   _profile_dump_path
}

fn profile_dump_enabled(bool default_on=false) bool {
   "Returns whether UI frame profile JSONL dumps are enabled."
   if _profile_dump_mode != -1 { return _profile_dump_mode == 1 }
   _profile_dump_mode = dump_enabled("NY_UI_PROFILE_DUMP", bool(default_on)) ? 1 : 0
   _profile_dump_mode == 1
}

fn profile_dump_file() str {
   "Returns the UI frame profile JSONL dump path."
   if _profile_dump_path.len <= 0 {
      _profile_dump_path = dump_path("NY_UI_PROFILE_DUMP_PATH", "nytrix_ui_profile.oasset.jsonl")
   }
   _profile_dump_path
}

fn profile_dump_row(int total_frame, int frames, f64 fps_est, f64 avg_fr, f64 avg_up, f64 avg_dr, f64 avg_wo, f64 avg_ui, f64 avg_ev, f64 avg_gp, f64 avg_sm) bool {
   "Writes one UI frame profile JSONL row."
   if !profile_dump_enabled(trace_enabled()) { return false }
   def row = "{\"format\":\"nytrix.ui.profile.v1\"" +
   ",\"frame\":" + to_str(int(total_frame)) +
   ",\"samples\":" + to_str(int(frames)) +
   ",\"fps\":" + to_str(fps_est) +
   ",\"frame_ms\":" + to_str(avg_fr) +
   ",\"update_ms\":" + to_str(avg_up) +
   ",\"draw_ms\":" + to_str(avg_dr) +
   ",\"world_ms\":" + to_str(avg_wo) +
   ",\"ui_ms\":" + to_str(avg_ui) +
   ",\"event_ms\":" + to_str(avg_ev) +
   ",\"gui_prep_ms\":" + to_str(avg_gp) +
   ",\"sim_ms\":" + to_str(avg_sm) +
   "}"
   append_line(profile_dump_file(), row)
}

fn bench_force(bool value=true) bool {
   "Forces no-surface benchmark JSONL dumps on or off for this process."
   _bench_mode = bool(value) ? 1 : 0
   bool(value)
}

fn bench_dump_set_path(str path) str {
   "Overrides the benchmark JSONL dump path for this process."
   _bench_dump_path = to_str(path)
   _bench_dump_path
}

fn bench_enabled(bool default_on=false) bool {
   "Returns whether no-surface benchmark profiling is enabled."
   if _bench_mode != -1 { return _bench_mode == 1 }
   if env_present_cached("NY_UI_BENCH_PROFILE") {
      _bench_mode = env_truthy_cached("NY_UI_BENCH_PROFILE") ? 1 : 0
   } else {
      _bench_mode = bool(default_on) ? 1 : 0
   }
   _bench_mode == 1
}

fn bench_dump_file(str profile_file="") str {
   "Returns the benchmark JSONL dump path."
   if _bench_dump_path.len > 0 { return _bench_dump_path }
   def raw = env_trim_cached("NY_UI_BENCH_PROFILE_DUMP_PATH")
   if raw.len > 0 {
      _bench_dump_path = raw
   } elif to_str(profile_file).len > 0 {
      _bench_dump_path = to_str(profile_file)
   } else {
      _bench_dump_path = dump_path("NY_UI_BENCH_PROFILE_DUMP_PATH", "nytrix_bench_profile.oasset.jsonl")
   }
   _bench_dump_path
}

fn bench_reset() any {
   "Resets accumulated no-surface benchmark timings."
   _bench_frames, _bench_begin_ns, _bench_draw_ns, _bench_end_ns = 0, 0, 0, 0
}

fn bench_record(int begin_ns, int draw_ns, int end_ns) int {
   "Accumulates one no-surface benchmark timing sample."
   if !bench_enabled(profile_dump_enabled(trace_enabled())) { return _bench_frames }
   _bench_frames = _bench_frames + 1
   _bench_begin_ns = _bench_begin_ns + int(begin_ns)
   _bench_draw_ns = _bench_draw_ns + int(draw_ns)
   _bench_end_ns = _bench_end_ns + int(end_ns)
   _bench_frames
}

fn bench_flush(int total_ns, dict renderer_stats, str profile_file="") bool {
   "Writes one no-surface benchmark JSONL row."
   if !bench_enabled(profile_dump_enabled(trace_enabled())) || _bench_frames <= 0 { return false }
   def frames = _bench_frames
   def row = "{\"format\":\"nytrix.ui.nosurface_bench.v1\"" +
   ",\"frames\":" + to_str(frames) +
   ",\"elapsed_ns\":" + to_str(int(total_ns)) +
   ",\"begin_ns_avg\":" + to_str(int(_bench_begin_ns / frames)) +
   ",\"draw_ns_avg\":" + to_str(int(_bench_draw_ns / frames)) +
   ",\"end_ns_avg\":" + to_str(int(_bench_end_ns / frames)) +
   ",\"begin_ns_total\":" + to_str(_bench_begin_ns) +
   ",\"draw_ns_total\":" + to_str(_bench_draw_ns) +
   ",\"end_ns_total\":" + to_str(_bench_end_ns) +
   ",\"draws\":" + to_str(int(renderer_stats.get("draws", 0))) +
   ",\"dynamic_draws\":" + to_str(int(renderer_stats.get("dynamic_draws", 0))) +
   ",\"static_draws\":" + to_str(int(renderer_stats.get("static_draws", 0))) +
   ",\"indexed_draws\":" + to_str(int(renderer_stats.get("indexed_draws", 0))) +
   ",\"submitted_vertices\":" + to_str(int(renderer_stats.get("submitted_vertices", 0))) +
   ",\"flushes\":" + to_str(int(renderer_stats.get("flushes", 0))) +
   ",\"pipeline_binds\":" + to_str(int(renderer_stats.get("pipeline_binds", 0))) +
   ",\"descriptor_binds\":" + to_str(int(renderer_stats.get("descriptor_binds", 0))) +
   "}"
   append_line(bench_dump_file(profile_file), row)
}

fn counter_reset(str prefix="") any {
   "Resets all counters, or counters with `prefix` when provided."
   def p = to_str(prefix)
   if p.len <= 0 {
      _counter_values = dict(64)
      return nil
   }
   def keys = _counter_values.keys()
   mut i = 0
   while i < keys.len {
      def k = to_str(keys.get(i, ""))
      if str.startswith(k, p) { _counter_values.remove(k) }
      i += 1
   }
}

fn counter_add(str name, f64 value=1.0) f64 {
   "Adds to a named floating-point counter and returns the new value."
   def k, v = _key(name), float(_counter_values.get(k, 0.0)) + float(value)
   _counter_values[k] = v
   v
}

fn counter_value(str name) f64 {
   "Returns a named floating-point counter value."
   float(_counter_values.get(_key(name), 0.0))
}

fn counter_json_fields(str prefix, list names) str {
   "Formats selected counters as JSON fields."
   mut out, i = "", 0
   while i < names.len {
      def n = to_str(names.get(i, ""))
      if n.len > 0 {
         out = out + ",\"" + to_str(prefix) + n + "\":" + to_str(counter_value(n))
      }
      i += 1
   }
   out
}

fn frame_reset() any {
   "Resets accumulated frame timing samples."
   _frame_samples = 0
   _frame_update_ms, _frame_draw_ms, _frame_world_ms, _frame_ui_ms = 0.0, 0.0, 0.0, 0.0
   _frame_frame_ms, _frame_event_ms, _frame_gui_prep_ms, _frame_sim_ms = 0.0, 0.0, 0.0, 0.0
}

fn frame_record(f64 update_ms=0.0,
   f64 draw_ms=0.0,
   f64 world_ms=0.0,
   f64 ui_ms=0.0,
   f64 frame_ms=0.0,
   f64 event_ms=0.0,
   f64 gui_prep_ms=0.0,
   f64 sim_ms=0.0) int {
   "Accumulates a UI frame timing sample and returns sample count."
   _frame_samples += 1
   _frame_update_ms = _frame_update_ms + float(update_ms)
   _frame_draw_ms = _frame_draw_ms + float(draw_ms)
   _frame_world_ms = _frame_world_ms + float(world_ms)
   _frame_ui_ms = _frame_ui_ms + float(ui_ms)
   _frame_frame_ms = _frame_frame_ms + float(frame_ms)
   _frame_event_ms = _frame_event_ms + float(event_ms)
   _frame_gui_prep_ms = _frame_gui_prep_ms + float(gui_prep_ms)
   _frame_sim_ms = _frame_sim_ms + float(sim_ms)
   _frame_samples
}

fn frame_samples() int {
   "Returns accumulated frame timing sample count."
   _frame_samples
}

fn _frame_total(str name) f64 {
   def k = to_str(name)
   if k == "update_ms" { return _frame_update_ms }
   if k == "draw_ms" { return _frame_draw_ms }
   if k == "world_ms" { return _frame_world_ms }
   if k == "ui_ms" { return _frame_ui_ms }
   if k == "frame_ms" { return _frame_frame_ms }
   if k == "event_ms" { return _frame_event_ms }
   if k == "gui_prep_ms" { return _frame_gui_prep_ms }
   if k == "sim_ms" { return _frame_sim_ms }
   0.0
}

fn frame_avg(str name) f64 {
   "Returns an average value from accumulated frame timing samples."
   if _frame_samples <= 0 { return 0.0 }
   _frame_total(name) / float(_frame_samples)
}

fn frame_fps(str name="frame_ms") f64 {
   "Returns FPS derived from an accumulated frame-millisecond field."
   def total = _frame_total(name)
   if _frame_samples <= 0 || total <= 0.0001 { return 0.0 }
   (1000.0 * float(_frame_samples)) / total
}

fn trace_process_enabled() bool {
   "Returns true when NY_TRACE should emit in-process RSS/CPU samples."
   ;; Explicit NY_TRACE_PROC=0 must win.  Do not let generic NY_TRACE/UI_TRACE
   ;; silently re-enable the heavy /proc sampler in perf/debug runs.
   if env_present_cached("NY_TRACE_PROC") { return env_truthy_cached("NY_TRACE_PROC") }
   if _ny_trace_perf() { return false }
   def mode = _ny_trace_mode()
   mode == "deep" || mode == "full" || mode == "spam" || env_truthy_cached("NY_UI_PROFILE_TRACE")
}

fn _proc_parse_first_int(str line) int {
   mut i = 0
   while i < line.len {
      def c = load8(line, i)
      if c >= 48 && c <= 57 { break }
      i += 1
   }
   mut v = 0
   while i < line.len {
      def c = load8(line, i)
      if c < 48 || c > 57 { break }
      v = v * 10 + (c - 48)
      i += 1
   }
   v
}

fn _proc_status_value(list lines, str key) int {
   mut i = 0
   while i < lines.len {
      def line = to_str(lines.get(i, ""))
      if str.startswith(line, key) { return _proc_parse_first_int(line) }
      i += 1
   }
   0
}

fn _proc_status_lines() list {
   match file_read("/proc/self/status") {
      ok(v) -> str.split(to_str(v), "\n")
      err(_) -> list(0)
   }
}

fn _proc_cpu_ticks() int {
   match file_read("/proc/self/stat") {
      ok(v) -> {
         def raw = to_str(v)
         def rp = str.find_last(raw, ")")
         if rp < 0 { return 0 }
         def rest = str.str_slice(raw, rp + 2, raw.len)
         def parts = str.split(rest, " ")
         if parts.len <= 12 { return 0 }
         int(parts.get(11, "0")) + int(parts.get(12, "0"))
      }
      err(_) -> 0
   }
}

fn _proc_fmt_kib(int kib) str {
   def mb = float(kib) / 1024.0
   if mb >= 1024.0 { return to_str(mb / 1024.0) + "G" }
   to_str(int(mb)) + "M"
}

fn _proc_smaps_extra(bool enabled) str {
   if !enabled { return "" }
   match file_read("/proc/self/smaps_rollup") {
      ok(v) -> {
         def lines = str.split(to_str(v), "\n")
         def pss = _proc_status_value(lines, "Pss:")
         def pc = _proc_status_value(lines, "Private_Clean:")
         def pd = _proc_status_value(lines, "Private_Dirty:")
         " pss=" + _proc_fmt_kib(pss) + " priv=" + _proc_fmt_kib(pc + pd)
      }
      err(_) -> ""
   }
}

fn trace_process_sample(int frame=0, str label="") bool {
   "Emits a bounded in-process RSS/CPU sample under NY_TRACE.  It is gated before any /proc read or string building."
   if !trace_process_enabled() { return false }
   mut every_frames = env_int_cached("NY_TRACE_PROC_EVERY_FRAMES", 120, 1, 1000000)
   ;; If the user did not explicitly set a cadence, keep NY_TRACE=1 clean.
   ;; This avoids the proc sampler becoming the allocation/perf problem it is
   ;; trying to diagnose.
   if !common.env_present("NY_TRACE_PROC_EVERY_FRAMES") {
      def tm = _ny_trace_mode()
      if tm == "spam" || tm == "3" { every_frames = 1 }
      elif tm == "deep" || tm == "2" || tm == "full" || tm == "verbose" { every_frames = 30 }
      else { every_frames = 120 }
   }
   ;; Robust frame throttle.  Use a next-frame threshold, not modulo/division.
   ;; It is stable across lowering paths and rejects before any /proc read.
   if frame > 2 && every_frames > 1 {
      if _proc_trace_next_frame > 0 && frame < _proc_trace_next_frame { return false }
      _proc_trace_next_frame = frame + every_frames
   }
   def every_ms = env_int_cached("NY_TRACE_PROC_EVERY_MS", 1000, 50, 60000)
   def now_ns = __ticks_ns()
   if frame <= 0 && _proc_trace_last_ns > 0 && now_ns - _proc_trace_last_ns < every_ms * 1000000 { return false }
   def lines = _proc_status_lines()
   if lines.len <= 0 { return false }
   def rss = _proc_status_value(lines, "VmRSS:")
   def vsz = _proc_status_value(lines, "VmSize:")
   def anon = _proc_status_value(lines, "RssAnon:")
   def file = _proc_status_value(lines, "RssFile:") + _proc_status_value(lines, "RssShmem:")
   def data = _proc_status_value(lines, "VmData:")
   def threads = _proc_status_value(lines, "Threads:")
   if rss > _proc_trace_peak_rss { _proc_trace_peak_rss = rss }
   mut delta = 0
   if _proc_trace_last_rss > 0 { delta = rss - _proc_trace_last_rss }
   def cpu_ticks = _proc_cpu_ticks()
   mut cpu_pct = 0.0
   if _proc_trace_last_ns > 0 && _proc_trace_last_cpu_ticks > 0 && cpu_ticks >= _proc_trace_last_cpu_ticks {
      def dt_ns = now_ns - _proc_trace_last_ns
      if dt_ns > 0 {
         ;; Linux USER_HZ is normally 100.  This is a debug estimate only.
         cpu_pct = (float(cpu_ticks - _proc_trace_last_cpu_ticks) / 100.0) * 100000000000.0 / float(dt_ns)
      }
   }
   _proc_trace_last_ns = now_ns
   _proc_trace_last_rss = rss
   _proc_trace_last_cpu_ticks = cpu_ticks
   if frame > 0 { _proc_trace_last_frame = frame }
   def smaps = _proc_smaps_extra(env_truthy_cached("NY_TRACE_PROC_SMAPS"))
   ;; Keep this one line small: it often runs while stdout is redirected.
   print_text("[proc] f=" + to_str(frame) + " rss=" + _proc_fmt_kib(rss) +
      " d=" + _proc_fmt_kib(delta) + " peak=" + _proc_fmt_kib(_proc_trace_peak_rss) +
      " vsz=" + _proc_fmt_kib(vsz) + " anon=" + _proc_fmt_kib(anon) +
      " file=" + _proc_fmt_kib(file) + " data=" + _proc_fmt_kib(data) +
   " thr=" + to_str(threads) + " tag=" + to_str(label) + smaps)
   true
}

mut _suite_exit_mode = -1
mut _snapshot_once_seen = dict(32)

fn save(str filename, any buf, int w, int h, str format="auto") any {
   "Saves a raw RGBA buffer to an image file."
   img_mod.save({"width": w, "height": h, "data": buf, "channels": 4}, filename, format)
}

fn _text(any value) str { is_str(value) ? value : "" }

fn _is_ascii_space(int c) bool { c == 32 || c == 9 || c == 10 || c == 13 || c == 11 || c == 12 }

fn _strip_ascii(str value) str {
   def n = str.len(value)
   mut lo = 0
   mut hi = n
   while lo < hi && _is_ascii_space(load8(value, lo)) { lo += 1 }
   while hi > lo && _is_ascii_space(load8(value, hi - 1)) { hi -= 1 }
   if lo == 0 && hi == n { return value }
   str.str_slice(value, lo, hi)
}

fn safe_name(any name) str {
   "Sanitizes a value for use as a dump artifact name."
   mut out = _text(name)
   out = str.str_replace(out, "/", "_")
   out = str.str_replace(out, "\\", "_")
   str.str_replace(out, ":", "_")
}

fn root_dir(any cli_dump_dir="") str {
   "Returns the root directory used for UI dump artifacts."
   def cli = to_str(cli_dump_dir)
   if cli.len > 0 { return cli }
   def env_dir = common.env_trim("NY_UI_DUMP_DIR")
   env_dir.len > 0 ? env_dir : path.join(path.cache_dir(), "probe/fb")
}

fn path_named(any name, any cli_dump_dir="") str {
   "Builds a dump path for a named artifact."
   root_dir(cli_dump_dir) + "/" + safe_name(name)
}

fn _snapshot_ext() str {
   def raw = str.lower(str.strip(common.env_trim("NY_UI_BATCH_DUMP_EXT")))
   case raw {
      "tga", ".tga" -> ".tga"
      "png", ".png" -> ".png"
      _ -> ".png"
   }
}

fn snapshot_path(any model_name, any batch_dir="", any cli_dump_dir="") str {
   "Builds the output path for a model snapshot."
   def base_dir = to_str(batch_dir).len > 0 ? to_str(batch_dir) : root_dir(cli_dump_dir)
   base_dir + "/" + safe_name(model_name) + _snapshot_ext()
}

fn parse_skip_list(any raw) list {
   "Parses a comma- or pipe-separated model skip list."
   mut out = []
   def normalized = str.str_replace(to_str(raw), "|", ",")
   def parts = str.split(normalized, ",")
   mut i = 0
   while i < parts.len {
      def item = str.strip(to_str(parts.get(i, "")))
      if item.len > 0 { out = out.append(item) }
      i += 1
   }
   out
}

fn split_model_list(any raw) list { str.split(str.str_replace(to_str(raw), ",", "|"), "|") }

fn model_skipped(any name, any skip_models) bool {
   "Reports whether a model name is present in the skip list."
   def target = str.lower(str.strip(to_str(name)))
   if target.len == 0 { return false }
   mut i = 0
   def n = is_list(skip_models) ? skip_models.len : 0
   while i < n {
      if str.lower(str.strip(to_str(skip_models.get(i, "")))) == target { return true }
      i += 1
   }
   false
}

fn suite_field(any spec, any idx, any fallback="") str {
   "Reads a normalized field from a dump suite row."
   if !is_list(spec) || int(idx) < 0 || int(idx) >= spec.len { return to_str(fallback) }
   to_str(spec.get(int(idx), fallback))
}

fn suite_parse_line(any line) list {
   "Parses one dump suite manifest line."
   def item = _strip_ascii(_text(line))
   if str.len(item) == 0 || str.startswith(item, "#") { return [] }
   def cols = str.split(item, "\t")
   if cols.len <= 0 { return [] }
   def filename = safe_name(_strip_ascii(_text(cols.get(0, ""))))
   if str.len(filename) == 0 { return [] }
   [
      filename,
      safe_name(str.lower(_strip_ascii(_text(cols.get(1, ""))))),
      str.lower(_strip_ascii(_text(cols.get(2, "")))),
      _strip_ascii(_text(cols.get(3, ""))),
      str.lower(_strip_ascii(_text(cols.get(4, "gui")))),
      str.lower(_strip_ascii(_text(cols.get(5, "")))),
      _strip_ascii(_text(cols.get(6, "")))
   ]
}

fn suite_snapshot_path(any specs, any index, any suite_dir="", any cli_dump_dir="") str {
   "Builds the snapshot path for a dump suite entry."
   if !is_list(specs) || int(index) < 0 || int(index) >= specs.len { return "" }
   def spec = specs.get(int(index), [])
   def base_dir = to_str(suite_dir).len > 0 ? to_str(suite_dir) : root_dir(cli_dump_dir)
   base_dir + "/" + safe_name(suite_field(spec, 0, "gui_dump.png"))
}

fn auto_dump_enabled() bool { common.env_truthy("NYTRIX_AUTO_DUMP") }

fn auto_dump_path(str fallback) str {
   "Returns the auto-dump output path."
   def env_path = common.env_trim("NYTRIX_AUTO_DUMP_PATH")
   env_path.len > 0 ? env_path : fallback
}

fn snapshot_once(any win, str out_path) bool {
   "Writes `out_path` once for a window id while auto-dump is enabled."
   if !auto_dump_enabled() { return false }
   def key = to_str(window.id(win)) + ":" + to_str(out_path)
   if _snapshot_once_seen.get(key, false) { return false }
   snapshot(out_path)
   _snapshot_once_seen[key] = true
   true
}

fn close_with_dump(any win, str dump_path="build/release/fb_dump.png") any {
   "Optionally snapshots the framebuffer, then requests window close."
   snapshot_once(win, auto_dump_path(dump_path))
   window.set_should_close(win, true)
}

fn auto_dump_delay_frames(int fallback=8) int {
   "Returns the configured auto-dump frame delay."
   common.env_int_clamped("NYTRIX_AUTO_DUMP_DELAY_FRAMES", fallback, 0, 1000000)
}

fn auto_dump_pre_frame(bool done, int frame_count, int delay_frames=8) bool {
   "Requests framebuffer capture before the target auto-dump frame."
   mut effective_delay = delay_frames
   if common.env_truthy("NYTRIX_AUTO_DUMP_EXIT") && effective_delay < 2 { effective_delay = 2 }
   if !auto_dump_enabled() || done || frame_count + 1 < effective_delay { return false }
   request_frame_capture()
   true
}

fn auto_dump_post_frame(any win, bool done, int frame_count, int delay_frames=8, str dump_path="build/release/fb_dump.png") bool {
   "Writes a requested framebuffer dump after the target frame."
   def exit_after_dump = common.env_truthy("NYTRIX_AUTO_DUMP_EXIT")
   mut effective_delay = delay_frames
   if exit_after_dump && effective_delay < 2 { effective_delay = 2 }
   if !auto_dump_enabled() || done || frame_count < effective_delay { return done }
   if !snapshot(auto_dump_path(dump_path)) {
      if exit_after_dump { window.set_should_close(win, true) }
      return false
   }
   if exit_after_dump { window.set_should_close(win, true) }
   true
}

fn gui_auto_dump_path(any cli_path="") str {
   "Returns the configured GUI auto-dump path."
   def cli = to_str(cli_path)
   if cli.len > 0 { return cli }
   env_trim_cached("NY_UI_GUI_AUTO_DUMP")
}

fn gui_auto_dump_delay() int {
   "Returns the configured GUI auto-dump frame delay."
   env_int_cached("NY_UI_GUI_AUTO_DUMP_DELAY_FRAMES", 6, 1, 1000000)
}

fn gui_auto_dump_exit_enabled() bool {
   "Returns whether GUI auto-dump should close the window after capture."
   env_truthy_cached("NY_UI_GUI_AUTO_DUMP_EXIT")
}

fn suite_parse_text(any txt) list {
   "Parses dump suite rows from text."
   if !is_str(txt) { return [] }
   mut out = []
   mut line = ""
   mut i = 0
   def n = str.len(txt)
   while i < n {
      def c = load8(txt, i)
      if c == 10 {
         def spec = suite_parse_line(line)
         if spec.len > 0 { out = out.append(spec) }
         line = ""
      } elif c != 13 {
         line += str.chr(c)
      }
      i += 1
   }
   if str.len(line) > 0 {
      def spec = suite_parse_line(line)
      if spec.len > 0 { out = out.append(spec) }
   }
   out
}

fn suite_parse_env(any cli_dump_dir="") dict {
   "Parses dump suite configuration from environment variables."
   mut specs = []
   def suite_file = env_trim_cached("NY_UI_GUI_DUMP_SUITE_FILE")
   def suite_list = env_trim_cached("NY_UI_GUI_DUMP_SUITE_LIST")
   if suite_file.len > 0 {
      match file_read(suite_file) {
         ok(txt) -> { specs = suite_parse_text(txt) }
         err(_) -> { print_line("ui:gui-suite:warn", "failed to read spec=" + suite_file) }
      }
   }
   if specs.len == 0 && suite_list.len > 0 {
      specs = suite_parse_text(suite_list)
   }
   mut out_dir = env_trim_cached("NY_UI_GUI_DUMP_SUITE_DIR")
   if out_dir.len == 0 { out_dir = root_dir(cli_dump_dir) }
   mut settle = 4
   if env_present_cached("NY_UI_GUI_DUMP_SUITE_SETTLE_FRAMES") {
      settle = env_int_cached("NY_UI_GUI_DUMP_SUITE_SETTLE_FRAMES", 4, 0, 1000000)
   }
   {
      "active": specs.len > 0,
      "specs": specs,
      "dir": out_dir,
      "settle_frames": settle
   }
}

fn suite_exit_enabled() bool {
   "Returns whether dump-suite completion should close the window."
   if _suite_exit_mode != -1 { return _suite_exit_mode == 1 }
   if env_present_cached("NY_UI_GUI_DUMP_SUITE_EXIT") {
      _suite_exit_mode = env_truthy_cached("NY_UI_GUI_DUMP_SUITE_EXIT") ? 1 : 0
   }
   else { _suite_exit_mode = 0 }
   _suite_exit_mode == 1
}

fn gizmo_mode(any name, any fallback) int {
   "Parses a gizmo mode name into its numeric mode."
   def s = str.lower(str.strip(to_str(name)))
   case s {
      "1", "rotate", "rot", "r" -> 1
      "2", "scale", "s" -> 2
      "0", "move", "translate", "g", "w" -> 0
      _ -> int(fallback)
   }
}

fn framebuffer_hash_line() str {
   "Returns a stable hash line for the current framebuffer."
   if _last_framebuffer_hash_line.len > 0 {
      def cached = _last_framebuffer_hash_line
      _last_framebuffer_hash_line = ""
      return cached
   }
   def fb = read_framebuffer()
   if !is_dict(fb) { return "" }
   def data = fb.get("data", 0)
   def fb_w = int(fb.get("width", 0))
   def fb_h = int(fb.get("height", 0))
   def fb_bpp = int(fb.get("bpp", 4))
   def fb_len = fb_w * fb_h * fb_bpp
   if data == 0 || fb_len <= 0 {
      if data { free(data) }
      return ""
   }
   def fb_bytes = bytes(fb_len)
   if !fb_bytes {
      if data { free(data) }
      return ""
   }
   memcpy(fb_bytes, data, fb_len)
   def line = "FB_HASH: " + str.to_hex(hash.xxh32(fb_bytes, 0, 0, fb_len))
   if data { free(data) }
   line
}

fn set_framebuffer_hash_line(any line) bool {
   "Stores a one-shot framebuffer hash line from a completed snapshot."
   _last_framebuffer_hash_line = to_str(line)
   true
}

#main {
   assert(force_frame_print_every(7) == 7 && frame_print_every() == 7, "ui profile frame print override")
   assert(generated_env_width(true, true) <= 128 && generated_env_width(false, false) >= 64, "ui profile generated env width")
   frame_reset()
   assert(frame_record(1.0, 2.0, 3.0, 4.0) == 1 && frame_avg("draw_ms") == 2.0, "ui profile frame averages")
   counter_reset()
   assert(counter_add("draws", 2.0) == 2.0 && counter_value("draws") == 2.0, "ui profile counters")
   assert(safe_name("a/b\\c:d") == "a_b_c_d", "dump safe name")
   assert(safe_name("Unicode❤♻Test") == "Unicode❤♻Test", "dump safe name utf8")
   assert(root_dir("tmp/out") == "tmp/out" && path_named("a/b", "tmp/out") == "tmp/out/a_b", "dump root and named path")
   assert(snapshot_path("Box/Model", "", "tmp/out") == "tmp/out/Box_Model.png", "dump snapshot path")
   assert(auto_dump_path("fallback.png").len > 0, "dump auto path")
   assert(parse_skip_list("Fox|Cube, Duck") == ["Fox", "Cube", "Duck"], "dump skip list")
   assert(split_model_list("Fox,Cube|Duck") == ["Fox", "Cube", "Duck"], "dump model list")
   assert(model_skipped("cube", ["Fox", "Cube"]) && !model_skipped("", ["Fox"]), "dump model skip")
   def spec = ["gui dump.png", "Gallery", "Theme", "note", "GUI", "focus", "extra"]
   assert(suite_field(spec, 1, "") == "Gallery" && suite_field(spec, 99, "fallback") == "fallback", "dump suite fields")
   def parsed = suite_parse_line("gui dump.png\tGallery\tTheme\tnote\tGUI\tfocus\textra")
   assert(parsed.get(0, "") == "gui dump.png" && parsed.get(1, "") == "gallery" && parsed.get(4, "") == "gui", "dump suite parse")
   assert(suite_parse_line("# comment").len == 0, "dump suite comment")
   assert(suite_snapshot_path([spec], 0, "tmp/suite") == "tmp/suite/gui dump.png" && suite_snapshot_path([spec], 10, "tmp/suite") == "", "dump suite snapshot")
   assert(auto_dump_delay_frames(3) >= 0, "dump auto delay")
   assert(suite_parse_text("shot.png\tpanel\tstate\tnote\tgui\n# skip\n").len == 1, "dump suite parse text")
   assert(gizmo_mode("rotate", 0) == 1 && gizmo_mode("S", 0) == 2 && gizmo_mode("w", 2) == 0 && gizmo_mode("unknown", 7) == 7, "dump gizmo modes")
   assert(is_str(framebuffer_hash_line()), "dump framebuffer hash")
   print("✓ std.os.ui.render.dump self-test passed")
}
