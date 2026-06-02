;; Keywords: event-loop
;; UI runtime setup, frame-loop coordination, auto-close policy, and resource loading.
module std.os.ui.runtime(dbg, demo_font_size, demo_font_filter, font_from_candidates, mono_font, ui_font_from_candidates, timeout_ns, deadline_passed, close_with_dump, auto_close, auto_close_if_idle, open_fullscreen, open_windowed, handle_close_events, step, build_crosshair_mesh, load_equirect_skybox)
use std.core
use std.core.str as str
use std.os (ticks)
use std.os.fs as osfs
use std.os.path as lib_path
use std.os.ui.consts
use std.os.ui.render
use std.os.ui.render as render
use std.os.ui.profile as ui_profile
use std.os.ui.render.term as ui_term
use std.os.ui.window as window
use std.os.prim
use std.parse.img.exr as exr
use std.core.common as common

mut _auto_dump_seen = dict(32)
mut _auto_dump_cache = -1
mut _headless_cache = -1

fn _demo_override(any: tag, any: suffix): str {
   def key_tag = "NY_" + str.upper(to_str(tag)) + "_" + str.upper(to_str(suffix))
   def key_shared = "NY_UI_DEMO_" + str.upper(to_str(suffix))
   def tag_value = common.env_trim(key_tag)
   tag_value.len > 0 ? tag_value : common.env_trim(key_shared)
}

fn _auto_dump_path(any: default_path): str {
   def env_dump_path = common.env_trim("NYTRIX_AUTO_DUMP_PATH")
   env_dump_path.len > 0 ? env_dump_path : to_str(default_path)
}

fn _auto_dump_enabled(): bool {
   _auto_dump_cache = common.cached_env_truthy(_auto_dump_cache, "NYTRIX_AUTO_DUMP")
   _auto_dump_cache == 1
}

fn _headless_enabled(): bool {
   _headless_cache = common.cached_env_truthy(_headless_cache, "NY_UI_HEADLESS")
   _headless_cache == 1
}

fn _auto_dump_once(any: win, str: out_path): bool {
   if(!_auto_dump_enabled()){ return false }
   def key = to_str(window.id(win)) + ":" + to_str(out_path)
   if(_auto_dump_seen.get(key, false)){ return false }
   snapshot(out_path)
   _auto_dump_seen[key] = true
   true
}

fn _request_close(any: win, bool: dump): any { if(dump){ close_with_dump(win) } else { window.set_should_close(win, true) } }

fn dbg(any: tag, any: msg): any {
   "Prints a bracketed debug line when NY_DEBUG is enabled."
   if(ui_profile.debug_enabled()){
      def txt = "[" + to_str(tag) + "] " + to_str(msg)
      ui_profile.print_text(txt)
      if(ui_term.is_open()){ ui_term.log(txt) }
   }
}

fn _clamp_font_size(any: value, any: min_size, any: max_size): any {
   def min_f, max_f = min_size + 0.0, max_size + 0.0
   min(max(value + 0.0, min_f), max_f)
}

fn demo_font_size(any: tag, f64: fallback, str: suffix="FONT_SIZE", any: min_size=8.0, any: max_size=96.0): any {
   "Returns a per-demo font size, overridable through environment variables."
   def raw = _demo_override(tag, suffix)
   raw.len == 0 ? fallback : _clamp_font_size(str.atof(raw), min_size, max_size)
}

fn _font_filter_mode(str: mode, int: fallback): int {
   case mode {
      "nearest", "point", "pixel" -> { return FONT_FILTER_NEAREST }
      "linear", "bilinear", "smooth" -> { return FONT_FILTER_LINEAR }
      _ -> fallback
   }
}

fn demo_font_filter(any: tag, int: fallback=FONT_FILTER_DEFAULT, str: suffix="FONT_FILTER"): int {
   "Returns a per-demo font filter, overridable through environment variables."
   def raw = _demo_override(tag, suffix)
   raw.len == 0 ? fallback : _font_filter_mode(str.lower(str.strip(raw)), fallback)
}

fn _resolve_font_candidate(any: raw): str {
   if(!raw || !is_str(raw)){ return "" }
   def s = str.strip(raw)
   if(s.len == 0){ return "" }
   def resolved = lib_path.resolve_repo_asset(s)
   if(is_str(resolved) && resolved.len > 0){ return resolved }
   s
}

fn font_from_candidates(int: size, any: candidates, int: font_filter=-1, str: pixel_name="monocraft"): int {
   "Loads the first available font from an explicit candidate list."
   if(!is_list(candidates)){ return 0 }
   mut i = 0
   def paths_n = candidates.len
   while(i < paths_n){
      def raw = candidates.get(i)
      if(raw && is_str(raw) && raw.len > 0){
         def str: raw_s = raw
         def resolved = _resolve_font_candidate(raw_s)
         if(resolved.len == 0){
            i += 1
            continue
         }
         mut font = 0
         if(str.find(str.lower(raw_s), str.lower(to_str(pixel_name))) >= 0){
            font = render.font_load(resolved, size, 0)
         } else {
            font = render.font_load(resolved, size, font_filter)
         }
         if(font){ return font }
      }
      i += 1
   }
   0
}

fn mono_font(int: size, any: candidates=0, int: font_filter=-1): int { font_from_candidates(size, candidates, font_filter, "monocraft") }

fn ui_font_from_candidates(int: size, any: candidates=0, any: fallback_candidates=0, int: font_filter=-1): int {
   def primary = font_from_candidates(size, candidates, font_filter, "")
   primary ? primary : font_from_candidates(size, fallback_candidates, font_filter, "monocraft")
}

fn timeout_ns(int: default_ns=0): int {
   "Returns the active timeout in nanoseconds from NY_UI_TIMEOUT, else `default_ns`."
   def env_t = common.env_trim("NY_UI_TIMEOUT")
   if(env_t.len > 0){ return int(str.atof(env_t) * 1e9) }
   default_ns
}

fn deadline_passed(any: start_ticks, int: default_ns=0): bool {
   "Returns true once the configured timeout has elapsed since `start_ticks`."
   def limit = timeout_ns(default_ns)
   if(limit <= 0){ return false }
   ticks() - start_ticks >= limit
}

fn close_with_dump(any: win, str: dump_path="build/release/fb_dump.tga"): any {
   "Optionally snapshots the framebuffer, then requests window close."
   def out_path = _auto_dump_path(dump_path)
   _auto_dump_once(win, out_path)
   window.set_should_close(win, true)
}

fn auto_close(any: win, any: start_ticks, int: default_ns=0, bool: dump=false): bool {
   "Closes `win` once the configured timeout elapses."
   if(!deadline_passed(start_ticks, default_ns)){ return false }
   _request_close(win, dump)
   true
}

fn auto_close_if_idle(any: win, any: last_ticks, int: default_ns=0, bool: dump=false): bool {
   "Closes `win` once the configured timeout elapses since `last_ticks`."
   def limit = timeout_ns(default_ns)
   if(limit <= 0){ return false }
   if(ticks() - last_ticks < limit){ return false }
   _request_close(win, dump)
   true
}

fn open_fullscreen(any: title, int: msaa=4, bool: raw=true, bool: cpu=false): any {
   "Opens a fullscreen focused example window."
   open_windowed(title, 1280, 720, WINDOW_FULLSCREEN | WINDOW_FOCUS_ON_SHOW, raw, cpu, msaa)
}

fn _focus_immediately_after_open(): bool {
   #windows { return false }
   #else { return true }
   #endif
}

fn open_windowed(any: title, int: w, int: h, int: flags=0, bool: raw=true, bool: cpu=false, int: msaa=0): any {
   "Opens an example window and focuses it when successful."
   def win = init_window(w, h, title, flags, raw, cpu, msaa)
   if(win && !_headless_enabled() && _focus_immediately_after_open()){ window.focus(win) }
   win
}

fn handle_close_events(any: win, bool: dump=false): bool {
   "Drains pending close/escape events and requests window shutdown when needed."
   mut e = window.check_event(win)
   while(e != 0){
      if(window.quit(e)){
         window.set_should_close(win, true)
      } elif(window.event_type(e) == EVENT_KEY_PRESSED &&
         window.event_data(e).get("key", 0) == KEY_ESCAPE){
         if(dump){ close_with_dump(win) }
         else { window.set_should_close(win, true) }
      }
      e = window.check_event(win)
   }
   window.should_close(win)
}

fn step(any: win, any: start_ticks=0, int: default_ns=0, bool: dump=false): bool {
   "Runs the shared example pump/timeout/close pass and returns whether the window should close."
   if(start_ticks){ auto_close(win, start_ticks, default_ns, dump) }
   window.poll_events()
   handle_close_events(win, dump)
}

fn build_crosshair_mesh(): list {
   "Builds the standard 5-quad crosshair mesh used by UI demos."
   def n = 30
   def buf = malloc(n * VERTEX_STRIDE)
   if(!buf){ return [0, 0] }
   def cc, co = [1.0, 1.0, 1.0, 0.7], [1.0, 1.0, 1.0, 0.4]
   def S = VERTEX_STRIDE
   push_vertex(buf + S* 0, -1.0, -1.0, 0.0, 0.0, 0.0, cc)
   push_vertex(buf + S* 1,  1.0, -1.0, 0.0, 1.0, 0.0, cc)
   push_vertex(buf + S* 2, -1.0,  1.0, 0.0, 0.0, 1.0, cc)
   push_vertex(buf + S* 3,  1.0, -1.0, 0.0, 1.0, 0.0, cc)
   push_vertex(buf + S* 4,  1.0,  1.0, 0.0, 1.0, 1.0, cc)
   push_vertex(buf + S* 5, -1.0,  1.0, 0.0, 0.0, 1.0, cc)
   push_vertex(buf + S* 6, -1.0, -9.0, 0.0, 0.0, 0.0, co)
   push_vertex(buf + S* 7,  1.0, -9.0, 0.0, 1.0, 0.0, co)
   push_vertex(buf + S* 8, -1.0, -5.0, 0.0, 0.0, 1.0, co)
   push_vertex(buf + S* 9,  1.0, -9.0, 0.0, 1.0, 0.0, co)
   push_vertex(buf + S*10,  1.0, -5.0, 0.0, 1.0, 1.0, co)
   push_vertex(buf + S*11, -1.0, -5.0, 0.0, 0.0, 1.0, co)
   push_vertex(buf + S*12, -1.0,  5.0, 0.0, 0.0, 0.0, co)
   push_vertex(buf + S*13,  1.0,  5.0, 0.0, 1.0, 0.0, co)
   push_vertex(buf + S*14, -1.0,  9.0, 0.0, 0.0, 1.0, co)
   push_vertex(buf + S*15,  1.0,  5.0, 0.0, 1.0, 0.0, co)
   push_vertex(buf + S*16,  1.0,  9.0, 0.0, 1.0, 1.0, co)
   push_vertex(buf + S*17, -1.0,  9.0, 0.0, 0.0, 1.0, co)
   push_vertex(buf + S*18, -9.0, -1.0, 0.0, 0.0, 0.0, co)
   push_vertex(buf + S*19, -5.0, -1.0, 0.0, 1.0, 0.0, co)
   push_vertex(buf + S*20, -9.0,  1.0, 0.0, 0.0, 1.0, co)
   push_vertex(buf + S*21, -5.0, -1.0, 0.0, 1.0, 0.0, co)
   push_vertex(buf + S*22, -5.0,  1.0, 0.0, 1.0, 1.0, co)
   push_vertex(buf + S*23, -9.0,  1.0, 0.0, 0.0, 1.0, co)
   push_vertex(buf + S*24,  5.0, -1.0, 0.0, 0.0, 0.0, co)
   push_vertex(buf + S*25,  9.0, -1.0, 0.0, 1.0, 0.0, co)
   push_vertex(buf + S*26,  5.0,  1.0, 0.0, 0.0, 1.0, co)
   push_vertex(buf + S*27,  9.0, -1.0, 0.0, 1.0, 0.0, co)
   push_vertex(buf + S*28,  9.0,  1.0, 0.0, 1.0, 1.0, co)
   push_vertex(buf + S*29,  5.0,  1.0, 0.0, 0.0, 1.0, co)
   [buf, render.mesh_create(buf, n, false)]
}

fn load_equirect_skybox(any: path): int {
   "Loads an equirectangular skybox texture from an explicit path."
   def p = str.strip(to_str(path))
   if(p.len == 0 || !osfs.is_file(p)){ return -1 }
   mut tex = -1
   if(str.endswith(str.lower(p), ".exr")){
      def exr_im = exr.load_path(p)
      if(exr_im && is_dict(exr_im)){
         tex = render.texture_upload_image_ex(exr_im, p, 37, true, true, 1, 10497, 33071, "", true)
      } else {
         dbg("skybox", "exr decode failed: " + to_str(exr.last_error()))
      }
   } else {
      tex = render.texture_load_ex(p, 37, true, true, 1, 10497, 33071)
   }
   tex
}
