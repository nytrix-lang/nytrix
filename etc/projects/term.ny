#!/usr/bin/env ny

;; Keywords: platform probe ui app example term vulkan png font
;; Terminal Emulator
use std.core
use std.os (exit)
use std.os.args
use std.os.path as ospath
use std.os.time (ticks, msleep)
use std.os.ui.consts
use std.os.ui.render (
   BACKEND_MOCK, BACKEND_VK,
   begin_frame_clear, close_window, end_frame,
   font_load, get_active_backend, get_swapchain_image_count,
   init_mock_surface, init_window, measure_text,
   request_frame_capture, set_backend_type, set_clear_color, set_win_size,
   snapshot
)

use std.os.ui.window as window
use std.os.ui.window.platform as platform
use std.os.ui.window.input as ui
use std.os.ui.render.vterm as vterm
use std.core.str as str
use std.core.common as common

mut win = 0
mut win_id = 0
mut font = 0
mut font_bold = 0
mut font_italic = 0
mut font_emoji = 0
mut vt = 0
mut win_w = 1280.0
mut win_h = 720.0
def START_FONT_SIZE = 16.0
mut font_size = START_FONT_SIZE
mut last_esc_ms = 0
mut _has_framebuffer_transparency = false
mut _last_activity_ticks = 0
mut _shell_exit_reported = false
mut _last_title = ""
mut _update_budget_ns = 6000000
mut _update_budget_reload_ticks = 0
mut _runtime_flags_reload_ticks = 0
mut _esc_close_enabled = false
mut _stay_open_on_exit = false
mut _fb_sync_next_ticks = 0
mut _needs_draw = true
mut _last_cursor_blink_phase = -1
mut _frame_count = 0
mut _auto_dump_done = false
mut _auto_dump_ready_frame = -1
mut _auto_dump_attempts = 0
mut _headless_begin_fail_count = 0
mut _headless_mode = false
mut _auto_dump_requested = false
mut _auto_dump_exit_enabled = false
mut _auto_dump_delay_frames = 8
mut _auto_dump_path = ""
mut _redraw_frames = 0
mut _left_button_was_down = false
mut _last_left_down_ticks = 0
mut _selection_trace_last = ""
def _VT_OFF_SEL_ACTIVE = 74
def _VT_OFF_SEL_SX = 78
def _VT_OFF_SEL_SY = 82
def _VT_OFF_SEL_EX = 86
def _VT_OFF_SEL_EY = 90
def _VT_OFF_SEL_DRAGGING = 98
def _VT_OFF_SEL_MOVED = 144
def TERM_FONT_DEFAULT = "/usr/share/fonts/TTF/DejaVuSansMono.ttf"
def TERM_FONT_CANDIDATES = [
   "/usr/share/fonts/TTF/JetBrainsMonoNerdFontMono-Regular.ttf",
   "/usr/share/fonts/TTF/JetBrainsMonoNLNerdFontMono-Regular.ttf",
   "/usr/share/fonts/TTF/MesloLGSNerdFontMono-Regular.ttf",
   "/usr/share/fonts/OTF/FiraMonoNerdFontMono-Regular.otf",
   "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
]

fn _dbg(str: tag, any: msg): any {
   if(common.env_truthy("NY_DEBUG")){ print("[" + tag + "] " + to_str(msg)) }
   0
}

fn _term_color(str: code, str: text): str {
   if(common.env_truthy("NO_COLOR")){ return text }
   def esc = chr(27)
   esc + "[" + code + "m" + text + esc + "[0m"
}

fn _term_diag(str: label, any: value): any {
   eprint("  " + _term_color("90", label + ": ") + to_str(value))
   0
}

fn _term_normalize_mod(any: mods): int { int(mods) & (MOD_SHIFT | MOD_CONTROL | MOD_ALT | MOD_SUPER | MOD_META) }

fn _term_redraw_frames(): int {
   if(common.env_present("NY_TERM_REDRAW_FRAMES")){
      return common.env_int_clamped("NY_TERM_REDRAW_FRAMES", 4, 2, 64)
   }
   mut frames = 4
   if(get_active_backend() == BACKEND_VK){
      def swap_images = get_swapchain_image_count()
      if(swap_images > 0){ frames = min(max(3, swap_images + 1), 6) }
   }
   frames
}

fn _term_frame_trace(str: label): any {
   if(!common.env_truthy("NY_TERM_FRAME_TRACE")){ return 0 }
   print("[term:frame] " + label +
      " frame=" + to_str(_frame_count) +
      " redraw=" + to_str(_redraw_frames) +
      " needs=" + to_str(_needs_draw) +
      " backend=" + to_str(get_active_backend()) +
   " swap_images=" + to_str(get_swapchain_image_count()))
   0
}

fn _term_bg_color(): list {
   def c = int(vt.get("def_bg", 0xff000000))
   vterm.abgr_to_color((c & 0x00ffffff) | 0xff000000)
}

fn _trace_selection_state(str: label, int: typ=0, any: ev_data=0): any {
   if(!common.env_truthy("NY_TERM_SELECTION_TRACE") || !is_dict(vt)){ return 0 }
   def st = vt.get("state", 0)
   if(!st){ return 0 }
   mut event_left = false
   mut event_button = -1
   if(is_dict(ev_data)){
      event_left = !!ev_data.get("left_down", false)
      event_button = int(ev_data.get("button", -1))
   }
   def active = load8(st, _VT_OFF_SEL_ACTIVE)
   def dragging = load8(st, _VT_OFF_SEL_DRAGGING)
   def moved = load8(st, _VT_OFF_SEL_MOVED)
   def state = "event_left=" + to_str(event_left) +
   " latch=" + to_str(_left_button_was_down) +
   " sel=" + to_str(active) + "/" + to_str(dragging) + "/" + to_str(moved) +
   " xy=" + to_str(load32(st, _VT_OFF_SEL_SX)) + "," + to_str(load32(st, _VT_OFF_SEL_SY)) + "->" + to_str(load32(st, _VT_OFF_SEL_EX)) + "," + to_str(load32(st, _VT_OFF_SEL_EY))
   def important = typ == EVENT_MOUSE_BUTTON_PRESSED ||
   typ == EVENT_MOUSE_BUTTON_RELEASED ||
   active != 0 ||
   dragging != 0 ||
   _left_button_was_down ||
   str.find(label, "synth") != -1
   if(!important && state == _selection_trace_last){ return 0 }
   _selection_trace_last = state
   print("[term:selection] " + label +
      " typ=" + to_str(typ) +
      " button=" + to_str(event_button) +
   " " + state)
   0
}

fn _request_redraw(int: frames=4): any {
   if(frames > _redraw_frames){ _redraw_frames = frames }
   _needs_draw = true
   0
}

fn _term_print_startup_failure(int: flags): any {
   eprint(_term_color("1;37", "[term] graphics startup failed"))
   _term_diag("window backend", window.backend())
   _term_diag("renderer", "vulkan")
   _term_diag("flags", "0x" + to_hex(flags))
   _term_diag("DISPLAY", common.value_or(common.env_trim("DISPLAY"), "<unset>"))
   _term_diag("WAYLAND_DISPLAY", common.value_or(common.env_trim("WAYLAND_DISPLAY"), "<unset>"))
   def requested = common.env_trim("NY_UI_BACKEND")
   if(requested.len > 0){ _term_diag("NY_UI_BACKEND", requested) }
   def x11_surface = common.env_trim("NY_UI_X11_VK_SURFACE")
   if(x11_surface.len > 0){ _term_diag("NY_UI_X11_VK_SURFACE", x11_surface) }
   0
}

fn _demo_override(any: tag, any: suffix): str {
   def key_tag = "NY_" + str.upper(to_str(tag)) + "_" + str.upper(to_str(suffix))
   def key_shared = "NY_UI_DEMO_" + str.upper(to_str(suffix))
   def tag_value = common.env_trim(key_tag)
   tag_value.len > 0 ? tag_value : common.env_trim(key_shared)
}

fn _demo_font_size(any: tag, f64: fallback, str: suffix="FONT_SIZE"): any {
   def raw = _demo_override(tag, suffix)
   if(raw.len == 0){ return fallback }
   min(max(str.atof(raw), 8.0), 96.0)
}

fn _font_filter_mode(str: mode, int: fallback): int {
   case mode {
      "nearest", "point", "pixel" -> { return FONT_FILTER_NEAREST }
      "linear", "bilinear", "smooth" -> { return FONT_FILTER_LINEAR }
      _ -> fallback
   }
}

fn _demo_font_filter(any: tag, int: fallback=FONT_FILTER_DEFAULT, str: suffix="FONT_FILTER"): int {
   def raw = _demo_override(tag, suffix)
   raw.len == 0 ? fallback : _font_filter_mode(str.lower(str.strip(raw)), fallback)
}

fn _font_from_candidates(int: size, list: candidates, int: font_filter=-1): int {
   mut i = 0
   while(i < candidates.len){
      def raw = candidates.get(i, "")
      def resolved = ospath.resolve_repo_asset(raw)
      def path = resolved.len > 0 ? resolved : raw
      def f = font_load(path, size, font_filter)
      if(f){ return f }
      i += 1
   }
   0
}

fn _open_windowed(any: title, int: w, int: h, int: flags=0, bool: raw=true, bool: cpu=false, int: msaa=0): any {
   def opened = init_window(w, h, title, flags, raw, cpu, msaa)
   if(opened && !common.env_truthy("NY_UI_HEADLESS")){ window.focus(opened) }
   opened
}

fn _close_with_dump(any: win, str: dump_path="build/release/fb_dump.tga"): any {
   if(common.env_truthy("NYTRIX_AUTO_DUMP")){
      def out_path = common.env_trim("NYTRIX_AUTO_DUMP_PATH")
      snapshot(out_path.len > 0 ? out_path : dump_path)
   }
   window.set_should_close(win, true)
}

fn _auto_close_if_idle(any: win, any: last_ticks, int: default_ns=0): bool {
   def raw = common.env_trim("NY_UI_TIMEOUT")
   def limit = raw.len > 0 ? int(str.atof(raw) * 1e9) : default_ns
   if(limit <= 0 || ticks() - last_ticks < limit){ return false }
   window.set_should_close(win, true)
   true
}

fn _font_cell_size(any: font, any: font_size): list {
   def fs = float(font_size)
   if(!font){ return [fs * 0.6, max(fs, 20.0)] }
   def probe = measure_text(font, "M")
   mut cw, ch = float(probe.get(0, 0.0)), float(probe.get(1, 0.0))
   if(cw <= 1.0){ cw = max(cw, float(measure_text(font, "A").get(0, 0.0))) }
   if(cw <= 1.0){ cw = max(cw, float(measure_text(font, "i").get(0, 0.0))) }
   if(cw <= 1.0){ cw = fs * 0.6 }
   if(ch <= 1.0){ ch = fs }
   [max(1.0, float(int(cw + 0.5))), max(1.0, float(int(ch + 0.5)))]
}

fn _framebuffer_size(any: live_win): list {
   mut fw, fh = 0.0, 0.0
   if(live_win){
      def fb = window.get_framebuffer_size(live_win)
      fw, fh = float(fb.get(0, 0)), float(fb.get(1, 0))
   }
   if(fw <= 0.0 || fh <= 0.0){
      def sz = window.size(live_win)
      fw, fh = float(sz.get(0, 1280)), float(sz.get(1, 720))
   }
   [fw, fh]
}

fn _live_window(): any {
   def last_win = window.last()
   if(is_dict(last_win)){ return last_win }
   if(is_dict(win)){ return win }
   if(win_id){ return window.get_win(win_id) }
   0
}

fn _font_map(): dict {
   {
      "regular": font,
      "bold": font_bold,
      "italic": font_italic,
      "emoji": font_emoji,
   }
}

fn _trace_vt_cursor(str: label): any {
   if(!common.env_truthy("NY_TERM_PTY_TRACE") || !is_dict(vt)){ return 0 }
   def st = vt.get("state", 0)
   if(!st){ return 0 }
   print("[term:vt] " + label + " cols=" + to_str(vt.get("cols", 0)) + " rows=" + to_str(vt.get("rows", 0)) + " cx=" + to_str(load32(st, 0)) + " cy=" + to_str(load32(st, 4)))
   0
}

fn _sync_framebuffer_size(): bool {
   def fb = _framebuffer_size(win)
   def nw = float(fb.get(0, win_w))
   def nh = float(fb.get(1, win_h))
   if(nw <= 0.0 || nh <= 0.0){ return false }
   if(int(nw) == int(win_w) && int(nh) == int(win_h)){ return false }
   win_w = nw
   win_h = nh
   _resize_term()
   true
}

fn _sync_framebuffer_size_if_due(any: now_ticks, bool: force=false): bool {
   if(!force && now_ticks < _fb_sync_next_ticks){ return false }
   _fb_sync_next_ticks = now_ticks + 16000000 ;; ~16ms
   _sync_framebuffer_size()
}

fn _refresh_update_budget(any: now_ticks): any {
   if(now_ticks < _update_budget_reload_ticks){ return 0 }
   mut budget_ms = 8.0
   def budget_env = common.env_trim("NY_TERM_UPDATE_BUDGET_MS")
   if(budget_env.len > 0){
      def bv = str.atof(budget_env)
      if(bv >= 1.0 && bv <= 33.0){ budget_ms = bv }
   }
   _update_budget_ns = int(budget_ms * 1000000.0)
   _update_budget_reload_ticks = now_ticks + 1000000000
   0
}

fn _refresh_runtime_flags(any: now_ticks): any {
   if(now_ticks < _runtime_flags_reload_ticks){ return 0 }
   _esc_close_enabled = common.env_present("NY_TERM_ESC_CLOSE")
   _stay_open_on_exit = common.env_truthy("NY_TERM_STAY_OPEN_ON_EXIT")
   _runtime_flags_reload_ticks = now_ticks + 1000000000
   0
}

fn _refresh_dump_flags(): any {
   _headless_mode = common.env_truthy("NY_UI_HEADLESS")
   _auto_dump_requested = common.env_truthy("NYTRIX_AUTO_DUMP") || _headless_mode
   _auto_dump_exit_enabled = _headless_mode || common.env_truthy("NYTRIX_AUTO_DUMP_EXIT")
   _auto_dump_delay_frames = 8
   if(_headless_mode){ _auto_dump_delay_frames = 0 }
   if(common.env_present("NYTRIX_AUTO_DUMP_DELAY_FRAMES")){
      _auto_dump_delay_frames = common.env_int_clamped("NYTRIX_AUTO_DUMP_DELAY_FRAMES", _auto_dump_delay_frames, 0, 1000000)
   }
   if(_auto_dump_delay_frames < 0){ _auto_dump_delay_frames = 0 }
   _auto_dump_path = common.env_trim("NYTRIX_AUTO_DUMP_PATH")
   0
}

fn startup(){
   _refresh_dump_flags()
   if(_headless_mode){
      platform.init_hint(platform.PLATFORM, platform.PLATFORM_NULL)
      set_backend_type(BACKEND_MOCK)
   }
   def mode = window.primary_mode()
   def screen_w = mode.get(0, 1280)
   def screen_h = mode.get(1, 720)
   def default_sz = window.default_window_size(screen_w, screen_h)
   def start_w = default_sz.get(0, 1280)
   def start_h = default_sz.get(1, 720)
   def flags = WINDOW_ALLOW_DND | WINDOW_CENTER | WINDOW_FOCUS_ON_SHOW
   font_size = _demo_font_size("term", START_FONT_SIZE)
   _dbg("term", "creating window flags=0x" + to_hex(flags))
   _dbg("term", "renderer " + (_headless_mode ? "mock" : "gpu=vulkan"))
   if(_headless_mode){
      win = window.open_window("Nytrix Terminal", 0, 0, start_w, start_h, flags | WINDOW_HIDE | WINDOW_NO_RESIZE)
      if(win && !init_mock_surface(start_w, start_h)){
         window.close(win)
         win = 0
      }
   } else {
      win = _open_windowed("Nytrix Terminal", start_w, start_h, flags, true, false, 0)
   }
   if(!win){
      _term_print_startup_failure(flags | WINDOW_VULKAN)
      exit(1)
   }
   win_id = window.id(win)
   _dbg("term", "window created win=0x" + to_hex(win_id))
   set_clear_color([0.0, 0.0, 0.0, 1.0])
   window.set_exit_key(win, KEY_NULL)
   window.set_input_exclusive(win, true)
   _reload_fonts()
   def sz = _framebuffer_size(win)
   win_w = float(sz.get(0, start_w))
   win_h = float(sz.get(1, start_h))
   _has_framebuffer_transparency = false
   _dbg("term", "before init_vt win_is_dict=" + to_str(is_dict(win)) + " id=0x" + to_hex(window.id(win)))
   _init_vt()
   _dbg("term", "after init_vt win_is_dict=" + to_str(is_dict(win)) + " id=0x" + to_hex(window.id(win)))
   window.set_cursor_mode(win, window.CURSOR_NORMAL)
   _dbg("term", "after cursor mode win_is_dict=" + to_str(is_dict(win)) + " id=0x" + to_hex(window.id(win)))
   _trace_vt_cursor("after init")
   _resize_term()
   _trace_vt_cursor("after resize")
   _dbg("term", "after resize_term win_is_dict=" + to_str(is_dict(win)) + " id=0x" + to_hex(window.id(win)))
   _last_title = vterm.get_title(vt)
   _fb_sync_next_ticks = ticks()
   _refresh_update_budget(ticks())
   _refresh_runtime_flags(ticks())
   _needs_draw = true
   _request_redraw(_term_redraw_frames())
   _last_cursor_blink_phase = vterm.cursor_blink_phase(vt, ticks())
   _auto_dump_ready_frame = -1
   _auto_dump_attempts = 0
   _trace_vt_cursor("before selftest")
   _selftest_input()
   _trace_vt_cursor("after selftest")
   win = window.get_win(win)
}

fn _init_vt(){
   def cell = _font_cell_size(font, font_size)
   def cw = float(cell.get(0, font_size * 0.6))
   def ch = float(cell.get(1, max(font_size, 20.0)))
   mut cols = int(win_w / cw)
   mut rows = int(win_h / ch)
   if(cols <= 0){ cols = 80 } if(rows <= 0){ rows = 24 }
   vt = vterm.new(cols, rows, _font_map(), 0, 0)
   if(!vt || !is_dict(vt)){
      print("[term] failed to allocate terminal state")
      exit(1)
   }
   vt = vt.set("char_w", cw)
   vt = vt.set("char_h", ch)
   vt = vt.set("px_w", int(float(cols) * cw))
   vt = vt.set("px_h", int(float(rows) * ch))
   vt = vt.set("window_id", win_id)
   def launch = _term_launch_spec()
   def shell_path = launch.get("path", vterm.default_shell_path())
   def shell_args = launch.get("args", vterm.default_shell_args(common.env_truthy("NY_TERM_LOGIN")))
   _dbg("term", "opening shell path=" + shell_path + " args=" + to_str(shell_args))
   match vterm.open(vt, shell_path, shell_args){
      ok(next_vt) -> { vt = next_vt }
      err(e) -> {
         print("[term] failed to open vterm: " + to_str(e))
         exit(1)
      }
   }
   set_clear_color(_term_bg_color())
}

fn _selftest_input(): any {
   if(!common.env_truthy("NY_TERM_SELFTEST_INPUT")){ return 0 }
   vt = vterm.handle_event(vt, EVENT_KEY_PRESSED, {"key": ui.KEY_A, "action": 1, "mod": 0, "ww": win_w, "wh": win_h})
   if(!common.env_truthy("NY_TERM_SELFTEST_KEY_ONLY")){
      vt = vterm.handle_event(vt, EVENT_KEY_CHAR, {"char": 97, "mod": 0, "plain": true, "ww": win_w, "wh": win_h})
   }
   0
}

fn _term_launch_spec(): dict {
   def login_pref = common.env_truthy("NY_TERM_LOGIN")
   def shell_path = vterm.default_shell_path()
   if(argc() <= 1){ return {"path": shell_path, "args": vterm.default_shell_args(login_pref)} }
   mut line = ""
   mut i = 1
   while(i < argc()){
      if(i > 1){ line = line + " " }
      line = line + to_str(argv(i))
      i += 1
   }
   line = str.strip(line)
   if(line.len == 0){ return {"path": shell_path, "args": vterm.default_shell_args(login_pref)} }
   return {"path": shell_path, "args": ["-lc", line]}
}

fn _reload_fonts(){
   def reg_path = common.env_trim("NY_TERM_FONT_REG")
   def bold_path = common.env_trim("NY_TERM_FONT_BOLD")
   def ital_path = common.env_trim("NY_TERM_FONT_ITAL")
   def emoji_path = common.env_trim("NY_TERM_FONT_EMOJI")
   def font_filter = _demo_font_filter("term", FONT_FILTER_NEAREST)
   def emoji_filter = _demo_font_filter("term", FONT_FILTER_LINEAR, "EMOJI_FONT_FILTER")
   mut f = 0
   if(reg_path.len > 0){
      f = font_load(reg_path, int(font_size), font_filter)
   } else {
      f = _font_from_candidates(int(font_size), TERM_FONT_CANDIDATES, font_filter)
   }
   if(!f){ f = font_load(TERM_FONT_DEFAULT, int(font_size), font_filter) }
   font = f
   if(bold_path.len > 0){
      f = font_load(bold_path, int(font_size), font_filter)
      font_bold = common.value_or(f, font)
   } else {
      font_bold = font
   }
   if(ital_path.len > 0){
      f = font_load(ital_path, int(font_size), font_filter)
      font_italic = common.value_or(f, font)
   } else {
      font_italic = font
   }
   def emoji_on = common.env_present("NY_TERM_EMOJI") ? common.env_enabled("NY_TERM_EMOJI") : true
   if(emoji_on){
      def emoji_default = "/usr/share/fonts/noto/NotoColorEmoji.ttf"
      def ep = (emoji_path.len > 0) ? emoji_path : emoji_default
      f = font_load(ep, int(font_size), emoji_filter)
      font_emoji = common.value_or(f, font)
   } else {
      font_emoji = font
   }
   if(vt != 0){
      vt = vt.set("fonts", _font_map())
      _resize_term()
   }
}

fn _input_event(int: typ): bool {
   typ == EVENT_KEY_PRESSED ||
   typ == EVENT_KEY_RELEASED ||
   typ == EVENT_KEY_CHAR ||
   typ == EVENT_MOUSE_SCROLL ||
   typ == EVENT_MOUSE_BUTTON_PRESSED ||
   typ == EVENT_MOUSE_BUTTON_RELEASED ||
   typ == EVENT_MOUSE_POS_CHANGED
}

fn _term_mouse_event(int: typ): bool {
   typ == EVENT_MOUSE_POS_CHANGED ||
   typ == EVENT_MOUSE_BUTTON_PRESSED ||
   typ == EVENT_MOUSE_BUTTON_RELEASED ||
   typ == EVENT_MOUSE_SCROLL
}

fn _scale_event_xy(int: typ, any: ev_data): any {
   if(!is_dict(ev_data) || !ev_data.contains("x") || !ev_data.contains("y")){ return ev_data }
   mut out = dict(16)
   for k in ev_data.keys(){ out[k] = ev_data[k] }
   def sz = window.size(win)
   def logical_w = max(1.0, float(sz.get(0, win_w)))
   def logical_h = max(1.0, float(sz.get(1, win_h)))
   def sx = win_w / logical_w
   def sy = win_h / logical_h
   def raw_x = float(ev_data.get("x", 0.0))
   def raw_y = float(ev_data.get("y", 0.0))
   out["raw_x"] = raw_x
   out["raw_y"] = raw_y
   out["x"] = min(max(raw_x * sx, 0.0), max(win_w - 1.0, 0.0))
   out["y"] = min(max(raw_y * sy, 0.0), max(win_h - 1.0, 0.0))
   if(out.contains("dx")){ out["dx"] = float(out.get("dx", 0.0)) * sx }
   if(out.contains("dy")){ out["dy"] = float(out.get("dy", 0.0)) * sy }
   if(common.env_truthy("NY_TERM_INPUT_TRACE")){
      print("[term:input] typ=" + to_str(typ) + " raw=" + to_str(raw_x) + "," + to_str(raw_y) +
         " scaled=" + to_str(out.get("x", 0.0)) + "," + to_str(out.get("y", 0.0)) +
         " logical=" + to_str(logical_w) + "x" + to_str(logical_h) +
      " fb=" + to_str(win_w) + "x" + to_str(win_h))
   }
   out
}

fn _vterm_event_data(int: typ, any: data): any {
   mut ev_data = data
   if(typ == EVENT_MOUSE_SCROLL && is_list(ev_data)){
      ev_data = {
         "dx": float(ev_data.get(0, 0.0)),
         "dy": float(ev_data.get(1, 0.0)),
         "scrolling": true,
      }
   }
   if(is_dict(ev_data)){
      ev_data = _scale_event_xy(typ, ev_data)
      if(typ == EVENT_KEY_PRESSED || typ == EVENT_KEY_RELEASED){
         ev_data = ev_data.set("key", ui.normalize_key(ev_data.get("key", 0)))
         ev_data = ev_data.set("mod", _term_normalize_mod(ev_data.get("mod", ev_data.get("mods", 0))))
      }
      if(_term_mouse_event(typ)){
         def event_button = int(ev_data.get("button", -1))
         def native_left = ev_data.contains("left_down")
         def left_down = native_left ? !!ev_data.get("left_down", false) : _left_down_for_event(typ, event_button)
         if(left_down){ _last_left_down_ticks = ticks() }
         if(!ev_data.contains("middle_down")){ ev_data = ev_data.set("middle_down", window.mouse_down(win, 2)) }
         if(!ev_data.contains("right_down")){ ev_data = ev_data.set("right_down", window.mouse_down(win, 1)) }
         ev_data = ev_data.set("left_down", left_down)
         if(common.env_truthy("NY_TERM_INPUT_TRACE")){
            print("[term:input] button-state typ=" + to_str(typ) +
               " button=" + to_str(event_button) +
               " left=" + to_str(left_down) +
               " native=" + to_str(native_left) +
            " latch=" + to_str(_left_button_was_down))
         }
      }
      ev_data = ev_data.set("ww", win_w)
      ev_data = ev_data.set("wh", win_h)
   }
   ev_data
}

fn _left_down_for_event(int: typ, int: event_button): bool {
   if(typ == EVENT_MOUSE_BUTTON_PRESSED && event_button == 0){ return true }
   if(typ == EVENT_MOUSE_BUTTON_RELEASED && event_button == 0){ return false }
   _left_button_was_down || window.mouse_down(win, 0)
}

fn _vterm_selection_dragging(): bool {
   if(!is_dict(vt)){ return false }
   def st = vt.get("state", 0)
   st && load8(st, _VT_OFF_SEL_DRAGGING) != 0
}

fn _vterm_selection_any(): bool {
   if(!is_dict(vt)){ return false }
   def st = vt.get("state", 0)
   st && (load8(st, _VT_OFF_SEL_ACTIVE) != 0 || load8(st, _VT_OFF_SEL_DRAGGING) != 0 || load8(st, _VT_OFF_SEL_MOVED) != 0)
}

fn _hard_clear_vterm_selection(str: label): bool {
   if(!is_dict(vt) || vt.get("sticky_selection", false)){ return false }
   def st = vt.get("state", 0)
   if(!st || !_vterm_selection_any()){ return false }
   _trace_selection_state("before " + label, 0, 0)
   store8(st, 0, _VT_OFF_SEL_ACTIVE)
   store8(st, 0, _VT_OFF_SEL_DRAGGING)
   store8(st, 0, _VT_OFF_SEL_MOVED)
   store32(st, 0, _VT_OFF_SEL_SX)
   store32(st, 0, _VT_OFF_SEL_SY)
   store32(st, 0, _VT_OFF_SEL_EX)
   store32(st, 0, _VT_OFF_SEL_EY)
   _left_button_was_down = false
   _last_left_down_ticks = 0
   _trace_selection_state("after " + label, 0, 0)
   if(common.env_truthy("NY_TERM_INPUT_TRACE")){ print("[term:input] " + label) }
   _request_redraw(_term_redraw_frames())
   true
}

fn _force_left_release(str: label, any: raw=0): bool {
   if(!is_dict(vt)){ return false }
   mut data = raw
   if(!is_dict(data)){
      def p = window.cursor_pos(win)
      data = {
         "button": 0,
         "x": float(p.get(0, 0.0)),
         "y": float(p.get(1, 0.0)),
      }
   }
   data = data.set("button", 0)
   data = data.set("left_down", false)
   if(!data.contains("middle_down")){ data = data.set("middle_down", window.mouse_down(win, 2)) }
   if(!data.contains("right_down")){ data = data.set("right_down", window.mouse_down(win, 1)) }
   data = _vterm_event_data(EVENT_MOUSE_BUTTON_RELEASED, data)
   _trace_selection_state("before " + label, EVENT_MOUSE_BUTTON_RELEASED, data)
   vt = vterm.handle_event(vt, EVENT_MOUSE_BUTTON_RELEASED, data)
   _left_button_was_down = false
   if(_vterm_selection_dragging() && !vt.get("sticky_selection", false)){ _hard_clear_vterm_selection(label + " fallback clear") }
   _trace_selection_state("after " + label, EVENT_MOUSE_BUTTON_RELEASED, data)
   if(common.env_truthy("NY_TERM_INPUT_TRACE")){ print("[term:input] " + label) }
   _request_redraw(_term_redraw_frames())
   true
}

fn _sync_lost_left_release(): bool {
   if(!is_dict(vt)){ return false }
   def down_now = window.mouse_down(win, 0)
   if((_left_button_was_down || _vterm_selection_dragging()) && !down_now){
      def age_ns = ticks() - _last_left_down_ticks
      def debounce_ns = common.env_int_clamped("NY_TERM_LOST_RELEASE_DEBOUNCE_MS", 45, 0, 500) * 1000000
      if(age_ns < debounce_ns){ return false }
      return _force_left_release("synthesized lost left release")
   }
   false
}

fn _left_release_debounce_ns(): int {
   common.env_int_clamped("NY_TERM_LOST_RELEASE_DEBOUNCE_MS", 45, 0, 500) * 1000000
}

fn _maybe_synthesize_left_release_from_motion(int: typ, any: ev_data): bool {
   if(typ != EVENT_MOUSE_POS_CHANGED || !is_dict(ev_data)){ return false }
   if(!_left_button_was_down || !!ev_data.get("left_down", true)){ return false }
   if(ticks() - _last_left_down_ticks < _left_release_debounce_ns()){ return false }
   mut release_data = ev_data
   release_data = release_data.set("button", 0)
   release_data = release_data.set("left_down", false)
   _trace_selection_state("before synth-release-from-motion", EVENT_MOUSE_BUTTON_RELEASED, release_data)
   vt = vterm.handle_event(vt, EVENT_MOUSE_BUTTON_RELEASED, release_data)
   _left_button_was_down = false
   _trace_selection_state("after synth-release-from-motion", EVENT_MOUSE_BUTTON_RELEASED, release_data)
   if(common.env_truthy("NY_TERM_INPUT_TRACE")){ print("[term:input] synthesized left release from motion") }
   _request_redraw(_term_redraw_frames())
   true
}

fn _update_left_button_latch(int: typ, any: ev_data): any {
   if(!is_dict(ev_data)){ return 0 }
   def event_button = int(ev_data.get("button", -1))
   if(typ == EVENT_MOUSE_BUTTON_PRESSED && event_button == 0){
      _left_button_was_down = true
   } elif(typ == EVENT_MOUSE_BUTTON_RELEASED && event_button == 0){
      _left_button_was_down = false
   } elif(typ == EVENT_MOUSE_POS_CHANGED && !!ev_data.get("left_down", false)){
      _left_button_was_down = true
   }
   0
}

fn _handle_app_key(int: k, int: md): bool {
   if(_esc_close_enabled && k == ui.KEY_ESCAPE && (md & (MOD_SHIFT|MOD_CONTROL|MOD_ALT|MOD_SUPER|MOD_META)) == 0 && vt != 0){
      def st = vt.get("state", 0)
      if(st != 0){
         def mode = load32(st, 32)
         if((mode & 2) == 0){
            def now_ms = int(ticks() / 1000000)
            if(last_esc_ms != 0 && (now_ms - last_esc_ms) < 400){
               _close_with_dump(win)
               return true
            }
            last_esc_ms = now_ms
         } else {
            last_esc_ms = 0
         }
      }
   }
   if((md & MOD_CONTROL) != 0){
      if(k == ui.KEY_EQUAL || k == ui.KEY_KP_ADD){ font_size += 1.0 _reload_fonts() return true }
      if(k == ui.KEY_MINUS || k == ui.KEY_KP_SUBTRACT){ if(font_size > 4.0){ font_size -= 1.0 _reload_fonts() } return true }
      if(k == ui.KEY_0 || k == ui.KEY_KP_0){ font_size = START_FONT_SIZE _reload_fonts() return true }
   }
   false
}

fn update(dt, now_ticks){
   common.touch(dt)
   def live = _live_window()
   if(is_dict(live)){ win = live }
   window.poll_events()
   _refresh_update_budget(now_ticks)
   _refresh_runtime_flags(now_ticks)
   _sync_framebuffer_size_if_due(now_ticks)
   mut e = window.check_event(win)
   mut event_count = 0
   mut mouse_event_count = 0
   mut input_hot = false
   while(e != 0){
      def typ = window.event_type(e)
      def data = window.event_data(e)
      if(typ == EVENT_WINDOW_RESIZED){
         _sync_framebuffer_size_if_due(now_ticks, true)
      } elif(typ == EVENT_KEY_PRESSED){
         def k = ui.normalize_key(data.get("key", 0))
         def md = _term_normalize_mod(data.get("mod", data.get("mods", 0)))
         input_hot = true
         if(_handle_app_key(k, md)){
            _request_redraw(_term_redraw_frames())
            e = window.check_event(win)
            continue
         }
      }
      if(_input_event(typ)){ input_hot = true }
      if(_term_mouse_event(typ)){ mouse_event_count += 1 }
      def ev_data = _vterm_event_data(typ, data)
      if(typ == EVENT_MOUSE_LEAVE || typ == EVENT_FOCUS_OUT){
         _left_button_was_down = false
         _last_left_down_ticks = 0
      }
      _trace_selection_state("before event", typ, ev_data)
      if(_maybe_synthesize_left_release_from_motion(typ, ev_data)){
         event_count += 1
         input_hot = true
      }
      vt = vterm.handle_event(vt, typ, ev_data)
      if(typ == EVENT_MOUSE_LEAVE || typ == EVENT_FOCUS_OUT){ _hard_clear_vterm_selection("leave/focus selection clear") }
      if(_term_mouse_event(typ)){ _update_left_button_latch(typ, ev_data) }
      _trace_selection_state("after event", typ, ev_data)
      if(window.quit(e)){
         _dbg("term", "quit event")
         window.set_should_close(win, true)
      }
      e = window.check_event(win)
      event_count += 1
   }
   common.touch(mouse_event_count)
   if(_sync_lost_left_release()){
      event_count += 1
      input_hot = true
   }
   mut updates = 0
   def update_start = now_ticks
   mut max_updates = input_hot ? 16 : 64
   mut budget_ns = _update_budget_ns
   if(input_hot && budget_ns > 4000000){ budget_ns = 4000000 }
   while(updates < max_updates){
      if(updates == 0){ _trace_vt_cursor("before update") }
      mut nvt = vterm.update(vt)
      def update_bytes = int(nvt.get("last_update_bytes", 0))
      vt = nvt
      if(update_bytes <= 0){ break }
      updates += 1
      if(updates == 1){ _trace_vt_cursor("after update") }
      if((ticks() - update_start) >= budget_ns){ break }
   }
   def cur_title = vterm.get_title(vt)
   if(cur_title != _last_title){
      window.set_title(win, cur_title)
      _last_title = cur_title
   }
   if(!vterm.is_running(vt)){
      if(!_shell_exit_reported){
         print("[term] shell exited")
         _shell_exit_reported = true
      }
      if(!_stay_open_on_exit){
         window.set_should_close(win, true)
      }
   }
   def activity = event_count > 0 || updates > 0
   _needs_draw = activity
   if(activity){ _request_redraw(_term_redraw_frames()) }
   if(vterm.needs_visual_refresh(vt, now_ticks, _last_cursor_blink_phase)){ _request_redraw(_term_redraw_frames()) }
   if(_redraw_frames > 0){ _needs_draw = true }
   if(activity){ _last_activity_ticks = now_ticks }
   if(!_needs_draw){ msleep(vterm.idle_sleep_ms(vt, now_ticks)) }
}

fn _resize_term(){
   def cell = _font_cell_size(font, font_size)
   def cw = float(cell.get(0, float(font_size) * 0.6))
   def ch = float(cell.get(1, max(float(font_size), 20.0)))
   def cols = int(win_w / cw)
   def rows = int(win_h / ch)
   set_win_size(int(win_w), int(win_h))
   if(cols <= 0 || rows <= 0){ return }
   vt = vterm.resize(vt, cols, rows)
   vt = vt.set("char_w", cw)
   vt = vt.set("char_h", ch)
   vt = vt.set("px_w", int(float(cols) * cw))
   vt = vt.set("px_h", int(float(rows) * ch))
}

fn draw(){
   def live = _live_window()
   if(is_dict(live)){ win = live }
   def dump_enabled = _auto_dump_requested && !_auto_dump_done
   if(dump_enabled){
      if(_auto_dump_ready_frame < 0 && _frame_count >= _auto_dump_delay_frames){
         request_frame_capture()
         _auto_dump_ready_frame = _frame_count + 1
      }
   }
   if(!_needs_draw && _redraw_frames <= 0 && !dump_enabled){
      return
   }
   _sync_lost_left_release()
   if(_vterm_selection_dragging() && !window.mouse_down(win, 0)){ _hard_clear_vterm_selection("draw-time lost selection clear") }
   _term_frame_trace("draw")
   if(!begin_frame_clear(_term_bg_color())){
      if(_headless_mode){
         _headless_begin_fail_count += 1
         if(_headless_begin_fail_count >= 30){
            print("[term] headless: begin_frame_clear failed repeatedly; forcing close")
            window.set_should_close(win, true)
         }
      }
      msleep(1)
      return
   }
   _headless_begin_fail_count = 0
   set_ortho_2d(0.0, win_w, 0.0, win_h)
   vt = vterm.draw(vt, win_w, win_h)
   end_frame()
   _frame_count += 1
   if(_redraw_frames > 0){ _redraw_frames -= 1 }
   _term_frame_trace("presented")
   if(dump_enabled && _auto_dump_ready_frame >= 0 && _frame_count > _auto_dump_ready_frame){
      def dump_path = _auto_dump_path.len > 0 ? _auto_dump_path : ospath.join(ospath.temp_dir(), "nytrix-term.png")
      if(snapshot(dump_path)){
         _auto_dump_done = true
         if(_auto_dump_exit_enabled){
            window.set_should_close(win, true)
         }
      } else {
         _auto_dump_attempts += 1
         if(_auto_dump_attempts < 4){
            request_frame_capture()
            _auto_dump_ready_frame = _frame_count + 1
         } else {
            print("[term] framebuffer dump failed: " + dump_path)
            _auto_dump_done = true
            if(_auto_dump_exit_enabled){
               window.set_should_close(win, true)
            }
         }
      }
   }
   def draw_now = ticks()
   _last_cursor_blink_phase = vterm.cursor_blink_phase(vt, draw_now)
   _needs_draw = _redraw_frames > 0 || vterm.needs_visual_refresh(vt, draw_now, _last_cursor_blink_phase)
   if(dump_enabled && !_auto_dump_done){ _needs_draw = true }
}

startup()
mut last_t = ticks()
mut startup_ticks = ticks()
_last_activity_ticks = startup_ticks
mut live_win = _live_window()
_dbg("term", "after startup win_id=0x" + to_hex(win_id) + " win_is_dict=" + to_str(is_dict(win)) + " live_is_dict=" + to_str(is_dict(live_win)) + " closing=" + to_str(live_win ? window.should_close(live_win) : true))
while(live_win && !window.should_close(live_win)){
   win = live_win
   def now = ticks()
   if(_auto_close_if_idle(win, _last_activity_ticks)){
      _dbg("term", "idle timeout close")
   }
   def dt = float(now - last_t) / 1e9
   last_t = now
   update(dt, now)
   draw()
   live_win = _live_window()
}

_dbg("term", "main loop exited frames=" + to_str(_frame_count))
vterm.close(vt)
close_window()

if(is_dict(win)){ window.close(win) }
exit(0)
