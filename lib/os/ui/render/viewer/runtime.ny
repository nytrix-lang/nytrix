;; Keywords: viewer runtime event-loop frame timing window resources os ui render
;; UI runtime setup, frame-loop coordination, auto-close policy, and resource loading.
;; References:
;; - std.os.ui.window
;; - std.os.ui.render.dump
module std.os.ui.render.viewer.runtime(dbg, default_font_size, default_font_filter, font_from_candidates, mono_font, ui_font_from_candidates, timeout_ns, timeout_hit, close_on_timeout, deadline_passed, fps_begin, fps_tick, fps_current, fps_finish, auto_close, auto_close_if_idle, open_fullscreen, open_windowed, handle_close_events, step, build_crosshair_mesh, load_equirect_skybox)
use std.core
use std.core.str as str
use std.os (ticks)
use std.os.fs as osfs
use std.os.path as lib_path
use std.os.ui.window.consts
use std.os.ui.render
use std.os.ui.render.dump as ui_profile
use std.os.ui.render.viewer.term as ui_term
use std.os.ui.window as window
use std.os.prim
use std.math.parse.img.exr as exr
use std.core.common as common

mut _headless_cache = -1

fn _default_override(any tag, any suffix) str {
   def key_tag = "NY_" + str.upper(to_str(tag)) + "_" + str.upper(to_str(suffix))
   def key_shared = "NY_UI_" + str.upper(to_str(suffix))
   def tag_value = common.env_trim(key_tag)
   tag_value.len > 0 ? tag_value : common.env_trim(key_shared)
}

fn _headless_enabled() bool {
   _headless_cache = common.cached_env_truthy(_headless_cache, "NY_UI_HEADLESS")
   _headless_cache == 1
}

fn _request_close(any win) any { window.set_should_close(win, true) }

fn dbg(any tag, any msg) any {
   "Prints a bracketed debug line when NY_DEBUG is enabled."
   if ui_profile.debug_enabled() {
      def txt = "[" + to_str(tag) + "] " + to_str(msg)
      ui_profile.print_text(txt)
      if ui_term.is_open() { ui_term.log(txt) }
   }
}

fn _clamp_font_size(any value, any min_size, any max_size) any {
   def min_f, max_f = min_size + 0.0, max_size + 0.0
   min(max(value + 0.0, min_f), max_f)
}

fn default_font_size(any tag, f64 fallback, str suffix="FONT_SIZE", any min_size=8.0, any max_size=96.0) any {
   "Returns a per-viewer font size, overridable through environment variables."
   def raw = _default_override(tag, suffix)
   raw.len == 0 ? fallback : _clamp_font_size(str.atof(raw), min_size, max_size)
}

fn _font_filter_mode(str mode, int fallback) int {
   case mode {
      "nearest", "point", "pixel" -> { return FONT_FILTER_NEAREST }
      "linear", "bilinear", "smooth" -> { return FONT_FILTER_LINEAR }
      _ -> fallback
   }
}

fn default_font_filter(any tag, int fallback=FONT_FILTER_DEFAULT, str suffix="FONT_FILTER") int {
   "Returns a per-viewer font filter, overridable through environment variables."
   def raw = _default_override(tag, suffix)
   raw.len == 0 ? fallback : _font_filter_mode(str.lower(str.strip(raw)), fallback)
}

fn _resolve_font_candidate(any raw) str {
   if !raw || !is_str(raw) { return "" }
   def s = str.strip(raw)
   if s.len == 0 { return "" }
   def resolved = lib_path.resolve_repo_asset(s)
   if is_str(resolved) && resolved.len > 0 { return resolved }
   s
}

fn font_from_candidates(int size, any candidates, int font_filter=-1, str pixel_name="monocraft") int {
   "Loads the first available font from an explicit candidate list."
   if !is_list(candidates) { return 0 }
   mut i = 0
   def paths_n = candidates.len
   while i < paths_n {
      def raw = candidates.get(i)
      if raw && is_str(raw) && raw.len > 0 {
         def raw_s = to_str(raw)
         def resolved = _resolve_font_candidate(raw_s)
         if resolved.len == 0 {
            i += 1
            continue
         }
         mut font = 0
         if str.find(str.lower(raw_s), str.lower(to_str(pixel_name))) >= 0 {
            font = font_load(resolved, size, 0)
         } else {
            font = font_load(resolved, size, font_filter)
         }
         if font { return font }
      }
      i += 1
   }
   0
}

fn mono_font(int size, any candidates=0, int font_filter=-1) int { font_from_candidates(size, candidates, font_filter, "monocraft") }

fn ui_font_from_candidates(int size, any candidates=0, any fallback_candidates=0, int font_filter=-1) int {
   "Loads a UI font from primary candidates, then fallback candidates."
   def primary = font_from_candidates(size, candidates, font_filter, "")
   primary ? primary : font_from_candidates(size, fallback_candidates, font_filter, "monocraft")
}

fn timeout_ns(int default_ns=0) int {
   "Returns the active timeout in nanoseconds from NY_UI_TIMEOUT, else `default_ns`."
   def env_t = common.env_trim("NY_UI_TIMEOUT")
   if env_t.len > 0 { return int(str.atof(env_t) * 1e9) }
   default_ns
}

fn timeout_hit(any start_ticks, int limit_ns) bool {
   "Returns true when a precomputed timeout limit has elapsed."
   limit_ns > 0 && ticks() - start_ticks >= limit_ns
}

fn close_on_timeout(any win, any start_ticks, int limit_ns) bool {
   "Closes `win` when a precomputed timeout limit has elapsed."
   if !timeout_hit(start_ticks, limit_ns) { return false }
   _request_close(win)
   true
}

fn deadline_passed(any start_ticks, int default_ns=0) bool {
   "Returns true once the configured timeout has elapsed since `start_ticks`."
   timeout_hit(start_ticks, timeout_ns(default_ns))
}

fn fps_begin() dict {
   "Creates a compact FPS counter for examples and viewer tools."
   def now = ticks()
   {"start": now, "last": now, "frames": 0, "total": 0, "fps": 0, "min_fps": 0, "max_fps": 0}
}

fn fps_tick(dict state, f64 dt=0.0) dict {
   "Advances an FPS counter and returns the same state object."
   def frames = int(state.get("frames", 0)) + 1
   def total = int(state.get("total", 0)) + 1
   def now = ticks()
   state["frames"] = frames
   state["total"] = total
   if now - int(state.get("last", now)) >= 1000000000 {
      state["fps"] = frames
      def min_fps = int(state.get("min_fps", 0))
      def max_fps = int(state.get("max_fps", 0))
      if frames > 0 && (min_fps == 0 || frames < min_fps) { state["min_fps"] = frames }
      if frames > max_fps { state["max_fps"] = frames }
      state["frames"] = 0
      state["last"] = now
   } elif dt > 0.00001 && int(state.get("fps", 0)) == 0 {
      state["fps"] = int(1.0 / dt)
   }
   state
}

fn fps_current(dict state, f64 dt=0.0) int {
   "Returns a stable FPS value, using delta-time until the first full-second sample."
   def sampled = int(state.get("fps", 0))
   if sampled > 0 { return sampled }
   dt > 0.00001 ? int(1.0 / dt) : 0
}

fn fps_finish(str tag, dict state) bool {
   "Prints an average FPS line when NY_UI_FPS_LOG is enabled."
   if !common.env_truthy("NY_UI_FPS_LOG") { return false }
   def elapsed = max(0.000001, float(ticks() - int(state.get("start", ticks()))) / 1000000000.0)
   def avg = int(float(int(state.get("total", 0))) / elapsed)
   ui_profile.print_text("[fps] " + tag + " avg=" + to_str(avg) + " min=" + to_str(int(state.get("min_fps", 0))) + " max=" + to_str(int(state.get("max_fps", 0))) + " frames=" + to_str(int(state.get("total", 0))))
   true
}

fn auto_close(any win, any start_ticks, int default_ns=0) bool {
   "Closes `win` once the configured timeout elapses."
   close_on_timeout(win, start_ticks, timeout_ns(default_ns))
}

fn auto_close_if_idle(any win, any last_ticks, int default_ns=0) bool {
   "Closes `win` once the configured timeout elapses since `last_ticks`."
   def limit = timeout_ns(default_ns)
   if limit <= 0 { return false }
   if ticks() - last_ticks < limit { return false }
   _request_close(win)
   true
}

fn open_fullscreen(any title, int msaa=4, bool raw=true, bool cpu=false) any {
   "Opens a fullscreen focused example window."
   open_windowed(title, 1280, 720, WINDOW_FULLSCREEN | WINDOW_FOCUS_ON_SHOW, raw, cpu, msaa)
}

fn _focus_immediately_after_open() bool {
   #windows { return false }
   #else { return true }
   #endif
}

fn open_windowed(any title, int w, int h, int flags=0, bool raw=true, bool cpu=false, int msaa=0) any {
   "Opens an example window and focuses it when successful."
   def win = init_window(w, h, title, flags, raw, cpu, msaa)
   if win && !_headless_enabled() && _focus_immediately_after_open() { window.focus(win) }
   win
}

fn handle_close_events(any win) bool {
   "Drains pending close/escape events and requests window shutdown when needed."
   mut e = window.check_event(win)
   while e != 0 {
      if window.quit(e) {
         window.set_should_close(win, true)
      } elif (window.event_type(e) == EVENT_KEY_PRESSED &&
         window.event_data(e).get("key", 0) == KEY_ESCAPE){
         window.set_should_close(win, true)
      }
      e = window.check_event(win)
   }
   window.should_close(win)
}

fn step(any win, any start_ticks=0, int default_ns=0) bool {
   "Runs the shared event pump, timeout, and close pass."
   if start_ticks { auto_close(win, start_ticks, default_ns) }
   window.poll_events()
   handle_close_events(win)
}

fn _push_mesh_quad(any buf, int base, f64 x0, f64 y0, f64 x1, f64 y1, any color) int {
   def S = VERTEX_STRIDE
   push_vertex(buf + S*(base + 0), x0, y0, 0.0, 0.0, 0.0, color)
   push_vertex(buf + S*(base + 1), x1, y0, 0.0, 1.0, 0.0, color)
   push_vertex(buf + S*(base + 2), x0, y1, 0.0, 0.0, 1.0, color)
   push_vertex(buf + S*(base + 3), x1, y0, 0.0, 1.0, 0.0, color)
   push_vertex(buf + S*(base + 4), x1, y1, 0.0, 1.0, 1.0, color)
   push_vertex(buf + S*(base + 5), x0, y1, 0.0, 0.0, 1.0, color)
   0
}

fn build_crosshair_mesh() list {
   "Builds the standard 5-quad crosshair mesh used by UI demos."
   def n = 30
   def buf = malloc(n * VERTEX_STRIDE)
   if !buf { return [0, 0] }
   def cc, co = [1.0, 1.0, 1.0, 0.7], [1.0, 1.0, 1.0, 0.4]
   _push_mesh_quad(buf, 0, -1.0, -1.0, 1.0, 1.0, cc)
   _push_mesh_quad(buf, 6, -1.0, -9.0, 1.0, -5.0, co)
   _push_mesh_quad(buf, 12, -1.0, 5.0, 1.0, 9.0, co)
   _push_mesh_quad(buf, 18, -9.0, -1.0, -5.0, 1.0, co)
   _push_mesh_quad(buf, 24, 5.0, -1.0, 9.0, 1.0, co)
   [buf, mesh_create_cpu(buf, n, false)]
}

fn load_equirect_skybox(any path) int {
   "Loads an equirectangular skybox texture from an explicit path."
   def p = str.strip(to_str(path))
   if p.len == 0 || !osfs.is_file(p) { return -1 }
   mut tex = -1
   if str.endswith(str.lower(p), ".exr") {
      def exr_im = exr.load_path(p)
      if exr_im && is_dict(exr_im) {
         tex = texture_upload_image_ex(exr_im, p, 37, true, true, 1, 10497, 33071, "", true)
      } else {
         dbg("skybox", "exr decode failed: " + to_str(exr.last_error()))
      }
   } else {
      tex = texture_load_ex(p, 37, true, true, 1, 10497, 33071)
   }
   tex
}

#main {
   def font_size = default_font_size("runtime_probe_unique", 18.0, "FONT_SIZE_639104")
   assert(font_size >= 8.0 && font_size <= 96.0, "runtime font size")
   assert(default_font_filter("runtime_probe_unique", FONT_FILTER_LINEAR, "FONT_FILTER_639104") == FONT_FILTER_LINEAR, "runtime font filter")
   assert(font_from_candidates(12, 0) == 0 && mono_font(12, 0) == 0 && ui_font_from_candidates(12, 0, 0) == 0, "runtime font candidates")
   mut fps = fps_begin()
   fps = fps_tick(fps, 0.016)
   assert(timeout_ns(0) >= 0 && !deadline_passed(ticks(), 0) && !timeout_hit(ticks(), 0) && fps_current(fps, 0.016) > 0 && load_equirect_skybox("") == -1, "runtime timing/assets")
   print("✓ std.os.ui.render.viewer.runtime self-test passed")
}
