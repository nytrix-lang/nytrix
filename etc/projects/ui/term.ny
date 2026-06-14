#!/usr/bin/env ny

;; Keywords: platform probe ui app example term renderer png font
;; Terminal Emulator
use std.core
use std.os (exit)
use std.os.args
use std.os.path as ospath
use std.os.time (ticks, msleep)
use std.os.process as process
use std.os.ui.window.consts
use std.os.ui.render (
   BACKEND_MOCK,
   begin_frame_clear, close_window, end_frame,
   get_active_backend_name, get_swapchain_image_count,
   init_mock_surface, init_window,
   request_frame_capture, set_backend_type, apply_backend_env, apply_backend_argv, set_clear_color, set_win_size,
   snapshot
)

use std.os.ui.window
use std.os.ui.window.platform as platform
use std.os.ui.window.input as ui
use std.os.ui.render.dump as ui_dump
use std.os.ui.render.viewer as view
use std.os.ui.render.viewer.vterm as vterm
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
mut _term_command_mode = false
mut _draw_fail_count = 0
mut _last_draw_fail_warn_ticks = 0
mut _suppress_plus_char_count = 0
mut _suppress_plus_char_until_ticks = 0
mut _font_zoom_plus_down = false
mut _font_zoom_minus_down = false
mut _font_zoom_zero_down = false
mut _font_zoom_hold_dir = 0
mut _font_zoom_pending_delta = 0
mut _font_reset_pending = false
mut _font_zoom_next_ticks = 0
mut _max_events_per_frame = 160
mut _input_event_budget_ns = 2000000
mut _key_update_max = 64
mut _key_update_budget_ns = 4000000
mut _mouse_update_max = 48
mut _mouse_update_budget_ns = 3000000
mut _scroll_update_max = 96
mut _scroll_update_budget_ns = 6000000
mut _pending_mouse_motion = 0

fn _arm_plus_char_suppress() any {
   if _suppress_plus_char_count < 32 {
      _suppress_plus_char_count += 1
   }
   _suppress_plus_char_until_ticks = ticks() + 120000000 ;; 120ms
   0
}

fn _clear_plus_char_suppress() any {
   _suppress_plus_char_count = 0
   _suppress_plus_char_until_ticks = 0
   0
}

fn _consume_zoom_plus_char(any data) bool {
   if _suppress_plus_char_count <= 0 || !is_dict(data) { return false }
   if ticks() > _suppress_plus_char_until_ticks {
      _clear_plus_char_suppress()
      return false
   }
   def ch = int(data.get("char", data.get("codepoint", 0)))
   if ch == 43 || ch == 61 { ;; '+' or '=' from Ctrl+Shift+= / Ctrl+=
      _suppress_plus_char_count -= 1
      if _suppress_plus_char_count <= 0 { _clear_plus_char_suppress() }
      return true
   }
   _clear_plus_char_suppress()
   false
}

fn _consume_terminal_control_char(any data) bool {
   if !is_dict(data) { return false }
   def ch = int(data.get("char", data.get("codepoint", 0)))
   ;; Enter/Return belongs to EVENT_KEY_PRESSED in the terminal path.
   ;; Some backends also emit EVENT_KEY_CHAR with LF/CR for the same physical
   ;; key. Forwarding that char gives the PTY a second newline while holding
   ;; Enter and produces the visual blank-row bursts.
   ch == 10 || ch == 13
}

fn _font_zoom_repeat_ns() int {
   common.env_int_clamped("NY_TERM_FONT_ZOOM_REPEAT_MS", 70, 25, 250) * 1000000
}

fn _set_font_size_live(float next_size) bool {
   def clamped = max(4.0, min(96.0, next_size))
   if clamped == font_size { return false }
   font_size = clamped
   _reload_fonts()
   _request_redraw(_term_redraw_frames())
   true
}

fn _queue_font_zoom(int dir) any {
   if dir == 0 { return 0 }
   _font_zoom_pending_delta = dir
   _font_zoom_hold_dir = dir
   _font_zoom_next_ticks = 0
   0
}

fn _start_font_zoom(int dir) any {
   if dir > 0 {
      if !_font_zoom_plus_down {
         _font_zoom_plus_down = true
         _queue_font_zoom(1)
      } else {
         _font_zoom_hold_dir = 1
      }
   } elif dir < 0 {
      if !_font_zoom_minus_down {
         _font_zoom_minus_down = true
         _queue_font_zoom(-1)
      } else {
         _font_zoom_hold_dir = -1
      }
   }
   0
}

fn _queue_font_reset() any {
   _font_reset_pending = true
   _font_zoom_pending_delta = 0
   _font_zoom_hold_dir = 0
   _font_zoom_next_ticks = 0
   0
}

fn _stop_font_zoom_key(int k) any {
   if k == ui.KEY_EQUAL || k == ui.KEY_KP_ADD {
      _font_zoom_plus_down = false
      if _font_zoom_hold_dir > 0 { _font_zoom_hold_dir = 0 }
   } elif k == ui.KEY_MINUS || k == ui.KEY_KP_SUBTRACT {
      _font_zoom_minus_down = false
      if _font_zoom_hold_dir < 0 { _font_zoom_hold_dir = 0 }
   } elif k == ui.KEY_0 || k == ui.KEY_KP_0 {
      _font_zoom_zero_down = false
   }
   0
}

fn _clear_font_zoom_keys() any {
   _font_zoom_plus_down = false
   _font_zoom_minus_down = false
   _font_zoom_zero_down = false
   _font_zoom_hold_dir = 0
   _font_zoom_pending_delta = 0
   _font_reset_pending = false
   _font_zoom_next_ticks = 0
   0
}

fn _stop_font_zoom_hold() any {
   _font_zoom_plus_down = false
   _font_zoom_minus_down = false
   _font_zoom_zero_down = false
   _font_zoom_hold_dir = 0
   0
}

fn _font_zoom_active() bool {
   _font_zoom_hold_dir != 0 || _font_zoom_pending_delta != 0 || _font_reset_pending
}

fn _apply_font_zoom_tick(any now_ticks) bool {
   if _font_reset_pending {
      _font_reset_pending = false
      _font_zoom_next_ticks = now_ticks + _font_zoom_repeat_ns()
      return _set_font_size_live(START_FONT_SIZE)
   }
   mut dir = 0
   if _font_zoom_pending_delta != 0 {
      dir = _font_zoom_pending_delta
      _font_zoom_pending_delta = 0
   } elif _font_zoom_hold_dir != 0 && now_ticks >= _font_zoom_next_ticks {
      dir = _font_zoom_hold_dir
   }
   if dir == 0 { return false }
   def changed = _set_font_size_live(font_size + float(dir))
   _font_zoom_next_ticks = now_ticks + _font_zoom_repeat_ns()
   changed
}

fn _term_color(str code, str text) str {
   if common.env_truthy("NO_COLOR") { return text }
   def esc = chr(27)
   esc + "[" + code + "m" + text + esc + "[0m"
}

fn _term_diag(str label, any value) any {
   eprint("  " + _term_color("90", label + ": ") + to_str(value))
   0
}

fn _term_has_help_arg() bool {
   mut i = 1
   while i < argc() {
      def a = str.lower(str.strip(to_str(argv(i))))
      if a == "-h" || a == "--help" || a == "help" { return true }
      i += 1
   }
   false
}

fn _term_help_line(str left, str right) any {
   print("  " + _term_color("1;36", left) + "  " + right)
   0
}

fn _term_print_help() any {
   print(_term_color("1;37", "Nytrix Terminal"))
   print(_term_color("90", "GPU terminal emulator / shell runner"))
   print("")
   print(_term_color("1;33", "Usage"))
   print("  ./make ny etc/projects/ui/term.ny " + _term_color("36", "[options]") + " " + _term_color("90", "[shell command]") )
   print("")
   print(_term_color("1;33", "Renderer"))
   _term_help_line("-gl, --gl", "use OpenGL")
   _term_help_line("-vk, --vk", "use Vulkan")
   _term_help_line("-auto", "auto-select renderer")
   _term_help_line("-mock, -cpu", "software/headless mock renderer")
   _term_help_line("--window", "force GUI terminal even for simple commands")
   _term_help_line("--direct", "force direct host-terminal command passthrough")
   print("")
   print(_term_color("1;33", "Debug"))
   _term_help_line("-v, --verbose", "bounded startup/input/render diagnostics")
   _term_help_line("-vv", "compact deep diagnostics/profiler summaries")
   _term_help_line("--trace-spam", "last-resort per-stage/per-glyph/per-frame tracing")
   print("")
   print(_term_color("1;33", "Useful env"))
   _term_help_line("NY_TERM_FONT_REG=path", "regular font override")
   _term_help_line("NY_TERM_EMOJI=0|1", "toggle emoji fallback")
   _term_help_line("NY_TERM_UPDATE_BUDGET_MS=n", "PTY update budget per frame")
   _term_help_line("NY_TERM_STAY_OPEN_ON_EXIT=1", "keep window open after shell exits")
   _term_help_line("NY_TERM_DIRECT=0|1", "toggle direct passthrough for non-interactive commands")
   _term_help_line("NY_UI_HEADLESS=1", "headless/mock surface mode")
   _term_help_line("NY_VK_SAFE_TEXT=1", "force slow Vulkan text fallback for driver debugging")
   _term_help_line("NY_VK_FAST_TEXT=0", "disable Vulkan atlas fast text")
   _term_help_line("NY_VK_TEXTURE_TEXT=1", "debug Vulkan texture-atlas TTF path")
   print("")
   print(_term_color("1;33", "Examples"))
   print("  ./make ny etc/projects/ui/term.ny -v -gl")
   print("  ./make ny etc/projects/ui/term.ny -vk htop")
   print("  ./make ny etc/projects/ui/term.ny id")
   print("  ./make ny etc/projects/ui/term.ny --window id")
   0
}

fn _term_force_window_arg() bool {
   mut i = 1
   while i < argc() {
      def a = str.lower(str.strip(to_str(argv(i))))
      if a == "--window" || a == "--gui" || a == "--term" || a == "--terminal" || a == "--no-direct" { return true }
      i += 1
   }
   common.env_truthy("NY_TERM_FORCE_WINDOW") || common.env_truthy("NY_TERM_GUI_COMMAND")
}

fn _term_force_direct_arg() bool {
   mut i = 1
   while i < argc() {
      def a = str.lower(str.strip(to_str(argv(i))))
      if a == "--direct" || a == "--run-direct" || a == "--host" { return true }
      i += 1
   }
   common.env_truthy("NY_TERM_DIRECT")
}

fn _term_command_line() str {
   mut line = ""
   mut passthrough = false
   mut i = 1
   while i < argc() {
      def arg = to_str(argv(i))
      if passthrough {
         if line.len > 0 { line = line + " " }
         line = line + arg
      } elif arg == "--" {
         passthrough = true
      } elif !_term_backend_flag(arg) {
         if line.len > 0 { line = line + " " }
         line = line + arg
      }
      i += 1
   }
   str.strip(line)
}

fn _term_command_first_arg() str {
   mut passthrough = false
   mut i = 1
   while i < argc() {
      def arg = to_str(argv(i))
      if passthrough { return str.lower(str.strip(arg)) }
      if arg == "--" {
         passthrough = true
      } elif !_term_backend_flag(arg) {
         return str.lower(str.strip(arg))
      }
      i += 1
   }
   ""
}

fn _term_interactive_command(str cmd) bool {
   def c = str.lower(str.strip(cmd))
   c == "" ||
   c == "bash" || c == "zsh" || c == "fish" || c == "sh" || c == "tmux" || c == "screen" ||
   c == "htop" || c == "btop" || c == "top" || c == "nvim" || c == "vim" || c == "vi" || c == "nano" ||
   c == "less" || c == "more" || c == "man" || c == "ssh" || c == "ftp" || c == "sftp" ||
   c == "python" || c == "python3" || c == "node" || c == "lua" || c == "ny"
}

fn _term_should_direct_command() bool {
   def line = _term_command_line()
   if line.len == 0 { return false }
   if _term_force_window_arg() { return false }
   if _term_force_direct_arg() { return true }
   if common.env_present("NY_TERM_DIRECT") { return common.env_truthy("NY_TERM_DIRECT") }
   !_term_interactive_command(_term_command_first_arg())
}

fn _term_run_direct_command() int {
   def line = _term_command_line()
   if line.len == 0 { return 0 }
   mut sh = "/bin/sh"
   #windows { sh = "cmd" } #endif
   mut args = [sh, "-lc", line]
   #windows { args = ["cmd", "/c", line] } #endif
   process.run(sh, args)
}

fn _term_redraw_frames() int {
   if common.env_present("NY_TERM_REDRAW_FRAMES") {
      return common.env_int_clamped("NY_TERM_REDRAW_FRAMES", 4, 2, 64)
   }
   mut frames = 2
   def swap_images = get_swapchain_image_count()
   if swap_images > 0 { frames = min(max(2, swap_images), 4) }
   frames
}

fn _term_bg_color() list {
   def c = int(vt.get("def_bg", 0xff000000))
   vterm.abgr_to_color((c & 0x00ffffff) | 0xff000000)
}

fn _request_redraw(int frames=4) any {
   if frames > _redraw_frames { _redraw_frames = frames }
   _needs_draw = true
   0
}

fn _warn_draw_failure(str msg, any now_ticks) any {
   if now_ticks - _last_draw_fail_warn_ticks < 1000000000 { return 0 }
   eprint(_term_color("33", "[term] warning: ") + msg)
   _last_draw_fail_warn_ticks = now_ticks
   0
}

fn _term_print_startup_failure(int flags) any {
   eprint(_term_color("1;37", "[term] graphics startup failed"))
   _term_diag("window backend", window.backend())
   _term_diag("renderer", get_active_backend_name())
   _term_diag("flags", "0x" + to_hex(flags))
   _term_diag("DISPLAY", common.value_or(common.env_trim("DISPLAY"), "<unset>"))
   _term_diag("WAYLAND_DISPLAY", common.value_or(common.env_trim("WAYLAND_DISPLAY"), "<unset>"))
   def requested = common.env_trim("NY_UI_BACKEND")
   if requested.len > 0 { _term_diag("NY_UI_BACKEND", requested) }
   def x11_surface = common.env_trim("NY_UI_X11_VK_SURFACE")
   if x11_surface.len > 0 { _term_diag("NY_UI_X11_VK_SURFACE", x11_surface) }
   0
}

fn _open_windowed(any title, int w, int h, int flags=0, bool raw=true, bool cpu=false, int msaa=0) any {
   def opened = init_window(w, h, title, flags, raw, cpu, msaa)
   if opened && !common.env_truthy("NY_UI_HEADLESS") { window.focus(opened) }
   opened
}

fn _auto_close_if_idle(any win, any last_ticks, int default_ns=0) bool {
   def raw = common.env_trim("NY_UI_TIMEOUT")
   def limit = raw.len > 0 ? int(str.atof(raw) * 1e9) : default_ns
   if limit <= 0 || ticks() - last_ticks < limit { return false }
   window.set_should_close(win, true)
   true
}

fn _live_window() any {
   def last_win = window.last()
   if is_dict(last_win) { return last_win }
   if is_dict(win) { return win }
   if win_id { return window.get_win(win_id) }
   0
}

fn _font_map() dict {
   {
      "regular": font,
      "bold": font_bold,
      "italic": font_italic,
      "emoji": font_emoji,
   }
}

fn _sync_framebuffer_size() bool {
   def fb = view.framebuffer_size(win, win_w, win_h)
   def nw = float(fb.get(0, win_w))
   def nh = float(fb.get(1, win_h))
   if nw <= 0.0 || nh <= 0.0 { return false }
   if int(nw) == int(win_w) && int(nh) == int(win_h) { return false }
   win_w = nw
   win_h = nh
   _resize_term()
   true
}

fn _sync_framebuffer_size_if_due(any now_ticks, bool force=false) bool {
   if !force && now_ticks < _fb_sync_next_ticks { return false }
   _fb_sync_next_ticks = now_ticks + 16000000 ;; ~16ms
   _sync_framebuffer_size()
}

fn _refresh_update_budget(any now_ticks) any {
   if now_ticks < _update_budget_reload_ticks { return 0 }
   mut budget_ms = 6.0
   def budget_env = common.env_trim("NY_TERM_UPDATE_BUDGET_MS")
   if budget_env.len > 0 {
      def bv = str.atof(budget_env)
      if bv >= 1.0 && bv <= 33.0 { budget_ms = bv }
   }
   _update_budget_ns = int(budget_ms * 1000000.0)
   _max_events_per_frame = common.env_int_clamped("NY_TERM_MAX_EVENTS_PER_FRAME", 128, 32, 1024)
   _input_event_budget_ns = common.env_int_clamped("NY_TERM_INPUT_EVENT_BUDGET_MS", 1, 1, 12) * 1000000
   _key_update_max = common.env_int_clamped("NY_TERM_KEY_UPDATE_MAX", 64, 16, 512)
   _key_update_budget_ns = common.env_int_clamped("NY_TERM_KEY_UPDATE_BUDGET_MS", 4, 1, 24) * 1000000
   _mouse_update_max = common.env_int_clamped("NY_TERM_MOUSE_UPDATE_MAX", 48, 16, 512)
   _mouse_update_budget_ns = common.env_int_clamped("NY_TERM_MOUSE_UPDATE_BUDGET_MS", 3, 1, 24) * 1000000
   _scroll_update_max = common.env_int_clamped("NY_TERM_SCROLL_UPDATE_MAX", 96, 16, 512)
   _scroll_update_budget_ns = common.env_int_clamped("NY_TERM_SCROLL_UPDATE_BUDGET_MS", 6, 1, 24) * 1000000
   _update_budget_reload_ticks = now_ticks + 1000000000
   0
}

fn _refresh_runtime_flags(any now_ticks) any {
   if now_ticks < _runtime_flags_reload_ticks { return 0 }
   _esc_close_enabled = common.env_present("NY_TERM_ESC_CLOSE")
   _stay_open_on_exit = common.env_present("NY_TERM_STAY_OPEN_ON_EXIT") ? common.env_truthy("NY_TERM_STAY_OPEN_ON_EXIT") : _term_command_mode
   _runtime_flags_reload_ticks = now_ticks + 1000000000
   0
}

fn _refresh_dump_flags() any {
   _headless_mode = common.env_truthy("NY_UI_HEADLESS")
   _auto_dump_requested = ui_dump.auto_dump_enabled() || _headless_mode
   _auto_dump_exit_enabled = _headless_mode || common.env_truthy("NYTRIX_AUTO_DUMP_EXIT")
   _auto_dump_delay_frames = ui_dump.auto_dump_delay_frames(_headless_mode ? 0 : 8)
   if _auto_dump_delay_frames < 0 { _auto_dump_delay_frames = 0 }
   _auto_dump_path = ui_dump.auto_dump_path("")
   0
}

fn startup() {
   _refresh_dump_flags()
   if _headless_mode {
      platform.init_hint(platform.PLATFORM, platform.PLATFORM_NULL)
      set_backend_type(BACKEND_MOCK)
   } else {
      apply_backend_env()
      apply_backend_argv()
   }
   def mode = window.primary_mode()
   def screen_w = mode.get(0, 1280)
   def screen_h = mode.get(1, 720)
   def default_sz = window.default_window_size(screen_w, screen_h)
   def start_w = default_sz.get(0, 1280)
   def start_h = default_sz.get(1, 720)
   def flags = WINDOW_ALLOW_DND | WINDOW_CENTER | WINDOW_FOCUS_ON_SHOW
   font_size = view.default_font_size("term", START_FONT_SIZE)
   if _headless_mode {
      win = window.open_window("Nytrix Terminal", 0, 0, start_w, start_h, flags | WINDOW_HIDE | WINDOW_NO_RESIZE)
      if win && !init_mock_surface(start_w, start_h) {
         window.close(win)
         win = 0
      }
   } else {
      win = _open_windowed("Nytrix Terminal", start_w, start_h, flags, true, false, 0)
   }
   if !win {
      _term_print_startup_failure(flags | WINDOW_VULKAN)
      exit(1)
   }
   win_id = window.id(win)
   set_clear_color([0.0, 0.0, 0.0, 1.0])
   window.set_exit_key(win, KEY_NULL)
   window.set_input_exclusive(win, true)
   _reload_fonts()
   def sz = view.framebuffer_size(win, start_w, start_h)
   win_w = float(sz.get(0, start_w))
   win_h = float(sz.get(1, start_h))
   _has_framebuffer_transparency = false
   _init_vt()
   window.set_cursor_mode(win, window.CURSOR_NORMAL)
   _resize_term()
   _last_title = vterm.get_title(vt)
   _fb_sync_next_ticks = ticks()
   _refresh_update_budget(ticks())
   _refresh_runtime_flags(ticks())
   _needs_draw = true
   _request_redraw(_term_redraw_frames())
   _last_cursor_blink_phase = vterm.cursor_blink_phase(vt, ticks())
   _auto_dump_ready_frame = -1
   _auto_dump_attempts = 0
   _selftest_input()
   win = window.get_win(win)
}

fn _init_vt() {
   def cell = view.terminal_cell_size(font, font_size)
   def cw = float(cell.get(0, font_size * 0.6))
   def ch = float(cell.get(1, max(font_size, 20.0)))
   mut cols = int(win_w / cw)
   mut rows = int(win_h / ch)
   if cols <= 0 { cols = 80 } if rows <= 0 { rows = 24 }
   vt = vterm.new(cols, rows, _font_map(), 0, 0)
   if !vt || !is_dict(vt) {
      print("[term] failed to allocate terminal state")
      exit(1)
   }
   vt = vt.set("char_w", cw).set("char_h", ch).set("px_w", int(float(cols) * cw)).set("px_h", int(float(rows) * ch)).set("window_id", win_id)
   def launch = _term_launch_spec()
   def shell_path = launch.get("path", vterm.default_shell_path())
   def shell_args = launch.get("args", vterm.default_shell_args(common.env_truthy("NY_TERM_LOGIN")))
   _term_command_mode = bool(launch.get("command", false))
   match vterm.open(vt, shell_path, shell_args) {
      ok(next_vt) -> { vt = next_vt }
      err(e) -> {
         print("[term] failed to open vterm: " + to_str(e))
         exit(1)
      }
   }
   set_clear_color(_term_bg_color())
}

fn _selftest_input() any {
   if !common.env_truthy("NY_TERM_SELFTEST_INPUT") { return 0 }
   vt = vterm.handle_event(vt, EVENT_KEY_PRESSED, {"key": ui.KEY_A, "action": 1, "mod": 0, "ww": win_w, "wh": win_h})
   if !common.env_truthy("NY_TERM_SELFTEST_KEY_ONLY") {
      vt = vterm.handle_event(vt, EVENT_KEY_CHAR, {"char": 97, "mod": 0, "plain": true, "ww": win_w, "wh": win_h})
   }
   0
}

fn _term_launch_spec() dict {
   def login_pref = common.env_truthy("NY_TERM_LOGIN")
   def shell_path = vterm.default_shell_path()
   def line = _term_command_line()
   if line.len == 0 { return {"path": shell_path, "args": vterm.default_shell_args(login_pref), "command": false} }
   return {"path": shell_path, "args": ["-lc", line], "command": true}
}

fn _term_backend_flag(any arg) bool {
   def a = str.lower(str.strip(to_str(arg)))
   a == "-gl" || a == "--gl" ||
   a == "-opengl" || a == "--opengl" ||
   a == "-webgl" || a == "--webgl" ||
   a == "-vk" || a == "--vk" ||
   a == "-vulkan" || a == "--vulkan" ||
   a == "-mock" || a == "--mock" ||
   a == "-cpu" || a == "--cpu" ||
   a == "-software" || a == "--software" ||
   a == "-auto" || a == "--auto" || a == "--render-auto" ||
   a == "-h" || a == "--help" || a == "help" ||
   a == "-v" || a == "--verbose" || a == "-vv" || a == "-vvv" || a == "--debug" ||
   a == "--debug-deep" || a == "-trace" || a == "--trace" ||
   a == "-trace-ui" || a == "--trace-ui" || a == "--trace-spam" ||
   a == "--window" || a == "--gui" || a == "--term" || a == "--terminal" || a == "--no-direct" ||
   a == "--direct" || a == "--run-direct" || a == "--host"
}

fn _reload_fonts() {
   def reg_path = common.env_trim("NY_TERM_FONT_REG")
   def bold_path = common.env_trim("NY_TERM_FONT_BOLD")
   def ital_path = common.env_trim("NY_TERM_FONT_ITAL")
   def emoji_path = common.env_trim("NY_TERM_FONT_EMOJI")
   def font_filter = view.default_font_filter("term", FONT_FILTER_NEAREST)
   def emoji_filter = view.default_font_filter("term", FONT_FILTER_LINEAR, "EMOJI_FONT_FILTER")
   def emoji_on = common.env_present("NY_TERM_EMOJI") ? common.env_enabled("NY_TERM_EMOJI") : true
   def fonts = view.terminal_font_map(font_size, font_filter, emoji_filter, reg_path, bold_path, ital_path, emoji_path, emoji_on)
   font = fonts.get("regular", 0)
   font_bold = fonts.get("bold", font)
   font_italic = fonts.get("italic", font)
   font_emoji = fonts.get("emoji", font)
   if vt != 0 {
      vt = vt.set("fonts", _font_map())
      _resize_term()
   }
}

fn _scale_event_xy(any ev_data) any {
   if !is_dict(ev_data) || !ev_data.contains("x") || !ev_data.contains("y") { return ev_data }
   ui.scale_event_xy(win, ev_data, win_w, win_h)
}

fn _vterm_event_data(int typ, any data) any {
   mut ev_data = data
   if typ == EVENT_MOUSE_SCROLL && is_list(ev_data) {
      ev_data = {
         "dx": float(ev_data.get(0, 0.0)),
         "dy": float(ev_data.get(1, 0.0)),
         "scrolling": true,
      }
   }
   if is_dict(ev_data) {
      ev_data = _scale_event_xy(ev_data)
      if typ == EVENT_KEY_PRESSED || typ == EVENT_KEY_RELEASED {
         ev_data = ev_data.set("key", ui.normalize_key(ev_data.get("key", 0))).set("mod", view.normalize_mod(ev_data.get("mod", ev_data.get("mods", 0))))
      }
      if ui.is_mouse_event(typ) {
         def event_button = int(ev_data.get("button", -1))
         def native_left = ev_data.contains("left_down")
         def left_down = native_left ? !!ev_data.get("left_down", false) : _left_down_for_event(typ, event_button)
         if left_down { _last_left_down_ticks = ticks() }
         if !ev_data.contains("middle_down") { ev_data = ev_data.set("middle_down", window.mouse_down(win, 2)) }
         if !ev_data.contains("right_down") { ev_data = ev_data.set("right_down", window.mouse_down(win, 1)) }
         ev_data = ev_data.set("left_down", left_down)
      }
      ev_data = ev_data.set("ww", win_w).set("wh", win_h)
   }
   ev_data
}

fn _left_down_for_event(int typ, int event_button) bool {
   (typ == EVENT_MOUSE_BUTTON_PRESSED && event_button == 0) ||
   (!(typ == EVENT_MOUSE_BUTTON_RELEASED && event_button == 0) && (_left_button_was_down || window.mouse_down(win, 0)))
}

fn _hard_clear_vterm_selection() bool {
   if !vterm.clear_selection(vt) { return false }
   _left_button_was_down = false
   _last_left_down_ticks = 0
   _request_redraw(_term_redraw_frames())
   true
}

fn _force_left_release(any raw=0) bool {
   if !is_dict(vt) { return false }
   mut data = raw
   if !is_dict(data) {
      def p = window.cursor_pos(win)
      data = {
         "button": 0,
         "x": float(p.get(0, 0.0)),
         "y": float(p.get(1, 0.0)),
      }
   }
   data = data.set("button", 0).set("left_down", false)
   if !data.contains("middle_down") { data = data.set("middle_down", window.mouse_down(win, 2)) }
   if !data.contains("right_down") { data = data.set("right_down", window.mouse_down(win, 1)) }
   data = _vterm_event_data(EVENT_MOUSE_BUTTON_RELEASED, data)
   vt = vterm.handle_event(vt, EVENT_MOUSE_BUTTON_RELEASED, data)
   _left_button_was_down = false
   if vterm.selection_dragging(vt) && !vt.get("sticky_selection", false) { _hard_clear_vterm_selection() }
   _request_redraw(_term_redraw_frames())
   true
}

fn _sync_lost_left_release() bool {
   if !is_dict(vt) { return false }
   def down_now = window.mouse_down(win, 0)
   if (_left_button_was_down || vterm.selection_dragging(vt)) && !down_now {
      def age_ns = ticks() - _last_left_down_ticks
      def debounce_ns = _left_release_debounce_ns()
      if age_ns < debounce_ns { return false }
      return _force_left_release()
   }
   false
}

fn _left_release_debounce_ns() int {
   common.env_int_clamped("NY_TERM_LOST_RELEASE_DEBOUNCE_MS", 45, 0, 500) * 1000000
}

fn _maybe_synthesize_left_release_from_motion(int typ, any ev_data) bool {
   if typ != EVENT_MOUSE_POS_CHANGED || !is_dict(ev_data) { return false }
   if !_left_button_was_down || !!ev_data.get("left_down", true) { return false }
   if ticks() - _last_left_down_ticks < _left_release_debounce_ns() { return false }
   mut release_data = ev_data
   release_data = release_data.set("button", 0).set("left_down", false)
   vt = vterm.handle_event(vt, EVENT_MOUSE_BUTTON_RELEASED, release_data)
   _left_button_was_down = false
   _request_redraw(_term_redraw_frames())
   true
}

fn _update_left_button_latch(int typ, any ev_data) any {
   if !is_dict(ev_data) { return 0 }
   def event_button = int(ev_data.get("button", -1))
   if typ == EVENT_MOUSE_BUTTON_PRESSED && event_button == 0 {
      _left_button_was_down = true
   } elif typ == EVENT_MOUSE_BUTTON_RELEASED && event_button == 0 {
      _left_button_was_down = false
   } elif typ == EVENT_MOUSE_POS_CHANGED && !!ev_data.get("left_down", false) {
      _left_button_was_down = true
   }
   0
}

fn _clear_pending_mouse_events() any {
   _pending_mouse_motion = 0
   0
}

fn _queue_mouse_motion(any data) any {
   _pending_mouse_motion = data
   0
}

fn _prepare_scroll_mouse_state() any {
   if window.mouse_down(win, 0) { return 0 }
   _left_button_was_down = false
   _last_left_down_ticks = 0
   if vt != 0 && is_dict(vt) && vterm.selection_dragging(vt) && !vt.get("sticky_selection", false) {
      if vterm.clear_selection(vt) { _request_redraw(1) }
   }
   0
}

fn _scroll_event_data(any data) any {
   mut scroll_data = _vterm_event_data(EVENT_MOUSE_SCROLL, data)
   if is_dict(scroll_data) {
      scroll_data = scroll_data.set("left_down", false).set("middle_down", false).set("right_down", false)
   }
   scroll_data
}

fn _dispatch_mouse_scroll(any data) bool {
   _prepare_scroll_mouse_state()
   vt = vterm.handle_event(vt, EVENT_MOUSE_SCROLL, _scroll_event_data(data))
   _request_redraw(1)
   true
}

fn _flush_pending_mouse_events() bool {
   mut did = false
   if is_dict(_pending_mouse_motion) {
      def motion_data = _vterm_event_data(EVENT_MOUSE_POS_CHANGED, _pending_mouse_motion)
      if _maybe_synthesize_left_release_from_motion(EVENT_MOUSE_POS_CHANGED, motion_data) { did = true }
      vt = vterm.handle_event(vt, EVENT_MOUSE_POS_CHANGED, motion_data)
      _update_left_button_latch(EVENT_MOUSE_POS_CHANGED, motion_data)
      _pending_mouse_motion = 0
      did = true
   }
   did
}

fn _event_budget_done(any event_start_ticks, int event_count) bool {
   event_count > 0 && (event_count % 16) == 0 && (ticks() - event_start_ticks) >= _input_event_budget_ns
}

fn _handle_app_key(int k, int md) bool {
   if _esc_close_enabled && k == ui.KEY_ESCAPE && (md & (MOD_SHIFT|MOD_CONTROL|MOD_ALT|MOD_SUPER|MOD_META)) == 0 && vt != 0 {
      def st = vt.get("state", 0)
      if st != 0 {
         def mode = load32(st, 32)
         if (mode & 2) == 0 {
            def now_ms = int(ticks() / 1000000)
            if last_esc_ms != 0 && (now_ms - last_esc_ms) < 400 {
               ui_dump.close_with_dump(win)
               return true
            }
            last_esc_ms = now_ms
         } else {
            last_esc_ms = 0
         }
      }
   }
   if (md & MOD_CONTROL) != 0 {
      if k == ui.KEY_EQUAL || k == ui.KEY_KP_ADD {
         _arm_plus_char_suppress()
         _start_font_zoom(1)
         return true
      }
      if k == ui.KEY_MINUS || k == ui.KEY_KP_SUBTRACT {
         _start_font_zoom(-1)
         return true
      }
      if k == ui.KEY_0 || k == ui.KEY_KP_0 {
         if !_font_zoom_zero_down {
            _font_zoom_zero_down = true
            _queue_font_reset()
         }
         return true
      }
   }
   false
}

fn update(now_ticks) {
   def live = _live_window()
   if is_dict(live) { win = live }
   window.poll_events()
   _refresh_update_budget(now_ticks)
   _refresh_runtime_flags(now_ticks)
   _sync_framebuffer_size_if_due(now_ticks)
   mut e = window.check_event(win)
   mut event_count = 0
   mut input_hot = false
   mut key_input_hot = false
   mut mouse_input_hot = false
   mut scroll_input_hot = false
   def max_events = _max_events_per_frame
   def event_start = ticks()
   while e != 0 && event_count < max_events {
      def typ = window.event_type(e)
      def data = window.event_data(e)
      if typ == EVENT_MOUSE_LEAVE || typ == EVENT_FOCUS_OUT {
         _clear_pending_mouse_events()
      } elif typ != EVENT_MOUSE_POS_CHANGED && typ != EVENT_MOUSE_SCROLL {
         if _flush_pending_mouse_events() {
            input_hot = true
            mouse_input_hot = true
         }
      }
      if typ == EVENT_WINDOW_RESIZED {
         _sync_framebuffer_size_if_due(now_ticks, true)
      } elif typ == EVENT_KEY_PRESSED {
         def k = ui.normalize_key(data.get("key", 0))
         def md = view.normalize_mod(data.get("mod", data.get("mods", 0)))
         input_hot = true
         key_input_hot = true
         if _handle_app_key(k, md) {
            _request_redraw(1)
            e = window.check_event(win)
            event_count += 1
            if _event_budget_done(event_start, event_count) { break }
            continue
         }
      } elif typ == EVENT_KEY_RELEASED {
         def k = ui.normalize_key(data.get("key", 0))
         def md = view.normalize_mod(data.get("mod", data.get("mods", 0)))
         _stop_font_zoom_key(k)
         if (md & MOD_CONTROL) == 0 {
            _stop_font_zoom_hold()
            _clear_plus_char_suppress()
         }
      } elif typ == EVENT_KEY_CHAR {
         input_hot = true
         key_input_hot = true
         if _consume_zoom_plus_char(data) || _consume_terminal_control_char(data) {
            _request_redraw(1)
            e = window.check_event(win)
            event_count += 1
            if _event_budget_done(event_start, event_count) { break }
            continue
         }
      } elif typ == EVENT_MOUSE_POS_CHANGED {
         _queue_mouse_motion(data)
         input_hot = true
         mouse_input_hot = true
         e = window.check_event(win)
         event_count += 1
         if _event_budget_done(event_start, event_count) { break }
         continue
      } elif typ == EVENT_MOUSE_SCROLL {
         if _flush_pending_mouse_events() {
            input_hot = true
            mouse_input_hot = true
         }
         _dispatch_mouse_scroll(data)
         input_hot = true
         mouse_input_hot = true
         scroll_input_hot = true
         e = window.check_event(win)
         event_count += 1
         if _event_budget_done(event_start, event_count) { break }
         continue
      }
      if ui.is_input_event(typ) { input_hot = true }
      if ui.is_mouse_event(typ) { mouse_input_hot = true }
      def ev_data = _vterm_event_data(typ, data)
      if typ == EVENT_MOUSE_LEAVE || typ == EVENT_FOCUS_OUT {
         _left_button_was_down = false
         _last_left_down_ticks = 0
         _clear_font_zoom_keys()
         _clear_plus_char_suppress()
      }
      if _maybe_synthesize_left_release_from_motion(typ, ev_data) {
         event_count += 1
         input_hot = true
         mouse_input_hot = true
      }
      vt = vterm.handle_event(vt, typ, ev_data)
      if typ == EVENT_MOUSE_LEAVE || typ == EVENT_FOCUS_OUT { _hard_clear_vterm_selection() }
      if ui.is_mouse_event(typ) { _update_left_button_latch(typ, ev_data) }
      if window.quit(e) {
         window.set_should_close(win, true)
      }
      e = window.check_event(win)
      event_count += 1
      if _event_budget_done(event_start, event_count) { break }
   }
   if _flush_pending_mouse_events() {
      event_count += 1
      input_hot = true
      mouse_input_hot = true
   }
   if e != 0 {
      input_hot = true
      _request_redraw(1)
   }
   if _apply_font_zoom_tick(ticks()) {
      event_count += 1
      input_hot = true
   }
   if _sync_lost_left_release() {
      event_count += 1
      input_hot = true
      mouse_input_hot = true
   }
   mut updates = 0
   mut last_update_bytes = 0
   mut pty_backlog_hot = false
   def update_start = ticks()
   mut max_updates = 64
   mut budget_ns = _update_budget_ns
   if input_hot && !(key_input_hot || mouse_input_hot || scroll_input_hot) {
      max_updates = 16
      if budget_ns > 4000000 { budget_ns = 4000000 }
   } elif scroll_input_hot {
      max_updates = _scroll_update_max
      if budget_ns < _scroll_update_budget_ns { budget_ns = _scroll_update_budget_ns }
   } elif key_input_hot || mouse_input_hot {
      if key_input_hot {
         max_updates = _key_update_max
         if budget_ns < _key_update_budget_ns { budget_ns = _key_update_budget_ns }
      } else {
         max_updates = _mouse_update_max
         if budget_ns < _mouse_update_budget_ns { budget_ns = _mouse_update_budget_ns }
      }
   }
   while updates < max_updates {
      mut nvt = vterm.update(vt)
      def update_bytes = int(nvt.get("last_update_bytes", 0))
      last_update_bytes = update_bytes
      vt = nvt
      if update_bytes <= 0 { break }
      updates += 1
      if (ticks() - update_start) >= budget_ns {
         pty_backlog_hot = true
         break
      }
   }
   if updates >= max_updates && last_update_bytes > 0 { pty_backlog_hot = true }
   def cur_title = vterm.get_title(vt)
   if cur_title != _last_title {
      window.set_title(win, cur_title)
      _last_title = cur_title
   }
   if !vterm.is_running(vt) {
      if !_shell_exit_reported {
         if common.env_truthy("NY_TERM_REPORT_EXIT") || ui_dump.debug_verbose_enabled() {
            eprint("[term] shell exited")
         }
         _shell_exit_reported = true
         _request_redraw(_term_redraw_frames())
      }
      if !_stay_open_on_exit {
         window.set_should_close(win, true)
      }
   }
   def activity = event_count > 0 || updates > 0
   _needs_draw = activity
   if activity { _request_redraw(1) }
   if vterm.needs_visual_refresh(vt, now_ticks, _last_cursor_blink_phase) { _request_redraw(_term_redraw_frames()) }
   if _redraw_frames > 0 { _needs_draw = true }
   if _font_zoom_active() { _needs_draw = true }
   if pty_backlog_hot { _request_redraw(1) }
   if activity { _last_activity_ticks = now_ticks }
   if !_needs_draw { msleep(vterm.idle_sleep_ms(vt, now_ticks)) }
}

fn _resize_term() {
   def cell = view.terminal_cell_size(font, font_size)
   def cw = float(cell.get(0, float(font_size) * 0.6))
   def ch = float(cell.get(1, max(float(font_size), 20.0)))
   def cols = int(win_w / cw)
   def rows = int(win_h / ch)
   set_win_size(int(win_w), int(win_h))
   if cols <= 0 || rows <= 0 { return }
   vt = vterm.resize(vt, cols, rows)
   vt = vt.set("char_w", cw).set("char_h", ch).set("px_w", int(float(cols) * cw)).set("px_h", int(float(rows) * ch))
}

fn draw() {
   def live = _live_window()
   if is_dict(live) { win = live }
   def dump_enabled = _auto_dump_requested && !_auto_dump_done
   if dump_enabled {
      if _auto_dump_ready_frame < 0 && _frame_count >= _auto_dump_delay_frames {
         request_frame_capture()
         _auto_dump_ready_frame = _frame_count + 1
      }
   }
   if !_needs_draw && _redraw_frames <= 0 && !dump_enabled {
      return
   }
   _sync_lost_left_release()
   if vterm.selection_dragging(vt) && !window.mouse_down(win, 0) { _hard_clear_vterm_selection() }
   if !begin_frame_clear(_term_bg_color()) {
      _draw_fail_count += 1
      if _headless_mode {
         _headless_begin_fail_count += 1
         if _headless_begin_fail_count >= 30 {
            print("[term] headless: begin_frame_clear failed repeatedly; forcing close")
            window.set_should_close(win, true)
         }
      } else {
         _warn_draw_failure("begin_frame_clear failed on backend=" + get_active_backend_name() + "; retrying instead of crashing", ticks())
      }
      msleep(1)
      return
   }
   _headless_begin_fail_count = 0
   _draw_fail_count = 0
   set_ortho_2d(0.0, win_w, 0.0, win_h)
   vt = vterm.draw(vt, win_w, win_h)
   end_frame()
   _frame_count += 1
   if _redraw_frames > 0 { _redraw_frames -= 1 }
   if dump_enabled && _auto_dump_ready_frame >= 0 && _frame_count > _auto_dump_ready_frame {
      def dump_path = _auto_dump_path.len > 0 ? _auto_dump_path : ospath.join(ospath.temp_dir(), "nytrix-term.png")
      if snapshot(dump_path) {
         _auto_dump_done = true
         if _auto_dump_exit_enabled {
            window.set_should_close(win, true)
         }
      } else {
         _auto_dump_attempts += 1
         if _auto_dump_attempts < 4 {
            request_frame_capture()
            _auto_dump_ready_frame = _frame_count + 1
         } else {
            print("[term] framebuffer dump failed: " + dump_path)
            _auto_dump_done = true
            if _auto_dump_exit_enabled {
               window.set_should_close(win, true)
            }
         }
      }
   }
   def draw_now = ticks()
   _last_cursor_blink_phase = vterm.cursor_blink_phase(vt, draw_now)
   _needs_draw = _redraw_frames > 0 || vterm.needs_visual_refresh(vt, draw_now, _last_cursor_blink_phase)
   if dump_enabled && !_auto_dump_done { _needs_draw = true }
}

if _term_has_help_arg() { _term_print_help() exit(0) }
if _term_should_direct_command() { exit(_term_run_direct_command()) }
startup()
def startup_ticks = ticks()
_last_activity_ticks = startup_ticks
mut live_win = _live_window()
while live_win && !window.should_close(live_win) {
   win = live_win
   def now = ticks()
   _auto_close_if_idle(win, _last_activity_ticks)
   update(now)
   draw()
   live_win = _live_window()
}

vterm.close(vt)
close_window()

if is_dict(win) { window.close(win) }
exit(0)
