;; Keywords: profile profiling tracing
;; UI profiling, tracing, environment toggles, and benchmark logging.
module std.os.ui.profile(
   env_truthy_cached, env_present_cached, env_enabled_cached, env_toggle_cached, env_int_cached,
   env_trim_cached, env_lower_cached, set_bool, reset_env_cache, debug_enabled, debug_verbose_enabled,
   debug_deep_enabled, gfx_frame_trace_enabled, enabled, trace_enabled, deep_enabled, gui_trace_enabled,
   dump_trace_enabled, text_immediate_enabled, text_group_batches_enabled, min_ms, now, elapsed_ns,
   elapsed_ms, ms_between, elapsed_s, format_tag, format_line, colorize_line, print_text, eprint_text,
   print_line, eprint_line, log, mark, mark_next, mark_done, trace, dump_enabled, dump_path, append_line,
   event_trace_enabled, event_trace, stage_enabled, stage_ms_since, stage_log, profile_dump_force,
   profile_dump_set_path, profile_dump_enabled, profile_dump_file, profile_dump_row, bench_force,
   bench_dump_set_path, bench_dump_file, bench_enabled, bench_reset, bench_record, bench_flush,
   counter_reset, counter_add, counter_value, counter_json_fields, frame_reset, frame_record,
   frame_samples, frame_avg, frame_fps
)

use std.core
use std.core.error
use std.core.str
use std.core.common as common
use std.core.term as term
use std.os.path as ospath
use std.os.sys as ossys

mut _env_bool_cache, _env_int_cache, _env_str_cache = dict(64), dict(32), dict(32)
mut _dump_path_cache, _counter_values = dict(16), dict(64)
mut _profile_dump_mode, _bench_mode = -1, -1
mut _profile_dump_path, _bench_dump_path = "", ""
mut _bench_frames, _bench_begin_ns, _bench_draw_ns, _bench_end_ns = 0, 0, 0, 0
mut _frame_samples = 0
mut _frame_update_ms, _frame_draw_ms, _frame_world_ms, _frame_ui_ms = 0.0, 0.0, 0.0, 0.0
mut _frame_frame_ms, _frame_event_ms, _frame_gui_prep_ms, _frame_sim_ms = 0.0, 0.0, 0.0, 0.0
mut _min_ms_cache = -1

fn _key(any: name): str { to_str(name) }

fn _bool_cache(): dict {
   if(!is_dict(_env_bool_cache)){ _env_bool_cache = dict(64) }
   _env_bool_cache
}

fn _int_cache(): dict {
   if(!is_dict(_env_int_cache)){ _env_int_cache = dict(32) }
   _env_int_cache
}

fn _str_cache(): dict {
   if(!is_dict(_env_str_cache)){ _env_str_cache = dict(32) }
   _env_str_cache
}

fn reset_env_cache(): any {
   "Clears cached UI profile environment flags."
   _env_bool_cache, _env_int_cache, _env_str_cache = dict(64), dict(32), dict(32)
   _dump_path_cache = dict(16)
   _min_ms_cache = -1
}

fn set_bool(any: name, any: value): bool {
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

fn env_truthy_cached(any: name): bool {
   "Returns a cached truthy environment flag."
   def k = _key(name)
   _bool_cache()
   if(_env_bool_cache.contains(k)){ return bool(_env_bool_cache.get(k, false)) }
   def v = common.env_truthy(k)
   _env_bool_cache[k] = v
   v
}

fn env_present_cached(any: name): bool {
   "Returns a cached environment-presence flag."
   def k = "present:" + _key(name)
   _bool_cache()
   if(_env_bool_cache.contains(k)){ return bool(_env_bool_cache.get(k, false)) }
   def v = common.env_present(_key(name))
   _env_bool_cache[k] = v
   v
}

fn env_enabled_cached(any: name): bool {
   "Returns a cached enabled environment flag."
   def k = "enabled:" + _key(name)
   _bool_cache()
   if(_env_bool_cache.contains(k)){ return bool(_env_bool_cache.get(k, false)) }
   def v = common.env_enabled(_key(name))
   _env_bool_cache[k] = v
   v
}

fn env_toggle_cached(any: name, bool: default_value=false): bool {
   "Returns a cached toggle environment flag with a default when unset."
   def k = "toggle:" + _key(name) + ":" + to_str(bool(default_value))
   _bool_cache()
   if(_env_bool_cache.contains(k)){ return bool(_env_bool_cache.get(k, false)) }
   def v = common.env_toggle(_key(name), bool(default_value))
   _env_bool_cache[k] = v
   v
}

fn env_int_cached(any: name, int: default_value=0, int: min_value=-2147483648, int: max_value=2147483647): int {
   "Returns a cached clamped integer environment value."
   def k = "int:" + _key(name) + ":" + to_str(int(default_value)) + ":" + to_str(int(min_value)) + ":" + to_str(int(max_value))
   _int_cache()
   if(_env_int_cache.contains(k)){ return int(_env_int_cache.get(k, default_value)) }
   def v = common.env_int_clamped(_key(name), int(default_value), int(min_value), int(max_value))
   _env_int_cache[k] = v
   v
}

fn env_trim_cached(any: name): str {
   "Returns a cached trimmed environment value."
   def k = "trim:" + _key(name)
   _str_cache()
   if(_env_str_cache.contains(k)){ return to_str(_env_str_cache.get(k, "")) }
   def v = common.env_trim(_key(name))
   _env_str_cache[k] = v
   v
}

fn env_lower_cached(any: name): str {
   "Returns a cached lower-cased environment value."
   def k = "lower:" + _key(name)
   _str_cache()
   if(_env_str_cache.contains(k)){ return to_str(_env_str_cache.get(k, "")) }
   def v = common.env_lower(_key(name))
   _env_str_cache[k] = v
   v
}

fn debug_enabled(): bool {
   "Returns true when NY_DEBUG is enabled."
   env_truthy_cached("NY_DEBUG")
}

fn debug_verbose_enabled(): bool {
   "Returns true when NY_DEBUG_VERBOSE is enabled."
   env_truthy_cached("NY_DEBUG_VERBOSE")
}

fn debug_deep_enabled(): bool {
   "Returns true when NY_DEBUG_DEEP is enabled."
   env_truthy_cached("NY_DEBUG_DEEP")
}

fn gfx_frame_trace_enabled(): bool {
   "Returns true when graphics frame tracing is enabled."
   env_truthy_cached("NY_GFX_FRAME_TRACE")
}

fn enabled(): bool {
   "Returns true when general GUI profiling is enabled."
   env_truthy_cached("NY_GUI_PROFILE") || env_truthy_cached("NY_UI_PROFILE")
}

fn trace_enabled(): bool {
   "Returns true when UI trace/profile trace output is enabled."
   env_truthy_cached("NY_UI_PROFILE_TRACE") || env_truthy_cached("NY_UI_TRACE")
}

fn deep_enabled(): bool {
   "Returns true when verbose UI profiling is enabled."
   env_truthy_cached("NY_UI_PROFILE_DEEP")
}

fn gui_trace_enabled(): bool {
   "Returns true when GUI trace output is enabled."
   env_truthy_cached("NY_UI_GUI_TRACE")
}

fn dump_trace_enabled(): bool {
   "Returns true when GUI dump tracing is enabled."
   env_truthy_cached("NY_UI_GUI_DUMP_TRACE")
}

fn text_immediate_enabled(): bool {
   "Returns true when text should bypass queued batching for diagnosis."
   env_truthy_cached("NY_GUI_TEXT_IMMEDIATE") || env_truthy_cached("NY_UI_TEXT_IMMEDIATE")
}

fn text_group_batches_enabled(): bool {
   "Returns true when queued text runs may be grouped by font/color."
   env_toggle_cached("NY_GUI_TEXT_GROUP_BATCH", true) && env_toggle_cached("NY_UI_TEXT_GROUP_BATCH", true)
}

fn min_ms(): f64 {
   "Returns the minimum profile mark duration to print, in milliseconds."
   if(_min_ms_cache < 0){
      _min_ms_cache = env_int_cached("NY_GUI_PROFILE_MIN_MS", 1, 0, 1000000)
   }
   float(_min_ms_cache)
}

fn now(): int {
   "Returns monotonic nanoseconds."
   __ticks_ns()
}

fn elapsed_ns(int: t0): int {
   "Returns nanoseconds elapsed since `t0`."
   __ticks_ns() - int(t0)
}

fn elapsed_ms(int: t0): f64 {
   "Returns milliseconds elapsed since `t0`."
   float(__ticks_ns() - int(t0)) / 1e6
}

fn ms_between(int: t1, int: t0): f64 {
   "Returns milliseconds between two monotonic timestamps."
   float(int(t1) - int(t0)) / 1e6
}

fn elapsed_s(int: t0): f64 {
   "Returns seconds elapsed since `t0`."
   float(__ticks_ns() - int(t0)) / 1e9
}

fn stage_ms_since(int: t0): f64 {
   "Returns milliseconds elapsed since `t0`, or zero for inactive timestamps."
   if(int(t0) <= 0){ return 0.0 }
   float(__ticks_ns() - int(t0)) / 1e6
}

fn format_tag(any: tag): str {
   "Formats a semantic colored bracket tag for terminal logs."
   term.log_tag(tag)
}

fn colorize_line(any: line): str {
   "Applies semantic colors to bracketed tags in a log line."
   term.log_text(line)
}

fn format_line(any: tag, any: msg=""): str {
   "Formats a semantic `[tag] message` line."
   format_tag(tag) + (to_str(msg).len > 0 ? (" " + to_str(msg)) : "")
}

fn print_text(any: line): bool {
   "Prints a log line with colored bracket tags."
   print(colorize_line(line))
   true
}

fn eprint_text(any: line): bool {
   "Prints a log line to stderr with colored bracket tags."
   eprint(colorize_line(line))
   true
}

fn print_line(any: tag, any: msg=""): bool {
   "Prints a semantic colored `[tag] message` line."
   print(format_line(tag, msg))
   true
}

fn eprint_line(any: tag, any: msg=""): bool {
   "Prints a semantic colored `[tag] message` line to stderr."
   eprint(format_line(tag, msg))
   true
}

fn log(any: label, any: ms, any: detail=""): bool {
   "Prints a thresholded GUI profile sample."
   if(!enabled()){ return false }
   def v = float(ms)
   if(v < min_ms()){ return false }
   print_line("gui:prof", to_str(label) + "=" + to_str(v) + "ms" + to_str(detail))
   true
}

fn mark(any: label, int: t0, any: detail=""): bool {
   "Prints a thresholded GUI profile sample from a start timestamp."
   if(!enabled()){ return false }
   def start = int(t0)
   if(start <= 0){ return false }
   log(label, float(__ticks_ns() - start) / 1e6, detail)
}

fn mark_next(bool: on, any: label, int: t0, any: detail=""): int {
   "Marks a profile sample and returns the next checkpoint timestamp."
   if(on){
      mark(label, t0, detail)
      def next_t = __ticks_ns()
      return next_t
   }
   int(t0)
}

fn mark_done(bool: on, any: label, int: t0, any: detail=""): bool {
   "Marks a terminal profile sample when enabled."
   if(!on){ return false }
   mark(label, t0, detail)
   true
}

fn trace(any: label, any: detail=""): bool {
   "Prints a low-volume UI trace line."
   if(!trace_enabled() && !enabled()){ return false }
   print_line("ui:trace", to_str(label) + to_str(detail))
   true
}

fn event_trace_enabled(): bool {
   "Returns true when raw UI event tracing is enabled."
   env_truthy_cached("NY_UI_EVENT_TRACE")
}

fn event_trace(int: typ, any: data, any: tag="event", bool: close_state=false): bool {
   "Prints one normalized UI event trace line."
   if(!event_trace_enabled()){ return false }
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

fn stage_enabled(): bool {
   "Returns true when batch/scene stage profiling is enabled."
   env_truthy_cached("NY_UI_BATCH_STAGE_TRACE") ||
   env_truthy_cached("NY_SCENE_PROFILE_TRACE") ||
   trace_enabled()
}

fn stage_log(any: prefix, any: model_name, any: stage, int: t0): bool {
   "Prints a batch/scene stage timing line."
   if(!stage_enabled()){ return false }
   print_line(prefix, "model=" + to_str(model_name) +
   " stage=" + to_str(stage) + " ms=" + to_str(stage_ms_since(t0)))
   true
}

fn dump_enabled(str: flag_env, bool: default_on=false): bool {
   "Returns a cached dump toggle."
   if(env_present_cached(flag_env)){
      return env_enabled_cached(flag_env)
   }
   bool(default_on)
}

fn dump_path(str: path_env, str: fallback_name): str {
   "Returns a cached dump path, defaulting to the OS temp directory."
   def k = _key(path_env) + ":" + _key(fallback_name)
   if(_dump_path_cache.contains(k)){ return to_str(_dump_path_cache.get(k, "")) }
   def raw = env_trim_cached(path_env)
   def p = (raw.len > 0) ? raw : ospath.join(ospath.temp_dir(), _key(fallback_name))
   _dump_path_cache[k] = p
   p
}

fn append_line(str: path, str: line): bool {
   "Appends a single line to a profiling/debug dump."
   def p = ospath.normalize(to_str(path))
   if(p.len <= 0){ return false }
   def s = to_str(line)
   def buf = s + ((s.len > 0 && load8(s, s.len - 1) == 10) ? "" : "\n")
   def open_res = ossys.sys_open(p, 1089, 420) ; WRONLY|CREAT|APPEND, 0644
   if(is_err(open_res)){ return false }
   def fd = unwrap(open_res)
   mut off = 0
   while(off < buf.len){
      def w = __write_off(fd, buf, buf.len - off, off)
      if(w <= 0){
         ossys.sys_close_quiet(fd)
         return false
      }
      off += w
   }
   ossys.sys_close_quiet(fd)
   true
}

fn profile_dump_force(bool: value=true): bool {
   "Forces UI profile JSONL dumps on or off for this process."
   _profile_dump_mode = bool(value) ? 1 : 0
   bool(value)
}

fn profile_dump_set_path(str: path): str {
   "Overrides the UI profile JSONL dump path for this process."
   _profile_dump_path = to_str(path)
   _profile_dump_path
}

fn profile_dump_enabled(bool: default_on=false): bool {
   "Returns whether UI frame profile JSONL dumps are enabled."
   if(_profile_dump_mode != -1){ return _profile_dump_mode == 1 }
   _profile_dump_mode = dump_enabled("NY_UI_PROFILE_DUMP", bool(default_on)) ? 1 : 0
   _profile_dump_mode == 1
}

fn profile_dump_file(): str {
   "Returns the UI frame profile JSONL dump path."
   if(_profile_dump_path.len <= 0){
      _profile_dump_path = dump_path("NY_UI_PROFILE_DUMP_PATH", "nytrix_ui_profile.oasset.jsonl")
   }
   _profile_dump_path
}

fn profile_dump_row(int: total_frame, int: frames, f64: fps_est, f64: avg_fr, f64: avg_up, f64: avg_dr, f64: avg_wo, f64: avg_ui, f64: avg_ev, f64: avg_gp, f64: avg_sm): bool {
   "Writes one UI frame profile JSONL row."
   if(!profile_dump_enabled(trace_enabled())){ return false }
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

fn bench_force(bool: value=true): bool {
   "Forces no-surface benchmark JSONL dumps on or off for this process."
   _bench_mode = bool(value) ? 1 : 0
   bool(value)
}

fn bench_dump_set_path(str: path): str {
   "Overrides the benchmark JSONL dump path for this process."
   _bench_dump_path = to_str(path)
   _bench_dump_path
}

fn bench_enabled(bool: default_on=false): bool {
   "Returns whether no-surface benchmark profiling is enabled."
   if(_bench_mode != -1){ return _bench_mode == 1 }
   if(env_present_cached("NY_UI_BENCH_PROFILE")){
      _bench_mode = env_truthy_cached("NY_UI_BENCH_PROFILE") ? 1 : 0
   } else {
      _bench_mode = bool(default_on) ? 1 : 0
   }
   _bench_mode == 1
}

fn bench_dump_file(str: profile_file=""): str {
   if(_bench_dump_path.len > 0){ return _bench_dump_path }
   def raw = env_trim_cached("NY_UI_BENCH_PROFILE_DUMP_PATH")
   if(raw.len > 0){
      _bench_dump_path = raw
   } elif(to_str(profile_file).len > 0){
      _bench_dump_path = to_str(profile_file)
   } else {
      _bench_dump_path = dump_path("NY_UI_BENCH_PROFILE_DUMP_PATH", "nytrix_bench_profile.oasset.jsonl")
   }
   _bench_dump_path
}

fn bench_reset(): any {
   "Resets accumulated no-surface benchmark timings."
   _bench_frames, _bench_begin_ns, _bench_draw_ns, _bench_end_ns = 0, 0, 0, 0
}

fn bench_record(int: begin_ns, int: draw_ns, int: end_ns): int {
   "Accumulates one no-surface benchmark timing sample."
   if(!bench_enabled(profile_dump_enabled(trace_enabled()))){ return _bench_frames }
   _bench_frames = _bench_frames + 1
   _bench_begin_ns = _bench_begin_ns + int(begin_ns)
   _bench_draw_ns = _bench_draw_ns + int(draw_ns)
   _bench_end_ns = _bench_end_ns + int(end_ns)
   _bench_frames
}

fn bench_flush(int: total_ns, dict: renderer_stats, str: profile_file=""): bool {
   "Writes one no-surface benchmark JSONL row."
   if(!bench_enabled(profile_dump_enabled(trace_enabled())) || _bench_frames <= 0){ return false }
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

fn counter_reset(str: prefix=""): any {
   "Resets all counters, or counters with `prefix` when provided."
   def p = to_str(prefix)
   if(p.len <= 0){
      _counter_values = dict(64)
      return nil
   }
   def keys = _counter_values.keys()
   mut i = 0
   while(i < keys.len){
      def k = to_str(keys.get(i, ""))
      if(startswith(k, p)){ _counter_values.remove(k) }
      i += 1
   }
}

fn counter_add(str: name, f64: value=1.0): f64 {
   "Adds to a named floating-point counter and returns the new value."
   def k, v = _key(name), float(_counter_values.get(k, 0.0)) + float(value)
   _counter_values[k] = v
   v
}

fn counter_value(str: name): f64 {
   "Returns a named floating-point counter value."
   float(_counter_values.get(_key(name), 0.0))
}

fn counter_json_fields(str: prefix, list: names): str {
   "Formats selected counters as JSON fields."
   mut out, i = "", 0
   while(i < names.len){
      def n = to_str(names.get(i, ""))
      if(n.len > 0){
         out = out + ",\"" + to_str(prefix) + n + "\":" + to_str(counter_value(n))
      }
      i += 1
   }
   out
}

fn frame_reset(): any {
   "Resets accumulated frame timing samples."
   _frame_samples = 0
   _frame_update_ms, _frame_draw_ms, _frame_world_ms, _frame_ui_ms = 0.0, 0.0, 0.0, 0.0
   _frame_frame_ms, _frame_event_ms, _frame_gui_prep_ms, _frame_sim_ms = 0.0, 0.0, 0.0, 0.0
}

fn frame_record(f64: update_ms=0.0,
   f64: draw_ms=0.0,
   f64: world_ms=0.0,
   f64: ui_ms=0.0,
   f64: frame_ms=0.0,
   f64: event_ms=0.0,
   f64: gui_prep_ms=0.0,
   f64: sim_ms=0.0): int {
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

fn frame_samples(): int { _frame_samples }

fn _frame_total(str: name): f64 {
   def k = to_str(name)
   if(k == "update_ms"){ return _frame_update_ms }
   if(k == "draw_ms"){ return _frame_draw_ms }
   if(k == "world_ms"){ return _frame_world_ms }
   if(k == "ui_ms"){ return _frame_ui_ms }
   if(k == "frame_ms"){ return _frame_frame_ms }
   if(k == "event_ms"){ return _frame_event_ms }
   if(k == "gui_prep_ms"){ return _frame_gui_prep_ms }
   if(k == "sim_ms"){ return _frame_sim_ms }
   0.0
}

fn frame_avg(str: name): f64 {
   "Returns an average value from accumulated frame timing samples."
   if(_frame_samples <= 0){ return 0.0 }
   _frame_total(name) / float(_frame_samples)
}

fn frame_fps(str: name="frame_ms"): f64 {
   "Returns FPS derived from an accumulated frame-millisecond field."
   def total = _frame_total(name)
   if(_frame_samples <= 0 || total <= 0.0001){ return 0.0 }
   (1000.0 * float(_frame_samples)) / total
}
