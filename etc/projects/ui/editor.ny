#!/usr/bin/env ny

;; Keywords: ui editor text panes syntax highlight example
;; Full editor testbed: buffers, highlighting, pane drag, mouse editing, clipboard, save, and drop-open.
use std.core
use std.core.common as common
use std.core.str as str
use std.math (max, min)
use std.math.crypto.encoding.base as b64
use std.os (exit, file_exists, file_read, file_write, ticks, msleep)
use std.os.args as cli
use std.os.fs as osfs
use std.os.path as ospath
use std.os.ui.window.consts as key
use std.os.ui.render as gfx
use std.os.ui.render.dump as ui_dump
use std.os.ui.render.reuse as ui_reuse
use std.os.ui.render.viewer.batch as ui_batch
use std.os.ui.render.viewer.editor as ed
use std.os.ui.render.viewer.editor.commands as cmd
use std.os.ui.render.viewer.editor.colorpicker as colorpicker
use std.os.ui.render.viewer.editor.diff as eddiff
use std.os.ui.render.viewer.editor.find as find
use std.os.ui.render.viewer.editor.history as history
use std.os.ui.render.viewer.editor.interaction as interact
use std.os.ui.render.viewer.editor.keychord as chord
use std.os.ui.render.viewer.editor.lsp as lsp
use std.os.ui.render.viewer.editor.outline as outline
use std.os.ui.render.viewer.editor.palette as pal
use std.os.ui.render.viewer.editor.project as project
use std.os.ui.render.viewer.editor.prompt as prompt
use std.os.ui.render.viewer.editor.runner as runner
use std.os.ui.render.viewer.editor.session as session
use std.os.ui.render.viewer.editor.terminal as termpane
use std.os.ui.render.viewer.editor.tools as tools
use std.os.ui.render.viewer.icons as icons
use std.os.ui.render.viewer.runtime as ui_runtime
use std.os.ui.render.viewer.widgets
use std.os.ui.window
use std.parse.img.svg as svg_img
use std.parse.syntax

fn _editor_color(str code, str text) str {
   if common.env_truthy("NO_COLOR") { return text }
   def esc = chr(27)
   esc + "[" + code + "m" + text + esc + "[0m"
}

fn _editor_has_help_arg() bool {
   mut i = 1
   while i < argc() {
      def a = str.lower(str.strip(to_str(argv(i))))
      if a == "-h" || a == "--help" || a == "help" { return true }
      i += 1
   }
   false
}

fn _editor_help_line(str left, str right) any {
   print("  " + _editor_color("1;36", left) + "  " + right)
   0
}

fn _editor_print_help() any {
   print(_editor_color("1;37", "Nytrix Editor"))
   print(_editor_color("90", "UI/editor testbed with buffers, panes, terminal, LSP hooks, and renderer diagnostics"))
   print("")
   print(_editor_color("1;33", "Usage"))
   print("  ./make ny etc/projects/ui/editor.ny " + _editor_color("36", "[options]") + " " + _editor_color("90", "[file-or-folder]") )
   print("")
   print(_editor_color("1;33", "Renderer"))
   _editor_help_line("-gl, --gl", "use OpenGL")
   _editor_help_line("-vk, --vk", "use Vulkan")
   _editor_help_line("-auto", "auto-select renderer")
   _editor_help_line("-mock, -cpu", "software/headless mock renderer")
   print("")
   print(_editor_color("1;33", "Debug"))
   _editor_help_line("-v, --verbose", "bounded startup/input/render diagnostics")
   _editor_help_line("-vv", "compact deep diagnostics/profiler summaries")
   _editor_help_line("--trace-spam", "last-resort per-stage/per-glyph/per-frame tracing")
   print("")
   print(_editor_color("1;33", "Useful env"))
   _editor_help_line("NY_EDITOR_FONT_SIZE=n", "body font size")
   _editor_help_line("NY_EDITOR_DENSITY=x", "scale editor chrome density")
   _editor_help_line("NY_EDITOR_INPUT_TRACE=1", "editor input trace")
   _editor_help_line("NY_EDITOR_FAST_PRESENT=1", "use immediate present instead of the default stable vsync")
   _editor_help_line("NY_UI_EDITOR_IDLE_REUSE=1", "enable present-only idle reuse for bench/debug")
   _editor_help_line("NY_UI_HEADLESS=1", "headless/mock mode")
   _editor_help_line("NY_VK_SAFE_TEXT=1", "force slow Vulkan text fallback for driver debugging")
   _editor_help_line("NY_VK_FAST_TEXT=0", "disable Vulkan atlas fast text")
   _editor_help_line("NY_VK_TEXTURE_TEXT=1", "debug Vulkan texture-atlas TTF path")
   print("")
   print(_editor_color("1;33", "Examples"))
   print("  ./make ny etc/projects/ui/editor.ny -v -gl src/main.ny")
   print("  ./make ny etc/projects/ui/editor.ny -vv -vk")
   0
}

def START_W = 1220
def START_H = 760
def START_FLAGS = key.WINDOW_CENTER | key.WINDOW_FOCUS_ON_SHOW | key.WINDOW_ALLOW_DND
def EDITOR_PRESENT_MODE = common.env_truthy("NY_EDITOR_FAST_PRESENT") ? "immediate" : "fifo"
def UI_FONT_CANDIDATES = [
   "/usr/share/fonts/TTF/JetBrainsMonoNerdFontMono-Regular.ttf",
   "/usr/share/fonts/TTF/JetBrainsMonoNLNerdFontMono-Regular.ttf",
   "/usr/share/fonts/OTF/FiraMonoNerdFontMono-Regular.otf",
   "/usr/share/fonts/TTF/MesloLGSNerdFontMono-Regular.ttf",
   "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
   "etc/assets/fonts/jetbrains.ttf",
   "etc/assets/fonts/maplemono.ttf",
   "etc/assets/fonts/monocraft.ttf",
]

def TITLE_FONT_CANDIDATES = [
   "etc/assets/fonts/monocraft.ttf",
   "etc/assets/fonts/maplemono.ttf",
   "/usr/share/fonts/TTF/JetBrainsMonoNerdFontMono-Regular.ttf",
   "etc/assets/fonts/jetbrains.ttf",
]

def MODELINE_FONT_CANDIDATES = [
   "etc/assets/fonts/jetbrains.ttf",
   "/usr/share/fonts/TTF/JetBrainsMonoNerdFontMono-Regular.ttf",
   "/usr/share/fonts/TTF/JetBrainsMonoNLNerdFontMono-Regular.ttf",
   "/usr/share/fonts/TTF/MesloLGSNerdFontMono-Regular.ttf",
   "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
   "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
]

fn _env_float_between(str name, f64 fallback, f64 lo, f64 hi) f64 {
   def raw = common.env_trim(name)
   if raw.len == 0 { return fallback }
   min(max(str.atof(raw), lo), hi)
}

fn _scaled_int(f64 value, f64 density, int lo, int hi) int {
   int(min(max(value * density, float(lo)), float(hi)))
}

def UI_DENSITY = _env_float_between("NY_EDITOR_DENSITY", 0.78, 0.54, 1.25)
def int FONT_TITLE_SIZE = int(common.env_int_clamped("NY_EDITOR_TITLE_FONT_SIZE", _scaled_int(22.0, UI_DENSITY, 12, 48), 8, 96))
def int FONT_BODY_SIZE = int(common.env_int_clamped("NY_EDITOR_FONT_SIZE", _scaled_int(16.0, UI_DENSITY, 9, 32), 8, 96))
def int FONT_SMALL_SIZE = int(common.env_int_clamped("NY_EDITOR_SMALL_FONT_SIZE", _scaled_int(13.0, UI_DENSITY, 8, 28), 8, 96))
def int FONT_MODELINE_SIZE = int(common.env_int_clamped("NY_EDITOR_MODELINE_FONT_SIZE", max(13, _scaled_int(14.0, UI_DENSITY, 10, 28)), 8, 96))
def TOP_H = max(16.0, 26.0 * UI_DENSITY)
def RAIL_MIN_W = 38.0
def EDIT_MIN_W = 56.0
def DOCK_MIN_H = 34.0
def EDIT_MIN_H = 28.0
def PANE_MAX = 4096.0

if _editor_has_help_arg() { _editor_print_help() exit(0) }
ui_dump.apply_verbose_argv()

if common.env_truthy("NY_UI_HEADLESS") { gfx.set_backend_type(gfx.BACKEND_MOCK) }
else {
   gfx.apply_backend_env()
   gfx.apply_backend_argv()
}

def int RENDER_BACKEND_MOCK = int(gfx.BACKEND_MOCK)
def win = gfx.init_window(START_W, START_H, "Nytrix Editor", START_FLAGS, EDITOR_PRESENT_MODE, false, 1)

if !win { panic("window init failed") }
window.set_exit_key(win, key.KEY_NULL)
def FONT_PRIME = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_/#[]{}().,:;+-=<>!?$%@&|\\\"'`~*" + chr(0x2022) + chr(0x2026) + chr(0x2190) + chr(0x2191) + chr(0x2192) + chr(0x2193) + chr(0x2713) + chr(0x2717) + chr(0x03BB)

fn _load_editor_font(list candidates, int size) any {
   def f = gfx.font_load_first(candidates, size)
   gfx.font_allow_color_fallback(f, true)
   gfx.font_prepare(f, FONT_PRIME)
   f
}

mut int font_title_size = FONT_TITLE_SIZE
mut int font_body_size = FONT_BODY_SIZE
mut int font_small_size = FONT_SMALL_SIZE
mut int font_modeline_size = FONT_MODELINE_SIZE
mut font_title = _load_editor_font(TITLE_FONT_CANDIDATES, font_title_size)
mut font_body = _load_editor_font(UI_FONT_CANDIDATES, font_body_size)
mut font_small = _load_editor_font(UI_FONT_CANDIDATES, font_small_size)
mut font_modeline = _load_editor_font(MODELINE_FONT_CANDIDATES, font_modeline_size)
def cursor_text = window.create_standard_cursor(window.IBEAM_CURSOR)
def cursor_resize = window.create_standard_cursor(window.RESIZE_EW_CURSOR)
def cursor_resize_ns = window.create_standard_cursor(window.RESIZE_NS_CURSOR)
mut f64 FONT_BODY_ADV = max(1.0, float(gfx.measure_text_fast(font_body, "M").get(0, 8.0)))
mut f64 FONT_SMALL_ADV = max(1.0, float(gfx.measure_text_fast(font_small, "M").get(0, 7.0)))
mut f64 FONT_MODELINE_ADV = max(1.0, float(gfx.measure_text_fast(font_modeline, "M").get(0, 7.0)))
mut f64 FONT_MODELINE_CLIP_ADV = max(9.6, FONT_MODELINE_ADV * 1.35)
def SECTION_H = project.TREE_HEADER_H
def SECTION_ICON_Y = 4.0
def SECTION_TEXT_Y = 6.0
def ROW_TEXT_Y = 4.0
def EDITOR_TEXT_TOP = 12.0
def INPUT_TRACE = common.env_truthy("NY_EDITOR_INPUT_TRACE")
def int BIG_FILE_LINES = 5000
def int HIGHLIGHT_MAX_LINE_BYTES = 900
def OUTLINE_MAX_LINES = 5000
def int LINE_DRAW_EXTRA_COLS = 96
def RAIL_TABS_H = 26.0
def CONTEXT_W = 284.0
def CONTEXT_ROW_H = 26.0
def FIND_BAR_H = 58.0
def f64 PALETTE_HEADER_H = 50.0
def f64 PALETTE_ROW_H = 24.0
def f64 PALETTE_DETAIL_H = 30.0
def int PALETTE_MAX_ROWS = 14
def f64 WHICH_KEY_ROW_H = 26.0
def int WHICH_KEY_MAX_ROWS = 36
def TOP_TAB_MIN_W = 64.0
def TOP_TAB_MAX_W = 144.0
def MOUSE_BACK = 3
def MOUSE_FORWARD = 4
def NAV_LIMIT = 96
def NAV_SEP = "\t"
def EDITOR_CACHE_PATH = "build/cache/editor_state.cfg"
def TERM_TAB_H = 20.0
def TERM_TAB_MIN_W = 64.0
def TERM_TAB_MAX_W = 118.0
def PANEL_SPLIT_HIT = 4.0
def CARET_BLINK_SEC = 0.22
def PROBE_TEXT = "abc\ndef\nghi\njkl\nmno\npqr\nstu\nvwx\nyz0\none\ntwo\nthree\nfour\nfive\nsix\nseven\neight\nnine\nten\neleven\ntwelve\nthirteen\nfourteen\nfifteen\nsixteen\nseventeen\neighteen\nnineteen\ntwenty\ntwenty-one\ntwenty-two\ntwenty-three\ntwenty-four\ntwenty-five\ntwenty-six\ntwenty-seven\ntwenty-eight\ntwenty-nine\nthirty\nthirty-one\nthirty-two\nthirty-three\nthirty-four\nthirty-five\nthirty-six\nthirty-seven\nthirty-eight\nthirty-nine\nforty"
def C_BG = gfx.color_hex("#000000")
def C_RAIL = gfx.color_hex("#020203")
def C_PANE = gfx.color_hex("#060607")
def C_PANE_2 = gfx.color_hex("#0b0b0d")
def C_TEXT = gfx.color_hex("#f5f5f6")
def C_MUTED = gfx.color_hex("#c6c6ca")
def C_DIM = gfx.color_hex("#808087")
def C_LINE = gfx.color_hex("#17171b")
def C_LINE_2 = gfx.color_hex("#292832")
def C_ACCENT = gfx.color_hex("#b6a0ff")
def C_ACCENT_2 = gfx.color_hex("#d6d3e6")
def C_CYAN = gfx.color_hex("#8ccfff")
def C_OK = gfx.color_hex("#8bdc9a")
def C_CHIP = gfx.color_hex("#050506")
def C_KEYWORD = gfx.color_hex("#b6a0ff")
def C_TYPE = gfx.color_hex("#bcaaff")
def C_STRING = gfx.color_hex("#b4b8ff")
def C_NUMBER = gfx.color_hex("#c9b6ff")
def C_COMMENT = gfx.color_hex("#77717f")
def C_FUNC = gfx.color_hex("#d1c2ff")
def C_OPERATOR = gfx.color_hex("#c4bed2")
def C_PROPERTY = gfx.color_hex("#ccbaff")
def C_WARN = gfx.color_hex("#ff9580")
def C_SELECT = gfx.color_hex("#b6a0ff")

fn _pack(any c) int { gfx.color_pack(float(c.get(0, 1.0)), float(c.get(1, 1.0)), float(c.get(2, 1.0)), float(c.get(3, 1.0))) }

fn _color_at(any c, int idx, f64 fallback=1.0) f64 { is_list(c) ? float(c.get(idx, fallback)) : fallback }

fn _flush_rects() int { ui_batch.flush() }

fn _fill_rect(f64 x, f64 y, f64 w, f64 h, any c) int {
   ui_batch.queue_rect(x, y, w, h, _pack(c))
   0
}

fn _stroke_rect(f64 x, f64 y, f64 w, f64 h, any c, f64 thick=1.0) int {
   if thick <= 1.0 {
      ui_batch.queue_outline(x, y, w, h, _pack(c))
      return 0
   }
   def col = _pack(c)
   ui_batch.queue_rect(x, y, w, thick, col)
   ui_batch.queue_rect(x, y + h - thick, w, thick, col)
   ui_batch.queue_rect(x, y, thick, h, col)
   ui_batch.queue_rect(x + w - thick, y, thick, h, col)
   0
}

fn _text(int font, any text, f64 x, f64 y, any color) int {
   _flush_rects()
   gfx.draw_text(font, text, x, y, color)
   0
}

fn _text_right(int font, any text, f64 x, f64 y, any color) int {
   _flush_rects()
   widgets.text_right(font, text, x, y, color)
   0
}

fn _text_center(int font, any text, f64 x, f64 y, any color) int {
   _flush_rects()
   widgets.text_center(font, text, x, y, color)
   0
}

fn _font_visual_h(int font, f64 fallback) f64 {
   mut h = float(gfx.font_line_height(font))
   if h < 8.0 { h = fallback }
   min(max(8.0, h), max(8.0, fallback))
}

fn _clamp_f64(f64 value, f64 lo, f64 hi) f64 {
   mut f64 out = value
   if out < lo { out = lo }
   if out > hi { out = hi }
   out
}

fn _clamp_int(int value, int lo, int hi) int {
   mut int out = value
   if out < lo { out = lo }
   if out > hi { out = hi }
   out
}

fn _center_y(f64 y, f64 h, f64 item_h) f64 {
   def f64 spare = h - item_h
   def f64 pad = spare > 0.0 ? spare : 0.0
   float(int(y + pad * 0.5 + 0.5))
}

fn _text_center_y_for(int font, f64 y, f64 h) f64 {
   _center_y(y, h, _font_visual_h(font, h))
}

fn _draw_tex_rect(int tex, f64 x, f64 y, f64 w, f64 h, any tint=gfx.WHITE) int {
   if tex <= 0 || w <= 0.0 || h <= 0.0 { return 0 }
   _flush_rects()
   gfx.draw_rect_tex(x, y, w, h, tex, _color_at(tint, 0, 1.0), _color_at(tint, 1, 1.0), _color_at(tint, 2, 1.0), _color_at(tint, 3, 1.0))
   0
}

def U_TEXT = _pack(C_TEXT)
def U_MUTED = _pack(C_MUTED)
def U_DIM = _pack(C_DIM)
def U_ACCENT = _pack(C_ACCENT)
def U_ACCENT_2 = _pack(C_ACCENT_2)
def U_CYAN = _pack(C_CYAN)
def U_OK = _pack(C_OK)
def U_WARN = _pack(C_WARN)
def U_KEYWORD = _pack(C_KEYWORD)
def U_TYPE = _pack(C_TYPE)
def U_STRING = _pack(C_STRING)
def U_NUMBER = _pack(C_NUMBER)
def U_COMMENT = _pack(C_COMMENT)
def U_FUNC = _pack(C_FUNC)
def U_OPERATOR = _pack(C_OPERATOR)
def U_PROPERTY = _pack(C_PROPERTY)
def TERM_BG = 0xff020101
def TERM_FG = 0xfff6f5f5
mut st = 0
mut hist = history.new()
mut find_state = find.state()
mut find_content_rev = -1
mut palette_state = pal.state()
mut chord_state = chord.empty_state()
mut command_cfg = cmd.config()
mut lsp_state = lsp.new()
mut ui_state = interact.new()
mut term_state = termpane.new(font_body, TERM_BG, TERM_FG)
mut completion_state = {"open": false, "items": [], "index": 0, "prefix": "", "x": 0.0, "y": 0.0}
mut peek_state = {"open": false, "title": "", "body": "", "path": "", "line": 0, "col": 0}
mut color_state = colorpicker.state()
mut project_drag = {"active": false, "armed": false, "entry": dict(8), "x": 0.0, "y": 0.0}
mut project_selection = []
mut project_selection_anchor = ""
mut section_drag = {"active": false, "section": "", "x": 0.0, "y": 0.0}
mut image_preview_cache = dict(32)
mut nav_back = []
mut nav_forward = []
mut nav_live_loc = []
mut nav_live_idx = 0
mut nav_prev_idx = -1
mut nav_next_idx = -1
mut nav_probe_phase = 0
mut content_rev = 0
mut syntax_cache_rev = -1
mut syntax_run_cache = dict(256)
mut syntax_flat_cache = dict(256)
mut syntax_lang_cache = dict(32)
mut visible_runs_cache_key = ""
mut visible_line_runs_cache = []
mut visible_text_runs_cache = []
mut width_cache_rev = -1
mut width_cache = dict(512)
mut ui_measure_cache = dict(512)
mut modeline_cache_key = ""
mut modeline_cache = dict(8)
mut outline_cache_rev = -1
mut outline_cache_name = ""
mut outline_cache = []
mut definition_cache_rev = -1
mut definition_cache_name = ""
mut definition_cache = dict(64)
mut project_definition_cache_key = ""
mut project_definition_cache = dict(512)
mut project_definition_scanned = dict(256)
mut project_definition_cache_complete = false
mut context_state = prompt.context_state()
mut rename_state = prompt.rename_state()
mut fps_state = ui_runtime.fps_begin()
mut reuse_state = ui_reuse.make()
mut int frame_count = 0
mut int reuse_present_count = 0
mut int begin_fail_count = 0
mut bool frame_events_seen = false
mut next_cache_save_time = 0.0
mut int force_full_redraw = 0
mut int last_escape_ns = 0
mut bool auto_dump_done = false
mut str cursor_kind = ""
mut int perf_advisor_reports = 0
mut edit_scroll_accum = 0.0
mut palette_scroll_accum = 0.0
mut which_key_scroll_accum = 0.0
mut which_key_scroll = 0
mut which_key_prefix = ""
mut suppress_char_cp = 0
mut tree_scroll_accum = 0.0
mut outline_scroll_accum = 0.0
mut git_scroll_accum = 0.0
mut timeline_scroll_accum = 0.0
mut project_state = project.new("")
def auto_dump_delay = ui_dump.auto_dump_delay_frames(8)
def timeout_limit = ui_runtime.timeout_ns(0)

fn _editor_probe(str name) bool {
   common.env_truthy("NY_EDITOR_PROBE_" + name) ||
   common.env_truthy("NY_EDITOR_" + name + "_PROBE") ||
   common.env_truthy("NY_EDITOR_" + name + "_SELFTEST")
}

def bool INPUT_PROBE = _editor_probe("INPUT")
def bool CONTEXT_PROBE = _editor_probe("CONTEXT")
def bool TERMINAL_PROBE = _editor_probe("TERMINAL")
def bool TERMINAL_DUMP_PROBE = _editor_probe("TERMINAL_DUMP")
def bool SYNTAX_PROBE = _editor_probe("SYNTAX")
def bool NAV_PROBE = _editor_probe("NAV")
def bool UNDO_PROBE = _editor_probe("UNDO")
def bool ESCAPE_PROBE = _editor_probe("ESCAPE")
def bool MODELINE_PROBE = _editor_probe("MODELINE")
def bool CHORD_PROBE = _editor_probe("CHORD")
def bool FIND_PROBE = _editor_probe("FIND")
def bool PALETTE_PROBE = _editor_probe("PALETTE")
def bool WHICH_KEY_PROBE = _editor_probe("WHICH_KEY")
def bool MULTICURSOR_PROBE = _editor_probe("MULTICURSOR")
def bool ZOOM_PROBE = _editor_probe("ZOOM")
def bool SCROLLBAR_PROBE = _editor_probe("SCROLLBAR")
def bool GOTO_PROBE = _editor_probe("GOTO")
def bool GIT_PROBE = _editor_probe("GIT")
def bool TAB_PROBE = _editor_probe("TAB")
def bool SELECT_PROBE = _editor_probe("SELECT")
def bool BENCH_PROBE = _editor_probe("BENCH")
def bool MOCK_BENCH_FAST = BENCH_PROBE && common.env_truthy("NY_UI_HEADLESS") && !common.env_truthy("NYTRIX_AUTO_DUMP")
def int BENCH_FRAME_LIMIT = int(common.env_int_clamped("NY_EDITOR_BENCH_FRAMES", 240, 1, 100000))
def int BENCH_WARMUP_FRAMES = int(common.env_int_clamped("NY_EDITOR_BENCH_WARMUP_FRAMES", 1, 0, 10000))
def bool FRAME_TRACE = common.env_truthy("NY_EDITOR_FRAME_TRACE")
def int FRAME_TRACE_LIMIT = int(common.env_int_clamped("NY_EDITOR_FRAME_TRACE_FRAMES", 5, 1, 100000))
def bool PERF_ADVISOR = FRAME_TRACE || common.env_truthy("NY_EDITOR_PERF_ADVISOR")
def int PERF_ADVISOR_MIN_MS = int(common.env_int_clamped("NY_EDITOR_PERF_ADVISOR_MIN_MS", 18, 1, 1000))
def int PERF_ADVISOR_LIMIT = int(common.env_int_clamped("NY_EDITOR_PERF_ADVISOR_FRAMES", 12, 1, 10000))
def int CACHE_SAVE_SECONDS = int(common.env_int_clamped("NY_EDITOR_CACHE_SAVE_SECONDS", 5, 1, 300))
def int PROJECT_OPEN_CACHE_LIMIT = int(common.env_int_clamped("NY_EDITOR_PROJECT_OPEN_CACHE_LIMIT", 1024, 0, 10000))
def int BEGIN_FAIL_LIMIT = int(common.env_int_clamped("NY_EDITOR_BEGIN_FAIL_LIMIT", 180, 1, 10000))
def int EVENT_FRAME_LIMIT = int(common.env_int_clamped("NY_EDITOR_MAX_EVENTS_PER_FRAME", 384, 32, 8192))
def int IDLE_REUSE_WARMUP = int(common.env_int_clamped("NY_EDITOR_IDLE_REUSE_WARMUP", 1, 0, 128))
def int IDLE_REUSE_INTERVAL = int(common.env_int_clamped("NY_EDITOR_IDLE_REUSE_INTERVAL", 1200, 1, 100000))
def int ESC_QUIT_WINDOW_NS = 900000000
palette_state["cfg"] = command_cfg

fn _trace_ms(int a, int b) str {
   def int delta = b - a
   def int us = delta > 0 ? delta / 1000 : 0
   to_str(us / 1000) + "." + to_str((us / 100) % 10) + to_str((us / 10) % 10) + "ms"
}

fn _measure_ui(int font, str text, f64 fallback=0.0) f64 {
   "Returns cached UI text width for stable chrome labels.  Editor line-prefix widths keep their own content-revision cache."
   if text.len <= 0 { return 0.0 }
   if !is_dict(ui_measure_cache) { ui_measure_cache = dict(512) }
   def key = to_str(font) + "\t" + text
   if ui_measure_cache.contains(key) { return float(ui_measure_cache.get(key, fallback)) }
   def w = float(gfx.measure_text_fast(font, text).get(0, fallback))
   if text.len <= 96 && ui_measure_cache.len < 2048 { ui_measure_cache[key] = w }
   w
}

fn _perf_advisor_frame(int frame, int t0, int t_update, int t_events, int t_begin, int t_draw, int t_flush, int t_end) int {
   if !PERF_ADVISOR || perf_advisor_reports >= PERF_ADVISOR_LIMIT { return 0 }
   def total_ms = (t_end - t0) / 1000000
   if total_ms < PERF_ADVISOR_MIN_MS { return 0 }
   perf_advisor_reports += 1
   def draw_ms = (t_draw - t_begin) / 1000000
   def event_ms = (t_events - t_update) / 1000000
   def flush_ms = (t_end - t_flush) / 1000000
   mut hint = ""
   if event_ms > draw_ms && event_ms >= 4 { hint = "input queue: lower NY_EDITOR_MAX_EVENTS_PER_FRAME or check terminal paste/mouse spam" }
   elif flush_ms >= 4 { hint = "present/driver: try -vk/-gl switch, disable vsync, or inspect renderer stats" }
   elif draw_ms >= 4 { hint = "draw path: enable idle reuse, shrink visible rows, or inspect text/rect batches" }
   else { hint = "mixed frame: run NY_EDITOR_FRAME_TRACE=1 NY_EDITOR_PERF_ADVISOR=1" }
   print("[editor:perf] frame=" + to_str(frame) + " total=" + to_str(total_ms) + "ms update=" + _trace_ms(t0, t_update) + " events=" + _trace_ms(t_update, t_events) + " draw=" + _trace_ms(t_begin, t_draw) + " flush=" + _trace_ms(t_flush, t_end) + " hint=" + hint)
   0
}

fn _trace_frame(
   int frame,
   int t0,
   int t_update,
   int t_events,
   int t_begin,
   int t_draw,
   int t_flush,
   int t_end
) int {
   if !FRAME_TRACE || frame >= FRAME_TRACE_LIMIT { return 0 }
   def rs = gfx.renderer_frame_stats()
   print("[editor:frame] #" + to_str(frame) +
      " update=" + _trace_ms(t0, t_update) +
      " events=" + _trace_ms(t_update, t_events) +
      " begin=" + _trace_ms(t_events, t_begin) +
      " draw=" + _trace_ms(t_begin, t_draw) +
      " flush=" + _trace_ms(t_draw, t_flush) +
      " end=" + _trace_ms(t_flush, t_end) +
      " total=" + _trace_ms(t0, t_end) +
      " vk.draws=" + to_str(int(rs.get("draws", 0))) +
      " vk.flush=" + to_str(int(rs.get("flush_total", 0))) +
   " text=" + to_str(int(rs.get("prim_text_calls", 0))) + "/" + to_str(int(rs.get("prim_text_glyphs", 0))))
   _perf_advisor_frame(frame, t0, t_update, t_events, t_begin, t_draw, t_flush, t_end)
   0
}

fn _trace_draw(
   int frame,
   int t0,
   int t_top,
   int t_rail,
   int t_editor,
   int t_terminal,
   int t_status,
   int t_popups
) int {
   if !FRAME_TRACE || frame >= FRAME_TRACE_LIMIT { return 0 }
   print("[editor:draw] #" + to_str(frame) +
      " top=" + _trace_ms(t0, t_top) +
      " rail=" + _trace_ms(t_top, t_rail) +
      " editor=" + _trace_ms(t_rail, t_editor) +
      " terminal=" + _trace_ms(t_editor, t_terminal) +
      " status=" + _trace_ms(t_terminal, t_status) +
   " popups=" + _trace_ms(t_status, t_popups))
   0
}

fn _request_full_redraw(int frames=2) int {
   if frames > force_full_redraw { force_full_redraw = frames }
   0
}

fn _editor_dynamic_ui_active() bool {
   if force_full_redraw > 0 { return true }
   if st.get("quit_prompt", false) { return true }
   if chord.pending(chord_state) { return true }
   if pal.is_open(palette_state) { return true }
   if find.is_open(find_state) { return true }
   if prompt.rename_is_open(rename_state) { return true }
   if prompt.context_is_open(context_state) { return true }
   if colorpicker.is_open(color_state) { return true }
   if bool(completion_state.get("open", false)) { return true }
   st.get("drag_terminal", false) || st.get("drag_divider", false) || st.get("drag_select", false) ||
   st.get("drag_scrollbar", false) || project_drag.get("active", false) || project_drag.get("armed", false) ||
   section_drag.get("active", false)
}

fn _editor_reuse_opts(f64 sw, f64 sh) dict {
   def any opts = {
      "enabled": common.env_truthy("NY_UI_EDITOR_IDLE_REUSE"),
      "gui_frame": true,
      "win_w": int(sw),
      "win_h": int(sh),
      "terminal_open": termpane.is_open(term_state),
      "first_frame_done": frame_count > 1,
      "scene_active": true,
      "input_active": frame_events_seen,
      "dynamic_active": _editor_dynamic_ui_active(),
      "capture_active": TERMINAL_DUMP_PROBE || common.env_truthy("NYTRIX_AUTO_DUMP"),
      "warmup": max(IDLE_REUSE_WARMUP, gfx.get_swapchain_image_count() + 2),
      "redraw_interval": IDLE_REUSE_INTERVAL
   }
   opts
}

fn _bench_finish() int {
   if !BENCH_PROBE { return 0 }
   def int start_ns = int(fps_state.get("start", ticks()))
   def int elapsed_ns = ticks() - start_ns
   def f64 elapsed = elapsed_ns > 0 ? float(elapsed_ns) / 1000000000.0 : 0.000001
   def int total = int(fps_state.get("total", 0))
   def int avg = int(float(total) / elapsed)
   print("[editor:bench] avg_fps=" + to_str(avg) +
      " frames=" + to_str(total) +
      " reused=" + to_str(reuse_present_count) +
      " reused_streak=" + to_str(ui_reuse.reused_frames(reuse_state)) +
   " full_ready=" + to_str(ui_reuse.ready_frames(reuse_state)))
   0
}

fn _set_cursor_kind(str kind) int {
   if cursor_kind == kind { return 0 }
   cursor_kind = kind
   window.set_cursor(win, kind == "resize-ew" ? cursor_resize : (kind == "resize-ns" ? cursor_resize_ns : cursor_text))
   0
}

fn _next_event() any {
   window.check_event(window.get_win(win))
}

fn _draw_icon(str name, f64 x, f64 y, f64 size, any tint=C_TEXT) int {
   if gfx.get_backend_type() == RENDER_BACKEND_MOCK {
      if MOCK_BENCH_FAST { return 0 }
      _stroke_rect(x, y, size, size, tint, 1.0)
      return 0
   }
   def tex = icons.icon_tex(name)
   if tex > 0 {
      _draw_tex_rect(tex, x, y, size, size, tint)
   } else {
      _stroke_rect(x, y, size, size, tint, 1.0)
   }
   0
}

fn _layout_sw(dict lay) f64 {
   float(lay.get("edit_x", 0.0)) + float(lay.get("edit_w", 0.0))
}

fn _layout_sh(dict lay) f64 {
   float(lay.get("status_y", 0.0)) + float(lay.get("status_h", ed.STATUS_H))
}

fn _palette_rect(f64 sw, f64 sh, int matches_len) list {
   mut f64 w = sw - 44.0
   if w < 220.0 { w = 220.0 }
   if w > 880.0 { w = 880.0 }
   mut f64 x = sw * 0.5 - w * 0.5
   if x < 8.0 { x = 8.0 }
   mut f64 y = sh * 0.09
   if y < 18.0 { y = 18.0 }
   mut int max_rows = int((sh - y - PALETTE_HEADER_H - PALETTE_DETAIL_H - 24.0) / PALETTE_ROW_H)
   if max_rows < 1 { max_rows = 1 }
   mut int rows = matches_len
   if rows < 1 { rows = 1 }
   if rows > max_rows { rows = max_rows }
   if rows > PALETTE_MAX_ROWS { rows = PALETTE_MAX_ROWS }
   def f64 h = PALETTE_HEADER_H + float(rows) * PALETTE_ROW_H + PALETTE_DETAIL_H + 10.0
   [x, y, w, h, rows]
}

fn _palette_contains(dict lay, f64 mx, f64 my) bool {
   if !pal.is_open(palette_state) { return false }
   def r = _palette_rect(_layout_sw(lay), _layout_sh(lay), _palette_matches().len)
   widgets.hit(mx, my, float(r.get(0, 0.0)), float(r.get(1, 0.0)), float(r.get(2, 0.0)), float(r.get(3, 0.0)))
}

fn _palette_row_hit(dict lay, f64 mx, f64 my) int {
   if !pal.is_open(palette_state) { return -1 }
   def matches = _palette_matches()
   def r = _palette_rect(_layout_sw(lay), _layout_sh(lay), matches.len)
   def x = float(r.get(0, 0.0))
   def y = float(r.get(1, 0.0))
   def w = float(r.get(2, 0.0))
   def rows = int(r.get(4, 0))
   palette_state = pal.set_visible(palette_state, rows)
   if !widgets.hit(mx, my, x, y, w, float(r.get(3, 0.0))) { return -2 }
   def row = int((my - (y + PALETTE_HEADER_H)) / PALETTE_ROW_H)
   def idx = pal.scroll(palette_state) + row
   (row >= 0 && row < rows && idx < matches.len) ? idx : -1
}

fn _which_key_rect(f64 sw, f64 sh, int total) list {
   mut f64 w = sw - 28.0
   if w < 320.0 { w = 320.0 }
   if w > 1180.0 { w = 1180.0 }
   def int cols = w > 960.0 ? 3 : (w > 640.0 ? 2 : 1)
   mut int max_lines = int((sh - 130.0) / WHICH_KEY_ROW_H)
   if max_lines < 1 { max_lines = 1 }
   mut int max_visible = max_lines * cols
   if max_visible > WHICH_KEY_MAX_ROWS { max_visible = WHICH_KEY_MAX_ROWS }
   if max_visible < 1 { max_visible = 1 }
   mut int visible = total
   if visible > max_visible { visible = max_visible }
   if visible < 1 { visible = 1 }
   def int line_count = int((visible + cols - 1) / cols)
   def f64 h = 74.0 + float(line_count) * WHICH_KEY_ROW_H + 12.0
   [14.0, sh - h - 14.0, w, h, cols, visible]
}

fn _which_key_contains(f64 sw, f64 sh, f64 mx, f64 my) bool {
   if !chord.pending(chord_state) { return false }
   def rows = pal.which_key(chord.describe(chord_state))
   if rows.len <= 0 { return false }
   def r = _which_key_rect(sw, sh, rows.len)
   widgets.hit(mx, my, float(r.get(0, 0.0)), float(r.get(1, 0.0)), float(r.get(2, 0.0)), float(r.get(3, 0.0)))
}

fn _clamp_which_key_scroll(int total, int visible) int {
   def int visible_rows = visible > 0 ? visible : 1
   def int max_scroll = total > visible_rows ? total - visible_rows : 0
   _clamp_int(which_key_scroll, 0, max_scroll)
}

fn _touch_content() int {
   _request_full_redraw(2)
   content_rev += 1
   syntax_cache_rev = -1
   width_cache_rev = -1
   outline_cache_rev = -1
   definition_cache_rev = -1
   syntax_run_cache = dict(256)
   syntax_flat_cache = dict(256)
   width_cache = dict(512)
   find_content_rev = -1
   color_state = colorpicker.close(color_state)
   0
}

fn _invalidate_project_definitions() int {
   project_definition_cache_key = ""
   project_definition_cache = dict(512)
   project_definition_scanned = dict(256)
   project_definition_cache_complete = false
   0
}

fn _invalidate_text_metrics() int {
   syntax_cache_rev = -1
   width_cache_rev = -1
   modeline_cache_key = ""
   visible_runs_cache_key = ""
   syntax_run_cache = dict(256)
   syntax_flat_cache = dict(256)
   width_cache = dict(512)
   0
}

def int FONT_ZOOM_MIN = 8
def int FONT_ZOOM_MAX = 42

fn _apply_editor_fonts() int {
   font_title = _load_editor_font(TITLE_FONT_CANDIDATES, font_title_size)
   font_body = _load_editor_font(UI_FONT_CANDIDATES, font_body_size)
   font_small = _load_editor_font(UI_FONT_CANDIDATES, font_small_size)
   font_modeline = _load_editor_font(MODELINE_FONT_CANDIDATES, font_modeline_size)
   FONT_BODY_ADV = _clamp_f64(float(gfx.measure_text_fast(font_body, "M").get(0, 8.0)), 1.0, 10000.0)
   FONT_SMALL_ADV = _clamp_f64(float(gfx.measure_text_fast(font_small, "M").get(0, 7.0)), 1.0, 10000.0)
   FONT_MODELINE_ADV = _clamp_f64(float(gfx.measure_text_fast(font_modeline, "M").get(0, 7.0)), 1.0, 10000.0)
   FONT_MODELINE_CLIP_ADV = _clamp_f64(FONT_MODELINE_ADV * 1.35, 9.6, 10000.0)
   term_state = termpane.set_font(term_state, font_body)
   _invalidate_text_metrics()
   0
}

fn _set_editor_font_size(int body_size, bool persist=true) int {
   def int next = _clamp_int(body_size, FONT_ZOOM_MIN, FONT_ZOOM_MAX)
   if next == font_body_size {
      if persist { _status("font " + to_str(font_body_size) + "px") }
      return 0
   }
   def int delta = next - FONT_BODY_SIZE
   font_body_size = next
   font_title_size = _clamp_int(FONT_TITLE_SIZE + delta, 8, 96)
   font_small_size = _clamp_int(FONT_SMALL_SIZE + delta, 8, 96)
   font_modeline_size = _clamp_int(FONT_MODELINE_SIZE + delta, 8, 96)
   _apply_editor_fonts()
   if persist { _save_ui_cache() }
   if persist { _status("font " + to_str(font_body_size) + "px") }
   0
}

fn _zoom_editor_font(int delta) int {
   _set_editor_font_size(font_body_size + delta, true)
}

fn _reset_editor_font() int {
   _set_editor_font_size(FONT_BODY_SIZE, true)
}

fn _line_limit(dict lay) int {
   def f64 limit = float(lay.get("edit_w", 0.0)) / 7.5 + float(LINE_DRAW_EXTRA_COLS)
   int(_clamp_f64(limit, 120.0, 1000000.0))
}

fn _draw_line_text(str line, int limit) str {
   if line.len <= limit { return line }
   str.str_slice(line, 0, limit > 3 ? limit - 3 : 0) + "..."
}

fn _highlight_on(str line) bool {
   line.len <= HIGHLIGHT_MAX_LINE_BYTES
}

fn _buffer_is_big(list lines) bool {
   lines.len > BIG_FILE_LINES
}

fn _follow_cursor() int {
   st["scroll_follow"] = true
   0
}

fn _buffer_source_path(dict b) str {
   def path = to_str(b.get("path", ""))
   path.len > 0 ? path : to_str(b.get("source_path", ""))
}

fn _editor_context_entry() dict {
   def b = ed.current_buffer(st)
   def path = _buffer_source_path(b)
   def any entry = {
      "scope": "editor", "name": to_str(b.get("name", "buffer")), "path": path,
      "rel": path.len > 0 ? path : to_str(b.get("name", "buffer")),
      "dir": false, "kind": to_str(b.get("kind", "file"))
   }
   entry
}

fn _context_file_entry(dict entry) dict {
   if is_dict(entry) && to_str(entry.get("path", "")).len > 0 { return entry }
   _editor_context_entry()
}

fn _open_context(f64 x, f64 y, dict entry, str scope) int {
   mut e = is_dict(entry) ? entry : dict(8)
   e["scope"] = scope
   context_state = prompt.context_open(context_state, x, y, e)
   palette_state = pal.close(palette_state)
   0
}

fn _timeline_context_entry(any row) dict {
   def typ = to_str(row.get(0, ""))
   if typ == "command" {
      def any command_entry = {"type": "command", "name": to_str(row.get(1, "command")), "id": to_str(row.get(2, "")), "path": "", "rel": to_str(row.get(2, "")), "dir": false}
      return command_entry
   }
   def path = to_str(row.get(2, ""))
   def any file_entry = {"type": "file", "name": to_str(row.get(1, ospath.basename(path))), "path": path, "rel": path, "dir": false, "kind": session.file_kind(path)}
   file_entry
}

fn _is_svg_path(str path) bool {
   def str ext = str.lower(ospath.extname(path))
   ext == ".svg"
}

fn _svg_attr(str src, str name) str {
   def marker = name + "="
   def idx = str.find(src, marker)
   if idx < 0 { return "" }
   def qpos = idx + marker.len
   if qpos >= src.len { return "" }
   def q = load8(src, qpos)
   if q != 34 && q != 39 { return "" }
   mut end = qpos + 1
   while end < src.len && load8(src, end) != q { end += 1 }
   end > qpos + 1 ? str.str_slice(src, qpos + 1, end) : ""
}

fn _svg_meta_lines(str path, any im=0) list {
   def src = session.read_text(path)
   def width = _svg_attr(src, "width")
   def height = _svg_attr(src, "height")
   def viewbox = _svg_attr(src, "viewBox")
   mut lines = [
      "type: SVG vector",
      "path: " + path,
      "backend: " + (svg_img.available() ? svg_img.backend_name() : "unavailable"),
      "bytes: " + to_str(src.len)
   ]
   if im && is_dict(im) {
      lines = lines.append("raster: " + to_str(int(im.get("width", 0))) + "x" + to_str(int(im.get("height", 0))))
   }
   if width.len > 0 || height.len > 0 { lines = lines.append("declared: " + width + " x " + height) }
   if viewbox.len > 0 { lines = lines.append("viewBox: " + viewbox) }
   if !svg_img.available() {
      def err = svg_img.last_error()
      if err.len > 0 { lines = lines.append("note: " + err) }
   }
   lines
}

fn _svg_preview_tex(str path) int {
   if path.len <= 0 { return -1 }
   if image_preview_cache.contains(path) {
      def cached = int(image_preview_cache.get(path, -1))
      if cached > 0 { return cached }
      if cached == -2 { return -1 }
   }
   def im = svg_img.load_path(path)
   if !im || !is_dict(im) {
      image_preview_cache[path] = -2
      return -1
   }
   def tex = icons.stable_texture_id(gfx.texture_upload_image_ex(im, path, 37, false, false, 1, 33071, 33071, "editor_svg:" + path, true))
   image_preview_cache[path] = tex
   tex
}

fn _image_preview_tex(str path) int {
   if path.len <= 0 { return -1 }
   if image_preview_cache.contains(path) {
      def cached = int(image_preview_cache.get(path, -1))
      if cached > 0 { return cached }
      if cached == -2 { return -1 }
   }
   if _is_svg_path(path) { return _svg_preview_tex(path) }
   def tex = gfx.texture_load_ex(path, 37, false, true, 1, 33071, 33071, true)
   image_preview_cache[path] = tex
   tex
}

fn _release_image_preview_cache() int {
   def keys = dict_keys(image_preview_cache)
   mut i = 0
   while i < keys.len {
      def tex = int(image_preview_cache.get(keys.get(i), -1))
      if tex > 0 { gfx.texture_destroy(tex) }
      i += 1
   }
   image_preview_cache = dict(32)
   0
}

fn _draw_svg_metadata(dict lay, dict b, int tex, f64 iw, f64 ih) bool {
   def path = _buffer_source_path(b)
   if !_is_svg_path(path) { return false }
   def x = float(lay.get("edit_x", 0.0))
   def y = float(lay.get("edit_y", 0.0))
   def w = float(lay.get("edit_w", 0.0))
   def h = float(lay.get("edit_h", 0.0))
   def pad = 12.0
   _fill_rect(x + pad, y + pad, w - pad * 2.0, h - pad * 2.0, gfx.color_hex("#010102"))
   _stroke_rect(x + pad, y + pad, w - pad * 2.0, h - pad * 2.0, C_LINE_2, 1.0)
   if tex > 0 && iw > 0.0 && ih > 0.0 {
      def f64 max_w = _clamp_f64(w * 0.46, 64.0, 1000000.0)
      def f64 max_h = _clamp_f64(h - 92.0, 64.0, 1000000.0)
      mut f64 scale = max_w / iw
      def f64 scale_h = max_h / ih
      if scale_h < scale { scale = scale_h }
      _draw_tex_rect(tex, x + pad + 12.0, y + pad + 14.0, iw * scale, ih * scale, gfx.WHITE)
   } else {
      _draw_icon("asset_image", x + pad + 24.0, y + pad + 24.0, 44.0, C_ACCENT)
      _text(font_body, "SVG preview unavailable", x + pad + 82.0, y + pad + 24.0, C_TEXT)
   }
   mut lines = _svg_meta_lines(path, 0)
   if tex > 0 { lines = lines.append("texture: " + to_str(int(iw)) + "x" + to_str(int(ih))) }
   def f64 mx = x + (tex > 0 ? _clamp_f64(w * 0.53, 220.0, 1000000.0) : pad + 82.0)
   mut i = 0
   while i < lines.len && y + pad + 50.0 + float(i) * 20.0 < y + h - pad {
      def int text_cols = int(_clamp_f64((x + w - pad - mx) / 7.2, 12.0, 1000000.0))
      _text(i == 0 ? font_body : font_small, widgets.preview(to_str(lines.get(i, "")), text_cols), mx, y + pad + 24.0 + float(i) * 20.0, i == 0 ? C_ACCENT_2 : C_MUTED)
      i += 1
   }
   true
}

fn _draw_image_preview(dict lay, dict b) bool {
   if to_str(b.get("kind", "")) != "image" { return false }
   def path = _buffer_source_path(b)
   def tex = _image_preview_tex(path)
   if _is_svg_path(path) {
      def sz = tex > 0 ? gfx.texture_size(tex) : [0, 0]
      return _draw_svg_metadata(lay, b, tex, float(sz.get(0, 0)), float(sz.get(1, 0)))
   }
   if tex <= 0 { return false }
   def sz = gfx.texture_size(tex)
   def iw = float(sz.get(0, 0))
   def ih = float(sz.get(1, 0))
   if iw <= 0.0 || ih <= 0.0 { return false }
   def x = float(lay.get("edit_x", 0.0))
   def y = float(lay.get("edit_y", 0.0))
   def w = float(lay.get("edit_w", 0.0))
   def h = float(lay.get("edit_h", 0.0))
   def f64 pad = 12.0
   def f64 label_h = 42.0
   def f64 avail_w = _clamp_f64(w - pad * 2.0, 16.0, 1000000.0)
   def f64 avail_h = _clamp_f64(h - pad * 2.0 - label_h, 16.0, 1000000.0)
   mut f64 scale = avail_w / iw
   def f64 scale_h = avail_h / ih
   if scale_h < scale { scale = scale_h }
   def f64 draw_w = iw * scale
   def f64 draw_h = ih * scale
   def f64 ix = x + (w - draw_w) * 0.5
   def f64 iy = y + pad + (avail_h - draw_h) * 0.5
   _fill_rect(x + pad, y + pad, w - pad * 2.0, h - pad * 2.0, gfx.color_hex("#010102"))
   _stroke_rect(x + pad, y + pad, w - pad * 2.0, h - pad * 2.0, C_LINE_2, 1.0)
   _draw_tex_rect(tex, ix, iy, draw_w, draw_h, gfx.WHITE)
   def name = to_str(b.get("name", ospath.basename(path)))
   def info = to_str(int(iw)) + "x" + to_str(int(ih)) + "  " + path
   _text(font_body, widgets.preview(name, int(_clamp_f64((w - pad * 2.0) / 9.0, 8.0, 1000000.0))), x + pad + 12.0, y + h - pad - 38.0, C_ACCENT_2)
   _text(font_small, widgets.preview(info, int(_clamp_f64((w - pad * 2.0) / 7.0, 8.0, 1000000.0))), x + pad + 12.0, y + h - pad - 18.0, C_DIM)
   true
}

fn _git_color(str code) any {
   if code == "??" { return C_CYAN }
   if str.str_contains(code, "D") { return C_WARN }
   if str.str_contains(code, "A") { return gfx.color_hex("#8bdc9a") }
   if str.str_contains(code, "R") { return C_ACCENT_2 }
   if str.str_contains(code, "M") { return C_WARN }
   C_DIM
}

fn _status(str msg) int {
   st = session.set_status(st, msg)
   ui_state = interact.message(ui_state, msg)
   0
}

fn _cache_parse(str raw) dict {
   def rows = str.split(raw, "\n")
   def int cap = _clamp_int(rows.len * 2, 64, 1000000)
   mut out = dict(cap)
   mut i = 0
   while i < rows.len {
      def line = str.strip(to_str(rows.get(i, "")))
      def eq = str.find(line, "=")
      if eq > 0 {
         out[str.str_slice(line, 0, eq)] = str.str_slice(line, eq + 1, line.len)
      }
      i += 1
   }
   out
}

fn _cache_value(any raw, str key_name, str fallback) str {
   if is_dict(raw) { return to_str(raw.get(key_name, fallback)) }
   def prefix = key_name + "="
   def rows = str.split(raw, "\n")
   mut i = 0
   while i < rows.len {
      def line = str.strip(to_str(rows.get(i, "")))
      if str.startswith(line, prefix) { return str.str_slice(line, prefix.len, line.len) }
      i += 1
   }
   fallback
}

fn _cache_bool(any raw, str key_name, bool fallback) bool {
   def str v = str.lower(str.strip(_cache_value(raw, key_name, fallback ? "1" : "0")))
   v == "1" || v == "true" || v == "on" || v == "yes"
}

fn _cache_float(any raw, str key_name, f64 fallback, f64 lo, f64 hi) f64 {
   def v = str.strip(_cache_value(raw, key_name, ""))
   if v.len <= 0 { return fallback }
   _clamp_f64(float(str.atof(v)), lo, hi)
}

fn _cache_int(any raw, str key_name, int fallback, int lo, int hi) int {
   def v = str.strip(_cache_value(raw, key_name, ""))
   if v.len <= 0 { return fallback }
   _clamp_int(int(str.atoi(v)), lo, hi)
}

fn _cache_decode(any raw, str key_name) str {
   def v = str.strip(_cache_value(raw, key_name, ""))
   v.len > 0 ? b64.decode64(v) : ""
}

fn _cache_encode(str value) str {
   b64.encode64(value)
}

fn _cache_project_op(any raw, str prefix, int idx) dict {
   def p = prefix + "_" + to_str(idx) + "_"
   def action = _cache_value(raw, p + "action", "")
   if action.len <= 0 { return dict(0) }
   {
      "action": action,
      "src_rel": _cache_decode(raw, p + "src_rel"),
      "src_path": _cache_decode(raw, p + "src_path"),
      "trash_path": _cache_decode(raw, p + "trash_path"),
      "name": _cache_decode(raw, p + "name"),
      "dir": _cache_bool(raw, p + "dir", false),
      "time": _cache_value(raw, p + "time", "0")
   }
}

fn _restore_project_ops(any raw) int {
   def undo_count = _cache_int(raw, "project_op_undo_count", 0, 0, 128)
   def redo_count = _cache_int(raw, "project_op_redo_count", 0, 0, 128)
   mut undo = []
   mut redo = []
   mut i = 0
   while i < undo_count {
      def op = _cache_project_op(raw, "project_op_undo", i)
      if is_dict(op) && op.len > 0 { undo = undo.append(op) }
      i += 1
   }
   i = 0
   while i < redo_count {
      def op2 = _cache_project_op(raw, "project_op_redo", i)
      if is_dict(op2) && op2.len > 0 { redo = redo.append(op2) }
      i += 1
   }
   if undo.len > 0 || redo.len > 0 { project_state = project.set_file_history(project_state, undo, redo) }
   0
}

fn _cache_append_project_ops(list rows, str prefix, list ops) list {
   def count = min(ops.len, 128)
   rows = rows.append(prefix + "_count=" + to_str(count))
   mut i = 0
   while i < count {
      def op = ops.get(i, dict(8))
      def p = prefix + "_" + to_str(i) + "_"
      rows = rows.append(p + "action=" + to_str(op.get("action", "")))
      rows = rows.append(p + "src_rel=" + _cache_encode(to_str(op.get("src_rel", ""))))
      rows = rows.append(p + "src_path=" + _cache_encode(to_str(op.get("src_path", ""))))
      rows = rows.append(p + "trash_path=" + _cache_encode(to_str(op.get("trash_path", ""))))
      rows = rows.append(p + "name=" + _cache_encode(to_str(op.get("name", ""))))
      rows = rows.append(p + "dir=" + (bool(op.get("dir", false)) ? "1" : "0"))
      rows = rows.append(p + "time=" + to_str(op.get("time", "0")))
      i += 1
   }
   rows
}

fn _restore_session_allowed() bool {
   cli.args().len <= 1 && !INPUT_PROBE && !CONTEXT_PROBE && !TERMINAL_PROBE && !TERMINAL_DUMP_PROBE && !SYNTAX_PROBE && !NAV_PROBE && !UNDO_PROBE && !ESCAPE_PROBE && !MODELINE_PROBE && !CHORD_PROBE && !FIND_PROBE && !PALETTE_PROBE && !WHICH_KEY_PROBE && !MULTICURSOR_PROBE && !ZOOM_PROBE && !SCROLLBAR_PROBE && !GOTO_PROBE && !GIT_PROBE && !TAB_PROBE && !SELECT_PROBE && !BENCH_PROBE
}

fn _restore_cached_buffers(any raw) list {
   mut out = []
   def count = _cache_int(raw, "buffer_count", 0, 0, 128)
   mut i = 0
   while i < count {
      def path = _cache_decode(raw, "buffer_" + to_str(i))
      if path.len > 0 && file_exists(path) { out = out.append(session.read_buffer(path)) }
      i += 1
   }
   out
}

fn _select_cached_active(any raw) int {
   def bs = st.get("buffers", [])
   if bs.len <= 0 { return 0 }
   def active_path = _cache_decode(raw, "active_path")
   if active_path.len > 0 {
      def want = ospath.normalize(active_path)
      mut i = 0
      while i < bs.len {
         if ospath.normalize(_buffer_source_path(bs.get(i, {}))) == want { return i }
         i += 1
      }
   }
   _cache_int(raw, "active", int(st.get("active", 0)), 0, bs.len - 1)
}

fn _valid_focus(str pane) bool {
   pane == "editor" || pane == "project" || pane == "terminal"
}

fn _session_paths() list {
   mut out = []
   def bs = st.get("buffers", [])
   mut i = 0
   while i < bs.len && out.len < 64 {
      def path = _buffer_source_path(bs.get(i, {}))
      if path.len > 0 && file_exists(path) { out = out.append(path) }
      i += 1
   }
   out
}

fn _restore_project_open(any raw) int {
   def count = _cache_int(raw, "project_open_count", 0, 0, 4096)
   mut open = dict(max(32, count * 2))
   mut i = 0
   while i < count {
      def rel = _cache_decode(raw, "project_open_" + to_str(i))
      if rel.len > 0 { open[rel] = _cache_bool(raw, "project_open_val_" + to_str(i), true) }
      i += 1
   }
   if count > 0 {
      project_state["open"] = open
      project_state = project.refresh(project_state)
      st["project_loaded"] = true
   }
   0
}

fn _restore_terminal(any raw) int {
   if !_cache_bool(raw, "term_open", false) { return 0 }
   def m = _cache_value(raw, "term_mode", "terminal")
   if m == "repl" { term_state = termpane.open_repl(term_state) }
   else { term_state = termpane.open_shell(term_state) }
   0
}

fn _load_ui_cache() int {
   if !_restore_session_allowed() { return 0 }
   match file_read(EDITOR_CACHE_PATH) {
      ok(raw_any) -> {
         def raw = to_str(raw_any)
         def cache = _cache_parse(raw)
         _restore_project_open(cache)
         _restore_project_ops(cache)
         def restored = _restore_cached_buffers(cache)
         if restored.len > 0 { st = ed.state(restored) }
         st["show_project"] = _cache_bool(cache, "show_project", bool(st.get("show_project", true)))
         st["show_outline"] = _cache_bool(cache, "show_outline", bool(st.get("show_outline", true)))
         st["show_hierarchy"] = _cache_bool(cache, "show_hierarchy", bool(st.get("show_hierarchy", true)))
         st["show_status"] = _cache_bool(cache, "show_status", bool(st.get("show_status", true)))
         st["show_titlebar"] = _cache_bool(cache, "show_titlebar", bool(st.get("show_titlebar", true)))
         st["show_line_numbers"] = _cache_bool(cache, "show_line_numbers", bool(st.get("show_line_numbers", true)))
         st["show_indent_guides"] = _cache_bool(cache, "show_indent_guides", bool(st.get("show_indent_guides", false)))
         st["rail_w"] = _cache_float(cache, "rail_w", float(st.get("rail_w", 250.0)), RAIL_MIN_W, PANE_MAX)
         st["rail_split"] = _cache_float(cache, "rail_split", float(st.get("rail_split", 0.0)), 0.0, 0.95)
         st["term_h"] = _cache_float(cache, "term_h", float(st.get("term_h", 220.0)), DOCK_MIN_H, PANE_MAX)
         _set_editor_font_size(_cache_int(cache, "font_body_size", font_body_size, FONT_ZOOM_MIN, FONT_ZOOM_MAX), false)
         def tab = _cache_value(cache, "rail_tab", to_str(st.get("rail_tab", "files")))
         if tab == "files" || tab == "git" || tab == "timeline" { st["rail_tab"] = tab }
         st = ed.select_buffer(st, _select_cached_active(cache))
         st["cursor_line"] = _cache_int(cache, "cursor_line", int(st.get("cursor_line", 0)), 0, 100000000)
         st["cursor_col"] = _cache_int(cache, "cursor_col", int(st.get("cursor_col", 0)), 0, 100000000)
         st["scroll"] = _cache_int(cache, "scroll", int(st.get("scroll", 0)), 0, 100000000)
         st["tree_scroll"] = _cache_int(cache, "tree_scroll", int(st.get("tree_scroll", 0)), 0, 1000000)
         st["outline_scroll"] = _cache_int(cache, "outline_scroll", int(st.get("outline_scroll", 0)), 0, 1000000)
         st["git_scroll"] = _cache_int(cache, "git_scroll", int(st.get("git_scroll", 0)), 0, 1000000)
         st["timeline_scroll"] = _cache_int(cache, "timeline_scroll", int(st.get("timeline_scroll", 0)), 0, 1000000)
         def focus = _cache_value(cache, "focus", to_str(st.get("focus", "editor")))
         if _valid_focus(focus) { st["focus"] = focus }
         _restore_terminal(cache)
         if to_str(st.get("focus", "editor")) != "terminal" { term_state["focus"] = false }
         st = ed.clamp_cursor(st)
         st["scroll_follow"] = false
      }
      err(_) -> { }
   }
   0
}

fn _save_ui_cache() int {
   if !_restore_session_allowed() { return 0 }
   mut rows = [
      "show_project=" + (bool(st.get("show_project", true)) ? "1" : "0"),
      "show_outline=" + (bool(st.get("show_outline", true)) ? "1" : "0"),
      "show_hierarchy=" + (bool(st.get("show_hierarchy", true)) ? "1" : "0"),
      "show_status=" + (bool(st.get("show_status", true)) ? "1" : "0"),
      "show_titlebar=" + (bool(st.get("show_titlebar", true)) ? "1" : "0"),
      "show_line_numbers=" + (bool(st.get("show_line_numbers", true)) ? "1" : "0"),
      "show_indent_guides=" + (bool(st.get("show_indent_guides", false)) ? "1" : "0"),
      "rail_w=" + to_str(float(st.get("rail_w", 250.0))),
      "rail_split=" + to_str(float(st.get("rail_split", 0.0))),
      "term_h=" + to_str(float(st.get("term_h", 220.0))),
      "font_body_size=" + to_str(font_body_size),
      "rail_tab=" + to_str(st.get("rail_tab", "files")),
      "active=" + to_str(int(st.get("active", 0))),
      "active_path=" + _cache_encode(_current_path()),
      "cursor_line=" + to_str(int(st.get("cursor_line", 0))),
      "cursor_col=" + to_str(int(st.get("cursor_col", 0))),
      "scroll=" + to_str(int(st.get("scroll", 0))),
      "tree_scroll=" + to_str(int(st.get("tree_scroll", 0))),
      "outline_scroll=" + to_str(int(st.get("outline_scroll", 0))),
      "git_scroll=" + to_str(int(st.get("git_scroll", 0))),
      "timeline_scroll=" + to_str(int(st.get("timeline_scroll", 0))),
      "focus=" + to_str(st.get("focus", "editor")),
      "term_open=" + (termpane.is_open(term_state) ? "1" : "0"),
      "term_mode=" + termpane.mode(term_state)
   ]
   def paths = _session_paths()
   rows = rows.append("buffer_count=" + to_str(paths.len))
   mut i = 0
   while i < paths.len {
      rows = rows.append("buffer_" + to_str(i) + "=" + _cache_encode(to_str(paths.get(i, ""))))
      i += 1
   }
   def open_map_raw = project_state.get("open", dict(8))
   def open_map = is_dict(open_map_raw) ? open_map_raw : dict(8)
   def open_keys = dict_keys(open_map)
   def open_count = min(open_keys.len, PROJECT_OPEN_CACHE_LIMIT)
   rows = rows.append("project_open_count=" + to_str(open_count))
   i = 0
   while i < open_count {
      def rel = to_str(open_keys.get(i, ""))
      rows = rows.append("project_open_" + to_str(i) + "=" + _cache_encode(rel))
      rows = rows.append("project_open_val_" + to_str(i) + "=" + (bool(open_map.get(rel, true)) ? "1" : "0"))
      i += 1
   }
   rows = _cache_append_project_ops(rows, "project_op_undo", project.file_undo_stack(project_state))
   rows = _cache_append_project_ops(rows, "project_op_redo", project.file_redo_stack(project_state))
   def body = str.join(rows, "\n") + "\n"
   match file_write(EDITOR_CACHE_PATH, body) { ok(_) -> { } err(_) -> { } }
   0
}

fn _current_path() str { _buffer_source_path(ed.current_buffer(st)) }

fn _current_text() str { ed.join_lines(ed.current_lines(st)) }

fn _find_seed() str {
   def selected = _selected_text_all()
   if selected.len > 0 && selected.len <= 160 && !str.str_contains(selected, "\n") { return selected }
   _word_at_cursor()
}

fn _find_refresh_now() int {
   find_state = find.refresh(find_state, ed.current_lines(st), int(st.get("cursor_line", 0)), int(st.get("cursor_col", 0)))
   find_content_rev = content_rev
   0
}

fn _find_refresh_if_needed() int {
   if find.is_open(find_state) && find_content_rev != content_rev { _find_refresh_now() }
   0
}

fn _find_message() str {
   def err = find.error(find_state)
   err.len > 0 ? "find: bad regex" : "find " + find.summary(find_state)
}

fn _find_open() int {
   def old = find.query(find_state)
   def seed = old.len > 0 ? old : _find_seed()
   if seed.len > 0 { find_state = find.set_query(find_state, seed) }
   find_state = find.open(find_state)
   context_state = prompt.context_close(context_state)
   palette_state = pal.close(palette_state)
   _completion_close()
   _find_refresh_now()
   _status(seed.len > 0 ? _find_message() : "find")
   0
}

fn _find_replace_open() int {
   _find_open()
   find_state = find.show_replace(find_state)
   _status("replace")
   0
}

fn _find_close() int {
   find_state = find.close(find_state)
   _status("find closed")
   0
}

fn _find_jump_current(bool announce=true) int {
   _find_refresh_if_needed()
   if find.count(find_state) <= 0 {
      if announce { _status(_find_message()) }
      return 0
   }
   def cur = find.current(find_state)
   st["cursor_line"] = int(cur.get("line", 0))
   st["cursor_col"] = int(cur.get("start", 0))
   _clear_selections()
   st = ed.clamp_cursor(st)
   _follow_cursor()
   if announce { _status(_find_message()) }
   0
}

fn _find_step(int dir) int {
   if !find.is_open(find_state) { _find_open() }
   if find.query(find_state).len <= 0 { _status("find") return 0 }
   _find_refresh_if_needed()
   find_state = dir < 0 ? find.prev(find_state) : find.next(find_state)
   _find_jump_current(true)
}

fn _find_toggle(str what) int {
   if what == "regex" { find_state = find.toggle_regex(find_state) }
   elif what == "case" { find_state = find.toggle_case(find_state) }
   elif what == "word" { find_state = find.toggle_word(find_state) }
   elif what == "replace" { find_state = find.toggle_replace(find_state) }
   _find_refresh_now()
   _status(_find_message())
   0
}

fn _find_apply_replace(dict res, bool all=false) int {
   find_state = res.get("st", find_state)
   def err = to_str(res.get("error", ""))
   if err.len > 0 || !bool(res.get("ok", false)) {
      _status("replace: " + (err.len > 0 ? err : "failed"))
      return 0
   }
   def n = int(res.get("count", 0))
   if n <= 0 { _status("replace: no matches") return 0 }
   hist = history.push(hist, st)
   st = ed.set_lines(st, res.get("lines", ed.current_lines(st)))
   st["cursor_line"] = int(res.get("line", int(st.get("cursor_line", 0))))
   st["cursor_col"] = int(res.get("col", int(st.get("cursor_col", 0))))
   _clear_selections()
   st = ed.clamp_cursor(st)
   _touch_content()
   _find_refresh_now()
   _follow_cursor()
   _status(all ? ("replaced " + to_str(n)) : "replaced")
   0
}

fn _find_replace_current() int {
   if !find.is_open(find_state) { _find_replace_open() }
   _find_refresh_now()
   _find_apply_replace(find.replace_current(find_state, ed.current_lines(st)), false)
}

fn _find_replace_all() int {
   if !find.is_open(find_state) { _find_replace_open() }
   _find_refresh_now()
   _find_apply_replace(find.replace_all(find_state, ed.current_lines(st)), true)
}

fn _project_relative_path(str path) str {
   if path.len <= 0 { return "" }
   def root = str.str_replace(ospath.normalize(project.root(project_state)), "\\", "/")
   def p = str.str_replace(ospath.normalize(path), "\\", "/")
   def prefix = root + "/"
   str.startswith(p, prefix) ? str.str_slice(p, prefix.len, p.len) : p
}

fn _copy_relative_path(str path) int {
   def rel = _project_relative_path(path)
   if rel.len <= 0 { _status("no path") return 0 }
   window.set_clipboard(win, rel)
   _status("copied relative path")
   0
}

fn _lsp_sync_current(bool force=false) int {
   if !lsp.enabled_by_default() { return 0 }
   def b = ed.current_buffer(st)
   if b.get("readonly", false) {
      if lsp_state.get("active", false) { lsp_state = lsp.stop(lsp_state) }
      return 0
   }
   def path = _current_path()
   if path.len <= 0 { return 0 }
   def cur_path = to_str(lsp_state.get("path", ""))
   if !force && lsp_state.get("active", false) && cur_path == path { return 0 }
   lsp_state = lsp.start(lsp_state, path, _current_text())
   0
}

fn _buffer_changed(bool force_lsp=true) int {
   _request_full_redraw(2)
   _follow_cursor()
   _touch_content()
   if force_lsp { _lsp_sync_current(true) }
   0
}

fn _ensure_project_tree() int {
   if !bool(st.get("project_loaded", false)) {
      project_state = project.refresh(project_state)
      st["project_loaded"] = true
      _invalidate_project_definitions()
   }
   0
}

fn _ensure_project_git() int {
   if !bool(st.get("git_loaded", false)) {
      project_state = project.refresh_status(project_state)
      st["git_loaded"] = true
   }
   0
}

fn _nav_location() list {
   [
      "",
      "",
      int(st.get("active", 0)),
      int(st.get("cursor_line", 0)),
      int(st.get("cursor_col", 0)),
      int(st.get("scroll", 0))
   ]
}

fn _nav_record_current() int {
   nav_live_loc = _nav_location()
   nav_live_idx = int(st.get("active", 0))
   0
}

fn _nav_trim(list xs) list {
   if xs.len <= NAV_LIMIT { return xs }
   mut out = []
   mut i = xs.len - NAV_LIMIT
   while i < xs.len {
      out = out.append(xs.get(i))
      i += 1
   }
   out
}

fn _nav_without_last(list xs) list {
   mut out = []
   mut i = 0
   while i + 1 < xs.len {
      out = out.append(xs.get(i))
      i += 1
   }
   out
}

fn _nav_push_current() int {
   nav_prev_idx = nav_live_idx
   nav_next_idx = -1
   0
}

fn _nav_push_current_if_target_diff(str target_path) int {
   _nav_push_current()
}

fn _nav_find_buffer(list loc) int {
   def want_path = to_str(loc.get(0, ""))
   def want_name = to_str(loc.get(1, ""))
   def want_idx = int(loc.get(2, -1))
   def bs = st.get("buffers", [])
   if want_path.len <= 0 && want_idx >= 0 && want_idx < bs.len { return want_idx }
   mut i = 0
   while i < bs.len {
      def b = bs.get(i, {})
      if want_path.len > 0 && str._str_eq(ospath.normalize(_buffer_source_path(b)), ospath.normalize(want_path)) { return i }
      if want_path.len == 0 && str._str_eq(to_str(b.get("name", "")), want_name) { return i }
      i += 1
   }
   (want_idx >= 0 && want_idx < bs.len) ? want_idx : -1
}

fn _nav_restore(list loc) bool {
   def idx = _nav_find_buffer(loc)
   if idx < 0 { return false }
   st = ed.select_buffer(st, idx)
   st["cursor_line"] = int(loc.get(3, 0))
   st["cursor_col"] = int(loc.get(4, 0))
   st["scroll"] = int(loc.get(5, 0))
   _clear_selections()
   st = ed.clamp_cursor(st)
   _buffer_changed(true)
   nav_live_loc = loc
   true
}

fn _nav_go(int dir) int {
   if dir < 0 {
      if nav_prev_idx < 0 { _status("no previous location") return 0 }
      def target = nav_prev_idx
      nav_next_idx = nav_live_idx
      nav_prev_idx = -1
      st = ed.select_buffer(st, target)
      _buffer_changed(true)
      nav_live_idx = target
      _status("back")
      return 0
   }
   if nav_next_idx < 0 { _status("no next location") return 0 }
   def target2 = nav_next_idx
   nav_prev_idx = nav_live_idx
   nav_next_idx = -1
   st = ed.select_buffer(st, target2)
   _buffer_changed(true)
   nav_live_idx = target2
   _status("forward")
   0
}

fn _extra_cursors() list { st.get("extra_cursors", []) }

fn _extra_cursor_count() int { _extra_cursors().len }

fn _cursor_row(any c) int { is_list(c) ? int(c.get(0, 0)) : int(c.get("line", 0)) }

fn _cursor_col(any c) int { is_list(c) ? int(c.get(1, 0)) : int(c.get("col", 0)) }

fn _cursor_make(int row, int col, bool primary=false) dict {
   {"line": row, "col": col, "primary": primary}
}

fn _cursor_primary(any c) bool { is_dict(c) && bool(c.get("primary", false)) }

fn _cursor_before(any a, any b) bool {
   def ar = _cursor_row(a)
   def br = _cursor_row(b)
   ar < br || (ar == br && _cursor_col(a) < _cursor_col(b))
}

fn _cursor_sorted_desc(list cursors) list {
   mut raw = cursors
   mut out = []
   while raw.len > 0 {
      mut best = 0
      mut i = 1
      while i < raw.len {
         if _cursor_before(raw.get(best), raw.get(i)) { best = i }
         i += 1
      }
      out = out.append(raw.get(best))
      mut rest = []
      i = 0
      while i < raw.len {
         if i != best { rest = rest.append(raw.get(i)) }
         i += 1
      }
      raw = rest
   }
   out
}

fn _all_cursors_desc() list {
   def lines = ed.current_lines(st)
   def max_row = max(0, lines.len - 1)
   mut out = []
   def extras = _extra_cursors()
   mut i = 0
   while i < extras.len {
      def row = min(max(_cursor_row(extras.get(i)), 0), max_row)
      def col = min(max(_cursor_col(extras.get(i)), 0), to_str(lines.get(row, "")).len)
      out = out.append(_cursor_make(row, col, false))
      i += 1
   }
   def prow = min(max(int(st.get("cursor_line", 0)), 0), max_row)
   def pcol = min(max(int(st.get("cursor_col", 0)), 0), to_str(lines.get(prow, "")).len)
   _cursor_sorted_desc(out.append(_cursor_make(prow, pcol, true)))
}

fn _set_processed_cursors(list cursors) int {
   mut extras = []
   mut saw_primary = false
   mut i = 0
   while i < cursors.len {
      def c = cursors.get(i)
      def row = max(0, _cursor_row(c))
      def col = max(0, _cursor_col(c))
      if _cursor_primary(c) && !saw_primary {
         st["cursor_line"] = row
         st["cursor_col"] = col
         saw_primary = true
      } else {
         extras = extras.append([row, col])
      }
      i += 1
   }
   if !saw_primary && cursors.len > 0 {
      def first = cursors.get(0)
      st["cursor_line"] = max(0, _cursor_row(first))
      st["cursor_col"] = max(0, _cursor_col(first))
   }
   st["extra_cursors"] = extras
   0
}

fn _clamp_extra_cursor(any c, list lines) list {
   def max_row = max(0, lines.len - 1)
   def row = min(max(_cursor_row(c), 0), max_row)
   def col = min(max(_cursor_col(c), 0), to_str(lines.get(row, "")).len)
   [row, col]
}

fn _move_extra_cursors_by(int drow, int dcol) int {
   if _extra_cursor_count() <= 0 || (drow == 0 && dcol == 0) { return 0 }
   def lines = ed.current_lines(st)
   mut out = []
   def extras = _extra_cursors()
   mut i = 0
   while i < extras.len {
      def old = extras.get(i)
      mut c = [_cursor_row(old) + drow, _cursor_col(old) + dcol]
      c = _clamp_extra_cursor(c, lines)
      if _cursor_row(c) != int(st.get("cursor_line", 0)) && !_cursor_at_row(out, _cursor_row(c)) {
         out = out.append(c)
      }
      i += 1
   }
   st["extra_cursors"] = out
   0
}

fn _adjust_processed_insert(list cursors, int row, int col, int delta, int last_len, int text_len) list {
   mut out = []
   mut i = 0
   while i < cursors.len {
      def c = cursors.get(i)
      def cr = _cursor_row(c)
      def cc = _cursor_col(c)
      mut nr = cr
      mut nc = cc
      if delta <= 0 {
         if cr == row && cc >= col { nc = cc + text_len }
      } else {
         if cr > row { nr = cr + delta }
         elif cr == row && cc >= col {
            nr = row + delta
            nc = last_len + (cc - col)
         }
      }
      out = out.append(_cursor_make(nr, nc, _cursor_primary(c)))
      i += 1
   }
   out
}

fn _adjust_processed_backspace(list cursors, int row, int col, int delta, int prev_len) list {
   mut out = []
   mut i = 0
   while i < cursors.len {
      def c = cursors.get(i)
      def cr = _cursor_row(c)
      def cc = _cursor_col(c)
      mut nr = cr
      mut nc = cc
      if delta == 0 {
         if cr == row && cc >= col { nc = max(0, cc - 1) }
      } else {
         if cr > row { nr = cr - 1 }
         elif cr == row {
            nr = row - 1
            nc = prev_len + cc
         }
      }
      out = out.append(_cursor_make(nr, nc, _cursor_primary(c)))
      i += 1
   }
   out
}

fn _adjust_processed_delete(list cursors, int row, int col, int delta, int base_len) list {
   mut out = []
   mut i = 0
   while i < cursors.len {
      def c = cursors.get(i)
      def cr = _cursor_row(c)
      def cc = _cursor_col(c)
      mut nr = cr
      mut nc = cc
      if delta == 0 {
         if cr == row && cc > col { nc = max(0, cc - 1) }
      } else {
         if cr == row + 1 {
            nr = row
            nc = base_len + cc
         } elif cr > row + 1 {
            nr = cr - 1
         }
      }
      out = out.append(_cursor_make(nr, nc, _cursor_primary(c)))
      i += 1
   }
   out
}

fn _cursor_at_row(list cursors, int row) bool {
   mut i = 0
   while i < cursors.len {
      if _cursor_row(cursors.get(i)) == row { return true }
      i += 1
   }
   false
}

fn _cursor_stack_edge(int dir) int {
   mut row = int(st.get("cursor_line", 0))
   def extras = _extra_cursors()
   mut i = 0
   while i < extras.len {
      def erow = _cursor_row(extras.get(i))
      row = dir < 0 ? min(row, erow) : max(row, erow)
      i += 1
   }
   row
}

fn _clear_extra_cursors(bool announce=true) int {
   if _extra_cursor_count() <= 0 { return 0 }
   st["extra_cursors"] = []
   st["multi_selects"] = []
   if announce { _status("single cursor") }
   0
}

fn _add_extra_cursor(int dir) int {
   def lines = ed.current_lines(st)
   if lines.len <= 0 { return 0 }
   def step = dir < 0 ? -1 : 1
   def primary_row = int(st.get("cursor_line", 0))
   mut row = _cursor_stack_edge(step) + step
   def cursors = _extra_cursors()
   while row >= 0 && row < lines.len && (row == primary_row || _cursor_at_row(cursors, row)) {
      row += step
   }
   if row < 0 || row >= lines.len {
      _status("no more lines")
      return 0
   }
   def col = min(max(int(st.get("cursor_col", 0)), 0), to_str(lines.get(row, "")).len)
   _add_extra_cursor_at(row, col)
}

fn _add_extra_cursor_at(int row, int col) int {
   def lines = ed.current_lines(st)
   if lines.len <= 0 { return 0 }
   def target_row = min(max(row, 0), lines.len - 1)
   def target_col = min(max(col, 0), to_str(lines.get(target_row, "")).len)
   mut cursors = _extra_cursors()
   if target_row == int(st.get("cursor_line", 0)) || _cursor_at_row(cursors, target_row) {
      _status("cursor already on line")
      return 0
   }
   cursors = cursors.append([target_row, target_col])
   st["extra_cursors"] = cursors
   _status(to_str(cursors.len + 1) + " cursors")
   0
}

fn _range_make(int al, int ac, int bl, int bc, bool primary=false) list {
   [al, ac, bl, bc, primary ? 1 : 0]
}

fn _range_primary(any r) bool { is_list(r) && int(r.get(4, 0)) != 0 }

fn _range_norm(any r) list {
   def al = int(r.get(0, 0))
   def ac = int(r.get(1, 0))
   def bl = int(r.get(2, 0))
   def bc = int(r.get(3, 0))
   if al > bl || (al == bl && ac > bc) { return _range_make(bl, bc, al, ac, _range_primary(r)) }
   _range_make(al, ac, bl, bc, _range_primary(r))
}

fn _range_valid(any r) bool {
   if !is_list(r) || r.len < 4 { return false }
   int(r.get(0, 0)) != int(r.get(2, 0)) || int(r.get(1, 0)) != int(r.get(3, 0))
}

fn _range_before(any a, any b) bool {
   def ar = int(a.get(0, 0))
   def br = int(b.get(0, 0))
   ar < br || (ar == br && int(a.get(1, 0)) < int(b.get(1, 0)))
}

fn _ranges_sorted_desc(list ranges) list {
   mut raw = ranges
   mut out = []
   while raw.len > 0 {
      mut best = 0
      mut i = 1
      while i < raw.len {
         if _range_before(raw.get(best), raw.get(i)) { best = i }
         i += 1
      }
      out = out.append(raw.get(best))
      mut rest = []
      i = 0
      while i < raw.len {
         if i != best { rest = rest.append(raw.get(i)) }
         i += 1
      }
      raw = rest
   }
   out
}

fn _multi_selection_valid() bool {
   def ranges = st.get("multi_selects", [])
   mut i = 0
   while i < ranges.len {
      if _range_valid(ranges.get(i, [])) { return true }
      i += 1
   }
   false
}

fn _selection_any_valid() bool {
   _multi_selection_valid() || ed.selection_valid(st)
}

fn _selection_ranges_active() list {
   if _multi_selection_valid() {
      mut out = []
      def ranges = st.get("multi_selects", [])
      mut i = 0
      while i < ranges.len {
         def r = ranges.get(i, [])
         if _range_valid(r) { out = out.append(_range_norm(r)) }
         i += 1
      }
      return out
   }
   if ed.selection_valid(st) {
      return [_range_norm(_range_make(
               int(st.get("sel_a_line", 0)), int(st.get("sel_a_col", 0)),
               int(st.get("sel_b_line", 0)), int(st.get("sel_b_col", 0)),
               true
      ))]
   }
   []
}

fn _multi_selection_anchor_if_needed() int {
   if st.get("multi_selects", []).len > 0 { return 0 }
   mut ranges = [_range_make(
         int(st.get("cursor_line", 0)), int(st.get("cursor_col", 0)),
         int(st.get("cursor_line", 0)), int(st.get("cursor_col", 0)),
         true
   )]
   def extras = _extra_cursors()
   mut i = 0
   while i < extras.len {
      def c = extras.get(i, [])
      ranges = ranges.append(_range_make(_cursor_row(c), _cursor_col(c), _cursor_row(c), _cursor_col(c), false))
      i += 1
   }
   st["multi_selects"] = ranges
   st["sel_a_line"] = int(st.get("cursor_line", 0))
   st["sel_a_col"] = int(st.get("cursor_col", 0))
   st["sel_b_line"] = int(st.get("cursor_line", 0))
   st["sel_b_col"] = int(st.get("cursor_col", 0))
   st["sel_active"] = false
   st["drag_select"] = false
   0
}

fn _multi_selection_update() int {
   def ranges = st.get("multi_selects", [])
   if ranges.len <= 0 { return 0 }
   mut out = []
   mut idx = 0
   if ranges.len > 0 {
      def r = ranges.get(0, [])
      out = out.append(_range_make(
            int(r.get(0, 0)), int(r.get(1, 0)),
            int(st.get("cursor_line", 0)), int(st.get("cursor_col", 0)),
            true
      ))
      idx = 1
   }
   def extras = _extra_cursors()
   mut ei = 0
   while ei < extras.len && idx < ranges.len {
      def r2 = ranges.get(idx, [])
      def c = extras.get(ei, [])
      out = out.append(_range_make(int(r2.get(0, 0)), int(r2.get(1, 0)), _cursor_row(c), _cursor_col(c), false))
      ei += 1
      idx += 1
   }
   st["multi_selects"] = out
   if out.len > 0 {
      def p = out.get(0, [])
      st["sel_a_line"] = int(p.get(0, 0))
      st["sel_a_col"] = int(p.get(1, 0))
      st["sel_b_line"] = int(p.get(2, 0))
      st["sel_b_col"] = int(p.get(3, 0))
      st["sel_active"] = _range_valid(p)
   }
   0
}

fn _clear_selections() int {
   st["sel_active"] = false
   st["drag_select"] = false
   st["multi_selects"] = []
   0
}

fn _range_text(list lines, any raw) str {
   def r = _range_norm(raw)
   def sl = min(max(int(r.get(0, 0)), 0), max(0, lines.len - 1))
   def el = min(max(int(r.get(2, 0)), sl), max(0, lines.len - 1))
   def sc = max(0, int(r.get(1, 0)))
   def ec = max(0, int(r.get(3, 0)))
   mut out = []
   mut i = sl
   while i <= el && i < lines.len {
      def line = to_str(lines.get(i, ""))
      def a = i == sl ? min(sc, line.len) : 0
      def b = i == el ? min(ec, line.len) : line.len
      out = out.append(str.str_slice(line, a, max(a, b)))
      i += 1
   }
   ed.join_lines(out)
}

fn _selected_text_all() str {
   def ranges = _selection_ranges_active()
   if ranges.len <= 0 { return "" }
   def lines = ed.current_lines(st)
   mut out = []
   mut i = 0
   while i < ranges.len {
      out = out.append(_range_text(lines, ranges.get(i, [])))
      i += 1
   }
   str.join(out, "\n")
}

fn _delete_range_from_lines(list lines, any raw) dict {
   def r = _range_norm(raw)
   def sl = min(max(int(r.get(0, 0)), 0), max(0, lines.len - 1))
   def el = min(max(int(r.get(2, 0)), sl), max(0, lines.len - 1))
   def first = to_str(lines.get(sl, ""))
   def last = to_str(lines.get(el, ""))
   def sc = min(max(int(r.get(1, 0)), 0), first.len)
   def ec = min(max(int(r.get(3, 0)), 0), last.len)
   mut out = []
   mut i = 0
   while i < sl { out = out.append(lines.get(i, "")) i += 1 }
   out = out.append(str.str_slice(first, 0, sc) + str.str_slice(last, ec, last.len))
   i = el + 1
   while i < lines.len { out = out.append(lines.get(i, "")) i += 1 }
   if out.len <= 0 { out = [""] }
   {"lines": out, "row": sl, "col": sc}
}

fn _insert_text_in_lines(list lines, int row, int col, str text) dict {
   row = min(max(row, 0), max(0, lines.len - 1))
   def line = to_str(lines.get(row, ""))
   col = min(max(col, 0), line.len)
   def parts = ed.split_lines(text)
   if parts.len <= 1 {
      lines[row] = ed.line_insert(line, col, text)
      return {"lines": lines, "row": row, "col": col + text.len}
   }
   def before = str.str_slice(line, 0, col)
   def after = str.str_slice(line, col, line.len)
   mut out = []
   mut i = 0
   while i < row { out = out.append(lines.get(i, "")) i += 1 }
   out = out.append(before + to_str(parts.get(0, "")))
   i = 1
   while i < parts.len - 1 { out = out.append(parts.get(i, "")) i += 1 }
   def tail = to_str(parts.get(parts.len - 1, ""))
   out = out.append(tail + after)
   i = row + 1
   while i < lines.len { out = out.append(lines.get(i, "")) i += 1 }
   {"lines": out, "row": row + parts.len - 1, "col": tail.len}
}

fn _replace_multi_selections(str text) int {
   def ranges = _ranges_sorted_desc(_selection_ranges_active())
   if ranges.len <= 0 { return 0 }
   hist = history.push(hist, st)
   mut lines = ed.current_lines(st)
   mut cursors = []
   mut i = 0
   while i < ranges.len {
      def r = ranges.get(i, [])
      def erased = _delete_range_from_lines(lines, r)
      lines = erased.get("lines", lines)
      mut row = int(erased.get("row", 0))
      mut col = int(erased.get("col", 0))
      if text.len > 0 {
         def ins = _insert_text_in_lines(lines, row, col, text)
         lines = ins.get("lines", lines)
         row = int(ins.get("row", row))
         col = int(ins.get("col", col))
      }
      cursors = cursors.append(_cursor_make(row, col, _range_primary(r)))
      i += 1
   }
   st = ed.set_lines(st, lines)
   _set_processed_cursors(cursors)
   _clear_selections()
   st = ed.clamp_cursor(st)
   _follow_cursor()
   _touch_content()
   if lsp_state.get("active", false) && _current_path().len > 0 {
      lsp_state["last_request"] = lsp.did_change(lsp_state, _current_path(), _current_text())
   }
   0
}

fn _edit_insert_extra(str text) int {
   if text.len <= 0 { return 0 }
   if _selection_any_valid() { return _replace_multi_selections(text) }
   if _extra_cursor_count() <= 0 {
      return _apply_hist(history.insert_text(hist, st, text))
   }
   hist = history.push(hist, st)
   mut lines = ed.current_lines(st)
   def cursors = _all_cursors_desc()
   mut updated = []
   mut i = 0
   while i < cursors.len {
      def row = min(max(_cursor_row(cursors.get(i)), 0), max(0, lines.len - 1))
      def line = to_str(lines.get(row, ""))
      def col = min(max(_cursor_col(cursors.get(i)), 0), line.len)
      def parts = ed.split_lines(text)
      if parts.len <= 1 {
         lines[row] = ed.line_insert(line, col, text)
         updated = _adjust_processed_insert(updated, row, col, 0, 0, text.len)
         updated = updated.append(_cursor_make(row, col + text.len, _cursor_primary(cursors.get(i))))
      } else {
         def before = str.str_slice(line, 0, col)
         def after = str.str_slice(line, col, line.len)
         mut out = []
         mut j = 0
         while j < row { out = out.append(lines.get(j, "")) j += 1 }
         out = out.append(before + to_str(parts.get(0, "")))
         j = 1
         while j < parts.len - 1 { out = out.append(parts.get(j, "")) j += 1 }
         def tail = to_str(parts.get(parts.len - 1, ""))
         out = out.append(tail + after)
         j = row + 1
         while j < lines.len { out = out.append(lines.get(j, "")) j += 1 }
         lines = out
         def delta = parts.len - 1
         updated = _adjust_processed_insert(updated, row, col, delta, tail.len, text.len)
         updated = updated.append(_cursor_make(row + delta, tail.len, _cursor_primary(cursors.get(i))))
      }
      i += 1
   }
   st = ed.set_lines(st, lines)
   _set_processed_cursors(updated)
   st = ed.clamp_cursor(st)
   _follow_cursor()
   _touch_content()
   0
}

fn _edit_backspace_extra() int {
   if _selection_any_valid() { return _replace_multi_selections("") }
   if _extra_cursor_count() <= 0 { return _apply_hist(history.backspace(hist, st)) }
   hist = history.push(hist, st)
   mut lines = ed.current_lines(st)
   def cursors = _all_cursors_desc()
   mut updated = []
   mut i = 0
   while i < cursors.len {
      def row = min(max(_cursor_row(cursors.get(i)), 0), max(0, lines.len - 1))
      def line = to_str(lines.get(row, ""))
      def col = min(max(_cursor_col(cursors.get(i)), 0), line.len)
      if col > 0 {
         lines[row] = ed.line_delete_before(line, col)
         updated = _adjust_processed_backspace(updated, row, col, 0, 0)
         updated = updated.append(_cursor_make(row, col - 1, _cursor_primary(cursors.get(i))))
      } elif row > 0 {
         def prev = to_str(lines.get(row - 1, ""))
         mut out = []
         mut j = 0
         while j < row - 1 { out = out.append(lines.get(j, "")) j += 1 }
         out = out.append(prev + line)
         j = row + 1
         while j < lines.len { out = out.append(lines.get(j, "")) j += 1 }
         lines = out
         updated = _adjust_processed_backspace(updated, row, col, -1, prev.len)
         updated = updated.append(_cursor_make(row - 1, prev.len, _cursor_primary(cursors.get(i))))
      } else {
         updated = updated.append(_cursor_make(row, col, _cursor_primary(cursors.get(i))))
      }
      i += 1
   }
   st = ed.set_lines(st, lines)
   _set_processed_cursors(updated)
   st = ed.clamp_cursor(st)
   _follow_cursor()
   _touch_content()
   0
}

fn _edit_delete_extra() int {
   if _selection_any_valid() { return _replace_multi_selections("") }
   if _extra_cursor_count() <= 0 { return _apply_hist(history.delete_char(hist, st)) }
   hist = history.push(hist, st)
   mut lines = ed.current_lines(st)
   def cursors = _all_cursors_desc()
   mut updated = []
   mut i = 0
   while i < cursors.len {
      def row = min(max(_cursor_row(cursors.get(i)), 0), max(0, lines.len - 1))
      def line = to_str(lines.get(row, ""))
      def col = min(max(_cursor_col(cursors.get(i)), 0), line.len)
      if col < line.len {
         lines[row] = str.str_slice(line, 0, col) + str.str_slice(line, col + 1, line.len)
         updated = _adjust_processed_delete(updated, row, col, 0, 0)
      } elif row + 1 < lines.len {
         mut out = []
         mut j = 0
         while j < row { out = out.append(lines.get(j, "")) j += 1 }
         out = out.append(line + to_str(lines.get(row + 1, "")))
         j = row + 2
         while j < lines.len { out = out.append(lines.get(j, "")) j += 1 }
         lines = out
         updated = _adjust_processed_delete(updated, row, col, -1, line.len)
      }
      updated = updated.append(_cursor_make(row, col, _cursor_primary(cursors.get(i))))
      i += 1
   }
   st = ed.set_lines(st, lines)
   _set_processed_cursors(updated)
   st = ed.clamp_cursor(st)
   _follow_cursor()
   _touch_content()
   0
}

fn _apply_hist(dict res) int {
   hist = res.get("hist", hist)
   st = res.get("st", st)
   _follow_cursor()
   _touch_content()
   if lsp_state.get("active", false) && _current_path().len > 0 {
      lsp_state["last_request"] = lsp.did_change(lsp_state, _current_path(), _current_text())
   }
   def msg = to_str(res.get("status", ""))
   if msg.len > 0 { _status(msg) }
   0
}

fn _undo() int { _apply_hist(history.undo(hist, st)) }

fn _redo() int { _apply_hist(history.redo(hist, st)) }

fn _edit_insert(str text) int { _edit_insert_extra(text) }

fn _edit_newline() int {
   if _selection_any_valid() { return _replace_multi_selections("\n") }
   if _extra_cursor_count() <= 0 { return _apply_hist(history.newline(hist, st)) }
   _edit_insert_extra("\n")
}

fn _edit_backspace() int { _edit_backspace_extra() }

fn _edit_delete() int { _edit_delete_extra() }

fn _open_file(str path) int {
   _nav_push_current_if_target_diff(path)
   st = session.open_file(st, path)
   _buffer_changed(true)
   _nav_record_current()
   0
}

fn _project_entry_at(dict lay, f64 mx, f64 my) dict {
   if !bool(st.get("show_hierarchy", true)) { return dict(0) }
   if _rail_tab() != "files" || !bool(st.get("show_project", true)) { return dict(0) }
   _ensure_project_tree()
   def rail_x = float(lay.get("rail_x", 0.0))
   def rail_y = float(lay.get("rail_y", 0.0))
   def rail_w = float(lay.get("rail_w", 0.0))
   def rail_h = float(lay.get("rail_h", 0.0))
   if rail_w <= 1.0 { return dict(0) }
   def body_y = rail_y + RAIL_TABS_H
   def body_h = rail_h - RAIL_TABS_H
   def rows = project.tree(project_state)
   def show_outline = bool(st.get("show_outline", true))
   def syms = show_outline ? _outline_symbols() : []
   def tree_h = show_outline ? _rail_tree_height(body_h, rows.len, syms.len) : body_h
   project.hit_entry(project_state, rail_x, body_y, rail_w, tree_h, int(st.get("tree_scroll", 0)), mx, my)
}

fn _project_rel(dict entry) str { to_str(entry.get("rel", "")) }

fn _project_selection_contains_rel(str rel) bool {
   if rel.len <= 0 { return false }
   mut i = 0
   while i < project_selection.len {
      if to_str(project_selection.get(i, "")) == rel { return true }
      i += 1
   }
   false
}

fn _project_selection_contains(dict entry) bool {
   _project_selection_contains_rel(_project_rel(entry))
}

fn _project_selection_status() int {
   def n = project_selection.len
   _status(n <= 0 ? "selection cleared" : (to_str(n) + " selected"))
}

fn _project_selection_set(dict entry) int {
   def rel = _project_rel(entry)
   if rel.len <= 0 { return 0 }
   project_selection = [rel]
   project_selection_anchor = rel
   0
}

fn _project_selection_toggle(dict entry) int {
   def rel = _project_rel(entry)
   if rel.len <= 0 { return 0 }
   mut out = []
   mut found = false
   mut i = 0
   while i < project_selection.len {
      def cur = to_str(project_selection.get(i, ""))
      if cur == rel { found = true }
      else { out = out.append(cur) }
      i += 1
   }
   if !found { out = out.append(rel) project_selection_anchor = rel }
   project_selection = out
   _project_selection_status()
}

fn _project_tree_index(str rel) int {
   def rows = project.tree(project_state)
   mut i = 0
   while i < rows.len {
      if _project_rel(rows.get(i, {})) == rel { return i }
      i += 1
   }
   -1
}

fn _project_selection_range(dict entry) int {
   def rel = _project_rel(entry)
   if rel.len <= 0 { return 0 }
   if project_selection_anchor.len <= 0 { project_selection_anchor = rel }
   def a = _project_tree_index(project_selection_anchor)
   def b = _project_tree_index(rel)
   if a < 0 || b < 0 {
      _project_selection_set(entry)
      _project_selection_status()
      return 0
   }
   def lo = min(a, b)
   def hi = max(a, b)
   def rows = project.tree(project_state)
   mut out = []
   mut i = lo
   while i <= hi && i < rows.len {
      def r = _project_rel(rows.get(i, {}))
      if r.len > 0 { out = out.append(r) }
      i += 1
   }
   project_selection = out
   _project_selection_status()
}

fn _project_selected_entries() list {
   def rows = project.tree(project_state)
   mut out = []
   mut i = 0
   while i < rows.len {
      def e = rows.get(i, {})
      if is_dict(e) && _project_selection_contains(e) { out = out.append(e) }
      i += 1
   }
   out
}

fn _project_action_entries(dict entry) list {
   def e = _project_entry_or_current(entry)
   if !is_dict(e) || e.len <= 0 { return [] }
   def scope = to_str(e.get("scope", "file"))
   if project_selection.len > 0 && (scope == "file" || scope.len <= 0 || _project_selection_contains(e)) {
      def rows = _project_selected_entries()
      if rows.len > 0 { return rows }
   }
   [e]
}

fn _project_rel_has_selected_parent(str rel, list rels) bool {
   mut i = 0
   while i < rels.len {
      def parent = to_str(rels.get(i, ""))
      if parent.len > 0 && parent != "." && parent != rel && str.startswith(rel, parent + "/") { return true }
      i += 1
   }
   false
}

fn _project_filter_top_entries(list entries) list {
   mut rels = []
   mut i = 0
   while i < entries.len {
      def rel = _project_rel(entries.get(i, {}))
      if rel.len > 0 { rels = rels.append(rel) }
      i += 1
   }
   mut out = []
   i = 0
   while i < entries.len {
      def e = entries.get(i, {})
      def rel = _project_rel(e)
      if rel.len > 0 && !_project_rel_has_selected_parent(rel, rels) { out = out.append(e) }
      i += 1
   }
   out
}

fn _project_entry_in_or_under_any(dict entry, list entries) bool {
   def rel = _project_rel(entry)
   if rel.len <= 0 { return false }
   mut i = 0
   while i < entries.len {
      def src = _project_rel(entries.get(i, {}))
      if src.len > 0 && (rel == src || str.startswith(rel + "/", src + "/")) { return true }
      i += 1
   }
   false
}

fn _project_copy_entries(list entries, bool relative) int {
   if entries.len <= 0 { return 0 }
   mut vals = []
   mut i = 0
   while i < entries.len {
      def e = entries.get(i, {})
      def v = relative ? _project_rel(e) : to_str(e.get("path", ""))
      if v.len > 0 { vals = vals.append(v) }
      i += 1
   }
   if vals.len <= 0 { return 0 }
   window.set_clipboard(win, str.join(vals, "\n"))
   _status(vals.len == 1 ? (relative ? "copied relative path" : "copied path") : ("copied " + to_str(vals.len) + " paths"))
}

fn _open_or_toggle_project_row(dict entry) int {
   def res = session.open_or_toggle_project_row(st, project_state, entry)
   st = res.get("st", st)
   project_state = res.get("project", project_state)
   _buffer_changed(!entry.get("dir", false))
   if !entry.get("dir", false) { _nav_record_current() }
   0
}

fn _new_file_path(dict entry, str name) str {
   def base = to_str(entry.get("path", project.root(project_state)))
   def parent = entry.get("dir", false) ? base : ospath.dirname(base)
   ospath.join(parent, str.strip(name))
}

fn _start_new_file(dict entry) int {
   if !is_dict(entry) || entry.len <= 0 {
      entry = session.project_entry_for_path(project_state, _current_path())
   }
   if !is_dict(entry) || entry.len <= 0 {
      entry = {"name": ospath.basename(project.root(project_state)), "path": project.root(project_state), "rel": ".", "dir": true}
   }
   entry["action"] = "new-file"
   entry["name"] = "untitled.ny"
   rename_state = prompt.rename_start(rename_state, entry)
   palette_state = pal.close(palette_state)
   context_state = prompt.context_close(context_state)
   _status("new file")
   0
}

fn _start_rename(dict entry) int {
   if !is_dict(entry) || entry.len <= 0 { entry = session.project_entry_for_path(project_state, to_str(ed.current_buffer(st).get("path", ""))) }
   if !is_dict(entry) || to_str(entry.get("rel", "")).len <= 0 || to_str(entry.get("rel", "")) == "." {
      _status("select a file first")
      return 0
   }
   rename_state = prompt.rename_start(rename_state, entry)
   palette_state = pal.close(palette_state)
   context_state = prompt.context_close(context_state)
   _status("rename")
   0
}

fn _finish_rename() int {
   if !prompt.rename_is_open(rename_state) { return 0 }
   def entry = prompt.rename_entry(rename_state)
   def action = to_str(entry.get("action", "rename"))
   def text = prompt.rename_text(rename_state)
   def created_path = action == "new-file" ? _new_file_path(entry, text) : ""
   if action == "new-file" {
      project_state = project.create_file(project_state, prompt.rename_rel(rename_state), text)
   } else {
      project_state = project.rename_entry(project_state, prompt.rename_rel(rename_state), text)
   }
   _invalidate_project_definitions()
   def err = to_str(project_state.get("last_error", ""))
   rename_state = prompt.rename_close(rename_state)
   if err.len > 0 { _status(err) }
   else {
      if action == "new-file" && created_path.len > 0 { _open_file(created_path) }
      _status(action == "new-file" ? "created file" : "renamed")
   }
   0
}

fn _context_action(str id) int {
   def entry = prompt.context_entry(context_state)
   context_state = prompt.context_close(context_state)
   def scope = to_str(entry.get("scope", "file"))
   def path = to_str(entry.get("path", ""))
   if id == "open" {
      if scope == "editor" {
         if path.len > 0 { _open_file(path) }
      } elif scope == "outline" {
         st["cursor_line"] = int(entry.get("line", 0))
         st["cursor_col"] = 0
         st = ed.clamp_cursor(st)
         _follow_cursor()
         _status("symbol " + to_str(entry.get("name", "")))
      } elif scope == "timeline" {
         if to_str(entry.get("type", "")) == "command" { _run_command(to_str(entry.get("id", ""))) }
         elif path.len > 0 { _open_file(path) }
      } elif scope == "terminal" {
         _set_focus("terminal")
      } else { _open_or_toggle_project_row(entry) }
   }
   elif id == "new-file" { _start_new_file(entry) }
   elif id == "delete-file" { _delete_project_entry(entry) }
   elif id == "undo-file-op" { _undo_file_op() }
   elif id == "redo-file-op" { _redo_file_op() }
   elif id == "git-diff" {
      if scope == "editor" {
         def hit = session.project_entry_for_path(project_state, _current_path())
         if is_dict(hit) && hit.len > 0 { _open_git_diff(hit) } else { _status("no git entry") }
      } else { _open_git_diff(entry) }
   }
   elif id == "run" {
      if scope != "editor" {
         if entry.get("dir", false) { _status("select a runnable file") return 0 }
         _open_file(path)
      }
      _run_current_file()
   }
   elif id == "check" {
      if scope != "editor" {
         if entry.get("dir", false) { _status("select a file to check") return 0 }
         _open_file(path)
      }
      _check_current_file()
   }
   elif id == "format" {
      if scope != "editor" {
         if entry.get("dir", false) { _status("select a file to format") return 0 }
         _open_file(path)
      }
      _format_current_file()
   }
   elif id == "debug-start" {
      if scope != "editor" {
         if entry.get("dir", false) { _status("select a file to debug") return 0 }
         _open_file(path)
      }
      _debug_start_current()
   }
   elif id == "debug-breakpoint" {
      if scope != "editor" {
         if entry.get("dir", false) { _status("select a file for breakpoint") return 0 }
         _open_file(path)
      }
      _debug_breakpoint()
   }
   elif id == "diff-clipboard" {
      if scope != "editor" {
         if entry.get("dir", false) { _status("select a file to diff") return 0 }
         _open_file(path)
      }
      _diff_clipboard()
   }
   elif id == "rename" { _start_rename(entry) }
   elif id == "refresh" { project_state = project.refresh_git(project_state) st["project_loaded"] = true st["git_loaded"] = true _invalidate_project_definitions() _status("project refreshed") }
   elif id == "copy-path" {
      if scope != "editor" && path.len > 0 { _project_copy_entries(_project_action_entries(entry), false) }
      else { _copy_file_path() }
   }
   elif id == "copy-relative-path" {
      if scope != "editor" && path.len > 0 { _project_copy_entries(_project_action_entries(entry), true) }
      else { _copy_relative_path(_current_path()) }
   }
   elif id == "copy-line-info" { _copy_line_info() }
   elif id == "copy" { _copy_selection_or_buffer() }
   elif id == "paste" { _paste_clipboard() }
   elif id == "go-definition" { _goto_definition() }
   elif id == "lsp-hover" { _lsp_request("hover") }
   elif id == "lsp-diagnostics" { _lsp_diagnostics() }
   elif id == "terminal-shell" { term_state = termpane.show_shell(term_state) _set_focus("terminal") }
   elif id == "terminal-repl" { term_state = termpane.show_repl(term_state) _set_focus("terminal") }
   elif id == "terminal-next" { term_state = termpane.next_tab(term_state) _set_focus("terminal") }
   elif id == "terminal-prev" { term_state = termpane.prev_tab(term_state) _set_focus("terminal") }
   elif id == "terminal-close" { term_state = termpane.close_tab(term_state) _set_focus(termpane.is_open(term_state) ? "terminal" : "editor") }
   elif id == "pane-editor" { _set_focus("editor") }
   0
}

fn _run_current_file() int {
   def path = _current_path()
   def selected = _selected_text_all()
   if selected.len > 0 {
      st = session.run_text(st, path, selected, "selection")
   } else {
      def sec = runner.section_at(ed.current_lines(st), int(st.get("cursor_line", 0)))
      if sec.get("found", false) && str.strip(to_str(sec.get("text", ""))).len > 0 {
         st = session.run_text(st, path, to_str(sec.get("text", "")), "section")
      } else {
         st = session.run_current_file(st)
      }
   }
   _follow_cursor()
   _touch_content()
   0
}

fn _diff_clipboard() int {
   def clip = window.get_clipboard(win)
   if clip.len <= 0 { _status("clipboard empty") return 0 }
   def b = ed.current_buffer(st)
   def name = to_str(b.get("name", "buffer"))
   def text = ed.join_lines(ed.current_lines(st))
   def out = eddiff.side_by_side(text, clip, name, "clipboard", 68)
   st = session.set_output_buffer(st, "*diff: " + name + " clipboard*", out)
   _mark_current_output("diff", _buffer_source_path(b))
   _follow_cursor()
   _touch_content()
   _status("diff clipboard")
   0
}

fn _mark_current_output(str kind, str source_path="") int {
   mut bs = st.get("buffers", [])
   mut b = ed.current_buffer(st)
   b["readonly"] = true
   b["kind"] = kind
   if source_path.len > 0 { b["source_path"] = source_path }
   bs[int(st.get("active", 0))] = b
   st["buffers"] = bs
   0
}

fn _open_git_diff(dict entry) int {
   if !is_dict(entry) || entry.len <= 0 { _status("select changed file") return 0 }
   def rel = to_str(entry.get("rel", entry.get("name", "")))
   def body = project.diff_text(project_state, entry)
   st = session.set_output_buffer(st, "*git diff: " + rel + "*", body)
   _mark_current_output("diff", to_str(entry.get("path", "")))
   _follow_cursor()
   _touch_content()
   _status("git diff " + rel)
   0
}

fn _check_current_file() int {
   st = session.check_current_file(st)
   _follow_cursor()
   _touch_content()
   0
}

fn _format_current_file() int {
   st = session.format_current_file(st)
   _follow_cursor()
   _touch_content()
   0
}

fn _argv_tail(list argv) list {
   mut out = []
   mut i = 1
   while i < argv.len {
      out = out.append(argv.get(i))
      i += 1
   }
   out
}

fn _debug_command_text(list argv) str {
   argv.len > 0 ? str.join(argv, " ") : "debug command missing"
}

fn _debug_start_current() int {
   def path = _current_path()
   if path.len <= 0 || !file_exists(path) { _status("save file before debug") return 0 }
   if !ed.current_buffer(st).get("readonly", false) && ed.current_buffer(st).get("dirty", false) { st = session.save(st) }
   def argv = tools.debug_command_for(path)
   if argv.len <= 0 { _status("debug command missing") return 0 }
   term_state = termpane.open_command(term_state, "debug", to_str(argv.get(0, "")), _argv_tail(argv), "$ " + _debug_command_text(argv) + "\n")
   _set_focus("terminal", false)
   _status("debug " + ospath.basename(path))
   0
}

fn _debug_send(str command) int {
   if !termpane.is_open(term_state) || termpane.mode(term_state) != "debug" { _debug_start_current() }
   if termpane.is_open(term_state) {
      term_state = termpane.send_text(term_state, command + "\n")
      _set_focus("terminal", false)
      _status("debug " + command)
   }
   0
}

fn _debug_line_target() str {
   def path = _current_path()
   if path.len <= 0 { return "" }
   path + ":" + to_str(int(st.get("cursor_line", 0)) + 1)
}

fn _debug_until_current_line() int {
   def target = _debug_line_target()
   if target.len <= 0 { _status("save file before until") return 0 }
   _debug_send("until " + target)
}

fn _debug_jump_current_line() int {
   def target = _debug_line_target()
   if target.len <= 0 { _status("save file before jump") return 0 }
   _debug_send("jump " + target)
}

def DEBUG_LINE_SKIP_WORDS = " and as bool break case continue def elif else false fn for if in int list match mut nil none return str true use while "

fn _debug_symbol_byte(int c) bool {
   (c >= 65 && c <= 90) || (c >= 97 && c <= 122) || (c >= 48 && c <= 57) || c == 95
}

fn _debug_symbol_start(int c) bool {
   (c >= 65 && c <= 90) || (c >= 97 && c <= 122) || c == 95
}

fn _debug_skip_symbol(str name) bool {
   name.len <= 0 || str.str_contains(DEBUG_LINE_SKIP_WORDS, " " + name + " ")
}

fn _debug_line_symbols(str line, int limit=10) list {
   mut out = []
   mut seen = dict(16)
   mut i = 0
   while i < line.len && out.len < limit {
      def c = load8(line, i)
      if _debug_symbol_start(c) {
         def start = i
         i += 1
         while i < line.len && _debug_symbol_byte(load8(line, i)) { i += 1 }
         def name = str.str_slice(line, start, i)
         if !_debug_skip_symbol(name) && !seen.contains(name) {
            seen[name] = true
            out = out.append(name)
         }
      } else {
         i += 1
      }
   }
   out
}

fn _debug_inspect_line() int {
   def lines = ed.current_lines(st)
   if lines.len <= 0 { _status("empty line") return 0 }
   def row = min(max(0, int(st.get("cursor_line", 0))), lines.len - 1)
   def syms = _debug_line_symbols(to_str(lines.get(row, "")))
   if syms.len <= 0 { _status("no symbols on line") return 0 }
   _debug_start_current()
   mut i = 0
   while i < syms.len {
      term_state = termpane.send_text(term_state, "print " + to_str(syms.get(i, "")) + "\n")
      i += 1
   }
   _set_focus("terminal", false)
   _status("inspect line " + to_str(row + 1))
   0
}

fn _debug_load_coredump() int {
   term_state = termpane.open_command(term_state, "debug", "coredumpctl", ["-1", "gdb"], "$ coredumpctl -1 gdb\n")
   _set_focus("terminal", false)
   _status("latest coredump")
   0
}

fn _debug_breakpoint() int {
   def path = _current_path()
   if path.len <= 0 { _status("save file before breakpoint") return 0 }
   _debug_send("break " + path + ":" + to_str(int(st.get("cursor_line", 0)) + 1))
}

fn _debug_current_file() int {
   _debug_start_current()
   0
}

fn _save() int {
   st = session.save(st)
   if lsp_state.get("active", false) && _current_path().len > 0 {
      lsp_state["last_request"] = lsp.did_save(lsp_state, _current_path())
   }
   0
}

fn _copy_buffer() int {
   window.set_clipboard(win, ed.join_lines(ed.current_lines(st)))
   _status("copied buffer")
   0
}

fn _paste_clipboard() int {
   def text = window.get_clipboard(win)
   if text.len <= 0 { _status("clipboard empty") return 0 }
   _edit_insert(text)
   _status("pasted")
   0
}

fn _copy_selection_or_buffer() int {
   def selected = _selected_text_all()
   if selected.len > 0 {
      window.set_clipboard(win, selected)
      _status("copied selection")
   } else {
      _copy_buffer()
   }
   0
}

fn _cut_selection() int {
   if !_selection_any_valid() { _status("no selection") return 0 }
   window.set_clipboard(win, _selected_text_all())
   _replace_multi_selections("")
   _status("cut")
}

fn _mark_whole_buffer() int {
   def lines = ed.current_lines(st)
   st["sel_a_line"] = 0
   st["sel_a_col"] = 0
   st["sel_b_line"] = max(0, lines.len - 1)
   st["sel_b_col"] = to_str(lines.get(max(0, lines.len - 1), "")).len
   st["cursor_line"] = int(st.get("sel_b_line", 0))
   st["cursor_col"] = int(st.get("sel_b_col", 0))
   st["sel_active"] = true
   st["multi_selects"] = []
   _status("marked buffer")
   0
}

fn _kill_line() int {
   def lines = ed.current_lines(st)
   def row = min(max(int(st.get("cursor_line", 0)), 0), max(0, lines.len - 1))
   def line = to_str(lines.get(row, ""))
   def col = min(max(int(st.get("cursor_col", 0)), 0), line.len)
   st["sel_a_line"] = row
   st["sel_a_col"] = col
   st["sel_b_line"] = row
   st["sel_b_col"] = line.len
   st["sel_active"] = true
   st["multi_selects"] = []
   _cut_selection()
}

fn _kill_buffer() int {
   mut bs = st.get("buffers", [])
   if bs.len <= 1 {
      st = ed.state([ed.buffer("untitled.ny", "", "")])
      _buffer_changed(true)
      _nav_record_current()
      _status("new empty buffer")
      return 0
   }
   def active = int(st.get("active", 0))
   mut out = []
   mut i = 0
   while i < bs.len {
      if i != active { out = out.append(bs.get(i)) }
      i += 1
   }
   st["buffers"] = out
   st["active"] = min(max(0, active), out.len - 1)
   st["cursor_line"] = 0
   st["cursor_col"] = 0
   st["scroll"] = 0
   _clear_selections()
   _buffer_changed(true)
   _nav_record_current()
   _status("killed buffer")
   0
}

fn _new_scratch() int {
   mut bs = st.get("buffers", [])
   def idx = bs.len + 1
   mut b = ed.buffer("*scratch " + to_str(idx) + "*", "", "")
   b["kind"] = "scratch"
   b["dirty"] = false
   bs = bs.append(b)
   st["buffers"] = bs
   st["active"] = bs.len - 1
   st["cursor_line"] = 0
   st["cursor_col"] = 0
   st["scroll"] = 0
   _clear_selections()
   _buffer_changed(true)
   _nav_record_current()
   _status("new scratch")
   0
}

fn _mutate_lines(list lines, str msg) int {
   hist = history.push(hist, st)
   st = ed.set_lines(st, lines)
   st = ed.clamp_cursor(st)
   _follow_cursor()
   _touch_content()
   if msg.len > 0 { _status(msg) }
   0
}

fn _save_all() int {
   mut bs = st.get("buffers", [])
   mut saved = 0
   mut i = 0
   while i < bs.len {
      mut b = bs.get(i, {})
      def path = to_str(b.get("path", ""))
      if path.len > 0 && !b.get("readonly", false) {
         if session.write_text(path, ed.join_lines(b.get("lines", [""]))) {
            b["dirty"] = false
            bs[i] = b
            saved += 1
         }
      }
      i += 1
   }
   st["buffers"] = bs
   _status("saved " + to_str(saved) + " buffer" + (saved == 1 ? "" : "s"))
   0
}

fn _reload_from_disk() int {
   def path = to_str(ed.current_buffer(st).get("path", ""))
   if path.len <= 0 || !file_exists(path) { _status("no file to reload") return 0 }
   mut bs = st.get("buffers", [])
   bs[int(st.get("active", 0))] = session.read_buffer(path)
   st["buffers"] = bs
   st["cursor_line"] = 0
   st["cursor_col"] = 0
   st["scroll"] = 0
   _clear_selections()
   _follow_cursor()
   _touch_content()
   _lsp_sync_current(true)
   _status("reloaded " + ospath.basename(path))
   0
}

fn _copy_file_path() int {
   def path = _current_path()
   if path.len <= 0 { _status("memory buffer") return 0 }
   window.set_clipboard(win, path)
   _status("copied path")
   0
}

fn _copy_line_info() int {
   def b = ed.current_buffer(st)
   def path = _buffer_source_path(b)
   def info = (path.len > 0 ? path : to_str(b.get("name", "buffer"))) + ":" +
   to_str(int(st.get("cursor_line", 0)) + 1) + ":" + to_str(int(st.get("cursor_col", 0)) + 1)
   window.set_clipboard(win, info)
   _status("copied line info")
   0
}

fn _word_byte(int c) bool {
   (c >= 65 && c <= 90) || (c >= 97 && c <= 122) || (c >= 48 && c <= 57) || c == 95
}

fn _word_at_cursor() str {
   def lines = ed.current_lines(st)
   def row = min(max(int(st.get("cursor_line", 0)), 0), max(0, lines.len - 1))
   def line = to_str(lines.get(row, ""))
   if line.len <= 0 { return "" }
   mut col = min(max(int(st.get("cursor_col", 0)), 0), line.len - 1)
   if !_word_byte(load8(line, col)) && col > 0 { col -= 1 }
   if !_word_byte(load8(line, col)) { return "" }
   mut a = col
   while a > 0 && _word_byte(load8(line, a - 1)) { a -= 1 }
   mut b = col + 1
   while b < line.len && _word_byte(load8(line, b)) { b += 1 }
   str.str_slice(line, a, b)
}

fn _word_prefix_before_cursor() str {
   def lines = ed.current_lines(st)
   def row = min(max(int(st.get("cursor_line", 0)), 0), max(0, lines.len - 1))
   def line = to_str(lines.get(row, ""))
   mut col = min(max(int(st.get("cursor_col", 0)), 0), line.len)
   mut a = col
   while a > 0 && _word_byte(load8(line, a - 1)) { a -= 1 }
   str.str_slice(line, a, col)
}

fn _completion_close() int {
   completion_state["open"] = false
   completion_state["items"] = []
   completion_state["index"] = 0
   0
}

fn _completion_push(list items, str label, str detail, str kind="symbol", str insert="") list {
   if label.len <= 0 || items.len >= 22 { return items }
   mut i = 0
   while i < items.len {
      def row = items.get(i, dict(0))
      if is_dict(row) && to_str(row.get("label", "")) == label { return items }
      i += 1
   }
   return items.append({"label": label, "detail": detail, "kind": kind, "insert": insert.len > 0 ? insert : label})
}

fn _completion_seed_items(str prefix) list {
   def words = ["fn", "def", "mut", "struct", "enum", "module", "use", "match", "case", "try", "catch", "return", "comptime", "nil", "true", "false", "self"]
   mut out = []
   mut i = 0
   while i < words.len {
      def w = to_str(words.get(i, ""))
      if prefix.len == 0 || str.startswith(w, prefix) { out = _completion_push(out, w, "language", "keyword", w) }
      i += 1
   }
   out
}

fn _completion_items(str prefix, bool lsp_hint=false) list {
   mut out = _completion_seed_items(prefix)
   def lines = ed.current_lines(st)
   mut li = 0
   while li < lines.len && out.len < 22 {
      def line = to_str(lines.get(li, ""))
      mut i = 0
      while i < line.len && out.len < 22 {
         if !_word_byte(load8(line, i)) { i += 1 continue }
         def start = i
         while i < line.len && _word_byte(load8(line, i)) { i += 1 }
         def word = str.str_slice(line, start, i)
         if word.len > prefix.len && (prefix.len == 0 || str.startswith(word, prefix)) {
            out = _completion_push(out, word, "buffer line " + to_str(li + 1), "symbol", word)
         }
      }
      li += 1
   }
   if lsp_hint {
      out = _completion_push(out, "LSP completion requested", to_str(lsp_state.get("last", "waiting for language server")), "lsp", prefix)
   }
   out
}

fn _completion_open(bool lsp_hint=false) int {
   def prefix = _word_prefix_before_cursor()
   def items = _completion_items(prefix, lsp_hint)
   completion_state["open"] = items.len > 0
   completion_state["items"] = items
   completion_state["index"] = 0
   completion_state["prefix"] = prefix
   if items.len <= 0 { _status("no completions") }
   0
}

fn _completion_help(str title, str detail) int {
   completion_state["open"] = true
   completion_state["items"] = [{"label": title, "detail": detail, "kind": "lsp", "insert": ""}]
   completion_state["index"] = 0
   completion_state["prefix"] = ""
   0
}

fn _completion_apply() int {
   if !bool(completion_state.get("open", false)) { return 0 }
   def items = completion_state.get("items", [])
   if items.len <= 0 { return _completion_close() }
   def idx = min(max(0, int(completion_state.get("index", 0))), items.len - 1)
   def item = items.get(idx, {})
   def insert = to_str(item.get("insert", ""))
   def prefix = to_str(completion_state.get("prefix", ""))
   if insert.len > prefix.len && str.startswith(insert, prefix) {
      _edit_insert(str.str_slice(insert, prefix.len, insert.len))
   } else {
      _status(to_str(item.get("detail", item.get("label", ""))))
   }
   _completion_close()
}

fn _completion_handle_key(any data) bool {
   if !bool(completion_state.get("open", false)) { return false }
   def items = completion_state.get("items", [])
   if window.event_key_is(data, key.KEY_ESCAPE) { _completion_close() return true }
   if window.event_key_is(data, key.KEY_DOWN) {
      completion_state["index"] = items.len <= 0 ? 0 : (int(completion_state.get("index", 0)) + 1) % items.len
      return true
   }
   if window.event_key_is(data, key.KEY_UP) {
      completion_state["index"] = items.len <= 0 ? 0 : (int(completion_state.get("index", 0)) + items.len - 1) % items.len
      return true
   }
   if window.event_key_is(data, key.KEY_ENTER) || window.event_key_is(data, key.KEY_TAB) { _completion_apply() return true }
   false
}

fn _definition_pos_in(list lines, str word) list {
   if word.len <= 0 { return [-1, 0] }
   def hit = _definition_index_from_lines(lines, "").get(word, dict(0))
   if is_dict(hit) && hit.len > 0 { return [int(hit.get("line", -1)), int(hit.get("col", 0))] }
   [-1, 0]
}

fn _definition_decl_name(str s, int start) str {
   mut i = start
   while i < s.len {
      def c = load8(s, i)
      if _word_byte(c) { break }
      i += 1
   }
   def first_start = i
   while i < s.len && _word_byte(load8(s, i)) { i += 1 }
   def first = str.str_slice(s, first_start, i)
   mut j = i
   while j < s.len && load8(s, j) == 32 { j += 1 }
   if j < s.len && load8(s, j) == 58 {
      j += 1
      while j < s.len && load8(s, j) == 32 { j += 1 }
      def second_start = j
      while j < s.len && _word_byte(load8(s, j)) { j += 1 }
      def second = str.str_slice(s, second_start, j)
      if second.len > 0 { return second }
   }
   first
}

fn _definition_add(dict idx, str kind, str name, str raw, int line, str path) dict {
   if name.len <= 0 || idx.contains(name) { return idx }
   def col = max(0, str.find(raw, name))
   idx[name] = {"kind": kind, "name": name, "path": path, "line": line, "col": col}
   idx
}

fn _definition_index_from_lines(list lines, str path="", int max_lines=0) dict {
   mut idx = dict(128)
   mut i = 0
   def n = max_lines > 0 ? min(lines.len, max_lines) : lines.len
   while i < n {
      def raw = to_str(lines.get(i, ""))
      def line = str.strip(raw)
      if str.startswith(line, "fn ") { idx = _definition_add(idx, "fn", _definition_decl_name(line, 3), raw, i, path) }
      elif str.startswith(line, "def ") { idx = _definition_add(idx, "def", _definition_decl_name(line, 4), raw, i, path) }
      elif str.startswith(line, "mut ") { idx = _definition_add(idx, "mut", _definition_decl_name(line, 4), raw, i, path) }
      elif str.startswith(line, "struct ") { idx = _definition_add(idx, "struct", _definition_decl_name(line, 7), raw, i, path) }
      elif str.startswith(line, "enum ") { idx = _definition_add(idx, "enum", _definition_decl_name(line, 5), raw, i, path) }
      elif str.startswith(line, "type ") { idx = _definition_add(idx, "type", _definition_decl_name(line, 5), raw, i, path) }
      elif str.startswith(line, "module ") { idx = _definition_add(idx, "module", _definition_decl_name(line, 7), raw, i, path) }
      i += 1
   }
   idx
}

fn _definition_line_for(str word) int {
   int(_definition_pos_in(ed.current_lines(st), word).get(0, -1))
}

fn _current_definition_index() dict {
   def b = ed.current_buffer(st)
   def name = to_str(int(st.get("active", 0))) + ":" + to_str(b.get("name", ""))
   if definition_cache_rev == content_rev && definition_cache_name == name { return definition_cache }
   definition_cache = _definition_index_from_lines(ed.current_lines(st), _current_path())
   definition_cache_rev = content_rev
   definition_cache_name = name
   definition_cache
}

fn _current_definition(str word) dict {
   def idx = _current_definition_index()
   idx.contains(word) ? idx.get(word, dict(0)) : dict(0)
}

fn _source_like_path(str path) bool {
   def ext = str.lower(ospath.extname(path))
   ext == ".ny" || ext == ".c" || ext == ".h" || ext == ".cpp" || ext == ".hpp" || ext == ".cc" || ext == ".cxx" || ext == ".py" || ext == ".js" || ext == ".ts"
}

fn _goto_max_files() int { common.env_int_clamped("NY_EDITOR_GOTO_MAX_FILES", 160, 16, 2000) }

fn _goto_max_bytes() int { common.env_int_clamped("NY_EDITOR_GOTO_MAX_BYTES", 524288, 4096, 8388608) }

fn _project_definition_key(str active_path, int max_files, int max_bytes) str {
   project.root(project_state) + "\t" + to_str(project.tree(project_state).len) + "\t" + ospath.normalize(active_path) + "\t" + to_str(max_files) + "\t" + to_str(max_bytes)
}

fn _prepare_project_definition_cache(str active_path, int max_files, int max_bytes) int {
   def key = _project_definition_key(active_path, max_files, max_bytes)
   if project_definition_cache_key == key { return 0 }
   project_definition_cache_key = key
   project_definition_cache = dict(512)
   project_definition_scanned = dict(256)
   project_definition_cache_complete = false
   0
}

fn _project_cache_file_defs(str path, str active_path, int max_bytes) int {
   if path.len <= 0 || project_definition_scanned.contains(path) { return 0 }
   project_definition_scanned[path] = true
   if ospath.normalize(path) == ospath.normalize(active_path) { return 0 }
   def text = session.read_text(path)
   if text.len <= 0 || text.len > max_bytes { return 0 }
   def defs = _definition_index_from_lines(ed.split_lines(text), path)
   def keys = dict_keys(defs)
   mut i = 0
   while i < keys.len {
      def k = to_str(keys.get(i, ""))
      if k.len > 0 && !project_definition_cache.contains(k) {
         project_definition_cache[k] = defs.get(k, dict(0))
      }
      i += 1
   }
   0
}

fn _find_project_definition(str word) dict {
   if word.len <= 0 { return dict(0) }
   _ensure_project_tree()
   def active_path = ospath.normalize(_current_path())
   def rows = project.tree(project_state)
   def max_files = _goto_max_files()
   def max_bytes = _goto_max_bytes()
   _prepare_project_definition_cache(active_path, max_files, max_bytes)
   if project_definition_cache.contains(word) { return project_definition_cache.get(word, dict(0)) }
   if project_definition_cache_complete { return dict(0) }
   mut checked = project_definition_scanned.len
   mut i = 0
   while i < rows.len && checked < max_files {
      def entry = rows.get(i, {})
      def path = to_str(entry.get("path", ""))
      if !entry.get("dir", false) && path.len > 0 && _source_like_path(path) {
         if !project_definition_scanned.contains(path) {
            checked += 1
            _project_cache_file_defs(path, active_path, max_bytes)
            if project_definition_cache.contains(word) { return project_definition_cache.get(word, dict(0)) }
         }
      }
      i += 1
   }
   if i >= rows.len { project_definition_cache_complete = true }
   dict(0)
}

fn _goto_definition() int {
   def word = _word_at_cursor()
   if word.len <= 0 { _status("no symbol") return 0 }
   def here = _current_definition(word)
   if is_dict(here) && here.len > 0 {
      _nav_push_current()
      st["cursor_line"] = int(here.get("line", 0))
      st["cursor_col"] = int(here.get("col", 0))
      st = ed.clamp_cursor(st)
      _follow_cursor()
      _status("definition " + word)
      return 0
   }
   def found = _find_project_definition(word)
   if is_dict(found) && found.len > 0 {
      _nav_push_current()
      st = session.open_file(st, to_str(found.get("path", "")))
      st["cursor_line"] = int(found.get("line", 0))
      st["cursor_col"] = int(found.get("col", 0))
      st = ed.clamp_cursor(st)
      _buffer_changed(true)
      _follow_cursor()
      _status("definition " + word)
      return 0
   }
   _lsp_request("definition")
   0
}

fn _select_line() int {
   def lines = ed.current_lines(st)
   if _extra_cursor_count() > 0 {
      mut ranges = []
      def prow = min(max(int(st.get("cursor_line", 0)), 0), max(0, lines.len - 1))
      ranges = ranges.append(_range_make(prow, 0, prow, to_str(lines.get(prow, "")).len, true))
      def extras = _extra_cursors()
      mut i = 0
      while i < extras.len {
         def row2 = min(max(_cursor_row(extras.get(i)), 0), max(0, lines.len - 1))
         ranges = ranges.append(_range_make(row2, 0, row2, to_str(lines.get(row2, "")).len, false))
         i += 1
      }
      st["multi_selects"] = ranges
      def p = ranges.get(0, [])
      st["sel_a_line"] = int(p.get(0, 0))
      st["sel_a_col"] = int(p.get(1, 0))
      st["sel_b_line"] = int(p.get(2, 0))
      st["sel_b_col"] = int(p.get(3, 0))
      st["sel_active"] = true
      _status("selected " + to_str(ranges.len) + " lines")
      return 0
   }
   def row = min(max(int(st.get("cursor_line", 0)), 0), max(0, lines.len - 1))
   st["cursor_line"] = row
   st["cursor_col"] = to_str(lines.get(row, "")).len
   st["sel_a_line"] = row
   st["sel_a_col"] = 0
   st["sel_b_line"] = row
   st["sel_b_col"] = int(st.get("cursor_col", 0))
   st["sel_active"] = true
   st["multi_selects"] = []
   _status("selected line")
   0
}

fn _duplicate_line() int {
   def lines = ed.current_lines(st)
   def row = min(max(int(st.get("cursor_line", 0)), 0), max(0, lines.len - 1))
   mut out = []
   mut i = 0
   while i < lines.len {
      out = out.append(lines.get(i, ""))
      if i == row { out = out.append(lines.get(i, "")) }
      i += 1
   }
   st["cursor_line"] = row + 1
   _mutate_lines(out, "duplicated line")
}

fn _delete_line() int {
   def lines = ed.current_lines(st)
   if lines.len <= 1 {
      st["cursor_line"] = 0
      st["cursor_col"] = 0
      return _mutate_lines([""], "deleted line")
   }
   def row = min(max(int(st.get("cursor_line", 0)), 0), lines.len - 1)
   mut out = []
   mut i = 0
   while i < lines.len {
      if i != row { out = out.append(lines.get(i, "")) }
      i += 1
   }
   st["cursor_line"] = min(row, out.len - 1)
   st["cursor_col"] = min(int(st.get("cursor_col", 0)), to_str(out.get(int(st.get("cursor_line", 0)), "")).len)
   _mutate_lines(out, "deleted line")
}

fn _move_line(int dir) int {
   def lines = ed.current_lines(st)
   if lines.len <= 1 { return 0 }
   def row = min(max(int(st.get("cursor_line", 0)), 0), lines.len - 1)
   def other = row + dir
   if other < 0 || other >= lines.len { return 0 }
   mut out = lines
   def a = out.get(row, "")
   out[row] = out.get(other, "")
   out[other] = a
   st["cursor_line"] = other
   _mutate_lines(out, dir < 0 ? "moved line up" : "moved line down")
}

fn _join_line(bool tight=false) int {
   def lines = ed.current_lines(st)
   def row = min(max(int(st.get("cursor_line", 0)), 0), max(0, lines.len - 1))
   if row + 1 >= lines.len { return 0 }
   def a = to_str(lines.get(row, ""))
   def b = to_str(lines.get(row + 1, ""))
   def mid = tight || a.len == 0 || b.len == 0 ? "" : " "
   mut out = []
   mut i = 0
   while i < lines.len {
      if i == row { out = out.append(a + mid + str.strip(b)) }
      elif i != row + 1 { out = out.append(lines.get(i, "")) }
      i += 1
   }
   st["cursor_col"] = a.len
   _mutate_lines(out, "joined lines")
}

fn _selection_line_span() list {
   def ranges = _selection_ranges_active()
   if ranges.len <= 0 { return [int(st.get("cursor_line", 0)), int(st.get("cursor_line", 0))] }
   mut lo = 1000000000
   mut hi = 0
   mut i = 0
   while i < ranges.len {
      def r = _range_norm(ranges.get(i, []))
      lo = min(lo, int(r.get(0, 0)))
      hi = max(hi, int(r.get(2, 0)))
      i += 1
   }
   [lo, hi]
}

fn _sort_lines() int {
   def lines = ed.current_lines(st)
   if lines.len <= 1 { return 0 }
   def span = _selection_line_span()
   def start = min(max(int(span.get(0, 0)), 0), lines.len - 1)
   def end = min(max(int(span.get(1, start)), start), lines.len - 1)
   mut part = []
   mut i = start
   while i <= end { part = part.append(lines.get(i, "")) i += 1 }
   sort(part)
   mut out = lines
   i = 0
   while i < part.len { out[start + i] = part.get(i, "") i += 1 }
   _mutate_lines(out, "sorted lines")
}

fn _rstrip_ws(str s) str {
   mut n = s.len
   while n > 0 {
      def c = load8(s, n - 1)
      if c != 32 && c != 9 { break }
      n -= 1
   }
   str.str_slice(s, 0, n)
}

fn _strip_trailing_ws() int {
   def lines = ed.current_lines(st)
   mut out = []
   mut changed = false
   mut i = 0
   while i < lines.len {
      def old = to_str(lines.get(i, ""))
      def clean = _rstrip_ws(old)
      if clean != old { changed = true }
      out = out.append(clean)
      i += 1
   }
   if !changed { _status("no trailing whitespace") return 0 }
   _mutate_lines(out, "stripped whitespace")
}

fn _comment_prefix() str {
   def ext = str.lower(ospath.extname(to_str(ed.current_buffer(st).get("name", ""))))
   if ext == ".py" || ext == ".sh" || ext == ".rb" || ext == ".pl" || ext == ".yaml" || ext == ".yml" { return "# " }
   if ext == ".lua" { return "-- " }
   "// "
}

fn _toggle_comment() int {
   def lines = ed.current_lines(st)
   if lines.len <= 0 { return 0 }
   def span = _selection_line_span()
   def start = min(max(int(span.get(0, 0)), 0), lines.len - 1)
   def end = min(max(int(span.get(1, start)), start), lines.len - 1)
   def prefix = _comment_prefix()
   mut all_commented = true
   mut i = start
   while i <= end {
      if !str.startswith(str.strip(to_str(lines.get(i, ""))), str.strip(prefix)) { all_commented = false break }
      i += 1
   }
   mut out = lines
   i = start
   while i <= end {
      def line = to_str(out.get(i, ""))
      if all_commented {
         def at = str.find(line, str.strip(prefix))
         if at >= 0 { out[i] = str.str_slice(line, 0, at) + str.str_slice(line, at + str.strip(prefix).len, line.len) }
      } else {
         out[i] = prefix + line
      }
      i += 1
   }
   _mutate_lines(out, all_commented ? "uncommented" : "commented")
}

fn _insert_pair(str open, str close) int {
   _apply_hist(history.insert_pair(hist, st, open, close))
}

fn _ctrl(any data) bool {
   (int(data.get("mods", data.get("mod", window.get_modifiers(win)))) & key.MOD_CONTROL) != 0
}

fn _cmd_mod(any data) bool {
   (int(data.get("mods", data.get("mod", window.get_modifiers(win)))) & (key.MOD_CONTROL | key.MOD_SUPER | key.MOD_META)) != 0
}

fn _shift(any data) bool {
   (int(data.get("mods", data.get("mod", window.get_modifiers(win)))) & key.MOD_SHIFT) != 0
}

fn _alt(any data) bool {
   (int(data.get("mods", data.get("mod", window.get_modifiers(win)))) & key.MOD_ALT) != 0
}

fn _key_event_char_cp(any data) int {
   def k = int(data.get("key", key.KEY_NULL))
   def shifted = _shift(data)
   case k {
      key.KEY_A..key.KEY_Z -> shifted ? k : k + 32
      key.KEY_0..key.KEY_9 -> k
      key.KEY_SPACE -> 32
      key.KEY_MINUS -> shifted ? 95 : 45
      key.KEY_EQUAL -> shifted ? 43 : 61
      key.KEY_SEMICOLON -> shifted ? 58 : 59
      key.KEY_APOSTROPHE -> shifted ? 34 : 39
      key.KEY_COMMA -> shifted ? 60 : 44
      key.KEY_PERIOD -> shifted ? 62 : 46
      key.KEY_SLASH -> shifted ? 63 : 47
      key.KEY_GRAVE -> shifted ? 126 : 96
      key.KEY_LEFT_BRACKET -> shifted ? 123 : 91
      key.KEY_RIGHT_BRACKET -> shifted ? 125 : 93
      key.KEY_BACKSLASH -> shifted ? 124 : 92
      _ -> 0
   }
}

fn _same_char_fold(int a, int b) bool {
   if a == b { return true }
   if a >= 65 && a <= 90 && a + 32 == b { return true }
   if a >= 97 && a <= 122 && a - 32 == b { return true }
   false
}

fn _suppress_char_from_key(any data) int {
   def cp = _key_event_char_cp(data)
   if cp > 0 {
      suppress_char_cp = cp
      if INPUT_TRACE { print("[editor:key] suppress-char cp=" + to_str(cp)) }
   }
   0
}

fn _consume_suppressed_char(any data) bool {
   def cp = int(data.get("char", 0))
   if suppress_char_cp <= 0 || cp <= 0 { return false }
   def expected = suppress_char_cp
   suppress_char_cp = 0
   if _same_char_fold(cp, expected) {
      if INPUT_TRACE { print("[editor:char] suppressed cp=" + to_str(cp)) }
      return true
   }
   false
}

fn _line_text_x(dict lay) f64 {
   float(lay.get("edit_x", 0.0)) + (bool(st.get("show_line_numbers", true)) ? 58.0 : 16.0)
}

fn _layout_with_terminal(dict lay, f64 sh) dict {
   lay = ed.with_bottom_dock(lay, sh, termpane.is_open(term_state), float(st.get("term_h", 0.0)))
   if termpane.is_open(term_state) {
      st["term_h"] = float(lay.get("dock_h", st.get("term_h", 0.0)))
      term_state = termpane.resize(
         term_state,
         float(lay.get("dock_x", 0.0)) + 8.0,
         float(lay.get("dock_y", 0.0)) + TERM_TAB_H,
         float(lay.get("dock_w", 1.0)) - 16.0,
         max(48.0, float(lay.get("dock_h", 1.0)) - TERM_TAB_H - 1.0)
      )
   }
   lay
}

fn _dock_resize_hit(dict lay, f64 mx, f64 my) bool {
   if !bool(lay.get("dock_open", false)) { return false }
   def x = float(lay.get("dock_x", 0.0))
   def y = float(lay.get("dock_y", 0.0))
   def w = float(lay.get("dock_w", 0.0))
   mx >= x && mx <= x + w && my >= y - PANEL_SPLIT_HIT && my <= y + PANEL_SPLIT_HIT
}

fn _dock_height_from_mouse(dict lay, f64 my) f64 {
   def ey = float(lay.get("edit_y", 0.0))
   def status_y = float(lay.get("status_y", ey + DOCK_MIN_H + EDIT_MIN_H))
   def max_h = max(DOCK_MIN_H, status_y - ey - EDIT_MIN_H)
   min(max(status_y - my, DOCK_MIN_H), max_h)
}

fn _term_tab_w(f64 w, int count) f64 {
   if count <= 0 { return TERM_TAB_MIN_W }
   min(TERM_TAB_MAX_W, max(TERM_TAB_MIN_W, (w - 52.0) / float(count + 1)))
}

fn _term_tab_hit(dict lay, f64 mx, f64 my) dict {
   if !termpane.is_open(term_state) { return {"action": ""} }
   def x = float(lay.get("dock_x", 0.0))
   def y = float(lay.get("dock_y", 0.0))
   def w = float(lay.get("dock_w", 0.0))
   if my < y + 2.0 || my > y + TERM_TAB_H - 2.0 || mx < x + 6.0 || mx > x + w - 6.0 { return {"action": ""} }
   def n = termpane.tab_count(term_state)
   def tw = _term_tab_w(w, n)
   def start_x = x + 6.0
   mut i = 0
   while i < n {
      def tx = start_x + float(i) * tw
      if mx >= tx && mx <= tx + tw - 2.0 {
         return {"action": mx >= tx + tw - 23.0 ? "close" : "select", "idx": i}
      }
      i += 1
   }
   def plus_x = start_x + float(n) * tw + 3.0
   if mx >= plus_x && mx <= plus_x + 24.0 { return {"action": "new", "idx": n} }
   {"action": ""}
}

fn _scrollbar_value_from_mouse(f64 track_y, f64 track_h, int total, int visible, f64 my) int {
   def max_scroll = max(0, total - visible)
   if max_scroll <= 0 || track_h <= 1.0 { return 0 }
   def thumb_h = max(18.0, track_h * float(visible) / float(max(visible, total)))
   def range = max(1.0, track_h - thumb_h)
   def pos = min(max((my - track_y - thumb_h * 0.5) / range, 0.0), 1.0)
   min(max_scroll, max(0, int(pos * float(max_scroll) + 0.5)))
}

fn _scrollbar_track_hit(f64 mx, f64 my, f64 x, f64 y, f64 w, f64 h) bool {
   h > 8.0 && mx >= x && mx <= x + w && my >= y && my <= y + h
}

fn _editor_scrollbar_target(dict lay, f64 mx, f64 my) dict {
   def lines = ed.current_lines(st)
   def visible = ed.visible_rows(float(lay.get("edit_h", 0.0)))
   if lines.len <= visible { return dict(0) }
   def x = float(lay.get("edit_x", 0.0)) + float(lay.get("edit_w", 0.0)) - 18.0
   def y = float(lay.get("edit_y", 0.0)) + 10.0
   def h = max(1.0, float(lay.get("edit_h", 0.0)) - 20.0)
   if !_scrollbar_track_hit(mx, my, x, y, 18.0, h) { return dict(0) }
   {"kind": "editor", "track_y": y, "track_h": h, "total": lines.len, "visible": visible}
}

fn _rail_scrollbar_target(dict lay, f64 mx, f64 my) dict {
   if !bool(st.get("show_hierarchy", true)) { return dict(0) }
   def rail_x = float(lay.get("rail_x", 0.0))
   def rail_y = float(lay.get("rail_y", 0.0))
   def rail_w = float(lay.get("rail_w", 0.0))
   def rail_h = float(lay.get("rail_h", 0.0))
   if rail_w <= 1.0 || mx < rail_x + rail_w - 16.0 || mx > rail_x + rail_w { return dict(0) }
   def body_y = rail_y + RAIL_TABS_H
   def body_h = rail_h - RAIL_TABS_H
   def tab = _rail_tab()
   if tab == "git" {
      def rows_git = project.changes(project_state)
      def visible_git = project.visible_count(body_h)
      def ty = body_y + project.TREE_HEADER_H
      def th = max(1.0, body_h - project.TREE_HEADER_H)
      if rows_git.len > visible_git && _scrollbar_track_hit(mx, my, rail_x + rail_w - 16.0, ty, 16.0, th) {
         return {"kind": "git", "track_y": ty, "track_h": th, "total": rows_git.len, "visible": visible_git}
      }
      return dict(0)
   }
   if tab == "timeline" {
      def rows_time = _timeline_rows()
      def visible_time = project.visible_count(body_h)
      def ty = body_y + project.TREE_HEADER_H
      def th = max(1.0, body_h - project.TREE_HEADER_H)
      if rows_time.len > visible_time && _scrollbar_track_hit(mx, my, rail_x + rail_w - 16.0, ty, 16.0, th) {
         return {"kind": "timeline", "track_y": ty, "track_h": th, "total": rows_time.len, "visible": visible_time}
      }
      return dict(0)
   }
   def show_project = bool(st.get("show_project", true))
   def show_outline = bool(st.get("show_outline", true))
   if show_project { _ensure_project_tree() }
   def rows = project.tree(project_state)
   def syms = show_outline ? _outline_symbols() : []
   def tree_h = (!show_project) ? 0.0 : (show_outline ? _rail_tree_height(body_h, rows.len, syms.len) : body_h)
   if show_project {
      def visible_tree = project.visible_count(tree_h)
      def ty_tree = body_y + project.TREE_HEADER_H
      def th_tree = max(1.0, tree_h - project.TREE_HEADER_H)
      if rows.len > visible_tree && _scrollbar_track_hit(mx, my, rail_x + rail_w - 16.0, ty_tree, 16.0, th_tree) {
         return {"kind": "tree", "track_y": ty_tree, "track_h": th_tree, "total": rows.len, "visible": visible_tree}
      }
   }
   if show_outline {
      def outline_y = body_y + (show_project ? tree_h + 1.0 : 0.0)
      def outline_h = body_h - (outline_y - body_y)
      def visible_outline = _outline_visible(outline_h)
      def ty_outline = outline_y + SECTION_H
      def th_outline = max(1.0, outline_h - SECTION_H)
      if syms.len > visible_outline && _scrollbar_track_hit(mx, my, rail_x + rail_w - 16.0, ty_outline, 16.0, th_outline) {
         return {"kind": "outline", "track_y": ty_outline, "track_h": th_outline, "total": syms.len, "visible": visible_outline}
      }
   }
   dict(0)
}

fn _scrollbar_target_at(dict lay, f64 mx, f64 my) dict {
   def edit = _editor_scrollbar_target(lay, mx, my)
   if edit.len > 0 { return edit }
   _rail_scrollbar_target(lay, mx, my)
}

fn _apply_scrollbar_target(dict target, f64 my) int {
   if !is_dict(target) || target.len <= 0 { return 0 }
   def value = _scrollbar_value_from_mouse(
      float(target.get("track_y", 0.0)),
      float(target.get("track_h", 1.0)),
      int(target.get("total", 0)),
      int(target.get("visible", 0)),
      my
   )
   case to_str(target.get("kind", "")){
      "editor" -> { st["scroll"] = value st["scroll_follow"] = false }
      "tree" -> { st["tree_scroll"] = value }
      "outline" -> { st["outline_scroll"] = value }
      "git" -> { st["git_scroll"] = value }
      "timeline" -> { st["timeline_scroll"] = value }
      _ -> {}
   }
   0
}

fn _rail_width_from_mouse(dict lay, f64 mx) f64 {
   def sw = float(lay.get("edit_x", 0.0)) + float(lay.get("edit_w", 0.0))
   min(max(mx, RAIL_MIN_W), max(RAIL_MIN_W, sw - EDIT_MIN_W))
}

fn _place_cursor(dict lay, f64 x, f64 y) int {
   def lines = ed.current_lines(st)
   if lines.len <= 0 { return 0 }
   def top = float(lay.get("edit_y", 0.0)) + 14.0
   def bottom = float(lay.get("edit_y", 0.0)) + float(lay.get("edit_h", 0.0)) - 8.0
   def row = min(max(ed.row_at(lay, min(max(y, top), bottom), int(st.get("scroll", 0))), 0), lines.len - 1)
   def line = to_str(lines.get(row, ""))
   st["cursor_line"] = row
   st["cursor_col"] = ed.col_at(font_body, line, x, _line_text_x(lay))
   st = ed.clamp_cursor(st)
   _follow_cursor()
   st["quit_prompt"] = false
   last_escape_ns = 0
   0
}

fn _click_editor(dict lay, f64 x, f64 y) int {
   def row = ed.row_at(lay, y, int(st.get("scroll", 0)))
   def lines = ed.current_lines(st)
   if row < 0 || row >= lines.len { return 0 }
   _place_cursor(lay, x, y)
}

fn _palette_matches() list {
   pal.state_matches(palette_state)
}

fn _palette_open() int {
   palette_state["cfg"] = command_cfg
   palette_state = pal.open(palette_state)
   _status("command palette")
   0
}

fn _set_ui_toggle(str key_name) bool {
   def k = "show_" + key_name
   def on = !bool(st.get(k, true))
   st[k] = on
   _request_full_redraw(2)
   _save_ui_cache()
   on
}

fn _set_focus(str pane, bool announce=true) int {
   st["focus"] = pane
   term_state["focus"] = pane == "terminal" && termpane.is_open(term_state)
   _request_full_redraw(2)
   if announce { _status("focus " + pane) }
   0
}

fn _focus_next() int {
   _set_focus(ed.focus_next(to_str(st.get("focus", "editor")), bool(st.get("show_hierarchy", true)), termpane.is_open(term_state)))
}

fn _pane_resize(str dir) int {
   if dir == "left" {
      st["rail_w"] = max(RAIL_MIN_W, float(st.get("rail_w", 250.0)) - 24.0)
   } elif dir == "right" {
      st["show_hierarchy"] = true
      st["rail_w"] = min(PANE_MAX, float(st.get("rail_w", 250.0)) + 24.0)
   } elif dir == "up" {
      st["term_h"] = min(PANE_MAX, float(st.get("term_h", 0.0)) + 24.0)
   } elif dir == "down" {
      st["term_h"] = max(DOCK_MIN_H, float(st.get("term_h", 0.0)) - 24.0)
   }
   _request_full_redraw(2)
   _save_ui_cache()
   _status("resize " + dir)
   0
}

fn _close_focused_pane() int {
   def focus = to_str(st.get("focus", "editor"))
   if focus == "terminal" {
      term_state = termpane.close(term_state)
      _set_focus("editor")
   } elif focus == "project" {
      st["show_hierarchy"] = false
      _save_ui_cache()
      _set_focus("editor")
   } else {
      _status("editor stays open")
   }
   0
}

fn _lsp_output(str title, str body) int {
   def path = _current_path()
   st = session.set_output_buffer(st, lsp.output_name(path), title + "\n\n" + body)
   0
}

fn _lsp_start() int {
   if ed.current_buffer(st).get("readonly", false) { _status("lsp skipped for preview") return 0 }
   if _current_path().len <= 0 { _status("save before lsp") return 0 }
   lsp_state = lsp.start(lsp_state, _current_path(), _current_text())
   _lsp_output("LSP status", lsp.status(lsp_state))
   _status(lsp.status(lsp_state))
   0
}

fn _lsp_request(str kind) int {
   if ed.current_buffer(st).get("readonly", false) { _status("lsp skipped for preview") return 0 }
   def path = _current_path()
   if path.len <= 0 { _status("save before lsp") return 0 }
   def line = int(st.get("cursor_line", 0))
   def col = int(st.get("cursor_col", 0))
   if !lsp_state.get("active", false) { lsp_state = lsp.start(lsp_state, path, _current_text()) }
   if kind == "completion" {
      lsp_state = lsp.completion(lsp_state, path, line, col)
      _completion_open(true)
   } elif kind == "hover" {
      lsp_state = lsp.hover(lsp_state, path, line, col)
      _completion_help("LSP hover", to_str(lsp_state.get("last", lsp.status(lsp_state))))
   } elif kind == "definition" {
      lsp_state = lsp.definition(lsp_state, path, line, col)
      _lsp_output("LSP " + kind, to_str(lsp_state.get("last_request", lsp.status(lsp_state))))
   }
   _status(to_str(lsp_state.get("last", "lsp")))
   0
}

fn _lsp_diagnostics() int {
   def path = _current_path()
   if path.len <= 0 { _status("save before diagnostics") return 0 }
   def res = tools.check_file(path)
   lsp_state["diagnostics"] = lsp.diagnostics_from_check(res)
   _lsp_output("LSP diagnostics", lsp.diagnostics_text(lsp_state))
   _status("diagnostics")
   0
}

fn _run_command(str id) int {
   ui_state = interact.history_add(ui_state, id)
   _request_full_redraw(2)
   if id != "quit" {
      st["quit_prompt"] = false
      last_escape_ns = 0
   }
   if id == "save" { _save() }
   elif id == "save-all" { _save_all() }
   elif id == "reload" { _reload_from_disk() }
   elif id == "copy-path" { _copy_file_path() }
   elif id == "copy-relative-path" { _copy_relative_path(_current_path()) }
   elif id == "copy-line-info" { _copy_line_info() }
   elif id == "open-file" { _status("use project tree or drag-drop to open") }
   elif id == "new-file" { _start_new_file(session.project_entry_for_path(project_state, _current_path())) }
   elif id == "delete-file" { _delete_project_entry(session.project_entry_for_path(project_state, _current_path())) }
   elif id == "undo-file-op" { _undo_file_op() }
   elif id == "redo-file-op" { _redo_file_op() }
   elif id == "new-scratch" { _new_scratch() }
   elif id == "kill-buffer" { _kill_buffer() }
   elif id == "diff-clipboard" { _diff_clipboard() }
   elif id == "run" { _run_current_file() }
   elif id == "check" { _check_current_file() }
   elif id == "format" { _format_current_file() }
   elif id == "debug" || id == "dap-debug" || id == "debug-start" { _debug_start_current() }
   elif id == "debug-breakpoint" { _debug_breakpoint() }
   elif id == "debug-continue" { _debug_send("continue") }
   elif id == "debug-next" { _debug_send("next") }
   elif id == "debug-step" { _debug_send("step") }
   elif id == "debug-finish" { _debug_send("finish") }
   elif id == "debug-backtrace" { _debug_send("bt") }
   elif id == "debug-until-line" { _debug_until_current_line() }
   elif id == "debug-jump-line" { _debug_jump_current_line() }
   elif id == "debug-inspect-line" { _debug_inspect_line() }
   elif id == "debug-reverse-continue" { _debug_send("reverse-continue") }
   elif id == "debug-reverse-next" { _debug_send("reverse-next") }
   elif id == "debug-reverse-step" { _debug_send("reverse-step") }
   elif id == "debug-coredump" { _debug_load_coredump() }
   elif id == "refresh" { project_state = project.refresh_git(project_state) st["project_loaded"] = true st["git_loaded"] = true _invalidate_project_definitions() _status("project refreshed") }
   elif id == "rename" { _start_rename(prompt.context_entry(context_state)) }
   elif id == "find" { _find_open() }
   elif id == "replace" { _find_replace_open() }
   elif id == "find-next" { _find_step(1) }
   elif id == "find-prev" { _find_step(-1) }
   elif id == "replace-current" { _find_replace_current() }
   elif id == "replace-all" { _find_replace_all() }
   elif id == "find-toggle-regex" { _find_toggle("regex") }
   elif id == "find-toggle-case" { _find_toggle("case") }
   elif id == "find-toggle-word" { _find_toggle("word") }
   elif id == "palette" { _palette_open() }
   elif id == "quit" {
      if st.get("quit_prompt", false) { window.set_should_close(win, true) }
      else { st["quit_prompt"] = true _status("quit? run quit again") }
   }
   elif id == "undo" { _undo() }
   elif id == "redo" { _redo() }
   elif id == "copy" { _copy_selection_or_buffer() }
   elif id == "cut" { _cut_selection() }
   elif id == "paste" { _paste_clipboard() }
   elif id == "mark-whole" { _mark_whole_buffer() }
   elif id == "select-line" { _select_line() }
   elif id == "kill-line" { _kill_line() }
   elif id == "duplicate-line" { _duplicate_line() }
   elif id == "delete-line" { _delete_line() }
   elif id == "move-line-up" { _move_line(-1) }
   elif id == "move-line-down" { _move_line(1) }
   elif id == "join-lines" { _join_line(false) }
   elif id == "join-lines-tight" { _join_line(true) }
   elif id == "sort-lines" { _sort_lines() }
   elif id == "toggle-comment" { _toggle_comment() }
   elif id == "strip-trailing-whitespace" { _strip_trailing_ws() }
   elif id == "color-picker" { _color_picker_open() }
   elif id == "next" { _nav_push_current() st = ed.switch_buffer(st, 1) _buffer_changed(true) _nav_record_current() _status("next buffer") }
   elif id == "prev" { _nav_push_current() st = ed.switch_buffer(st, -1) _buffer_changed(true) _nav_record_current() _status("previous buffer") }
   elif id == "go-back" { _nav_go(-1) }
   elif id == "go-forward" { _nav_go(1) }
   elif id == "go-definition" { _goto_definition() }
   elif id == "toggle-sidebar" {
      def on = !bool(st.get("show_hierarchy", true))
      st["show_hierarchy"] = on
      if on && !bool(st.get("show_project", true)) && !bool(st.get("show_outline", true)) { st["show_project"] = true }
      if !on && to_str(st.get("focus", "editor")) == "project" { _set_focus("editor", false) }
      _save_ui_cache()
      _status(on ? "hierarchy on" : "hierarchy off")
   }
   elif id == "toggle-titlebar" { _status(_set_ui_toggle("titlebar") ? "title bar on" : "title bar off") }
   elif id == "zoom-in" { _zoom_editor_font(1) }
   elif id == "zoom-out" { _zoom_editor_font(-1) }
   elif id == "zoom-reset" { _reset_editor_font() }
   elif id == "pane-next" { _focus_next() }
   elif id == "pane-project" { st["show_hierarchy"] = true _save_ui_cache() _set_focus("project") }
   elif id == "pane-editor" { _set_focus("editor") }
   elif id == "pane-terminal" { if termpane.is_open(term_state) { _set_focus("terminal") } else { _status("terminal closed") } }
   elif id == "pane-close" { _close_focused_pane() }
   elif id == "pane-only" { st["show_hierarchy"] = false term_state = termpane.close(term_state) _save_ui_cache() _set_focus("editor") }
   elif id == "pane-split-below" { term_state = termpane.open_shell(term_state) _set_focus("terminal") }
   elif id == "pane-split-right" { st["show_hierarchy"] = true st["show_project"] = true st["show_outline"] = true _save_ui_cache() _set_focus("project") }
   elif id == "pane-balance" { st["rail_w"] = 240.0 st["rail_split"] = 0.0 st["term_h"] = 200.0 _save_ui_cache() _status("panes balanced") }
   elif id == "toggle-fullscreen" { window.toggle_window_fullscreen(win) _status("fullscreen") }
   elif id == "pane-resize-left" { _pane_resize("left") }
   elif id == "pane-resize-right" { _pane_resize("right") }
   elif id == "pane-resize-up" { _pane_resize("up") }
   elif id == "pane-resize-down" { _pane_resize("down") }
   elif id == "cancel" { _clear_selections() _status("cancel") }
   elif id == "backspace" { _edit_backspace() }
   elif id == "delete-char" { _edit_delete() }
   elif id == "newline" { _edit_newline() }
   elif id == "move-left" { _move_cursor("left", false) }
   elif id == "move-right" { _move_cursor("right", false) }
   elif id == "move-up" { _move_cursor("up", false) }
   elif id == "move-down" { _move_cursor("down", false) }
   elif id == "move-left-fast" { _move_cursor("left", true) }
   elif id == "move-right-fast" { _move_cursor("right", true) }
   elif id == "move-up-fast" { _move_cursor("up", true) }
   elif id == "move-down-fast" { _move_cursor("down", true) }
   elif id == "page-up" { _page_cursor(-1, false) }
   elif id == "page-down" { _page_cursor(1, false) }
   elif id == "line-start" { _line_edge("start", false) }
   elif id == "line-end" { _line_edge("end", false) }
   elif id == "buffer-start" { _buffer_edge("start", false) }
   elif id == "buffer-end" {
      _buffer_edge("end", false)
   }
   elif id == "cursor-add-above" { _add_extra_cursor(-1) }
   elif id == "cursor-add-below" { _add_extra_cursor(1) }
   elif id == "cursor-clear-extra" { _clear_extra_cursors() }
   elif id == "toggle-project" { _status(_set_ui_toggle("project") ? "project tree on" : "project tree off") }
   elif id == "toggle-outline" { _status(_set_ui_toggle("outline") ? "outline on" : "outline off") }
   elif id == "toggle-status" { _status(_set_ui_toggle("status") ? "status on" : "status off") }
   elif id == "toggle-line-numbers" { _status(_set_ui_toggle("line_numbers") ? "line numbers on" : "line numbers off") }
   elif id == "toggle-indent-guides" { _status(_set_ui_toggle("indent_guides") ? "indent guides on" : "indent guides off") }
   elif id == "toggle-terminal" { term_state = termpane.toggle_shell(term_state) _set_focus(termpane.is_open(term_state) ? "terminal" : "editor") }
   elif id == "toggle-repl" { term_state = termpane.toggle_repl(term_state) _set_focus(termpane.is_open(term_state) ? "terminal" : "editor") }
   elif id == "terminal-shell" { term_state = termpane.show_shell(term_state) _set_focus("terminal") }
   elif id == "terminal-repl" { term_state = termpane.show_repl(term_state) _set_focus("terminal") }
   elif id == "terminal-next" { term_state = termpane.next_tab(term_state) _set_focus("terminal") }
   elif id == "terminal-prev" { term_state = termpane.prev_tab(term_state) _set_focus("terminal") }
   elif id == "terminal-close" { term_state = termpane.close_tab(term_state) _set_focus(termpane.is_open(term_state) ? "terminal" : "editor") }
   elif id == "toggle-lsp-popups" { lsp_state = lsp.toggle_popups(lsp_state) _status(to_str(lsp_state.get("last", "lsp"))) }
   elif id == "lsp-status" { _lsp_output("LSP status", lsp.status(lsp_state) + "\n\n" + lsp.diagnostics_text(lsp_state)) _status("lsp status") }
   elif id == "lsp-start" { _lsp_start() }
   elif id == "lsp-restart" { lsp_state = lsp.restart(lsp_state, _current_path(), _current_text()) _lsp_output("LSP status", lsp.status(lsp_state)) _status(lsp.status(lsp_state)) }
   elif id == "lsp-stop" { lsp_state = lsp.stop(lsp_state) _status(lsp.status(lsp_state)) }
   elif id == "lsp-diagnostics" { _lsp_diagnostics() }
   elif id == "lsp-complete" { _lsp_request("completion") }
   elif id == "lsp-hover" { _lsp_request("hover") }
   elif id == "lsp-definition" { _lsp_request("definition") }
   else { _status("unknown command " + id) }
   if id != "palette" { palette_state = pal.close(palette_state) }
   0
}

fn _palette_choose() int {
   def row = pal.selected(palette_state)
   if row.len <= 0 { _status("no command") return 0 }
   _run_command(to_str(row.get(1, "")))
}

fn _handle_palette_key(any data) int {
   def res = pal.handle_key(palette_state, data)
   palette_state = res.get("st", palette_state)
   if res.get("choose", false) { _palette_choose() }
   0
}

fn _handle_palette_char(any data) int {
   palette_state = pal.handle_char(palette_state, data)
   0
}

fn _handle_find_key(any data) int {
   if _ctrl(data) && window.event_key_is(data, key.KEY_G) { _find_close() return 0 }
   if _ctrl(data) && window.event_key_is(data, key.KEY_R) { _find_toggle("regex") return 0 }
   if _ctrl(data) && window.event_key_is(data, key.KEY_C) { _find_toggle("case") return 0 }
   if _ctrl(data) && window.event_key_is(data, key.KEY_W) { _find_toggle("word") return 0 }
   if _ctrl(data) && window.event_key_is(data, key.KEY_H) { _find_toggle("replace") return 0 }
   if window.event_key_is(data, key.KEY_UP) { _find_step(-1) return 0 }
   if window.event_key_is(data, key.KEY_DOWN) { _find_step(1) return 0 }
   if window.event_key_is(data, key.KEY_F3) { _find_step(_shift(data) ? -1 : 1) return 0 }
   def res = find.handle_key(find_state, data)
   find_state = res.get("st", find_state)
   def action = to_str(res.get("action", ""))
   if action == "refresh" {
      _find_refresh_now()
      _status(_find_message())
   } elif action == "jump" {
      _find_jump_current(true)
   } elif action == "replace-current" {
      _find_replace_current()
   } elif action == "replace-all" {
      _find_replace_all()
   } elif action == "close" {
      _find_close()
   }
   0
}

fn _handle_find_char(any data) int {
   def before = find.query(find_state)
   def repl_before = find.replacement(find_state)
   find_state = find.handle_char(find_state, data)
   if find.query(find_state) != before {
      _find_refresh_now()
      _status(_find_message())
   } elif find.replacement(find_state) != repl_before {
      _status("replace")
   }
   0
}

fn _handle_rename_key(any data) int {
   def res = prompt.rename_handle_key(rename_state, data)
   rename_state = res.get("st", rename_state)
   if res.get("submit", false) { _finish_rename() }
   0
}

fn _handle_rename_char(any data) int {
   rename_state = prompt.rename_handle_char(rename_state, data)
   0
}

fn _selection_anchor_if_needed() int {
   if _extra_cursor_count() > 0 {
      _multi_selection_anchor_if_needed()
      return 0
   }
   if !ed.selection_valid(st) && !bool(st.get("sel_active", false)) {
      st = ed.set_selection_anchor(st)
      st["multi_selects"] = []
      st["drag_select"] = false
   }
   0
}

fn _move_cursor_ext(str dir, bool jump, bool extend=false) int {
   _follow_cursor()
   if extend { _selection_anchor_if_needed() }
   def old_row = int(st.get("cursor_line", 0))
   def old_col = int(st.get("cursor_col", 0))
   def lines = ed.current_lines(st)
   mut row = int(st.get("cursor_line", 0))
   mut col = int(st.get("cursor_col", 0))
   if dir == "left" {
      mut steps = jump ? 4 : 1
      while steps > 0 {
         if col > 0 { col -= 1 }
         elif row > 0 {
            row -= 1
            col = to_str(lines.get(row, "")).len
         }
         steps -= 1
      }
      st["cursor_line"] = row
      st["cursor_col"] = col
   } elif dir == "right" {
      mut steps2 = jump ? 4 : 1
      while steps2 > 0 {
         def line = to_str(lines.get(row, ""))
         if col < line.len { col += 1 }
         elif row + 1 < lines.len {
            row += 1
            col = 0
         }
         steps2 -= 1
      }
      st["cursor_line"] = row
      st["cursor_col"] = col
   } elif dir == "up" {
      st["cursor_line"] = row - (jump ? 8 : 1)
   } elif dir == "down" {
      st["cursor_line"] = row + (jump ? 8 : 1)
   }
   st = ed.clamp_cursor(st)
   _move_extra_cursors_by(int(st.get("cursor_line", 0)) - old_row, int(st.get("cursor_col", 0)) - old_col)
   if extend {
      _finish_selection_move(true)
   } else {
      _finish_selection_move(false)
   }
   0
}

fn _move_cursor(str dir, bool jump) int {
   _move_cursor_ext(dir, jump, false)
}

fn _finish_selection_move(bool extend) int {
   if extend {
      if st.get("multi_selects", []).len > 0 { _multi_selection_update() }
      else { st = ed.update_selection(st) }
      st["drag_select"] = false
   } else {
      _clear_selections()
   }
   0
}

fn _page_cursor(int dir, bool extend=false) int {
   _follow_cursor()
   if extend { _selection_anchor_if_needed() }
   def old_row = int(st.get("cursor_line", 0))
   def old_col = int(st.get("cursor_col", 0))
   st["cursor_line"] = int(st.get("cursor_line", 0)) + dir * 12
   st = ed.clamp_cursor(st)
   _move_extra_cursors_by(int(st.get("cursor_line", 0)) - old_row, int(st.get("cursor_col", 0)) - old_col)
   _finish_selection_move(extend)
}

fn _line_edge(str edge, bool extend=false) int {
   _follow_cursor()
   if extend { _selection_anchor_if_needed() }
   def old_row = int(st.get("cursor_line", 0))
   def old_col = int(st.get("cursor_col", 0))
   if edge == "start" {
      st["cursor_col"] = 0
   } else {
      st["cursor_col"] = to_str(ed.current_lines(st).get(int(st.get("cursor_line", 0)), "")).len
   }
   st = ed.clamp_cursor(st)
   _move_extra_cursors_by(int(st.get("cursor_line", 0)) - old_row, int(st.get("cursor_col", 0)) - old_col)
   _finish_selection_move(extend)
}

fn _buffer_edge(str edge, bool extend=false) int {
   _follow_cursor()
   if extend { _selection_anchor_if_needed() }
   def old_row = int(st.get("cursor_line", 0))
   def old_col = int(st.get("cursor_col", 0))
   def lines = ed.current_lines(st)
   if edge == "start" {
      st["cursor_line"] = 0
      st["cursor_col"] = 0
   } else {
      def last = max(0, lines.len - 1)
      st["cursor_line"] = last
      st["cursor_col"] = to_str(lines.get(last, "")).len
   }
   st = ed.clamp_cursor(st)
   _move_extra_cursors_by(int(st.get("cursor_line", 0)) - old_row, int(st.get("cursor_col", 0)) - old_col)
   _finish_selection_move(extend)
}

fn _handle_command(str ch) bool {
   if ch.len <= 0 {
      _status(chord.describe(chord_state) + "-")
      return true
   }
   if ch == "cancel" { chord_state = chord.clear(chord_state) _run_command("cancel") return true }
   def id = cmd.id_for_chord(ch)
   if id.len <= 0 { return false }
   _run_command(id)
   true
}

fn _direct_key_fallback(any data) bool {
   if _alt(data) && _shift(data) && window.event_key_is(data, key.KEY_UP) { _run_command("cursor-add-above") return true }
   if _alt(data) && _shift(data) && window.event_key_is(data, key.KEY_DOWN) { _run_command("cursor-add-below") return true }
   if _ctrl(data) && window.event_key_is(data, key.KEY_Z) {
      _run_command(_shift(data) ? "redo" : "undo")
      return true
   }
   if _ctrl(data) && window.event_key_is(data, key.KEY_Y) { _run_command("redo") return true }
   if _ctrl(data) && window.event_key_is(data, key.KEY_C) { _run_command("copy") return true }
   if _ctrl(data) && window.event_key_is(data, key.KEY_X) { _run_command("cut") return true }
   if _ctrl(data) && window.event_key_is(data, key.KEY_V) { _run_command("paste") return true }
   if _ctrl(data) && (window.event_key_is(data, key.KEY_EQUAL) || window.event_key_is(data, key.KEY_KP_EQUAL) || window.event_key_is(data, key.KEY_KP_ADD)) {
      _run_command("zoom-in")
      return true
   }
   if _ctrl(data) && (window.event_key_is(data, key.KEY_MINUS) || window.event_key_is(data, key.KEY_KP_SUBTRACT)) {
      _run_command("zoom-out")
      return true
   }
   if _ctrl(data) && (window.event_key_is(data, key.KEY_0) || window.event_key_is(data, key.KEY_KP_0)) {
      _run_command("zoom-reset")
      return true
   }
   if window.event_key_is(data, key.KEY_LEFT) { _move_cursor_ext("left", _ctrl(data), _shift(data)) return true }
   if window.event_key_is(data, key.KEY_RIGHT) { _move_cursor_ext("right", _ctrl(data), _shift(data)) return true }
   if window.event_key_is(data, key.KEY_UP) { _move_cursor_ext("up", _ctrl(data), _shift(data)) return true }
   if window.event_key_is(data, key.KEY_DOWN) { _move_cursor_ext("down", _ctrl(data), _shift(data)) return true }
   if window.event_key_is(data, key.KEY_PAGE_UP) { _page_cursor(-1, _shift(data)) return true }
   if window.event_key_is(data, key.KEY_PAGE_DOWN) { _page_cursor(1, _shift(data)) return true }
   if window.event_key_is(data, key.KEY_HOME) { _line_edge("start", _shift(data)) return true }
   if window.event_key_is(data, key.KEY_END) { _line_edge("end", _shift(data)) return true }
   if window.event_key_is(data, key.KEY_BACKSPACE) { _run_command("backspace") return true }
   if window.event_key_is(data, key.KEY_DELETE) { _run_command("delete-char") return true }
   if window.event_key_is(data, key.KEY_ENTER) { _run_command("newline") return true }
   if _ctrl(data) {
      if window.event_key_is(data, key.KEY_A) { _run_command("mark-whole") return true }
      if window.event_key_is(data, key.KEY_E) { _run_command("line-end") return true }
      if window.event_key_is(data, key.KEY_B) { _run_command("toggle-sidebar") return true }
      if window.event_key_is(data, key.KEY_F) { _run_command("find") return true }
      if window.event_key_is(data, key.KEY_P) { _run_command("move-up") return true }
      if window.event_key_is(data, key.KEY_N) { _run_command("move-down") return true }
      if window.event_key_is(data, key.KEY_D) { _run_command("delete-char") return true }
      if window.event_key_is(data, key.KEY_K) { _run_command("kill-line") return true }
   }
   false
}

fn _handle_key(any data) int {
   if suppress_char_cp > 0 { suppress_char_cp = 0 }
   if INPUT_TRACE {
      print("[editor:key] enter rename=" + to_str(prompt.rename_is_open(rename_state)) + " palette=" + to_str(pal.is_open(palette_state)))
   }
   if _handle_escape_key(data) { return 0 }
   if prompt.rename_is_open(rename_state) { _handle_rename_key(data) return 0 }
   if pal.is_open(palette_state) { _handle_palette_key(data) return 0 }
   if find.is_open(find_state) { _handle_find_key(data) return 0 }
   if _completion_handle_key(data) { return 0 }
   def key_chord = chord.event_chord(data)
   if INPUT_TRACE {
      print("[editor:key] chord=" + key_chord + " key=" + to_str(int(data.get("key", -1))) + " mods=" + to_str(int(data.get("mods", data.get("mod", 0)))))
   }
   ui_state = interact.key_seen(ui_state, key_chord)
   def had_prefix = chord.pending(chord_state)
   if key_chord == "C-c" && !had_prefix { _copy_selection_or_buffer() }
   def res = chord.command_chord(chord_state, data)
   chord_state = res.get(0)
   def resolved = to_str(res.get(1, ""))
   if INPUT_TRACE { print("[editor:key] resolved=" + resolved + " prefix=" + chord.describe(chord_state)) }
   if _handle_command(resolved) {
      _suppress_char_from_key(data)
      return 0
   }
   if had_prefix {
      _suppress_char_from_key(data)
      return 0
   }
   if INPUT_TRACE { print("[editor:key] fallback") }
   _direct_key_fallback(data)
   0
}

fn _handle_char(any data) int {
   if _consume_suppressed_char(data) { return 0 }
   if prompt.rename_is_open(rename_state) { _handle_rename_char(data) return 0 }
   if pal.is_open(palette_state) { _handle_palette_char(data) return 0 }
   if find.is_open(find_state) { _handle_find_char(data) return 0 }
   if _ctrl(data) { return 0 }
   st["quit_prompt"] = false
   last_escape_ns = 0
   def cp = int(data.get("char", 0))
   if cp == 40 { _insert_pair("(", ")") return 0 }
   if cp == 91 { _insert_pair("[", "]") return 0 }
   if cp == 123 { _insert_pair("{", "}") return 0 }
   if cp == 34 { _insert_pair("\"", "\"") return 0 }
   if cp == 39 { _insert_pair("'", "'") return 0 }
   if cp >= 32 && cp != 127 {
      _edit_insert(chr(cp))
      if bool(lsp_state.get("popups", true)) && _word_byte(cp) && _word_prefix_before_cursor().len >= 2 { _completion_open(false) }
      else { _completion_close() }
   }
   0
}

fn _context_hit_action(f64 mx, f64 my) str {
   prompt.context_hit(context_state, mx, my, CONTEXT_W, CONTEXT_ROW_H)
}

fn _handle_mouse_nav_button(any data) bool {
   if !is_dict(data) { return false }
   def button = int(data.get("button", -1))
   if button == MOUSE_BACK { _run_command("go-back") return true }
   if button == MOUSE_FORWARD { _run_command("go-forward") return true }
   false
}

fn _is_escape_key(any data) bool {
   window.event_key_is(data, key.KEY_ESCAPE) || window.event_key_is(data, key.KEY_ESC)
}

fn _handle_escape_key(any data) bool {
   if !_is_escape_key(data) { return false }
   mut cleared = false
   if prompt.rename_is_open(rename_state) { rename_state = prompt.rename_close(rename_state) cleared = true }
   if pal.is_open(palette_state) { palette_state = pal.close(palette_state) cleared = true }
   if find.is_open(find_state) { find_state = find.close(find_state) cleared = true }
   if colorpicker.is_open(color_state) { color_state = colorpicker.close(color_state) cleared = true }
   if prompt.context_is_open(context_state) { context_state = prompt.context_close(context_state) cleared = true }
   if bool(completion_state.get("open", false)) { _completion_close() cleared = true }
   if bool(peek_state.get("open", false)) { peek_state["open"] = false cleared = true }
   if chord.pending(chord_state) { chord_state = chord.empty_state() cleared = true }
   if _extra_cursor_count() > 0 { _clear_extra_cursors(false) cleared = true }
   if _selection_any_valid() || bool(st.get("sel_active", false)) { _clear_selections() cleared = true }
   if project_selection.len > 0 { project_selection = [] project_selection_anchor = "" cleared = true }
   st["drag_select"] = false
   st["drag_divider"] = false
   st["drag_terminal"] = false
   if cleared {
      st["quit_prompt"] = false
      last_escape_ns = 0
      _request_full_redraw(2)
      _status("cleared")
      return true
   }
   def now = ticks()
   if st.get("quit_prompt", false) || (last_escape_ns > 0 && now - last_escape_ns <= ESC_QUIT_WINDOW_NS) {
      window.set_should_close(win, true)
      return true
   }
   last_escape_ns = now
   st["quit_prompt"] = true
   _request_full_redraw(2)
   _status("Esc again quits")
   true
}

fn _handle_mouse_press(dict lay, any data) int {
   def p = window.mouse_pos(win)
   def mx = float(data.get("x", p.get(0, 0.0)))
   def my = float(data.get("y", p.get(1, 0.0)))
   def button = int(data.get("button", 0))
   if pal.is_open(palette_state) {
      def pi = _palette_row_hit(lay, mx, my)
      if pi >= 0 {
         palette_state = pal.set_index(palette_state, pi)
         _palette_choose()
         return 0
      }
      if pi == -2 {
         palette_state = pal.close(palette_state)
         return 0
      }
   }
   def tab_hit_top = _top_tab_hit(lay, mx, my)
   def top_action = to_str(tab_hit_top.get("action", ""))
   if top_action.len > 0 {
      def idx_top = int(tab_hit_top.get("idx", -1))
      if top_action == "close" { _close_top_tab(idx_top) }
      else { _select_top_tab(idx_top) }
      return 0
   }
   def term_hit = _term_tab_hit(lay, mx, my)
   def term_action = to_str(term_hit.get("action", ""))
   if term_action.len > 0 {
      if term_action == "new" { term_state = termpane.open_shell(term_state) }
      elif term_action == "close" { term_state = termpane.close_tab(term_state, int(term_hit.get("idx", -1))) }
      elif term_action == "select" { term_state = termpane.select_tab(term_state, int(term_hit.get("idx", 0))) }
      _request_full_redraw(3)
      _set_focus(termpane.is_open(term_state) ? "terminal" : "editor", false)
      return 0
   }
   def pane = ed.pane_at(lay, mx, my)
   if pane == "editor" || pane == "project" || pane == "terminal" { _set_focus(pane, false) }
   st["quit_prompt"] = false
   last_escape_ns = 0
   def rail_x = float(lay.get("rail_x", 0.0))
   def rail_y = float(lay.get("rail_y", 0.0))
   def rail_w = float(lay.get("rail_w", 0.0))
   def rail_h = float(lay.get("rail_h", 0.0))
   def tab_hit = (bool(st.get("show_hierarchy", true)) && rail_w > 1.0) ? _rail_tab_at(rail_x, rail_y, rail_w, mx, my) : ""
   if tab_hit.len > 0 {
      def old_tab = _rail_tab()
      st["rail_tab"] = tab_hit
      if tab_hit != old_tab {
         _request_full_redraw(3)
         if tab_hit == "git" { _ensure_project_git() }
      }
      _set_focus("project", false)
      return 0
   }
   def action = _context_hit_action(mx, my)
   if action.len > 0 { _context_action(action) return 0 }
   context_state = prompt.context_close(context_state)
   if (button == 0 || button == 1) && _dock_resize_hit(lay, mx, my) {
      st["drag_terminal"] = true
      return 0
   }
   if button == 0 {
      def scroll_target = _scrollbar_target_at(lay, mx, my)
      if scroll_target.len > 0 {
         st["drag_scrollbar"] = true
         st["scrollbar_target"] = scroll_target
         _apply_scrollbar_target(scroll_target, my)
         return 0
      }
   }
   def section_hit = _rail_section_at(lay, mx, my)
   if section_hit.len > 0 && button == 0 {
      section_drag = {"active": true, "section": section_hit, "x": mx, "y": my}
      _set_focus("project", false)
      return 0
   }
   if ed.divider_hit(lay, mx, my) {
      st["drag_divider"] = true
      return 0
   }
   if pane == "editor" {
      if button == 0 && _alt(data) && _shift(data) {
         def lines_alt = ed.current_lines(st)
         if lines_alt.len > 0 {
            def row_alt = min(max(ed.row_at(lay, my, int(st.get("scroll", 0))), 0), lines_alt.len - 1)
            def line_alt = to_str(lines_alt.get(row_alt, ""))
            _add_extra_cursor_at(row_alt, ed.col_at(font_body, line_alt, mx, _line_text_x(lay)))
         }
         st["drag_select"] = false
         return 0
      }
      _click_editor(lay, mx, my)
      if button == 0 && bool(st.get("show_line_numbers", true)) && mx < _line_text_x(lay) - 8.0 {
         _debug_breakpoint()
         st["drag_select"] = false
         return 0
      }
      if button == 1 {
         _open_context(mx, my, _editor_context_entry(), "editor")
         st["drag_select"] = false
         return 0
      }
      st["drag_select"] = button == 0
      if _shift(data) {
         st = ed.update_selection(st)
      } else {
         st = ed.set_selection_anchor(st)
      }
      return 0
   }
   if pane != "project" || rail_w <= 1.0 || !bool(st.get("show_hierarchy", true)) { return 0 }
   def tab = _rail_tab()
   def body_y = rail_y + RAIL_TABS_H
   def body_h = rail_h - RAIL_TABS_H
   if pane == "project" && tab == "git" {
      def rows_git = project.changes(project_state)
      def visible_git = project.visible_count(body_h)
      def idx_git = project.row_at(rail_x, body_y, rail_w, body_h, mx, my, min(visible_git, rows_git.len - int(st.get("git_scroll", 0))))
      if idx_git >= 0 {
         def entry = rows_git.get(int(st.get("git_scroll", 0)) + idx_git, {})
         if button == 1 { _open_context(mx, my, entry, "git") }
         else { _open_git_diff(entry) }
         return 0
      }
   }
   if pane == "project" && tab == "timeline" {
      def rows_time = _timeline_rows()
      def visible_time = project.visible_count(body_h)
      def idx_time = project.row_at(rail_x, body_y, rail_w, body_h, mx, my, min(visible_time, rows_time.len - int(st.get("timeline_scroll", 0))))
      if idx_time >= 0 {
         def row = rows_time.get(int(st.get("timeline_scroll", 0)) + idx_time, [])
         if button == 1 {
            _open_context(mx, my, _timeline_context_entry(row), "timeline")
         } elif to_str(row.get(0, "")) == "command" { _run_command(to_str(row.get(2, ""))) }
         else {
            def path = to_str(row.get(2, ""))
            if path.len > 0 { _open_file(path) }
         }
         return 0
      }
   }
   def show_project = bool(st.get("show_project", true))
   def show_outline = bool(st.get("show_outline", true))
   if tab == "files" && show_project { _ensure_project_tree() }
   def rows = project.tree(project_state)
   def syms = _outline_symbols()
   def tree_h = (!show_project) ? 0.0 : (show_outline ? _rail_tree_height(body_h, rows.len, syms.len) : body_h)
   if tab == "files" && show_project {
      def entry = project.hit_entry(project_state, rail_x, body_y, rail_w, tree_h, int(st.get("tree_scroll", 0)), mx, my)
      if is_dict(entry) && entry.len > 0 {
         if button == 1 {
            if !_project_selection_contains(entry) { _project_selection_set(entry) }
            entry["selection_count"] = project_selection.len
            _open_context(mx, my, entry, "file")
         } else {
            if _shift(data) {
               _project_selection_range(entry)
               return 0
            }
            if _cmd_mod(data) {
               _project_selection_toggle(entry)
               return 0
            }
            _project_selection_set(entry)
            project_drag = {"active": false, "armed": true, "entry": entry, "x": mx, "y": my}
            _open_or_toggle_project_row(entry)
         }
         return 0
      }
   }
   def outline_off = show_project ? tree_h + 1.0 : 0.0
   def outline_y = body_y + outline_off
   def outline_h = body_h - outline_off
   def outline_visible = _outline_visible(outline_h)
   def outline_scroll = _outline_clamp(int(st.get("outline_scroll", 0)), syms.len, outline_visible)
   st["outline_scroll"] = outline_scroll
   def oi = (tab == "files" && show_outline) ? outline.row_at(rail_x, outline_y, rail_w, outline_h, mx, my, min(outline_visible, syms.len - outline_scroll)) : -1
   if tab == "files" && show_outline && oi >= 0 {
      def sym = syms.get(outline_scroll + oi, {})
      if button == 1 {
         mut entry = sym
         entry["path"] = _current_path()
         entry["rel"] = _current_path()
         entry["dir"] = false
         _open_context(mx, my, entry, "outline")
         return 0
      }
      st["cursor_line"] = int(sym.get("line", 0))
      st["cursor_col"] = 0
      st = ed.clamp_cursor(st)
      _follow_cursor()
      _status("symbol " + to_str(sym.get("name", "")))
      return 0
   }
   0
}

fn _handle_mouse_move(dict lay, any data) int {
   def p = window.mouse_pos(win)
   def mx = float(data.get("x", p.get(0, 0.0)))
   def my = float(data.get("y", p.get(1, 0.0)))
   if project_drag.get("armed", false) {
      def dx = mx - float(project_drag.get("x", mx))
      def dy = my - float(project_drag.get("y", my))
      if (dx * dx + dy * dy) > 36.0 {
         project_drag["active"] = true
         if INPUT_TRACE {
            def drag_entry = project_drag.get("entry", dict(0))
            print("[editor:drag] project " + (is_dict(drag_entry) ? to_str(drag_entry.get("rel", "")) : ""))
         }
      }
   }
   if st.get("drag_select", false) {
      _place_cursor(lay, mx, my)
      st = ed.update_selection(st)
      return 0
   }
   if st.get("drag_terminal", false) {
      st["term_h"] = _dock_height_from_mouse(lay, my)
      return 0
   }
   if st.get("drag_scrollbar", false) {
      _apply_scrollbar_target(st.get("scrollbar_target", dict(0)), my)
      return 0
   }
   if !st.get("drag_divider", false) { return 0 }
   st["rail_w"] = _rail_width_from_mouse(lay, mx)
   0
}

fn _move_dst_path(dict src, dict dst) str {
   def base = to_str(dst.get("path", ""))
   if base.len <= 0 { return "" }
   def parent = dst.get("dir", false) ? base : ospath.dirname(base)
   ospath.join(parent, ospath.basename(to_str(src.get("path", ""))))
}

fn _retarget_open_buffers(str old_path, str new_path) int {
   if old_path.len <= 0 || new_path.len <= 0 { return 0 }
   def old_norm = ospath.normalize(old_path)
   mut bs = st.get("buffers", [])
   mut i = 0
   while i < bs.len {
      mut b = bs.get(i, {})
      if ospath.normalize(to_str(b.get("path", ""))) == old_norm {
         b["path"] = new_path
         b["name"] = ospath.basename(new_path)
         bs[i] = b
      }
      i += 1
   }
   st["buffers"] = bs
   0
}

fn _drop_open_buffer_path(str path) int {
   if path.len <= 0 { return 0 }
   def want = ospath.normalize(path)
   def active = int(st.get("active", 0))
   def bs = st.get("buffers", [])
   mut out = []
   mut removed_before = 0
   mut removed = false
   mut i = 0
   while i < bs.len {
      def b = bs.get(i, {})
      def bp = ospath.normalize(_buffer_source_path(b))
      if bp == want {
         removed = true
         if i < active { removed_before += 1 }
      } else {
         out = out.append(b)
      }
      i += 1
   }
   if !removed { return 0 }
   if out.len <= 0 { out = [ed.buffer("untitled.ny", "", "")] }
   st["buffers"] = out
   st["active"] = min(max(0, active - removed_before), out.len - 1)
   st["cursor_line"] = 0
   st["cursor_col"] = 0
   st["scroll"] = 0
   _clear_selections()
   _buffer_changed(true)
   _nav_record_current()
   0
}

fn _project_entry_or_current(dict entry) dict {
   if is_dict(entry) && entry.len > 0 && to_str(entry.get("rel", "")).len > 0 { return entry }
   def cur = _current_path()
   if cur.len > 0 {
      def hit = session.project_entry_for_path(project_state, cur)
      if is_dict(hit) && hit.len > 0 { return hit }
      return {"name": ospath.basename(cur), "path": cur, "rel": str.str_replace(ospath.normalize(cur), ospath.normalize(project.root(project_state)) + ospath.sep(), ""), "dir": false}
   }
   dict(0)
}

fn _delete_project_entry(dict entry) int {
   def entries = _project_filter_top_entries(_project_action_entries(entry))
   if entries.len <= 0 { _status("select a file first") return 0 }
   mut deleted = 0
   mut first_name = ""
   mut i = 0
   while i < entries.len {
      def e = entries.get(i, {})
      def rel = to_str(e.get("rel", ""))
      def path = to_str(e.get("path", ""))
      if first_name.len <= 0 { first_name = to_str(e.get("name", ospath.basename(path))) }
      project_state = project.trash_entry(project_state, rel)
      def err = to_str(project_state.get("last_error", ""))
      if err.len > 0 { _status(err) return 0 }
      if path.len > 0 { _drop_open_buffer_path(path) }
      deleted += 1
      i += 1
   }
   project_selection = []
   project_selection_anchor = ""
   st["project_loaded"] = true
   st["git_loaded"] = true
   _invalidate_project_definitions()
   _save_ui_cache()
   _status(deleted == 1 ? ("moved to trash " + first_name) : ("moved " + to_str(deleted) + " items to trash"))
   0
}

fn _undo_file_op() int {
   project_state = project.undo_file_op(project_state)
   def err = to_str(project_state.get("last_error", ""))
   if err.len > 0 { _status(err) return 0 }
   st["project_loaded"] = true
   st["git_loaded"] = true
   _invalidate_project_definitions()
   _save_ui_cache()
   _status("file op undone")
   0
}

fn _redo_file_op() int {
   project_state = project.redo_file_op(project_state)
   def err = to_str(project_state.get("last_error", ""))
   if err.len > 0 { _status(err) return 0 }
   st["project_loaded"] = true
   st["git_loaded"] = true
   _invalidate_project_definitions()
   _save_ui_cache()
   _status("file op redone")
   0
}

fn _finish_project_drag(dict lay, f64 mx, f64 my) int {
   if !project_drag.get("armed", false) { return 0 }
   def src = project_drag.get("entry", dict(8))
   def active = bool(project_drag.get("active", false))
   project_drag = {"active": false, "armed": false, "entry": dict(8), "x": 0.0, "y": 0.0}
   if !active || !is_dict(src) || src.len <= 0 { return 0 }
   def dst = _project_entry_at(lay, mx, my)
   if !is_dict(dst) || dst.len <= 0 { _status("move cancelled") return 0 }
   def dst_rel = to_str(dst.get("rel", ""))
   def entries = _project_filter_top_entries(_project_selection_contains(src) ? _project_selected_entries() : [src])
   if entries.len <= 0 { _status("move cancelled") return 0 }
   if _project_entry_in_or_under_any(dst, entries) { _status("cannot move into selection") return 0 }
   mut moved = 0
   mut first_name = ""
   mut i = 0
   while i < entries.len {
      def e = entries.get(i, {})
      if first_name.len <= 0 { first_name = to_str(e.get("name", "")) }
      def new_path = _move_dst_path(e, dst)
      project_state = project.move_entry(project_state, to_str(e.get("rel", "")), dst_rel)
      def err = to_str(project_state.get("last_error", ""))
      if err.len > 0 { _status(err) return 0 }
      _retarget_open_buffers(to_str(e.get("path", "")), new_path)
      moved += 1
      i += 1
   }
   project_selection = []
   project_selection_anchor = ""
   st["project_loaded"] = true
   st["git_loaded"] = true
   _invalidate_project_definitions()
   _buffer_changed(false)
   _status(moved == 1 ? ("moved " + first_name) : ("moved " + to_str(moved) + " items"))
   0
}

fn _finish_section_drag(f64 mx, f64 my) int {
   if !section_drag.get("active", false) { return 0 }
   def section = to_str(section_drag.get("section", ""))
   def dx = mx - float(section_drag.get("x", mx))
   def dy = my - float(section_drag.get("y", my))
   section_drag = {"active": false, "section": "", "x": 0.0, "y": 0.0}
   if section.len <= 0 { return 0 }
   if (dx * dx + dy * dy) > 49.0 { _hide_rail_section(section) }
   else { _toggle_rail_section(section) }
   0
}

fn _handle_mouse_release(dict lay, any data) int {
   def p = window.mouse_pos(win)
   def mx = is_dict(data) ? float(data.get("x", p.get(0, 0.0))) : float(p.get(0, 0.0))
   def my = is_dict(data) ? float(data.get("y", p.get(1, 0.0))) : float(p.get(1, 0.0))
   _finish_section_drag(mx, my)
   _finish_project_drag(lay, mx, my)
   st["drag_divider"] = false
   st["drag_terminal"] = false
   st["drag_select"] = false
   st["drag_scrollbar"] = false
   st["scrollbar_target"] = dict(0)
   0
}

fn _handle_scroll(dict lay, any data) int {
   def p = window.mouse_pos(win)
   def mx = float(data.get("x", p.get(0, 0.0)))
   def my = float(data.get("y", p.get(1, 0.0)))
   mut dy = 0.0
   if is_dict(data) { dy = float(data.get("dy", 0.0)) }
   elif is_list(data) { dy = float(data.get(1, 0.0)) }
   if dy == 0.0 { return 0 }
   if INPUT_TRACE { print("[editor:scroll] mx=" + to_str(mx) + " my=" + to_str(my) + " dy=" + to_str(dy)) }
   if pal.is_open(palette_state) {
      if _palette_contains(lay, mx, my) {
         palette_scroll_accum += -dy * 3.0
         def step_pal = int(palette_scroll_accum)
         if step_pal != 0 {
            palette_scroll_accum -= float(step_pal)
            palette_state = pal.scroll_by(palette_state, step_pal)
         }
      }
      return 0
   }
   if _which_key_contains(_layout_sw(lay), _layout_sh(lay), mx, my) {
      def rows_key = pal.which_key(chord.describe(chord_state))
      def rect_key = _which_key_rect(_layout_sw(lay), _layout_sh(lay), rows_key.len)
      def visible_key = int(rect_key.get(5, 1))
      which_key_scroll_accum += -dy * 3.0
      def step_key = int(which_key_scroll_accum)
      if step_key != 0 {
         which_key_scroll_accum -= float(step_key)
         which_key_scroll += step_key
         which_key_scroll = _clamp_which_key_scroll(rows_key.len, visible_key)
      }
      return 0
   }
   if widgets.hit(mx, my, float(lay.get("rail_x", 0.0)), float(lay.get("rail_y", 0.0)), float(lay.get("rail_w", 0.0)), float(lay.get("rail_h", 0.0))) {
      def rail_h = float(lay.get("rail_h", 0.0))
      def body_h = rail_h - RAIL_TABS_H
      def tab = _rail_tab()
      if tab == "git" {
         def rows_git = project.changes(project_state)
         def visible_git = project.visible_count(body_h)
         git_scroll_accum += -dy * 3.0
         def step_git = int(git_scroll_accum)
         if step_git != 0 {
            git_scroll_accum -= float(step_git)
            st["git_scroll"] = project.clamp_scroll(int(st.get("git_scroll", 0)) + step_git, rows_git.len, visible_git)
         }
         return 0
      }
      if tab == "timeline" {
         def rows_time = _timeline_rows()
         def visible_time = project.visible_count(body_h)
         timeline_scroll_accum += -dy * 3.0
         def step_time = int(timeline_scroll_accum)
         if step_time != 0 {
            timeline_scroll_accum -= float(step_time)
            st["timeline_scroll"] = project.clamp_scroll(int(st.get("timeline_scroll", 0)) + step_time, rows_time.len, visible_time)
         }
         return 0
      }
      def show_project = bool(st.get("show_project", true))
      def show_outline = bool(st.get("show_outline", true))
      if show_project { _ensure_project_tree() }
      def rows = project.tree(project_state)
      def syms = _outline_symbols()
      def tree_h = (!show_project) ? 0.0 : (show_outline ? _rail_tree_height(body_h, rows.len, syms.len) : body_h)
      def outline_y = float(lay.get("rail_y", 0.0)) + RAIL_TABS_H + (show_project ? tree_h + 1.0 : 0.0)
      if show_outline && my >= outline_y {
         def visible = _outline_visible(body_h - (outline_y - float(lay.get("rail_y", 0.0)) - RAIL_TABS_H))
         outline_scroll_accum += -dy * 3.0
         def step = int(outline_scroll_accum)
         if step != 0 {
            outline_scroll_accum -= float(step)
            st["outline_scroll"] = _outline_clamp(int(st.get("outline_scroll", 0)) + step, syms.len, visible)
         }
         if INPUT_TRACE { print("[editor:scroll] outline step=" + to_str(step) + " scroll=" + to_str(int(st.get("outline_scroll", 0)))) }
         return 0
      }
      if show_project {
         def visible = project.visible_count(tree_h)
         tree_scroll_accum += -dy * 3.0
         def step = int(tree_scroll_accum)
         if step != 0 {
            tree_scroll_accum -= float(step)
            st["tree_scroll"] = project.clamp_scroll(int(st.get("tree_scroll", 0)) + step, rows.len, visible)
         }
         if INPUT_TRACE { print("[editor:scroll] tree step=" + to_str(step) + " scroll=" + to_str(int(st.get("tree_scroll", 0)))) }
         return 0
      }
      return 0
   }
   if !widgets.hit(mx, my, float(lay.get("edit_x", 0.0)), float(lay.get("edit_y", 0.0)), float(lay.get("edit_w", 0.0)), float(lay.get("edit_h", 0.0))) {
      if INPUT_TRACE { print("[editor:scroll] miss") }
      return 0
   }
   def lines = ed.current_lines(st)
   def visible_rows = ed.visible_rows(float(lay.get("edit_h", 0.0)))
   def max_scroll = max(0, lines.len - visible_rows)
   edit_scroll_accum += -dy * 5.0
   def step = int(edit_scroll_accum)
   if step != 0 {
      edit_scroll_accum -= float(step)
      st["scroll"] = min(max_scroll, max(0, int(st.get("scroll", 0)) + step))
      st["scroll_follow"] = false
   }
   if INPUT_TRACE { print("[editor:scroll] edit step=" + to_str(step) + " max=" + to_str(max_scroll) + " scroll=" + to_str(int(st.get("scroll", 0)))) }
   0
}

fn _outline_symbols() list {
   def name = to_str(ed.current_buffer(st).get("name", ""))
   def cache_name = to_str(int(st.get("active", 0))) + ":" + name
   if outline_cache_rev == content_rev && outline_cache_name == cache_name { return outline_cache }
   outline_cache = outline.symbols(ed.current_lines(st), name, OUTLINE_MAX_LINES)
   outline_cache_rev = content_rev
   outline_cache_name = cache_name
   outline_cache
}

fn _drop_paths(any data) list {
   if is_list(data) { return data }
   if is_str(data) { return [data] }
   if !is_dict(data) { return [] }
   def paths = data.get("paths", data.get("files", []))
   if is_list(paths) { return paths }
   if is_str(paths) { return [paths] }
   def one = data.get("path", data.get("file", ""))
   is_str(one) && one.len > 0 ? [one] : []
}

fn _drop_move_into(dict target, str src_path) str {
   if src_path.len <= 0 || !is_dict(target) || target.len <= 0 { return "" }
   def base = to_str(target.get("path", ""))
   if base.len <= 0 { return "" }
   def parent = target.get("dir", false) ? base : ospath.dirname(base)
   def dst = ospath.join(parent, ospath.basename(src_path))
   if ospath.normalize(src_path) == ospath.normalize(dst) { return src_path }
   if file_exists(dst) { _status("drop target exists") return "" }
   match osfs.rename(src_path, dst) {
      ok(_) -> {
         project_state = project.refresh_git(project_state)
         st["project_loaded"] = true
         st["git_loaded"] = true
         _invalidate_project_definitions()
         dst
      }
      err(e) -> {
         _status("drop move failed " + to_str(e))
         ""
      }
   }
}

fn _handle_drop(dict lay, any data) int {
   def files = _drop_paths(data)
   if files.len <= 0 { _status("drop failed") return 0 }
   def p = window.mouse_pos(win)
   def mx = is_dict(data) ? float(data.get("x", p.get(0, 0.0))) : float(p.get(0, 0.0))
   def my = is_dict(data) ? float(data.get("y", p.get(1, 0.0))) : float(p.get(1, 0.0))
   def target = _project_entry_at(lay, mx, my)
   if is_dict(target) && target.len > 0 {
      mut opened = false
      mut moved = 0
      mut i = 0
      while i < files.len {
         def dst = _drop_move_into(target, to_str(files.get(i, "")))
         if dst.len > 0 {
            moved += 1
            if !opened && !osfs.is_dir(dst) { _open_file(dst) opened = true }
         }
         i += 1
      }
      if moved > 0 { _status("moved " + to_str(moved) + " dropped item" + (moved == 1 ? "" : "s")) }
      return 0
   }
   st = session.append_file(st, to_str(files.get(0, "")))
   _buffer_changed(true)
   0
}

fn _process_events(dict lay) int {
   term_state["focus"] = to_str(st.get("focus", "editor")) == "terminal" && termpane.is_open(term_state)
   mut e = _next_event()
   mut ev_count = 0
   mut motion_count = 0
   mut scroll_count = 0
   mut have_motion = false
   mut motion_data = dict(8)
   while e && ev_count < EVENT_FRAME_LIMIT {
      ev_count += 1
      def typ = window.event_type(e)
      def data = window.event_data(e)
      if INPUT_TRACE { print("[editor:event] type=" + to_str(typ) + " dict=" + to_str(is_dict(data)) + " data=" + to_str(data)) }
      def is_key_char = typ == window.EVENT_KEY_CHAR
      def is_mouse_press = typ == window.EVENT_MOUSE_BUTTON_PRESSED
      def is_mouse_release = typ == window.EVENT_MOUSE_BUTTON_RELEASED
      def is_mouse_move = typ == window.EVENT_MOUSE_POS_CHANGED
      def is_scroll = typ == window.EVENT_MOUSE_SCROLL
      def is_drop = typ == window.EVENT_DATA_DROP
      mut scroll_ev = 0
      if is_scroll {
         if is_dict(data) {
            scroll_ev = data
         } else {
            def p = window.mouse_pos(win)
            if is_list(data) {
               scroll_ev = {"dx": float(data.get(0, 0.0)), "dy": float(data.get(1, 0.0)), "x": float(p.get(0, 0.0)), "y": float(p.get(1, 0.0))}
            } else {
               scroll_ev = {"dx": 0.0, "dy": float(data), "x": float(p.get(0, 0.0)), "y": float(p.get(1, 0.0))}
            }
         }
      }
      if typ == key.EVENT_KEY_PRESSED {
         if INPUT_TRACE { print("[editor:event] dispatch-key") }
         if is_dict(data) {
            if _handle_escape_key(data) {
               e = _next_event()
               continue
            }
            mut handled_terminal = false
            if termpane.is_open(term_state) && to_str(st.get("focus", "editor")) == "terminal" {
               def ch = chord.event_chord(data)
               if ch != "F6" && ch != "F7" && ch != "C-g" {
                  def tres = termpane.handle_event(term_state, typ, data)
                  term_state = tres.get(0, term_state)
                  handled_terminal = bool(tres.get(1, false))
               }
            }
            if !handled_terminal {
               if INPUT_TRACE { print("[editor:event] call-editor-key") }
               _handle_key(data)
            }
         }
         e = _next_event()
         continue
      }
      if is_key_char && is_dict(data) && _consume_suppressed_char(data) {
         e = _next_event()
         continue
      }
      if is_mouse_move {
         if is_dict(data) {
            motion_data = data
            have_motion = true
            motion_count += 1
            e = _next_event()
            continue
         }
      }
      if is_scroll {
         scroll_count += 1
         if is_dict(data) {
            if pal.is_open(palette_state) || _which_key_contains(_layout_sw(lay), _layout_sh(lay), float(data.get("x", -1.0)), float(data.get("y", -1.0))) {
               if INPUT_TRACE { print("[editor:event] scroll-overlay") }
               _handle_scroll(lay, data)
            } elif termpane.contains(term_state, float(data.get("x", -1.0)), float(data.get("y", -1.0))) {
               if INPUT_TRACE { print("[editor:event] scroll-terminal") }
               def tres = termpane.handle_event(term_state, window.EVENT_MOUSE_SCROLL, data)
               term_state = tres.get(0, term_state)
            } else {
               if INPUT_TRACE { print("[editor:event] scroll-editor") }
               _handle_scroll(lay, data)
            }
         } else {
            if !is_dict(scroll_ev) { scroll_ev = {"dx": 0.0, "dy": 0.0, "x": 0.0, "y": 0.0} }
            if pal.is_open(palette_state) || _which_key_contains(_layout_sw(lay), _layout_sh(lay), float(scroll_ev.get("x", -1.0)), float(scroll_ev.get("y", -1.0))) {
               if INPUT_TRACE { print("[editor:event] scroll-overlay") }
               _handle_scroll(lay, scroll_ev)
            } elif termpane.contains(term_state, float(scroll_ev.get("x", -1.0)), float(scroll_ev.get("y", -1.0))) {
               if INPUT_TRACE { print("[editor:event] scroll-terminal") }
               def tres = termpane.handle_event(term_state, window.EVENT_MOUSE_SCROLL, scroll_ev)
               term_state = tres.get(0, term_state)
            } else {
               if INPUT_TRACE { print("[editor:event] scroll-editor") }
               _handle_scroll(lay, scroll_ev)
            }
         }
         e = _next_event()
         continue
      }
      if is_mouse_press && _handle_mouse_nav_button(data) {
         e = _next_event()
         continue
      }
      if is_mouse_release && is_dict(data) {
         def button = int(data.get("button", -1))
         if button == MOUSE_BACK || button == MOUSE_FORWARD {
            e = _next_event()
            continue
         }
      }
      if is_mouse_press && is_dict(data) && int(data.get("button", 0)) == 1 && termpane.is_open(term_state) && termpane.contains(term_state, float(data.get("x", -1.0)), float(data.get("y", -1.0))) {
         _open_context(float(data.get("x", 0.0)), float(data.get("y", 0.0)), {"name": termpane.tab_title(term_state, termpane.active_tab(term_state)), "path": "", "dir": false}, "terminal")
         e = _next_event()
         continue
      }
      if termpane.is_open(term_state) {
         def mouse_ev = is_mouse_press || is_mouse_release || is_mouse_move
         def term_mouse = mouse_ev && is_dict(data) && termpane.contains(term_state, float(data.get("x", -1.0)), float(data.get("y", -1.0)))
         if term_mouse || (!mouse_ev && to_str(st.get("focus", "editor")) == "terminal") {
            def tres = termpane.handle_event(term_state, typ, data)
            term_state = tres.get(0, term_state)
            if tres.get(1, false) {
               if is_mouse_press && term_mouse { _set_focus("terminal", false) }
               e = _next_event()
               continue
            }
         }
      }
      if is_key_char {
         if is_dict(data) { _handle_char(data) }
      } elif is_mouse_press {
         if is_dict(data) { _handle_mouse_press(lay, data) }
      } elif is_mouse_release {
         _handle_mouse_release(lay, data)
      } elif is_drop {
         _handle_drop(lay, data)
      }
      e = _next_event()
   }
   if e {
      frame_events_seen = true
      force_full_redraw = max(force_full_redraw, 1)
      _status("input queue throttled; continuing next frame")
   }
   if have_motion {
      if pal.is_open(palette_state) {
         def pi = _palette_row_hit(lay, float(motion_data.get("x", -1.0)), float(motion_data.get("y", -1.0)))
         if pi >= 0 { palette_state = pal.set_index(palette_state, pi) }
      }
      mut consumed = false
      def editor_dragging = st.get("drag_terminal", false) || st.get("drag_divider", false) || st.get("drag_select", false) || st.get("drag_scrollbar", false) || project_drag.get("armed", false)
      if termpane.is_open(term_state) && !editor_dragging {
         def tres = termpane.handle_event(term_state, window.EVENT_MOUSE_POS_CHANGED, motion_data)
         term_state = tres.get(0, term_state)
         consumed = bool(tres.get(1, false))
      }
      if !consumed { _handle_mouse_move(lay, motion_data) }
   }
   if INPUT_TRACE && ev_count > 0 {
      print("[editor:input] events=" + to_str(ev_count) + " motion=" + to_str(motion_count) + " scroll=" + to_str(scroll_count))
   }
   if !window.mouse_down(win, 0) && !window.mouse_down(win, 1) {
      st["drag_divider"] = false
      st["drag_terminal"] = false
      st["drag_select"] = false
      st["drag_scrollbar"] = false
      st["scrollbar_target"] = dict(0)
      project_drag["active"] = false
      project_drag["armed"] = false
      section_drag["active"] = false
      section_drag["section"] = ""
   }
   st = ed.clamp_cursor(st)
   frame_events_seen = ev_count > 0 || have_motion || scroll_count > 0
   0
}

fn _tok_color(int kind) any {
   case kind {
      syntax.TOK_KEYWORD, syntax.TOK_DIRECTIVE -> C_KEYWORD
      syntax.TOK_TYPE, syntax.TOK_STRUCT, syntax.TOK_TAG -> C_TYPE
      syntax.TOK_STRING -> C_STRING
      syntax.TOK_NUMBER, syntax.TOK_CONSTANT -> C_NUMBER
      syntax.TOK_COMMENT -> C_COMMENT
      syntax.TOK_FUNCTION -> C_FUNC
      syntax.TOK_OPERATOR, syntax.TOK_PREPROC, syntax.TOK_PUNCT -> C_OPERATOR
      syntax.TOK_PROPERTY, syntax.TOK_ATTR, syntax.TOK_KEY -> C_PROPERTY
      syntax.TOK_PARAM, syntax.TOK_LABEL, syntax.TOK_REGISTER -> C_CYAN
      syntax.TOK_VALUE -> C_STRING
      syntax.TOK_VARIABLE -> C_TEXT
      _ -> C_TEXT
   }
}

fn _tok_color_u32(int kind) int {
   case kind {
      syntax.TOK_KEYWORD, syntax.TOK_DIRECTIVE -> U_KEYWORD
      syntax.TOK_TYPE, syntax.TOK_STRUCT, syntax.TOK_TAG -> U_TYPE
      syntax.TOK_STRING -> U_STRING
      syntax.TOK_NUMBER, syntax.TOK_CONSTANT -> U_NUMBER
      syntax.TOK_COMMENT -> U_COMMENT
      syntax.TOK_FUNCTION -> U_FUNC
      syntax.TOK_OPERATOR, syntax.TOK_PREPROC, syntax.TOK_PUNCT -> U_OPERATOR
      syntax.TOK_PROPERTY, syntax.TOK_ATTR, syntax.TOK_KEY -> U_PROPERTY
      syntax.TOK_PARAM, syntax.TOK_LABEL, syntax.TOK_REGISTER -> U_CYAN
      syntax.TOK_VALUE -> U_STRING
      syntax.TOK_VARIABLE -> U_TEXT
      _ -> U_TEXT
   }
}

fn _syntax_lang(str filename) str {
   if syntax_lang_cache.contains(filename) { return to_str(syntax_lang_cache.get(filename, "text")) }
   def lang = syntax.detect_language(filename)
   syntax_lang_cache[filename] = lang
   lang
}

fn _syntax_alias(str raw) str {
   def s = str.lower(str.strip(raw))
   case s {
      "ny", "nytrix" -> "nytrix"
      "c", "h", "cpp", "c++", "hpp", "cc", "cxx" -> "c"
      "py", "python" -> "python"
      "js", "javascript", "jsx", "mjs", "cjs" -> "javascript"
      "ts", "typescript", "tsx", "mts", "cts" -> "typescript"
      "lua" -> "lua"
      "sh", "shell", "bash", "zsh", "fish" -> "bash"
      "cmake" -> "cmake"
      "yaml", "yml" -> "yaml"
      "xml" -> "xml"
      "html", "htm" -> "html"
      "md", "markdown", "mdx" -> "markdown"
      "asm", "assembly", "s" -> "assembly"
      "json" -> "json"
      _ -> ""
   }
}

fn _markdown_fence_marker(str line) int {
   mut i = 0
   while i < line.len && i < 4 && load8(line, i) == 32 { i += 1 }
   if i > 3 || i + 2 >= line.len { return 0 }
   def marker = load8(line, i)
   if marker != 96 && marker != 126 { return 0 }
   mut j = i
   while j < line.len && load8(line, j) == marker { j += 1 }
   if j - i < 3 { return 0 }
   marker
}

fn _markdown_fence_lang(str line) str {
   def marker = _markdown_fence_marker(line)
   if marker == 0 { return "" }
   mut i = 0
   while i < line.len && i < 4 && load8(line, i) == 32 { i += 1 }
   mut j = i
   while j < line.len && load8(line, j) == marker { j += 1 }
   mut raw = str.strip(str.str_slice(line, j, line.len))
   mut k = 0
   while k < raw.len {
      def c = load8(raw, k)
      if c == 32 || c == 9 { break }
      k += 1
   }
   if k < raw.len { raw = str.str_slice(raw, 0, k) }
   _syntax_alias(raw)
}

fn _markdown_fenced_lang_at(int target_li) str {
   if target_li < 0 { return "" }
   def lines = ed.current_lines(st)
   mut open = false
   mut marker = 0
   mut lang = ""
   mut i = 0
   while i <= target_li && i < lines.len {
      def line = to_str(lines.get(i, ""))
      def m = _markdown_fence_marker(line)
      if m != 0 {
         if !open {
            open = true
            marker = m
            lang = _markdown_fence_lang(line)
         } elif m == marker {
            if i == target_li { return "" }
            open = false
            marker = 0
            lang = ""
         }
         if i == target_li { return "" }
      } elif i == target_li && open {
         return lang
      }
      i += 1
   }
   ""
}

fn _tokenize_language(str lang, str line) list {
   case lang {
      "nytrix" -> syntax.nytrix_tokenize(line, [])
      "c" -> syntax.c_tokenize(line, [])
      "python" -> syntax.python_tokenize(line, [])
      "javascript" -> syntax.javascript_tokenize(line, [])
      "typescript" -> syntax.typescript_tokenize(line, [])
      "lua" -> syntax.lua_tokenize(line, [])
      "bash" -> syntax.bash_tokenize(line, [])
      "cmake" -> syntax.cmake_tokenize(line, [])
      "yaml" -> syntax.yaml_tokenize(line, [])
      "xml" -> syntax.xml_tokenize(line, [])
      "html" -> syntax.html_tokenize(line, [])
      "markdown" -> syntax.markdown_tokenize(line, [])
      "assembly" -> syntax.assembly_tokenize(line, [])
      "json" -> syntax.json_tokenize(line, [])
      _ -> []
   }
}

fn _tokenize_line(str filename, str line, int li=-1) list {
   def lang = _syntax_lang(filename)
   if lang == "markdown" {
      def fenced_lang = _markdown_fenced_lang_at(li)
      if fenced_lang.len > 0 { return _tokenize_language(fenced_lang, line) }
   }
   _tokenize_language(lang, line)
}

fn _syntax_key(str filename, int li, str line) str {
   filename + ":" + to_str(li) + ":" + to_str(line.len) + ":" + to_str(hash(line))
}

fn _prefix_x(str line, int col, bool mono_line) f64 {
   mono_line ? float(min(max(col, 0), line.len)) * FONT_BODY_ADV : _measure_prefix(line, col)
}

fn _syntax_runs(str filename, str line, int li) list {
   if syntax_cache_rev != content_rev {
      syntax_cache_rev = content_rev
      syntax_run_cache = dict(256)
      syntax_flat_cache = dict(256)
   }
   def key = _syntax_key(filename, li, line)
   if syntax_run_cache.contains(key) { return syntax_run_cache.get(key, []) }
   def toks = _tokenize_line(filename, line, li)
   mut runs = []
   if toks.len == 0 {
      if line.len > 0 { runs = runs.append([line, syntax.TOK_TEXT, 0.0]) }
      syntax_run_cache[key] = runs
      return runs
   }
   def mono_line = _mono_fast_prefix(line, line.len)
   mut cursor = 0
   mut i = 0
   while i < toks.len {
      def tok = toks.get(i)
      def kind = int(tok.get(0, syntax.TOK_TEXT))
      def start = min(max(int(tok.get(1, 0)), 0), line.len)
      def end = min(line.len, start + max(0, int(tok.get(2, 0))))
      if start > cursor {
         runs = runs.append([str.str_slice(line, cursor, start), syntax.TOK_TEXT, _prefix_x(line, cursor, mono_line)])
      }
      def seg_start = max(start, cursor)
      if end > seg_start {
         runs = runs.append([str.str_slice(line, seg_start, end), kind, _prefix_x(line, seg_start, mono_line)])
         cursor = end
      }
      i += 1
   }
   if cursor < line.len {
      runs = runs.append([str.str_slice(line, cursor, line.len), syntax.TOK_TEXT, _prefix_x(line, cursor, mono_line)])
   }
   syntax_run_cache[key] = runs
   runs
}

fn _syntax_rel_runs(str filename, str line, int li) list {
   if syntax_cache_rev != content_rev {
      syntax_cache_rev = content_rev
      syntax_run_cache = dict(256)
      syntax_flat_cache = dict(256)
   }
   def key = _syntax_key(filename, li, line)
   if syntax_flat_cache.contains(key) { return syntax_flat_cache.get(key, []) }
   def runs = _syntax_runs(filename, line, li)
   mut flat = []
   mut i = 0
   while i < runs.len {
      def run = runs.get(i)
      def text = to_str(run.get(0, ""))
      if text.len > 0 {
         flat = flat.append(text)
         flat = flat.append(float(run.get(2, 0.0)))
         flat = flat.append(_tok_color_u32(int(run.get(1, syntax.TOK_TEXT))))
      }
      i += 1
   }
   syntax_flat_cache[key] = flat
   flat
}

fn _append_plain_run(list out, str line, f64 x, f64 y) list {
   if line.len <= 0 { return out }
   out = out.append(line)
   out = out.append(x)
   out = out.append(y)
   out = out.append(U_TEXT)
   out
}

fn _diff_buffer_name(str filename) bool {
   str.startswith(filename, "*git diff:") || str.startswith(filename, "*diff:")
}

fn _diff_line_color(str line) int {
   if str.startswith(line, "diff --git") { return U_CYAN }
   if str.startswith(line, "+++") || str.startswith(line, "---") { return U_CYAN }
   if str.startswith(line, "@@") { return U_ACCENT }
   if str.startswith(line, "+") { return U_OK }
   if str.startswith(line, "-") { return U_WARN }
   if str.startswith(line, ">") { return U_OK }
   if str.startswith(line, "<") { return U_WARN }
   if str.startswith(line, "left:") || str.startswith(line, "right:") { return U_ACCENT_2 }
   U_MUTED
}

fn _append_diff_run(list out, str line, f64 x, f64 y) list {
   _append_text_run(out, line, x, y, _diff_line_color(line))
}

fn _append_text_run(list out, str text, f64 x, f64 y, int color) list {
   if text.len <= 0 { return out }
   out = out.append(text)
   out = out.append(x)
   out = out.append(y)
   out = out.append(color)
   out
}

fn _append_right_run(list out, int font, str text, f64 right_x, f64 y, int color) list {
   if text.len <= 0 { return out }
   def w = _measure_ui(font, text, widgets.text_w(font, text, 0, 0, 7.0))
   _append_text_run(out, text, right_x - w, y, color)
}

fn _append_center_run(list out, int font, str text, f64 cx, f64 y, int color) list {
   if text.len <= 0 { return out }
   def w = _measure_ui(font, text, 0.0)
   _append_text_run(out, text, cx - w * 0.5, y, color)
}

fn _append_highlight_runs(list out, str filename, str line, int li, f64 x, f64 y, bool highlight=true) list {
   if _diff_buffer_name(filename) { return _append_diff_run(out, line, x, y) }
   if !highlight { return _append_plain_run(out, line, x, y) }
   def runs = _syntax_rel_runs(filename, line, li)
   if runs.len == 0 { return _append_plain_run(out, line, x, y) }
   mut i = 0
   while i + 2 < runs.len {
      out = out.append(to_str(runs.get(i, "")))
      out = out.append(x + float(runs.get(i + 1, 0.0)))
      out = out.append(y)
      out = out.append(int(runs.get(i + 2, U_TEXT)))
      i += 3
   }
   out
}

fn _visible_runs_key(
   str filename,
   int scroll,
   int rows,
   f64 text_x,
   f64 y,
   f64 num_x,
   int draw_limit,
   bool show_numbers,
   bool big_file
) str {
   to_str(content_rev) + "\t" + filename + "\t" + to_str(scroll) + "\t" + to_str(rows) +
   "\t" + to_str(int(text_x * 10.0)) + "\t" + to_str(int(y * 10.0)) +
   "\t" + to_str(int(num_x * 10.0)) + "\t" + to_str(draw_limit) +
   "\t" + (show_numbers ? "1" : "0") + "\t" + (big_file ? "1" : "0")
}

fn _prepare_visible_runs(
   list lines,
   str filename,
   int scroll,
   int rows,
   f64 text_x,
   f64 y,
   f64 num_x,
   int draw_limit,
   bool show_numbers,
   bool big_file
) int {
   def key = _visible_runs_key(filename, scroll, rows, text_x, y, num_x, draw_limit, show_numbers, big_file)
   if key == visible_runs_cache_key { return 0 }
   mut line_runs = []
   mut text_runs = []
   mut i = 0
   while i < rows && scroll + i < lines.len {
      def li = scroll + i
      def yy = y + EDITOR_TEXT_TOP + float(i) * ed.LINE_H
      def line = to_str(lines.get(li, ""))
      def draw_line = _draw_line_text(line, draw_limit)
      if show_numbers {
         line_runs = line_runs.append(to_str(li + 1))
         line_runs = line_runs.append(num_x)
         line_runs = line_runs.append(yy + 2.0)
      }
      text_runs = _append_highlight_runs(text_runs, filename, draw_line, li, text_x, yy, _highlight_on(draw_line))
      i += 1
   }
   visible_runs_cache_key = key
   visible_line_runs_cache = line_runs
   visible_text_runs_cache = text_runs
   0
}

fn _syntax_runs_text(list runs) str {
   mut out = ""
   mut i = 0
   while i < runs.len {
      def run = runs.get(i, [])
      if is_list(run) { out = out + to_str(run.get(0, "")) }
      i += 1
   }
   out
}

fn _draw_highlighted_line(str filename, str line, int li, f64 x, f64 y) int {
   def runs = _syntax_runs(filename, line, li)
   if runs.len == 0 { return 0 }
   mut i = 0
   while i < runs.len {
      def run = runs.get(i)
      _text(font_body, to_str(run.get(0, "")), x + float(run.get(2, 0.0)), y, _tok_color(int(run.get(1, syntax.TOK_TEXT))))
      i += 1
   }
   0
}

fn _measure_prefix(str line, int col) f64 {
   if width_cache_rev != content_rev {
      width_cache_rev = content_rev
      width_cache = dict(512)
   }
   def n = min(max(col, 0), line.len)
   if n <= 0 { return 0.0 }
   if _mono_fast_prefix(line, n) { return float(n) * FONT_BODY_ADV }
   def key = to_str(n) + ":" + line
   if width_cache.contains(key) { return float(width_cache.get(key, 0.0)) }
   def w = float(gfx.measure_text_fast(font_body, str.str_slice(line, 0, n)).get(0, 0.0))
   width_cache[key] = w
   w
}

fn _mono_fast_prefix(str line, int n) bool {
   mut i = 0
   while i < n {
      def c = load8(line, i)
      if c < 32 || c > 126 { return false }
      i += 1
   }
   true
}

fn _hex_color(str hex) any {
   hex.len >= 7 ? gfx.color_hex(hex) : C_ACCENT
}

fn _color_picker_open() int {
   def hit = colorpicker.at_cursor(ed.current_lines(st), int(st.get("cursor_line", 0)), int(st.get("cursor_col", 0)))
   if !is_dict(hit) || hit.len <= 0 {
      color_state = colorpicker.close(color_state)
      _status("no hex color")
      return 0
   }
   color_state = colorpicker.open(color_state, hit)
   _status(to_str(hit.get("hex", "")) + " " + colorpicker.rgb_label(to_str(hit.get("hex", ""))))
   0
}

fn _draw_color_swatches_visible(list lines, int scroll, int rows, f64 text_x, f64 y, int draw_limit) int {
   def swatches = colorpicker.swatches_visible(lines, scroll, rows, draw_limit)
   mut i = 0
   while i < swatches.len {
      def hit = swatches.get(i, {})
      def li = int(hit.get("line", -1))
      if li >= scroll && li < lines.len {
         def line = _draw_line_text(to_str(lines.get(li, "")), draw_limit)
         def start = min(max(int(hit.get("start", 0)), 0), line.len)
         def stop = min(max(int(hit.get("end", start)), start), line.len)
         def hex = to_str(hit.get("hex", ""))
         if hex.len >= 7 && stop > start {
            def yy = y + EDITOR_TEXT_TOP + float(li - scroll) * ed.LINE_H
            def sx = text_x + _measure_prefix(line, start)
            def ex = text_x + _measure_prefix(line, stop)
            def col = _hex_color(hex)
            _fill_rect(sx, yy + ed.LINE_H - 4.0, max(12.0, ex - sx), 2.0, gfx.color_alpha(col, 0.85))
            _fill_rect(sx - 13.0, yy + 3.0, 8.0, 8.0, col)
            _stroke_rect(sx - 13.0, yy + 3.0, 8.0, 8.0, C_LINE_2, 1.0)
         }
      }
      i += 1
   }
   0
}

fn _draw_color_picker(dict lay, f64 sw, f64 sh) int {
   if !colorpicker.is_open(color_state) { return 0 }
   def hex = colorpicker.hex(color_state)
   if hex.len < 7 { return 0 }
   def row = colorpicker.line(color_state)
   def scroll = int(st.get("scroll", 0))
   def x0 = float(lay.get("edit_x", 0.0))
   def y0 = float(lay.get("edit_y", 0.0))
   def rows = ed.visible_rows(float(lay.get("edit_h", 0.0)))
   mut x = min(max(x0 + 42.0, 16.0), max(16.0, sw - 236.0))
   mut y = min(max(y0 + 40.0, 16.0), max(16.0, sh - 112.0))
   if row >= scroll && row < scroll + rows {
      def line = to_str(ed.current_lines(st).get(row, ""))
      x = min(max(_line_text_x(lay) + _measure_prefix(line, colorpicker.start(color_state)) + 18.0, 16.0), max(16.0, sw - 236.0))
      y = min(max(y0 + EDITOR_TEXT_TOP + float(row - scroll) * ed.LINE_H + 20.0, 16.0), max(16.0, sh - 112.0))
   }
   def w = 220.0
   def h = 88.0
   def col = _hex_color(hex)
   _fill_rect(x, y, w, h, gfx.color_alpha(C_RAIL, 0.97))
   _stroke_rect(x, y, w, h, C_ACCENT, 1.0)
   _fill_rect(x + 14.0, y + 16.0, 46.0, 46.0, col)
   _stroke_rect(x + 14.0, y + 16.0, 46.0, 46.0, C_LINE_2, 1.0)
   _text(font_body, hex, x + 76.0, y + 18.0, C_TEXT)
   _text(font_small, colorpicker.rgb_label(hex), x + 76.0, y + 43.0, C_MUTED)
   0
}

fn _draw_selection_for_line(str line, int li, f64 text_x, f64 y, list r) int {
   if r.len < 4 { return 0 }
   def sl = int(r.get(0, 0))
   def sc = int(r.get(1, 0))
   def el = int(r.get(2, 0))
   def ec = int(r.get(3, 0))
   if li < sl || li > el { return 0 }
   def a = li == sl ? sc : 0
   def b = li == el ? ec : line.len
   if b <= a { return 0 }
   def sx = text_x + _measure_prefix(line, a)
   def ex = text_x + _measure_prefix(line, b)
   _fill_rect(sx, y - 2.0, max(4.0, ex - sx), ed.LINE_H, gfx.color_alpha(C_SELECT, 0.70))
   0
}

fn _draw_find_for_line(str line, int li, f64 text_x, f64 y) int {
   if !find.is_open(find_state) || find.count(find_state) <= 0 { return 0 }
   def rs = find.results(find_state)
   def active_idx = find.index(find_state)
   mut i = 0
   while i < rs.len {
      def r = rs.get(i)
      def row = int(r.get("line", -1))
      if row > li { break }
      if row == li {
         def a = min(max(int(r.get("start", 0)), 0), line.len)
         def b = min(max(int(r.get("end", a)), a), line.len)
         if b > a {
            def sx = text_x + _measure_prefix(line, a)
            def ex = text_x + _measure_prefix(line, b)
            _fill_rect(sx, y - 2.0, max(4.0, ex - sx), ed.LINE_H, i == active_idx ? gfx.color_alpha(C_ACCENT, 0.52) : gfx.color_alpha(C_CYAN, 0.24))
         }
      }
      i += 1
   }
   0
}

fn _draw_find_visible(list lines, int scroll, int rows, f64 text_x, f64 y) int {
   if !find.is_open(find_state) || find.count(find_state) <= 0 { return 0 }
   def rs = find.results(find_state)
   def active_idx = find.index(find_state)
   def last = scroll + rows
   mut i = 0
   while i < rs.len {
      def r = rs.get(i)
      def row = int(r.get("line", -1))
      if row >= last { break }
      if row >= scroll && row < lines.len {
         def line = to_str(lines.get(row, ""))
         def a = min(max(int(r.get("start", 0)), 0), line.len)
         def b = min(max(int(r.get("end", a)), a), line.len)
         if b > a {
            def yy = y + EDITOR_TEXT_TOP + float(row - scroll) * ed.LINE_H
            def sx = text_x + _measure_prefix(line, a)
            def ex = text_x + _measure_prefix(line, b)
            _fill_rect(sx, yy - 2.0, max(4.0, ex - sx), ed.LINE_H, i == active_idx ? gfx.color_alpha(C_ACCENT, 0.52) : gfx.color_alpha(C_CYAN, 0.24))
         }
      }
      i += 1
   }
   0
}

fn _draw_minimap(dict lay, int rows, int scroll, int total) int {
   if total <= rows { return 0 }
   def x = float(lay.get("edit_x", 0.0)) + float(lay.get("edit_w", 0.0)) - 10.0
   def y = float(lay.get("edit_y", 0.0)) + 10.0
   def h = float(lay.get("edit_h", 0.0)) - 20.0
   def thumb_h = max(18.0, h * float(rows) / float(total))
   def max_scroll = max(1, total - rows)
   def thumb_y = y + (h - thumb_h) * float(scroll) / float(max_scroll)
   _fill_rect(x, y, 3.0, h, gfx.color_alpha(C_LINE, 0.65))
   _fill_rect(x - 1.0, thumb_y, 5.0, thumb_h, gfx.color_alpha(C_ACCENT, 0.78))
   0
}

fn _outline_visible(f64 h) int {
   max(0, int((h - SECTION_H) / outline.ROW_H))
}

fn _outline_clamp(int scroll, int total, int visible) int {
   min(max(0, scroll), max(0, total - visible))
}

fn _rail_tree_height(f64 body_h, int tree_rows, int outline_rows) f64 {
   def sep = 1.0
   def avail = max(1.0, body_h - sep)
   if avail < 120.0 { return avail * 0.50 }
   def min_pane = min(150.0, max(64.0, avail * 0.24))
   def tree_need = project.TREE_HEADER_H + float(max(1, tree_rows)) * project.TREE_ROW_H
   def outline_need = SECTION_H + float(max(1, outline_rows)) * outline.ROW_H
   mut h = avail * tree_need / max(1.0, tree_need + outline_need)
   if outline_rows <= 0 { h = max(h, avail * 0.72) }
   if tree_rows <= 3 && outline_rows > 8 { h = min(h, avail * 0.36) }
   min(max(min_pane, h), max(min_pane, avail - min_pane))
}

fn _top_tab_w(f64 sw, int count) f64 {
   min(TOP_TAB_MAX_W, max(TOP_TAB_MIN_W, (sw - 12.0) / float(max(1, count))))
}

fn _top_tab_hit(dict lay, f64 mx, f64 my) dict {
   def top_h = float(lay.get("top", 0.0))
   if !bool(st.get("show_titlebar", true)) || top_h <= 1.0 || my < 0.0 || my > top_h { return {"action": "", "idx": -1} }
   def bs = st.get("buffers", [])
   def sw = _layout_sw(lay)
   def tab_w = _top_tab_w(sw, bs.len)
   def start_x = 6.0
   mut i = 0
   while i < bs.len {
      def x = start_x + float(i) * tab_w
      if mx >= x && mx <= x + tab_w && my >= 3.0 && my <= top_h - 3.0 {
         return {"action": mx >= x + tab_w - 24.0 ? "close" : "select", "idx": i}
      }
      i += 1
   }
   {"action": "", "idx": -1}
}

fn _select_top_tab(int idx) int {
   def bs = st.get("buffers", [])
   if idx < 0 || idx >= bs.len { return 0 }
   if idx != int(st.get("active", 0)) { _nav_push_current() }
   st = ed.select_buffer(st, idx)
   _buffer_changed(true)
   _nav_record_current()
   _set_focus("editor", false)
   0
}

fn _close_top_tab(int idx) int {
   def bs = st.get("buffers", [])
   if idx < 0 || idx >= bs.len { return 0 }
   st["active"] = idx
   _kill_buffer()
   _set_focus("editor", false)
   0
}

fn _draw_top(dict lay, int fps) int {
   def sw = _layout_sw(lay)
   def top_h = float(lay.get("top", 32.0))
   if top_h <= 1.0 { return 0 }
   _fill_rect(0.0, 0.0, sw, top_h, C_RAIL)
   _fill_rect(0.0, top_h - 1.0, sw, 1.0, C_LINE)
   def bs = st.get("buffers", [])
   def active = int(st.get("active", 0))
   def tab_w = _top_tab_w(sw, bs.len)
   def tab_text_y = _text_center_y_for(font_modeline, 3.0, top_h - 5.0)
   mut runs = []
   mut i = 0
   while i < bs.len {
      def b = bs.get(i, {})
      def x = 6.0 + float(i) * tab_w
      if x < sw - 8.0 {
         def on = i == active
         def dirty = bool(b.get("dirty", false))
         def kind = to_str(b.get("kind", project.file_kind(to_str(b.get("name", "")))))
         def icon = kind == "image" ? "asset_image" : (kind == "diff" ? "vcsbranches" : (kind == "term" ? "terminal" : "codeedit"))
         _fill_rect(x, 3.0, min(tab_w - 2.0, sw - x - 6.0), top_h - 5.0, on ? C_PANE_2 : C_PANE)
         _stroke_rect(x, 3.0, min(tab_w - 2.0, sw - x - 6.0), top_h - 5.0, on ? C_LINE_2 : C_LINE, 1.0)
         if on { _fill_rect(x, top_h - 2.0, min(tab_w - 2.0, sw - x - 6.0), 2.0, C_ACCENT) }
         _draw_icon(icon, x + 8.0, _center_y(3.0, top_h - 5.0, 14.0), 14.0, on ? C_ACCENT : C_MUTED)
         def tab_label = _modeline_text((dirty ? "* " : "") + to_str(b.get("name", "buffer")))
         runs = _append_text_run(runs, _preview_modeline(tab_label, max(20.0, tab_w - 56.0)), x + 28.0, tab_text_y, on ? U_TEXT : U_MUTED)
         runs = _append_text_run(runs, "x", x + tab_w - 18.0, tab_text_y, on ? U_MUTED : U_DIM)
      }
      i += 1
   }
   _flush_rects()
   if runs.len > 0 { gfx.draw_text_runs_flat_colors(font_modeline, runs) }
   0
}

fn _draw_git_badge(str code, f64 right_x, f64 y) int {
   mut runs = []
   runs = _queue_git_badge(runs, code, right_x, y)
   _flush_rects()
   if runs.len > 0 { gfx.draw_text_runs_flat_colors(font_small, runs) }
   0
}

fn _queue_git_badge(list runs, str code, f64 right_x, f64 y) list {
   def label = project.status_label(code)
   if label.len <= 0 { return runs }
   def col = _git_color(code)
   _fill_rect(right_x - 18.0, y + 3.0, 14.0, 14.0, gfx.color_alpha(col, 0.18))
   _stroke_rect(right_x - 18.0, y + 3.0, 14.0, 14.0, col, 1.0)
   _append_center_run(runs, font_small, label, right_x - 11.0, y + 4.0, _pack(col))
}

fn _draw_section_title(str icon, str title, f64 x, f64 y, f64 w, str right="") int {
   _fill_rect(x, y, w, SECTION_H, C_PANE_2)
   _draw_icon(icon, x + 8.0, y + SECTION_ICON_Y, 15.0, C_MUTED)
   mut runs = []
   runs = _append_text_run(runs, title, x + 30.0, y + SECTION_TEXT_Y, U_MUTED)
   if right.len > 0 { runs = _append_right_run(runs, font_small, right, x + w - 8.0, y + SECTION_TEXT_Y, U_MUTED) }
   _flush_rects()
   if runs.len > 0 { gfx.draw_text_runs_flat_colors(font_small, runs) }
   0
}

fn _rail_tab() str { to_str(st.get("rail_tab", "files")) }

fn _rail_tab_at(f64 x, f64 y, f64 w, f64 mx, f64 my) str {
   if !widgets.hit(mx, my, x, y, w, RAIL_TABS_H) { return "" }
   def tab_w = w / 3.0
   def idx = int((mx - x) / tab_w)
   idx <= 0 ? "files" : (idx == 1 ? "timeline" : "git")
}

fn _rail_section_at(dict lay, f64 mx, f64 my) str {
   if !bool(st.get("show_hierarchy", true)) || _rail_tab() != "files" { return "" }
   def x = float(lay.get("rail_x", 0.0))
   def y = float(lay.get("rail_y", 0.0))
   def w = float(lay.get("rail_w", 0.0))
   def h = float(lay.get("rail_h", 0.0))
   if !widgets.hit(mx, my, x, y + RAIL_TABS_H, w, h - RAIL_TABS_H) { return "" }
   def body_y = y + RAIL_TABS_H
   def body_h = h - RAIL_TABS_H
   def show_project = bool(st.get("show_project", true))
   def show_outline = bool(st.get("show_outline", true))
   if show_project { _ensure_project_tree() }
   def rows = project.tree(project_state)
   def syms = show_outline ? _outline_symbols() : []
   def tree_h = (!show_project) ? 0.0 : (show_outline ? _rail_tree_height(body_h, rows.len, syms.len) : body_h)
   if show_project && my >= body_y && my < body_y + project.TREE_HEADER_H { return "project" }
   def outline_y = body_y + (show_project ? tree_h + 1.0 : 0.0)
   if show_outline && my >= outline_y && my < outline_y + SECTION_H { return "outline" }
   ""
}

fn _hide_rail_section(str section) int {
   if section == "project" { st["show_project"] = false }
   elif section == "outline" { st["show_outline"] = false }
   if !bool(st.get("show_project", true)) && !bool(st.get("show_outline", true)) {
      st["show_hierarchy"] = false
      _set_focus("editor", false)
   }
   _save_ui_cache()
   _status(section + " hidden")
   0
}

fn _toggle_rail_section(str section) int {
   if section == "project" { st["show_project"] = !bool(st.get("show_project", true)) }
   elif section == "outline" { st["show_outline"] = !bool(st.get("show_outline", true)) }
   if bool(st.get("show_project", true)) || bool(st.get("show_outline", true)) { st["show_hierarchy"] = true }
   _save_ui_cache()
   _status(section + (section == "project" ? (bool(st.get("show_project", true)) ? " on" : " off") : (bool(st.get("show_outline", true)) ? " on" : " off")))
   0
}

fn _draw_rail_tabs(f64 x, f64 y, f64 w) int {
   def active = _rail_tab()
   def tabs = [["files", "filetree", "Files"], ["timeline", "history", "Time"], ["git", "vcsbranches", "Git"]]
   def tab_w = w / 3.0
   mut runs = []
   mut i = 0
   while i < tabs.len {
      def t = tabs.get(i)
      def id = to_str(t.get(0, ""))
      def tx = x + float(i) * tab_w
      def on = id == active
      _fill_rect(tx, y, tab_w, RAIL_TABS_H, on ? gfx.color_alpha(C_ACCENT, 0.13) : C_PANE_2)
      _stroke_rect(tx, y, tab_w, RAIL_TABS_H, on ? C_ACCENT : C_LINE, 1.0)
      _draw_icon(to_str(t.get(1, "")), tx + 8.0, y + 7.0, 15.0, on ? C_ACCENT : C_MUTED)
      runs = _append_text_run(runs, widgets.preview(to_str(t.get(2, "")), int(max(4.0, (tab_w - 30.0) / 7.0))), tx + 27.0, y + 9.0, on ? U_TEXT : U_MUTED)
      i += 1
   }
   _flush_rects()
   if runs.len > 0 { gfx.draw_text_runs_flat_colors(font_small, runs) }
   0
}

fn _draw_git_panel(f64 x, f64 y, f64 w, f64 h) int {
   _ensure_project_git()
   def rows = project.changes(project_state)
   def counts = project.counts(project_state)
   def dirty = int(counts.get("modified", 0)) + int(counts.get("added", 0)) + int(counts.get("deleted", 0)) + int(counts.get("untracked", 0))
   _draw_section_title("vcsbranches", "CHANGES", x, y, w, project.branch(project_state) + (dirty > 0 ? " +" + to_str(dirty) : ""))
   def visible = project.visible_count(h)
   def scroll = project.clamp_scroll(int(st.get("git_scroll", 0)), rows.len, visible)
   st["git_scroll"] = scroll
   mut runs = []
   mut i = 0
   while i < visible && scroll + i < rows.len {
      def yy = project.row_y(y, i)
      def e = rows.get(scroll + i, {})
      def code = to_str(e.get("git", ""))
      def col = _git_color(code)
      _draw_icon(project.file_icon(e), x + 10.0, yy + 3.0, 16.0, col)
      runs = _append_text_run(runs, widgets.preview(to_str(e.get("rel", "")), int(max(8.0, (w - 74.0) / 7.2))), x + 32.0, yy + ROW_TEXT_Y, U_MUTED)
      runs = _queue_git_badge(runs, code, x + w - 8.0, yy)
      i += 1
   }
   if rows.len == 0 { runs = _append_text_run(runs, "working tree clean", x + 12.0, y + 36.0, U_MUTED) }
   _flush_rects()
   if runs.len > 0 { gfx.draw_text_runs_flat_colors(font_small, runs) }
   i
}

fn _timeline_rows() list {
   mut out = []
   def bs = st.get("buffers", [])
   mut bi = bs.len - 1
   while bi >= 0 && out.len < 12 {
      def b = bs.get(bi, {})
      out = out.append(["buffer", to_str(b.get("name", "buffer")), to_str(b.get("path", b.get("source_path", "")))])
      bi -= 1
   }
   def hist_rows = interact.history(ui_state)
   mut hi = hist_rows.len - 1
   while hi >= 0 && out.len < 64 {
      def id = to_str(hist_rows.get(hi, ""))
      def row = cmd.by_id(id)
      out = out.append(["command", row.len > 0 ? to_str(row.get(0, id)) : id, id])
      hi -= 1
   }
   out
}

fn _draw_timeline_panel(f64 x, f64 y, f64 w, f64 h) int {
   def rows = _timeline_rows()
   _draw_section_title("history", "TIMELINE", x, y, w, to_str(rows.len))
   def visible = project.visible_count(h)
   def scroll = project.clamp_scroll(int(st.get("timeline_scroll", 0)), rows.len, visible)
   st["timeline_scroll"] = scroll
   mut runs = []
   mut i = 0
   while i < visible && scroll + i < rows.len {
      def yy = project.row_y(y, i)
      def row = rows.get(scroll + i, [])
      def kind = to_str(row.get(0, ""))
      def label = to_str(row.get(1, ""))
      def detail = to_str(row.get(2, ""))
      _draw_icon(kind == "buffer" ? "textfile" : "statusindicator", x + 10.0, yy + 3.0, 16.0, kind == "buffer" ? C_ACCENT_2 : C_MUTED)
      runs = _append_text_run(runs, widgets.preview(label, int(max(8.0, (w - 50.0) / 7.2))), x + 32.0, yy + ROW_TEXT_Y, kind == "buffer" ? U_TEXT : U_MUTED)
      if detail.len > 0 { runs = _append_right_run(runs, font_small, widgets.preview(detail, 12), x + w - 8.0, yy + ROW_TEXT_Y, U_DIM) }
      i += 1
   }
   if rows.len == 0 { runs = _append_text_run(runs, "no timeline yet", x + 12.0, y + 36.0, U_MUTED) }
   _flush_rects()
   if runs.len > 0 { gfx.draw_text_runs_flat_colors(font_small, runs) }
   i
}

fn _draw_project_tree(f64 x, f64 y, f64 w, f64 h) int {
   _ensure_project_tree()
   def counts = project.counts(project_state)
   def dirty = int(counts.get("modified", 0)) + int(counts.get("added", 0)) + int(counts.get("deleted", 0)) + int(counts.get("untracked", 0))
   def branch = project.branch(project_state)
   def right = (branch.len > 0 ? branch : "cwd") + (dirty > 0 ? " +" + to_str(dirty) : "")
   _draw_section_title("filetree", ospath.basename(project.root(project_state)), x, y, w, right)
   def rows = project.tree(project_state)
   def visible = project.visible_count(h)
   def scroll = project.clamp_scroll(int(st.get("tree_scroll", 0)), rows.len, visible)
   st["tree_scroll"] = scroll
   def active_path = _buffer_source_path(ed.current_buffer(st))
   def active_norm = active_path.len > 0 ? ospath.normalize(active_path) : ""
   mut runs = []
   mut i = 0
   while i < visible && scroll + i < rows.len {
      def yy = project.row_y(y, i)
      if yy + project.TREE_ROW_H > y + h { break }
      def e = rows.get(scroll + i, {})
      def depth = int(e.get("depth", 0))
      def path = to_str(e.get("path", ""))
      def active = path.len > 0 && active_norm.len > 0 && (path == active_path || path == active_norm)
      def selected = _project_selection_contains(e)
      if selected { _fill_rect(x + 4.0, yy, w - 8.0, project.TREE_ROW_H, gfx.color_alpha(C_ACCENT, active ? 0.25 : 0.14)) }
      elif active { _fill_rect(x + 4.0, yy, w - 8.0, project.TREE_ROW_H, gfx.color_alpha(C_ACCENT, 0.18)) }
      mut ix = x + 8.0 + float(depth) * 12.0
      if e.get("dir", false) {
         _draw_icon(e.get("open", false) ? "guitreearrowdown" : "guitreearrowright", ix, yy + 4.0, 12.0, selected ? C_ACCENT : C_MUTED)
         ix += 14.0
      }
      _draw_icon(project.file_icon(e), ix, yy + 3.0, 16.0, (active || selected) ? C_ACCENT : C_MUTED)
      runs = _append_text_run(runs, widgets.preview(to_str(e.get("name", "")), int(max(8.0, (w - (ix - x) - 46.0) / 7.2))), ix + 20.0, yy + ROW_TEXT_Y, (active || selected) ? U_TEXT : U_MUTED)
      runs = _queue_git_badge(runs, to_str(e.get("git", "")), x + w - 8.0, yy)
      i += 1
   }
   if rows.len > visible && visible > 0 {
      def track_y = y + project.TREE_HEADER_H
      def track_h = h - project.TREE_HEADER_H
      def thumb_h = max(20.0, track_h * float(visible) / float(rows.len))
      def thumb_y = track_y + (track_h - thumb_h) * float(scroll) / float(max(1, rows.len - visible))
      _fill_rect(x + w - 4.0, track_y, 2.0, track_h, gfx.color_alpha(C_LINE, 0.65))
      _fill_rect(x + w - 5.0, thumb_y, 4.0, thumb_h, gfx.color_alpha(C_ACCENT, 0.55))
   }
   _flush_rects()
   if runs.len > 0 { gfx.draw_text_runs_flat_colors(font_small, runs) }
   i
}

fn _draw_outline(f64 x, f64 y, f64 w, f64 h) int {
   def syms = _outline_symbols()
   _draw_section_title("graphedit", "OUTLINE", x, y, w, to_str(syms.len))
   def visible = _outline_visible(h)
   def scroll = _outline_clamp(int(st.get("outline_scroll", 0)), syms.len, visible)
   st["outline_scroll"] = scroll
   mut runs = []
   mut i = 0
   while i < visible && scroll + i < syms.len {
      def yy = outline.row_y(y, i)
      if yy + outline.ROW_H > y + h { break }
      def s = syms.get(scroll + i, {})
      def line = int(s.get("line", 0))
      def active = line == int(st.get("cursor_line", 0))
      if active { _fill_rect(x + 4.0, yy, w - 8.0, outline.ROW_H, gfx.color_alpha(C_ACCENT, 0.16)) }
      _draw_icon(to_str(s.get("icon", "graphnode")), x + 10.0, yy + 3.0, 15.0, active ? C_ACCENT : C_MUTED)
      runs = _append_text_run(runs, widgets.preview(to_str(s.get("name", "")), int(max(8.0, (w - 72.0) / 7.2))), x + 30.0, yy + 4.0, active ? U_TEXT : U_MUTED)
      runs = _append_right_run(runs, font_small, to_str(line + 1), x + w - 8.0, yy + 4.0, U_MUTED)
      i += 1
   }
   if syms.len > visible && visible > 0 {
      def track_y = y + SECTION_H
      def track_h = h - SECTION_H
      def thumb_h = max(20.0, track_h * float(visible) / float(syms.len))
      def thumb_y = track_y + (track_h - thumb_h) * float(scroll) / float(max(1, syms.len - visible))
      _fill_rect(x + w - 4.0, track_y, 2.0, track_h, gfx.color_alpha(C_LINE, 0.65))
      _fill_rect(x + w - 5.0, thumb_y, 4.0, thumb_h, gfx.color_alpha(C_ACCENT, 0.55))
   }
   if syms.len == 0 { runs = _append_text_run(runs, "no symbols", x + 12.0, y + 34.0, U_MUTED) }
   _flush_rects()
   if runs.len > 0 { gfx.draw_text_runs_flat_colors(font_small, runs) }
   0
}

fn _draw_rail(dict lay) int {
   def x = float(lay.get("rail_x", 0.0))
   def y = float(lay.get("rail_y", 0.0))
   def w = float(lay.get("rail_w", 0.0))
   def h = float(lay.get("rail_h", 0.0))
   if w <= 1.0 || !bool(st.get("show_hierarchy", true)) { return 0 }
   if MOCK_BENCH_FAST && frame_count >= BENCH_WARMUP_FRAMES { return 0 }
   _fill_rect(x, y, w, h, C_RAIL)
   _stroke_rect(x, y, w, h, to_str(st.get("focus", "editor")) == "project" ? C_ACCENT : C_LINE, to_str(st.get("focus", "editor")) == "project" ? 2.0 : 1.0)
   _draw_rail_tabs(x, y, w)
   def body_y = y + RAIL_TABS_H
   def body_h = h - RAIL_TABS_H
   def tab = _rail_tab()
   if tab == "git" { _draw_git_panel(x, body_y, w, body_h) return 0 }
   if tab == "timeline" { _draw_timeline_panel(x, body_y, w, body_h) return 0 }
   def show_project = bool(st.get("show_project", true))
   def show_outline = bool(st.get("show_outline", true))
   if !show_project && !show_outline {
      _draw_section_title("boxcontainer", "PANES", x, body_y, w, "hidden")
      _text(font_small, "C-SPC t p project  C-SPC t o outline", x + 12.0, body_y + 36.0, C_MUTED)
      return 0
   }
   if show_project { _ensure_project_tree() }
   def rows = project.tree(project_state)
   if show_project && show_outline {
      def syms = _outline_symbols()
      def tree_h = _rail_tree_height(body_h, rows.len, syms.len)
      _draw_project_tree(x, body_y, w, tree_h)
      _fill_rect(x, body_y + tree_h, w, 1.0, C_LINE)
      _draw_outline(x, body_y + tree_h + 1.0, w, body_h - tree_h - 1.0)
   } elif show_project {
      _draw_project_tree(x, body_y, w, body_h)
   } else {
      _draw_outline(x, body_y, w, body_h)
   }
   0
}

fn _sync_scroll(int rows, int total) int {
   def cursor = int(st.get("cursor_line", 0))
   mut scroll = int(st.get("scroll", 0))
   def max_scroll = max(0, total - rows)
   if bool(st.get("scroll_follow", true)) {
      if cursor < scroll { scroll = cursor }
      if cursor >= scroll + rows { scroll = cursor - rows + 1 }
   }
   st["scroll"] = min(max_scroll, max(0, scroll))
   0
}

fn _draw_editor(dict lay, bool caret) int {
   def x = float(lay.get("edit_x", 0.0))
   def y = float(lay.get("edit_y", 0.0))
   def w = float(lay.get("edit_w", 0.0))
   def h = float(lay.get("edit_h", 0.0))
   def rows = ed.visible_rows(h)
   def cur_buf = ed.current_buffer(st)
   def lines = ed.current_lines(st)
   _sync_scroll(rows, lines.len)
   _find_refresh_if_needed()
   _fill_rect(x, y, w, h, C_PANE)
   _stroke_rect(x, y, w, h, C_LINE, 1.0)
   def div_x = float(lay.get("divider_x", x - 9.0))
   _fill_rect(div_x - 1.0, y, 2.0, h, st.get("drag_divider", false) ? C_ACCENT : C_LINE)
   if _draw_image_preview(lay, cur_buf) { return 0 }
   def text_x = _line_text_x(lay)
   def filename = to_str(cur_buf.get("name", ""))
   def scroll = int(st.get("scroll", 0))
   mut line_runs = []
   mut text_runs = []
   def show_numbers = bool(st.get("show_line_numbers", true))
   def show_guides = bool(st.get("show_indent_guides", false))
   def draw_limit = _line_limit(lay)
   def big_file = _buffer_is_big(lines)
   def selection_ranges = _selection_ranges_active()
   def cursor_row = int(st.get("cursor_line", 0))
   if cursor_row >= scroll && cursor_row < scroll + rows {
      def cy0 = y + EDITOR_TEXT_TOP + float(cursor_row - scroll) * ed.LINE_H
      _fill_rect(x + 1.0, cy0 - 2.0, w - 2.0, ed.LINE_H, gfx.color_alpha(C_ACCENT, 0.10))
   }
   if show_guides || selection_ranges.len > 0 {
      mut i = 0
      while i < rows && scroll + i < lines.len {
         def li = scroll + i
         def yy = y + EDITOR_TEXT_TOP + float(i) * ed.LINE_H
         def line = to_str(lines.get(li, ""))
         def draw_line = _draw_line_text(line, draw_limit)
         if show_guides {
            mut g = 0
            while g < draw_line.len && load8(draw_line, g) == 32 {
               if g > 0 && g % 3 == 0 { _fill_rect(text_x + float(g) * FONT_BODY_ADV, yy - 1.0, 1.0, ed.LINE_H - 2.0, gfx.color_alpha(C_LINE_2, 0.58)) }
               g += 1
            }
         }
         mut si = 0
         while si < selection_ranges.len {
            _draw_selection_for_line(line, li, text_x, yy, selection_ranges.get(si, []))
            si += 1
         }
         if show_numbers {
            line_runs = line_runs.append(to_str(li + 1))
            line_runs = line_runs.append(x + 16.0)
            line_runs = line_runs.append(yy + 2.0)
         }
         text_runs = _append_highlight_runs(text_runs, filename, draw_line, li, text_x, yy, _highlight_on(draw_line))
         i += 1
      }
   } else {
      _prepare_visible_runs(lines, filename, scroll, rows, text_x, y, x + 16.0, draw_limit, show_numbers, big_file)
      line_runs = visible_line_runs_cache
      text_runs = visible_text_runs_cache
   }
   _draw_find_visible(lines, scroll, rows, text_x, y)
   if !big_file { _draw_color_swatches_visible(lines, scroll, rows, text_x, y, draw_limit) }
   _flush_rects()
   if line_runs.len > 0 { gfx.draw_text_runs_flat(font_small, line_runs, C_MUTED) }
   if text_runs.len > 0 { gfx.draw_text_runs_flat_colors(font_body, text_runs) }
   _draw_minimap(lay, rows, scroll, lines.len)
   if caret && int(st.get("cursor_line", 0)) >= scroll && int(st.get("cursor_line", 0)) < scroll + rows {
      def line = to_str(lines.get(int(st.get("cursor_line", 0)), ""))
      def cx = text_x + _measure_prefix(line, int(st.get("cursor_col", 0)))
      def cy = y + EDITOR_TEXT_TOP + float(int(st.get("cursor_line", 0)) - scroll) * ed.LINE_H
      _fill_rect(cx, cy - 1.0, 2.0, ed.LINE_H - 2.0, C_ACCENT)
   }
   if caret && _extra_cursor_count() > 0 {
      def extras = _extra_cursors()
      mut ei = 0
      while ei < extras.len {
         def erow = _cursor_row(extras.get(ei))
         if erow >= scroll && erow < scroll + rows {
            def eline = to_str(lines.get(erow, ""))
            def ecol = min(max(_cursor_col(extras.get(ei)), 0), eline.len)
            def ecx = text_x + _measure_prefix(eline, ecol)
            def ecy = y + EDITOR_TEXT_TOP + float(erow - scroll) * ed.LINE_H
            _fill_rect(ecx, ecy - 1.0, 2.0, ed.LINE_H - 2.0, C_ACCENT_2)
         }
         ei += 1
      }
   }
   0
}

fn _draw_terminal(dict lay) int {
   if !termpane.is_open(term_state) { return 0 }
   def x = float(lay.get("dock_x", 0.0))
   def y = float(lay.get("dock_y", 0.0))
   def w = float(lay.get("dock_w", 0.0))
   def h = float(lay.get("dock_h", 0.0))
   _fill_rect(x, y, w, h, C_RAIL)
   _flush_rects()
   term_state = termpane.draw(term_state)
   _fill_rect(x, y, w, TERM_TAB_H, C_PANE_2)
   mut runs = []
   def n = termpane.tab_count(term_state)
   def active = termpane.active_tab(term_state)
   def tw = _term_tab_w(w, n)
   mut i = 0
   while i < n {
      def tx = x + 6.0 + float(i) * tw
      def tab_w = min(tw - 2.0, x + w - tx - 40.0)
      if tab_w <= 16.0 { break }
      def on = i == active
      _fill_rect(tx, y + 3.0, tab_w, TERM_TAB_H - 5.0, on ? C_PANE : C_CHIP)
      _stroke_rect(tx, y + 3.0, tab_w, TERM_TAB_H - 5.0, on ? C_ACCENT : C_LINE, 1.0)
      if on { _fill_rect(tx, y + TERM_TAB_H - 2.0, tab_w, 2.0, C_ACCENT) }
      runs = _append_text_run(runs, widgets.preview(termpane.tab_title(term_state, i), int(max(4.0, (tab_w - 30.0) / FONT_SMALL_ADV))), tx + 9.0, y + 8.0, on ? U_TEXT : U_MUTED)
      runs = _append_text_run(runs, "x", tx + tab_w - 15.0, y + 8.0, on ? U_MUTED : U_DIM)
      i += 1
   }
   def plus_x = x + 6.0 + float(n) * tw + 3.0
   if plus_x + 24.0 < x + w - 8.0 {
      _fill_rect(plus_x, y + 3.0, 24.0, TERM_TAB_H - 5.0, C_CHIP)
      _stroke_rect(plus_x, y + 3.0, 24.0, TERM_TAB_H - 5.0, C_LINE, 1.0)
      runs = _append_center_run(runs, font_small, "+", plus_x + 12.0, y + 8.0, U_ACCENT)
   }
   runs = _append_right_run(runs, font_small, "F6 shell  F7 repl", x + w - 10.0, y + 8.0, U_DIM)
   _flush_rects()
   if runs.len > 0 { gfx.draw_text_runs_flat_colors(font_small, runs) }
   _stroke_rect(x, y, w, h, to_str(st.get("focus", "editor")) == "terminal" ? C_ACCENT : C_LINE, to_str(st.get("focus", "editor")) == "terminal" ? 2.0 : 1.0)
   0
}

fn _command_icon(str tag, str id) str {
   if id == "run" { return "asset_play" }
   if id == "check" || id == "lsp-diagnostics" { return "importcheck" }
   if id == "format" { return "codehighlighter" }
   if id == "diff-clipboard" { return "vcsbranches" }
   if id == "palette" || tag == "search" { return "search" }
   if tag == "file" { return "file" }
   if tag == "diff" { return "vcsbranches" }
   if tag == "run" { return "asset_script" }
   if tag == "debug" { return "debug" }
   if tag == "project" { return "filetree" }
   if tag == "edit" { return "codeedit" }
   if tag == "buffer" { return "textfile" }
   if tag == "pane" { return "boxcontainer" }
   if tag == "terminal" { return "terminal" }
   if tag == "lsp" { return "asset_world" }
   if tag == "toggle" { return "checkbutton" }
   "statusindicator"
}

fn _command_color(str tag) any {
   if tag == "run" { return C_OK }
   if tag == "diff" { return C_WARN }
   if tag == "debug" || tag == "lsp" { return C_CYAN }
   if tag == "project" || tag == "pane" { return C_ACCENT_2 }
   if tag == "edit" || tag == "buffer" { return C_ACCENT }
   if tag == "search" { return C_CYAN }
   if tag == "terminal" { return C_MUTED }
   C_DIM
}

fn _queue_key_badge(list runs, str label, f64 x, f64 y, any col=C_MUTED) list {
   if label.len <= 0 { return runs }
   def bw = max(30.0, float(gfx.measure_text_fast(font_small, label).get(0, 0.0)) + 16.0)
   _fill_rect(x, y, bw, 20.0, gfx.color_alpha(C_CHIP, 0.96))
   _stroke_rect(x, y, bw, 20.0, gfx.color_alpha(col, 0.55), 1.0)
   _append_center_run(runs, font_small, label, x + bw * 0.5, y + 5.0, _pack(col))
}

fn _draw_find_bar(f64 sw, f64 sh) int {
   if !find.is_open(find_state) { return 0 }
   _find_refresh_if_needed()
   def w = min(760.0, max(280.0, sw - 34.0))
   def x = sw * 0.5 - w * 0.5
   def y = max(8.0, sh - FIND_BAR_H - ed.STATUS_H - 10.0)
   _fill_rect(x, y, w, FIND_BAR_H, gfx.color_alpha(C_RAIL, 0.98))
   _stroke_rect(x, y, w, FIND_BAR_H, find.error(find_state).len > 0 ? C_WARN : C_ACCENT, 1.0)
   _draw_icon("search", x + 13.0, y + 13.0, 18.0, find.error(find_state).len > 0 ? C_WARN : C_ACCENT)
   mut body_runs = []
   mut small_runs = []
   def q = find.query(find_state)
   def repl = find.replacement(find_state)
   def replace_on = find.replace_on(find_state)
   def field = find.active_field(find_state)
   body_runs = _append_text_run(body_runs, _preview_body(q.len > 0 ? q : "find in buffer", w - 300.0), x + 40.0, y + 10.0, field == "find" ? U_ACCENT : (q.len > 0 ? U_TEXT : U_DIM))
   if replace_on {
      _draw_icon("edit", x + 14.0, y + 35.0, 16.0, field == "replace" ? C_ACCENT : C_MUTED)
      body_runs = _append_text_run(body_runs, _preview_body(repl.len > 0 ? repl : "replace with", w - 300.0), x + 40.0, y + 34.0, field == "replace" ? U_ACCENT : (repl.len > 0 ? U_TEXT : U_DIM))
   }
   small_runs = _append_right_run(small_runs, font_small, find.summary(find_state), x + w - 14.0, y + 16.0, find.error(find_state).len > 0 ? U_WARN : U_ACCENT)
   _fill_rect(x + 13.0, y + (replace_on ? 58.0 : 39.0), w - 26.0, 1.0, C_LINE)
   def chips = [
      ["Enter", "next"],
      ["S-Enter", "prev"],
      ["C-h", replace_on ? "replace on" : "replace"],
      ["Tab", field],
      ["C-Enter", "all"],
      ["C-r", find.regex_on(find_state) ? "regex on" : "regex"],
      ["C-c", find.case_on(find_state) ? "case on" : "case"],
      ["C-w", find.word_on(find_state) ? "word on" : "word"]
   ]
   mut cx = x + 14.0
   mut i = 0
   while i < chips.len {
      def chip = chips.get(i)
      def key_label = to_str(chip.get(0, ""))
      def text = to_str(chip.get(1, ""))
      def hot = str.str_contains(text, "on") || (key_label == "Tab" && replace_on)
      small_runs = _queue_key_badge(small_runs, key_label, cx, y + (replace_on ? 62.0 : 43.0), hot ? C_ACCENT : C_MUTED)
      cx += max(30.0, float(gfx.measure_text_fast(font_small, key_label).get(0, 0.0)) + 16.0) + 5.0
      small_runs = _append_text_run(small_runs, text, cx, y + (replace_on ? 67.0 : 48.0), hot ? U_ACCENT : U_DIM)
      cx += float(gfx.measure_text_fast(font_small, text).get(0, 0.0)) + 14.0
      i += 1
   }
   small_runs = _append_right_run(small_runs, font_small, "Esc close", x + w - 14.0, y + (replace_on ? 67.0 : 48.0), U_MUTED)
   _flush_rects()
   if body_runs.len > 0 { gfx.draw_text_runs_flat_colors(font_body, body_runs) }
   if small_runs.len > 0 { gfx.draw_text_runs_flat_colors(font_small, small_runs) }
   0
}

fn _draw_palette(f64 sw, f64 sh) int {
   if !pal.is_open(palette_state) { return 0 }
   def matches = _palette_matches()
   def rect = _palette_rect(sw, sh, matches.len)
   def x = float(rect.get(0, 0.0))
   def y = float(rect.get(1, 0.0))
   def w = float(rect.get(2, 0.0))
   def h = float(rect.get(3, 0.0))
   def rows = int(rect.get(4, 0))
   palette_state = pal.set_visible(palette_state, rows)
   def start = pal.scroll(palette_state)
   def visible_matches = pal.visible_matches(palette_state)
   _fill_rect(x, y, w, h, gfx.color_alpha(C_RAIL, 0.98))
   _stroke_rect(x, y, w, h, C_ACCENT, 2.0)
   _draw_icon("search", x + 16.0, y + 17.0, 20.0, C_ACCENT)
   def q = pal.query(palette_state)
   _fill_rect(x + 14.0, y + 48.0, w - 28.0, 1.0, C_LINE)
   mut body_runs = []
   mut small_runs = []
   body_runs = _append_text_run(body_runs, _preview_body(q.len > 0 ? q : "command", w - 260.0), x + 44.0, y + 17.0, q.len > 0 ? U_TEXT : U_DIM)
   def pos_label = matches.len > 0 ? to_str(min(matches.len, pal.index(palette_state) + 1)) + "/" + to_str(matches.len) : "0/0"
   small_runs = _append_right_run(small_runs, font_small, pos_label + "  Enter run  Tab/C-n next  wheel scroll", x + w - 16.0, y + 20.0, U_MUTED)
   def chips = [["file", "f"], ["edit", "e"], ["search", "s"], ["run", "r"], ["pane", "w"], ["term", "t"], ["lsp", "l"]]
   mut ci = 0
   mut chip_x = x + 18.0
   while ci < chips.len {
      def chip = chips.get(ci)
      def label = to_str(chip.get(0, ""))
      def hot = to_str(chip.get(1, ""))
      def bw = 18.0 + float(label.len + hot.len) * FONT_SMALL_ADV
      _fill_rect(chip_x, y + 54.0, bw, 17.0, C_CHIP)
      _stroke_rect(chip_x, y + 54.0, bw, 17.0, C_LINE, 1.0)
      small_runs = _append_text_run(small_runs, hot + " " + label, chip_x + 7.0, y + 57.0, U_DIM)
      chip_x += bw + 6.0
      ci += 1
   }
   mut i = 0
   while i < rows && i < visible_matches.len {
      def row = visible_matches.get(i)
      def actual = start + i
      def yy = y + PALETTE_HEADER_H + float(i) * PALETTE_ROW_H
      def tag = cmd.row_tag(row)
      def active = actual == pal.index(palette_state)
      def col = _command_color(tag)
      if active { _fill_rect(x + 10.0, yy - 4.0, w - 20.0, PALETTE_ROW_H - 4.0, gfx.color_alpha(C_ACCENT, 0.20)) }
      _draw_icon(_command_icon(tag, cmd.row_id(row)), x + 18.0, yy + 4.0, 17.0, active ? C_ACCENT : col)
      body_runs = _append_text_run(body_runs, _preview_body(to_str(row.get(0, "")), w * 0.36), x + 42.0, yy + 4.0, active ? U_TEXT : U_MUTED)
      small_runs = _append_text_run(small_runs, _preview_small(to_str(row.get(3, "")), w * 0.30), x + w * 0.44, yy + 8.0, active ? U_ACCENT_2 : U_DIM)
      small_runs = _queue_key_badge(small_runs, _preview_small(to_str(row.get(2, "")), 120.0), x + w - 146.0, yy + 3.0, active ? C_ACCENT : C_MUTED)
      i += 1
   }
   if matches.len == 0 { body_runs = _append_text_run(body_runs, "No commands", x + 18.0, y + PALETTE_HEADER_H, U_WARN) }
   if matches.len > rows {
      def track_x = x + w - 8.0
      def track_y = y + PALETTE_HEADER_H
      def track_h = float(rows) * PALETTE_ROW_H - 8.0
      def thumb_h = max(22.0, track_h * float(rows) / float(max(rows, matches.len)))
      def thumb_y = track_y + (track_h - thumb_h) * float(start) / float(max(1, matches.len - rows))
      _fill_rect(track_x, track_y, 3.0, track_h, gfx.color_alpha(C_LINE_2, 0.72))
      _fill_rect(track_x, thumb_y, 3.0, thumb_h, gfx.color_alpha(C_ACCENT, 0.82))
   }
   def sel = matches.len > 0 ? matches.get(min(max(pal.index(palette_state), 0), matches.len - 1), []) : []
   if sel.len > 0 {
      def detail_y = y + h - PALETTE_DETAIL_H - 4.0
      _fill_rect(x + 10.0, detail_y, w - 20.0, PALETTE_DETAIL_H - 8.0, C_CHIP)
      _stroke_rect(x + 10.0, detail_y, w - 20.0, PALETTE_DETAIL_H - 8.0, C_LINE, 1.0)
      small_runs = _append_text_run(small_runs, _preview_small(to_str(sel.get(1, "")) + "  " + to_str(sel.get(4, "")), w * 0.28), x + 22.0, detail_y + 9.0, U_ACCENT)
      small_runs = _append_text_run(small_runs, _preview_small(to_str(sel.get(3, "")), w * 0.56), x + w * 0.34, detail_y + 9.0, U_MUTED)
   }
   _flush_rects()
   if body_runs.len > 0 { gfx.draw_text_runs_flat_colors(font_body, body_runs) }
   if small_runs.len > 0 { gfx.draw_text_runs_flat_colors(font_small, small_runs) }
   0
}

fn _completion_color(str kind) int {
   if kind == "keyword" { return C_KEYWORD }
   if kind == "lsp" { return C_CYAN }
   C_ACCENT
}

fn _draw_completion(dict lay, f64 sw, f64 sh) int {
   if !bool(completion_state.get("open", false)) { return 0 }
   def items = completion_state.get("items", [])
   if items.len <= 0 { return 0 }
   def lines = ed.current_lines(st)
   def row = int(st.get("cursor_line", 0))
   def scroll = int(st.get("scroll", 0))
   def line = to_str(lines.get(min(max(row, 0), max(0, lines.len - 1)), ""))
   def visible_row = row - scroll
   def base_x = _line_text_x(lay) + _measure_prefix(line, int(st.get("cursor_col", 0)))
   def base_y = float(lay.get("edit_y", 0.0)) + EDITOR_TEXT_TOP + float(max(0, visible_row)) * ed.LINE_H + ed.LINE_H + 4.0
   def rows = min(items.len, 9)
   def w = min(420.0, max(260.0, sw * 0.42))
   def h = 34.0 + float(rows) * 26.0
   def x = min(max(8.0, base_x), sw - w - 8.0)
   def y = min(max(8.0, base_y), sh - h - 42.0)
   def prefix = to_str(completion_state.get("prefix", ""))
   _fill_rect(x, y, w, h, gfx.color_alpha(C_RAIL, 0.98))
   _stroke_rect(x, y, w, h, C_ACCENT, 1.0)
   _draw_icon("search", x + 10.0, y + 9.0, 16.0, C_ACCENT)
   mut body_runs = []
   mut small_runs = []
   body_runs = _append_text_run(body_runs, prefix.len > 0 ? prefix : "completion", x + 34.0, y + 8.0, U_TEXT)
   small_runs = _append_right_run(small_runs, font_small, "Enter accept  Esc close", x + w - 12.0, y + 11.0, U_MUTED)
   _fill_rect(x + 10.0, y + 30.0, w - 20.0, 1.0, C_LINE)
   mut i = 0
   while i < rows {
      def item = items.get(i, {})
      def active = i == int(completion_state.get("index", 0))
      def yy = y + 40.0 + float(i) * 26.0
      def kind = to_str(item.get("kind", "symbol"))
      def col = _completion_color(kind)
      if active { _fill_rect(x + 8.0, yy - 5.0, w - 16.0, 24.0, gfx.color_alpha(C_ACCENT, 0.18)) }
      _draw_icon(kind == "lsp" ? "net" : (kind == "keyword" ? "codeedit" : "doc"), x + 14.0, yy - 1.0, 15.0, active ? C_ACCENT : col)
      body_runs = _append_text_run(body_runs, widgets.preview(to_str(item.get("label", "")), int(max(10.0, (w - 178.0) / 8.2))), x + 36.0, yy, active ? U_TEXT : U_MUTED)
      small_runs = _append_right_run(small_runs, font_small, widgets.preview(to_str(item.get("detail", "")), 24), x + w - 14.0, yy + 3.0, active ? U_ACCENT_2 : U_DIM)
      i += 1
   }
   _flush_rects()
   if body_runs.len > 0 { gfx.draw_text_runs_flat_colors(font_body, body_runs) }
   if small_runs.len > 0 { gfx.draw_text_runs_flat_colors(font_small, small_runs) }
   0
}

fn _which_key_command_row(str prefix, str head) list {
   def full = str.strip(prefix + " " + head)
   def id = cmd.id_for_chord(full)
   id.len > 0 ? cmd.by_id(id) : []
}

fn _which_key_tag(str prefix, str head) str {
   def row = _which_key_command_row(prefix, head)
   row.len > 0 ? cmd.row_tag(row) : "prefix"
}

fn _which_key_detail(str prefix, str head) str {
   def row = _which_key_command_row(prefix, head)
   if row.len > 0 { return to_str(row.get(3, "")) }
   "continue with " + str.strip(prefix + " " + head)
}

fn _draw_which_key(f64 sw, f64 sh) int {
   if pal.is_open(palette_state) || prompt.rename_is_open(rename_state) { return 0 }
   if !chord.pending(chord_state) {
      def seen = interact.show_key(ui_state)
      if seen.len <= 0 { return 0 }
      def w0 = min(460.0, max(190.0, float(seen.len) * FONT_BODY_ADV + 82.0))
      def x0 = sw - w0 - 18.0
      def y0 = sh - 92.0
      _fill_rect(x0, y0, w0, 50.0, gfx.color_alpha(C_RAIL, 0.95))
      _stroke_rect(x0, y0, w0, 50.0, C_LINE_2, 1.0)
      _draw_icon("keyboard", x0 + 13.0, y0 + 13.0, 20.0, C_ACCENT)
      mut kruns = []
      kruns = _append_text_run(kruns, _preview_body(seen, w0 - 92.0), x0 + 42.0, y0 + 11.0, U_TEXT)
      kruns = _append_right_run(kruns, font_small, "last key", x0 + w0 - 14.0, y0 + 16.0, U_DIM)
      _flush_rects()
      if kruns.len > 0 { gfx.draw_text_runs_flat_colors(font_body, kruns) }
      return 0
   }
   def rows = pal.which_key(chord.describe(chord_state))
   if rows.len <= 0 { return 0 }
   def prefix = chord.describe(chord_state)
   if prefix != which_key_prefix {
      which_key_prefix = prefix
      which_key_scroll = 0
      which_key_scroll_accum = 0.0
   }
   def rect = _which_key_rect(sw, sh, rows.len)
   def x = float(rect.get(0, 0.0))
   def y = float(rect.get(1, 0.0))
   def w = float(rect.get(2, 0.0))
   def h = float(rect.get(3, 0.0))
   def cols = int(rect.get(4, 3))
   def visible = int(rect.get(5, 1))
   which_key_scroll = _clamp_which_key_scroll(rows.len, visible)
   def start = which_key_scroll
   _fill_rect(x, y, w, h, gfx.color_alpha(C_RAIL, 0.96))
   _stroke_rect(x, y, w, h, C_ACCENT, 2.0)
   _draw_icon("keyboard", x + 16.0, y + 16.0, 22.0, C_ACCENT)
   _fill_rect(x + 14.0, y + 52.0, w - 28.0, 1.0, C_LINE)
   mut body_runs = []
   mut small_runs = []
   body_runs = _append_text_run(body_runs, "prefix " + prefix, x + 48.0, y + 15.0, U_TEXT)
   small_runs = _append_text_run(small_runs, "keys, groups, categories, and action detail", x + 48.0, y + 36.0, U_DIM)
   def key_pos = rows.len > 0 ? to_str(start + 1) + "-" + to_str(min(rows.len, start + visible)) + "/" + to_str(rows.len) : "0/0"
   small_runs = _append_right_run(small_runs, font_small, key_pos + "  wheel scroll  Esc/C-g close", x + w - 14.0, y + 20.0, U_MUTED)
   def col_w = (w - 32.0) / float(cols)
   mut i = 0
   while i < visible {
      def row = rows.get(start + i, ["", ""])
      def cx = x + 14.0 + float(i % cols) * col_w
      def cy = y + 66.0 + float(i / cols) * WHICH_KEY_ROW_H
      def head = to_str(row.get(0, ""))
      def tag = _which_key_tag(prefix, head)
      def col = _command_color(tag)
      def key_label = _preview_small(head, min(116.0, col_w * 0.30))
      _draw_icon(_command_icon(tag, ""), cx + 4.0, cy + 8.0, 14.0, col)
      small_runs = _queue_key_badge(small_runs, key_label, cx + 22.0, cy + 2.0, col)
      body_runs = _append_text_run(body_runs, _preview_small(to_str(row.get(1, "")), col_w - 172.0), cx + 108.0, cy + 2.0, U_TEXT)
      small_runs = _append_text_run(small_runs, _preview_small(tag, 62.0), cx + 108.0, cy + 19.0, _pack(col))
      small_runs = _append_text_run(small_runs, _preview_small(_which_key_detail(prefix, head), col_w - 188.0), cx + 160.0, cy + 19.0, U_DIM)
      i += 1
   }
   if rows.len > visible {
      def track_x = x + w - 8.0
      def track_y = y + 66.0
      def track_h = h - 78.0
      def thumb_h = max(20.0, track_h * float(visible) / float(max(visible, rows.len)))
      def thumb_y = track_y + (track_h - thumb_h) * float(start) / float(max(1, rows.len - visible))
      _fill_rect(track_x, track_y, 3.0, track_h, gfx.color_alpha(C_LINE_2, 0.72))
      _fill_rect(track_x, thumb_y, 3.0, thumb_h, gfx.color_alpha(C_ACCENT, 0.78))
   }
   _flush_rects()
   if body_runs.len > 0 { gfx.draw_text_runs_flat_colors(font_body, body_runs) }
   if small_runs.len > 0 { gfx.draw_text_runs_flat_colors(font_small, small_runs) }
   0
}

fn _draw_context_menu(f64 sw, f64 sh) int {
   if !prompt.context_is_open(context_state) { return 0 }
   def w = CONTEXT_W
   def row_h = CONTEXT_ROW_H
   def actions = prompt.context_actions_for(context_state)
   def h = float(actions.len) * row_h
   context_state = prompt.context_clamp(context_state, sw, sh, w, row_h)
   def x = prompt.context_x(context_state)
   def y = prompt.context_y(context_state)
   def mp = window.mouse_pos(win)
   def hover = prompt.context_hover(context_state, float(mp.get(0, -1.0)), float(mp.get(1, -1.0)), w, row_h)
   def hover_idx = int(hover.get("idx", -1))
   _fill_rect(x, y, w, h, gfx.color_alpha(C_RAIL, 0.98))
   _stroke_rect(x, y, w, h, C_LINE, 1.0)
   mut runs = []
   mut i = 0
   while i < actions.len {
      def row = actions.get(i)
      def yy = y + float(i) * row_h
      def id = to_str(row.get(1, ""))
      def key_label = to_str(row.get(2, ""))
      def tag = to_str(row.get(3, ""))
      def detail = to_str(row.get(4, ""))
      def col = _command_color(tag)
      if i == hover_idx { _fill_rect(x + 6.0, yy + 3.0, w - 12.0, row_h - 6.0, gfx.color_alpha(C_ACCENT, 0.18)) }
      elif i > 0 { _fill_rect(x + 8.0, yy, w - 16.0, 1.0, gfx.color_alpha(C_LINE, 0.50)) }
      _draw_icon(_command_icon(tag, id), x + 10.0, yy + 9.0, 15.0, i == hover_idx ? C_ACCENT : col)
      runs = _append_text_run(runs, widgets.preview(to_str(row.get(0, "")), 26), x + 33.0, yy + 7.0, i == hover_idx ? U_TEXT : U_MUTED)
      if key_label.len > 0 { runs = _append_right_run(runs, font_small, key_label, x + w - 10.0, yy + 7.0, i == hover_idx ? U_ACCENT : U_DIM) }
      if detail.len > 0 { runs = _append_text_run(runs, widgets.preview(detail, 42), x + 33.0, yy + 22.0, U_DIM) }
      i += 1
   }
   _flush_rects()
   if runs.len > 0 { gfx.draw_text_runs_flat_colors(font_small, runs) }
   0
}

fn _draw_rename_prompt(f64 sw, f64 sh) int {
   if !prompt.rename_is_open(rename_state) { return 0 }
   def w = min(520.0, sw - 44.0)
   def h = 86.0
   def x = sw * 0.5 - w * 0.5
   def y = max(24.0, sh * 0.18)
   _fill_rect(x, y, w, h, gfx.color_alpha(C_RAIL, 0.98))
   _stroke_rect(x, y, w, h, C_ACCENT, 2.0)
   _text(font_small, "rename " + prompt.rename_rel(rename_state), x + 16.0, y + 14.0, C_MUTED)
   _fill_rect(x + 14.0, y + 42.0, w - 28.0, 30.0, C_PANE)
   _stroke_rect(x + 14.0, y + 42.0, w - 28.0, 30.0, C_LINE, 1.0)
   def text = prompt.rename_text(rename_state)
   _text(font_body, text, x + 24.0, y + 49.0, C_TEXT)
   _fill_rect(x + 25.0 + float(gfx.measure_text_fast(font_body, text).get(0, 0.0)), y + 48.0, 2.0, 18.0, C_ACCENT)
   0
}

fn _modeline_path(str path, str fallback) str {
   if path.len <= 0 { return fallback }
   def root = ospath.normalize(project.root(project_state))
   def p = ospath.normalize(path)
   def prefix = root + ospath.sep()
   if str.startswith(p, prefix) { return str.str_slice(p, prefix.len, p.len) }
   p
}

fn _modeline_safe(str text) str {
   if text.len <= 0 || str.ascii_all_printable(text) { return text }
   mut out = ""
   mut i = 0
   mut spaced = false
   while i < text.len {
      def c = load8(text, i) & 255
      if c >= 32 && c <= 126 {
         out = out + chr(c)
         spaced = false
         i += 1
      } elif c >= 128 {
         def w = str._utf8_seq_len(text, i, text.len)
         if w > 0 {
            out = out + str.str_slice(text, i, i + w)
            spaced = false
            i += w
         } else {
            out = out + "?"
            spaced = false
            i += 1
         }
      } else {
         if !spaced { out = out + " " }
         spaced = true
         i += 1
      }
   }
   out
}

fn _modeline_utf8_clean(str text) bool {
   mut i = 0
   while i < text.len {
      def c = load8(text, i) & 255
      if c < 32 || c == 127 { return false }
      if c < 128 {
         i += 1
      } else {
         def w = str._utf8_seq_len(text, i, text.len)
         if w <= 0 { return false }
         i += w
      }
   }
   true
}

fn _modeline_text(str text) str {
   _modeline_safe(text)
}

fn _modeline_meta(dict b) dict {
   def name = to_str(b.get("name", "buffer"))
   def kind = to_str(b.get("kind", ""))
   def path = _buffer_source_path(b)
   def key = name + "\t" + kind + "\t" + path + "\t" + to_str(bool(b.get("readonly", false)))
   if modeline_cache_key == key { return modeline_cache }
   def run_cmd = b.get("readonly", false) ? [] : runner.command_for(path)
   modeline_cache_key = key
   modeline_cache = {
      "name": name,
      "kind": kind,
      "path": path,
      "display_path": _modeline_path(path, name),
      "run_hint": run_cmd.len > 0 ? to_str(run_cmd.get(0, "")) : "no-runner",
      "language": kind.len > 0 && kind != "code" && kind != "doc" && kind != "file" ? kind : syntax.detect_language(name)
   }
   modeline_cache
}

fn _modeline_selection() str {
   def ranges = _selection_ranges_active()
   if ranges.len <= 0 { return "" }
   if ranges.len > 1 { return "sel x" + to_str(ranges.len) }
   def r = ranges.get(0, [])
   def lines = int(r.get(2, 0)) - int(r.get(0, 0)) + 1
   lines <= 1 ? "sel" : "sel " + to_str(lines) + "L"
}

fn _preview_px(str text, f64 px, f64 adv) str {
   widgets.preview(text, int(max(4.0, px / max(1.0, adv))))
}

fn _preview_small(str text, f64 px) str { _preview_px(text, px, FONT_SMALL_ADV) }

fn _preview_body(str text, f64 px) str { _preview_px(text, px, FONT_BODY_ADV) }

fn _preview_utf8(str text, int limit) str {
   if limit <= 0 { return "" }
   if str.ascii_all_printable(text) {
      if text.len <= limit { return text }
      if limit <= 3 { return str.str_slice(text, 0, min(limit, text.len)) }
      return str.str_slice(text, 0, min(text.len, limit - 3)) + "..."
   }
   if str.utf8_len(text) <= limit { return text }
   if limit <= 3 { return str.utf8_slice(text, 0, limit) }
   str.utf8_slice(text, 0, limit - 3) + "..."
}

fn _preview_modeline(str text, f64 px) str {
   _preview_utf8(text, int(max(4.0, px / max(1.0, FONT_MODELINE_CLIP_ADV))))
}

fn _draw_status(dict lay) int {
   def pad = float(lay.get("chrome_pad", 8.0))
   def y = float(lay.get("status_y", 0.0))
   def h = float(lay.get("status_h", ed.STATUS_H))
   def sw = float(lay.get("edit_x", 0.0)) + float(lay.get("edit_w", 0.0))
   _fill_rect(0.0, y, sw, h, C_PANE_2)
   _stroke_rect(0.0, y, sw, h, C_LINE, 1.0)
   def b = ed.current_buffer(st)
   def meta = _modeline_meta(b)
   def display_path = _modeline_text(to_str(meta.get("display_path", "buffer")))
   def counts = project.counts(project_state)
   def mod_n = int(counts.get("modified", 0))
   def add_n = int(counts.get("added", 0))
   def del_n = int(counts.get("deleted", 0))
   def untracked_n = int(counts.get("untracked", 0))
   def dirty = mod_n + add_n + del_n + untracked_n
   def branch = _modeline_text(project.branch(project_state))
   def run_hint = _modeline_text(to_str(meta.get("run_hint", "no-runner")))
   def language = _modeline_text(to_str(meta.get("language", "text")))
   def mode = b.get("readonly", false) ? "RO" : (b.get("dirty", false) ? "MOD" : "OK")
   def line = int(st.get("cursor_line", 0)) + 1
   def col = int(st.get("cursor_col", 0)) + 1
   def total = max(1, ed.current_lines(st).len)
   def loc = "Ln " + to_str(line) + ":" + to_str(col) + "  " + to_str((line * 100) / total) + "%"
   def page = "buf " + to_str(int(st.get("active", 0)) + 1) + "/" + to_str(st.get("buffers", []).len)
   def git = branch.len > 0 ? branch + (dirty > 0 ? " +" + to_str(dirty) : "") : "no git"
   def diff = "+" + to_str(add_n) + " ~" + to_str(mod_n) + " -" + to_str(del_n) + (untracked_n > 0 ? " ?" + to_str(untracked_n) : "")
   def sel = _modeline_selection()
   def cursors = _extra_cursor_count() > 0 ? "cur " + to_str(_extra_cursor_count() + 1) : ""
   def term = termpane.is_open(term_state) ? "term:" + termpane.mode(term_state) : "term:off"
   def lsp_label = lsp_state.get("active", false) ? "lsp:on" : "lsp:off"
   def ms10 = max(0, int(float(st.get("frame_ms", 0.0)) * 10.0))
   def fps = "FPS " + to_str(int(st.get("fps", 0))) + " " + to_str(ms10 / 10) + "." + to_str(ms10 % 10) + "ms"
   def seen = _modeline_text(interact.show_key(ui_state))
   def key_hint = chord.pending(chord_state) ? ("prefix " + chord.describe(chord_state)) : (seen.len > 0 ? ("key " + seen) : "C-SPC leader")
   def state_icon = mode == "MOD" ? "MOD" : (mode == "RO" ? "RO" : "OK")
   def left = state_icon + "  " + page + "  " + to_str(st.get("focus", "editor")) + "  " + _preview_modeline(display_path, sw * 0.18)
   def mid = key_hint + "  " + git + "  " + diff + "  " + language + "  " + run_hint
   def right_state = fps + "  " + loc + (sel.len > 0 ? "  " + sel : "") + (cursors.len > 0 ? "  " + cursors : "") + "  " + lsp_label + "  " + term
   def live_status = _modeline_text(interact.status(ui_state))
   def right = live_status != "ready" ? fps + "  " + live_status + "  " + loc + (sel.len > 0 ? "  " + sel : "") + (cursors.len > 0 ? "  " + cursors : "") + "  " + lsp_label + "  " + term : right_state
   def content_w = max(1.0, sw - pad * 2.0 - 16.0)
   def right_w = min(max(228.0, content_w * 0.34), max(128.0, content_w - 260.0))
   def left_w = min(max(170.0, content_w * 0.36), max(96.0, content_w - right_w - 160.0))
   def left_x = pad + 8.0
   def right_x = sw - pad - 8.0
   def right_start = right_x - right_w
   def left_icon = mode == "MOD" ? "statuswarning" : (mode == "RO" ? "lock" : "statussuccess")
   def left_text_x = left_x + 20.0
   def mid_icon_x = left_x + left_w + 10.0
   def mid_x = mid_icon_x + 20.0
   def mid_w = max(24.0, right_start - mid_x - 12.0)
   def text_y = _text_center_y_for(font_modeline, y, h)
   _fill_rect(0.0, y, 4.0, h, mode == "MOD" ? C_WARN : (mode == "RO" ? C_CYAN : C_ACCENT))
   mut runs = []
   runs = _append_text_run(runs, _preview_modeline(left, max(24.0, left_w - 20.0)), left_text_x, text_y, mode == "MOD" ? U_WARN : (mode == "RO" ? U_CYAN : U_ACCENT))
   runs = _append_text_run(runs, _preview_modeline(mid, mid_w), mid_x, text_y, chord.pending(chord_state) ? U_ACCENT : (dirty > 0 ? U_ACCENT_2 : U_DIM))
   runs = _append_right_run(runs, font_modeline, _preview_modeline(right, right_w), right_x, text_y, live_status != "ready" ? U_ACCENT : U_MUTED)
   _flush_rects()
   def icon_y = _center_y(y, h, 14.0)
   _draw_icon(left_icon, left_x, icon_y, 14.0, mode == "MOD" ? C_WARN : (mode == "RO" ? C_CYAN : C_ACCENT))
   _draw_icon("vcsbranches", mid_icon_x, icon_y, 14.0, dirty > 0 ? C_ACCENT_2 : C_DIM)
   if runs.len > 0 { gfx.draw_text_runs_flat_colors(font_modeline, runs) }
   0
}

fn _bench_text() str {
   mut out = ""
   mut i = 0
   while i < 420 {
      out = out + "fn bench_" + to_str(i) + "() int { return " + to_str(i) + " + value_" + to_str(i % 17) + " }\n"
      i += 1
   }
   out
}

fn _initial_buffers() list {
   if TERMINAL_DUMP_PROBE { return [ed.buffer("terminal-visual.ny", "", "use std.core\nprint(\"terminal visual probe\")\n")] }
   if BENCH_PROBE || SCROLLBAR_PROBE { return [ed.buffer(BENCH_PROBE ? "bench.ny" : "scrollbar.ny", "", _bench_text())] }
   if ZOOM_PROBE { return [ed.buffer("zoom-test.ny", "", "use std.core\n")] }
   if GOTO_PROBE { return [ed.buffer("goto-test.ny", "", "use std.core\nfn local_target() int { return 1 }\nfn main() int {\n   local_target()\n   project_target()\n}\n")] }
   if MULTICURSOR_PROBE { return [ed.buffer("multi-cursor.ny", "", "aaa\nbbb\nccc")] }
   if FIND_PROBE || PALETTE_PROBE || WHICH_KEY_PROBE { return [ed.buffer("ui-probe.ny", "", PROBE_TEXT)] }
   if ESCAPE_PROBE { return [ed.buffer("escape-test.ny", "", "use std.core\n")] }
   if NAV_PROBE || TAB_PROBE { return [ed.buffer("nav-a.ny", "", "alpha"), ed.buffer("nav-b.ny", "", "beta")] }
   if SELECT_PROBE { return [ed.buffer("select-all.ny", "", "one\ntwo\nthree")] }
   if GIT_PROBE { return [ed.buffer("git-panel.ny", "", "use std.core\n")] }
   if CHORD_PROBE { return [ed.buffer("chord-test.ny", "", "")] }
   if UNDO_PROBE { return [ed.buffer("undo-test.ny", "", "a")] }
   if INPUT_PROBE { return [ed.buffer("input-test.ny", "", PROBE_TEXT)] }
   session.seed_buffers(cli.args())
}

def initial_buffers = _initial_buffers()
st = ed.state(initial_buffers)
st["focus"] = "editor"
st["show_hierarchy"] = true
st["show_titlebar"] = true
st["show_project"] = true
st["show_outline"] = true
st["show_status"] = true
st["show_line_numbers"] = true
st["show_indent_guides"] = false
st["scroll_follow"] = true
st["outline_scroll"] = 0
st["rail_tab"] = "files"
st["project_loaded"] = false
st["git_loaded"] = false
st["drag_terminal"] = false
st["drag_scrollbar"] = false
st["scrollbar_target"] = dict(0)
st["git_scroll"] = 0
st["timeline_scroll"] = 0
st["rail_split"] = 0.0
st["term_h"] = 220.0
_load_ui_cache()
next_cache_save_time = gfx.get_time() + float(CACHE_SAVE_SECONDS)
_nav_record_current()
fps_state = ui_runtime.fps_begin()

if NAV_PROBE {
   _nav_push_current()
   st = ed.select_buffer(st, 1)
   _buffer_changed(false)
   _nav_record_current()
}

_lsp_sync_current(true)

if GOTO_PROBE {
   def root = common.env_trim("NY_EDITOR_PROBE_GOTO_ROOT")
   mut target_path = common.env_trim("NY_EDITOR_PROBE_GOTO_TARGET")
   if target_path.len <= 0 && root.len > 0 { target_path = ospath.join(root, "defs.ny") }
   project_state = project.new(root)
   project_state = project.refresh(project_state, 1, 32)
   st["project_loaded"] = true
   _invalidate_project_definitions()
   st["cursor_line"] = 3
   st["cursor_col"] = 6
   _goto_definition()
   assert(int(st.get("cursor_line", -1)) == 1 && int(st.get("cursor_col", -1)) == 3, "goto local definition")
   st["cursor_line"] = 4
   st["cursor_col"] = 7
   assert(root.len > 0 && file_exists(target_path), "goto project fixture")
   _goto_definition()
   assert(ospath.normalize(_current_path()) == ospath.normalize(target_path), "goto project definition path")
   assert(int(st.get("cursor_line", -1)) == 0, "goto project definition line")
   st = ed.state([ed.buffer("goto-empty.ny", "", "   \n")])
   st["cursor_line"] = 0
   st["cursor_col"] = 0
   _goto_definition()
   assert(int(st.get("cursor_line", -1)) == 0, "goto no symbol stays put")
   window.set_should_close(win, true)
}

if ZOOM_PROBE {
   def start_size = font_body_size
   _run_command("zoom-in")
   assert(font_body_size == min(start_size + 1, FONT_ZOOM_MAX), "editor zoom in")
   assert(term_state.get("font", 0) == font_body, "terminal font follows editor zoom")
   _run_command("zoom-out")
   assert(font_body_size == start_size, "editor zoom out")
   _run_command("zoom-reset")
   assert(font_body_size == FONT_BODY_SIZE, "editor zoom reset")
   window.set_should_close(win, true)
}

if FIND_PROBE {
   _run_command("find")
   find_state = find.set_query(find_state, "tw")
   _find_refresh_now()
   assert(find.is_open(find_state) && find.count(find_state) >= 2, "ctrl-f find opens and matches")
   _find_step(1)
   assert(int(st.get("cursor_line", -1)) >= 10, "find next jumps")
   st = ed.state([ed.buffer("find-replace.ny", "", "one two\ntwo two")])
   hist = history.new()
   find_state = find.state()
   _find_replace_open()
   find_state = find.set_query(find_state, "(tw)(o)")
   find_state = find.toggle_regex(find_state)
   find_state = find.set_replacement(find_state, "\\2\\1")
   _find_refresh_now()
   _find_replace_all()
   assert(_current_text() == "one otw\notw otw", "regex replace all")
   _undo()
   assert(_current_text() == "one two\ntwo two", "replace undo")
   window.set_should_close(win, true)
}

if PALETTE_PROBE {
   _palette_open()
   palette_state = pal.handle_char(palette_state, {"char": 102, "mods": 0})
   palette_state = pal.handle_char(palette_state, {"char": 105, "mods": 0})
   assert(pal.is_open(palette_state) && pal.state_matches(palette_state).len > 0, "palette filters commands")
   window.set_should_close(win, true)
}

if WHICH_KEY_PROBE {
   def top_keys = pal.which_key("C-SPC")
   assert(top_keys.len > 0 && cmd.id_for_chord("C-f") == "find", "which-key and ctrl-f command registry")
   assert(cmd.id_for_chord("C-a") == "mark-whole", "ctrl-a command registry")
   assert(_which_key_tag("C-SPC", "SPC") == "ui", "which-key complete command metadata")
   assert(str.str_contains(_which_key_detail("C-SPC", "f"), "continue"), "which-key prefix detail")
   window.set_should_close(win, true)
}

if SELECT_PROBE {
   st["cursor_line"] = 1
   st["cursor_col"] = 1
   _direct_key_fallback({"key": key.KEY_A, "mods": key.MOD_CONTROL})
   assert(_selection_any_valid() && _selected_text_all() == "one\ntwo\nthree", "ctrl-a selects whole buffer")
   _handle_escape_key({"key": key.KEY_ESCAPE, "mods": 0})
   assert(!_selection_any_valid() && !bool(st.get("quit_prompt", false)), "escape clears select all")
   window.set_should_close(win, true)
}

if MULTICURSOR_PROBE {
   st["cursor_line"] = 0
   st["cursor_col"] = 2
   _direct_key_fallback({"key": key.KEY_DOWN, "mods": key.MOD_ALT | key.MOD_SHIFT})
   _direct_key_fallback({"key": key.KEY_DOWN, "mods": key.MOD_ALT | key.MOD_SHIFT})
   assert(_extra_cursor_count() == 2, "alt-shift-down grows cursor stack")
   _handle_escape_key({"key": key.KEY_ESCAPE, "mods": 0})
   assert(_extra_cursor_count() == 0 && !bool(st.get("quit_prompt", false)), "escape clears extra cursors")
   _direct_key_fallback({"key": key.KEY_DOWN, "mods": key.MOD_ALT | key.MOD_SHIFT})
   assert(_extra_cursor_count() == 1, "alt-shift-down adds cursor")
   _move_cursor_ext("right", false, false)
   assert(int(st.get("cursor_col", 0)) == 3 && _cursor_col(_extra_cursors().get(0, [])) == 3, "real cursor moves extra cursors")
   _move_cursor_ext("left", false, false)
   _edit_insert("!")
   assert(_current_text() == "aa!a\nbb!b\nccc", "multi cursor insert")
   _edit_newline()
   assert(_current_text() == "aa!\na\nbb!\nb\nccc", "multi cursor newline")
   _edit_backspace()
   assert(_current_text() == "aa!a\nbb!b\nccc", "multi cursor backspace merge")
   _undo()
   assert(_current_text() == "aa!\na\nbb!\nb\nccc" && _extra_cursor_count() == 1, "multi cursor undo restores state")
   st = ed.state([ed.buffer("multi-line-cursor.ny", "", "abc\ndef\nghi\njkl")])
   hist = history.new()
   st["cursor_line"] = 0
   st["cursor_col"] = 1
   _add_extra_cursor(1)
   _add_extra_cursor(1)
   assert(_extra_cursor_count() == 2, "repeated add-cursor below walks past existing cursors")
   _edit_insert("X\nY")
   assert(_current_text() == "aX\nYbc\ndX\nYef\ngX\nYhi\njkl", "multi cursor multiline insert")
   _handle_escape_key({"key": key.KEY_ESCAPE, "mods": 0})
   assert(_extra_cursor_count() == 0, "escape returns to one cursor after multiline edit")
   st = ed.state([ed.buffer("multi-select.ny", "", "abc\ndef\nghi")])
   hist = history.new()
   st["cursor_line"] = 0
   st["cursor_col"] = 1
   _add_extra_cursor(1)
   _move_cursor_ext("right", false, true)
   assert(_selection_ranges_active().len == 2 && _selected_text_all() == "b\ne", "shift-selection extends from all cursors")
   _edit_insert("Z")
   assert(_current_text() == "aZc\ndZf\nghi" && _extra_cursor_count() == 1, "typing replaces all cursor selections")
   st = ed.state([ed.buffer("multi-select-line.ny", "", "abc\ndef\nghi")])
   hist = history.new()
   st["cursor_line"] = 0
   st["cursor_col"] = 0
   _add_extra_cursor(1)
   _select_line()
   assert(_selection_ranges_active().len == 2 && _selected_text_all() == "abc\ndef", "select-line selects all cursor lines")
   window.set_should_close(win, true)
}

if TERMINAL_DUMP_PROBE {
   term_state["open"] = true
   term_state["focus"] = true
   term_state["mode"] = "terminal"
   term_state["tabs"] = [{"mode": "terminal", "vt": term_state.get("vt")}]
   term_state["active"] = 0
   term_state = termpane.write_text(term_state, "terminal visual probe\n")
   term_state = termpane.write_text(term_state, "fg should be light, bg should be dark\n")
   term_state = termpane.write_text(term_state, "0123456789 abcdefghijklmnopqrstuvwxyz\n")
   assert(termpane.visible_cell_count(term_state) > 0, "terminal visual text written")
   _set_focus("terminal")
} elif TERMINAL_PROBE {
   if common.env_truthy("NY_UI_HEADLESS") {
      term_state["open"] = true
      term_state["focus"] = true
      term_state["mode"] = "terminal"
      term_state["tabs"] = [{"mode": "terminal", "vt": term_state.get("vt")}]
      term_state["active"] = 0
      term_state = termpane.write_text(term_state, "EDITOR TERMINAL OK\n")
   } else {
      term_state = termpane.open_shell(term_state)
      term_state = termpane.write_text(term_state, "EDITOR TERMINAL OK\n")
      term_state = termpane.send_text(term_state, "printf 'cwd: '; pwd\n")
   }
   _set_focus("terminal")
}

if SYNTAX_PROBE {
   def probe = "fn main() int { return 1 + value_name }"
   assert(_syntax_runs_text(_syntax_runs("probe.ny", probe, 0)) == probe, "syntax highlight preserves full line")
   def old_buffers = st.get("buffers", [])
   def old_active = int(st.get("active", 0))
   st["buffers"] = [ed.buffer("readme.md", "", "```ny\nfn main() int { return 1 }\n```")]
   st["active"] = 0
   def md_runs = _syntax_runs("readme.md", "fn main() int { return 1 }", 1)
   assert(_syntax_runs_text(md_runs) == "fn main() int { return 1 }", "markdown ny fence preserves line")
   assert(md_runs.len > 0 && int(md_runs.get(0, []).get(1, syntax.TOK_TEXT)) == syntax.TOK_KEYWORD, "markdown ny fence uses nytrix syntax")
   st["buffers"] = old_buffers
   st["active"] = old_active
   mut big = []
   mut bi = 0
   while bi < BIG_FILE_LINES + 12 {
      big = big.append("fn big_" + to_str(bi) + "() int { return " + to_str(bi) + " }")
      bi += 1
   }
   _prepare_visible_runs(big, "huge.ny", BIG_FILE_LINES + 4, 4, 10.0, 10.0, 0.0, 180, false, true)
   assert(visible_text_runs_cache.len > 3 && int(visible_text_runs_cache.get(3, U_TEXT)) == U_KEYWORD, "big file visible syntax stays highlighted")
   assert(to_str(colorpicker.scan_hex_literal("bg: #b6a0ff;", 6).get("hex", "")) == "#b6a0ff", "hex color literal scanner")
   assert(to_str(colorpicker.scan_hex_literal("fg: #acf;", 6).get("hex", "")) == "#aaccff", "short hex color literal scanner")
   window.set_should_close(win, true)
}

if MODELINE_PROBE {
   def safe = _modeline_safe("ok \tbox " + chr(0x03BB) + chr(0x2192) + chr(0x2713))
   assert(_modeline_utf8_clean(safe) && str.utf8_valid(safe), "modeline preserves valid utf8 and strips controls")
   assert(str.utf8_valid(_preview_modeline(safe, 36.0)), "modeline clips on utf8 boundaries")
   window.set_should_close(win, true)
}

if TERMINAL_PROBE && common.env_truthy("NY_UI_HEADLESS") {
   assert(termpane.is_open(term_state), "terminal pane open")
   window.set_should_close(win, true)
}

while !gfx.window_should_close(win) {
   def t_frame0 = FRAME_TRACE ? ticks() : 0
   def dt = gfx.get_delta_time()
   chord_state = chord.tick(chord_state, dt)
   ui_state = interact.tick(ui_state, dt)
   if termpane.is_open(term_state) { term_state = termpane.update(term_state) }
   fps_state = ui_runtime.fps_tick(fps_state, dt)
   def fps = ui_runtime.fps_current(fps_state, dt)
   if int(st.get("fps", -1)) != fps { st["fps"] = fps }
   st["frame_ms"] = dt * 1000.0
   if float(st.get("status_timer", 0.0)) > 0.0 { st["status_timer"] = max(0.0, float(st.get("status_timer", 0.0)) - dt) }
   def fb = gfx.framebuffer_size_f64(START_W, START_H)
   def sw = max(1.0, float(fb.get(0, START_W)))
   def sh = max(1.0, float(fb.get(1, START_H)))
   def rail_w = bool(st.get("show_hierarchy", true)) ? float(st.get("rail_w", 250.0)) : 0.0
   def top_h = bool(st.get("show_titlebar", true)) ? TOP_H : 0.0
   mut lay = ed.editor_layout(sw, sh, rail_w, top_h)
   lay = _layout_with_terminal(lay, sh)
   def t_update = FRAME_TRACE ? ticks() : 0
   if TERMINAL_DUMP_PROBE && frame_count == 0 {
      assert(termpane.visible_cell_count(term_state) > 0, "terminal visual text survived layout")
      print("[terminal:dump]\n" + termpane.debug_text(term_state, 3, 100))
   }
   if INPUT_PROBE && !TERMINAL_PROBE && frame_count == 0 {
      window.push_event(win, window.EVENT_KEY_PRESSED, {"key": key.KEY_RIGHT, "mods": 0})
      window.push_event(win, window.EVENT_KEY_PRESSED, {"key": key.KEY_DOWN, "mods": 0})
      window.push_event(win, window.EVENT_MOUSE_SCROLL, {"dy": -2.0, "x": float(lay.get("edit_x", 0.0)) + 40.0, "y": float(lay.get("edit_y", 0.0)) + 40.0})
   }
   if CONTEXT_PROBE && frame_count == 0 {
      window.push_event(win, window.EVENT_MOUSE_BUTTON_PRESSED, {"button": 1, "x": float(lay.get("edit_x", 0.0)) + 80.0, "y": float(lay.get("edit_y", 0.0)) + 42.0})
   }
   if NAV_PROBE && frame_count == 0 && nav_probe_phase == 0 {
      window.push_event(win, window.EVENT_MOUSE_BUTTON_PRESSED, {"button": MOUSE_BACK, "x": float(lay.get("edit_x", 0.0)) + 30.0, "y": float(lay.get("edit_y", 0.0)) + 30.0})
   }
   if TAB_PROBE && frame_count == 0 {
      def tab_w_probe = _top_tab_w(sw, st.get("buffers", []).len)
      window.push_event(win, window.EVENT_MOUSE_BUTTON_PRESSED, {"button": 0, "x": 6.0 + tab_w_probe + 18.0, "y": 10.0})
   }
   if TAB_PROBE && frame_count == 1 {
      window.push_event(win, window.EVENT_MOUSE_BUTTON_PRESSED, {"button": 0, "x": 18.0, "y": 10.0})
   }
   if GIT_PROBE && frame_count == 0 {
      def tab_w_git = rail_w / 3.0
      def rail_x_probe = float(lay.get("rail_x", 0.0))
      def rail_y_probe = float(lay.get("rail_y", 0.0))
      window.push_event(win, window.EVENT_MOUSE_BUTTON_PRESSED, {"button": 0, "x": rail_x_probe + tab_w_git * 2.0 + 12.0, "y": rail_y_probe + 10.0})
   }
   if UNDO_PROBE && frame_count == 0 {
      window.push_event(win, window.EVENT_KEY_CHAR, {"char": 98, "mods": 0})
      window.push_event(win, window.EVENT_KEY_PRESSED, {"key": key.KEY_Z, "mods": key.MOD_CONTROL})
   }
   if ESCAPE_PROBE && frame_count == 0 {
      window.push_event(win, window.EVENT_KEY_PRESSED, {"key": key.KEY_ESCAPE, "mods": 0})
   }
   if ESCAPE_PROBE && frame_count == 1 {
      window.push_event(win, window.EVENT_KEY_PRESSED, {"key": key.KEY_ESCAPE, "mods": 0})
   }
   if CHORD_PROBE && frame_count == 0 {
      window.push_event(win, window.EVENT_KEY_PRESSED, {"key": key.KEY_SPACE, "mods": key.MOD_CONTROL})
      window.push_event(win, window.EVENT_KEY_CHAR, {"char": 32, "mods": key.MOD_CONTROL})
      window.push_event(win, window.EVENT_KEY_PRESSED, {"key": key.KEY_R, "mods": 0})
      window.push_event(win, window.EVENT_KEY_CHAR, {"char": 114, "mods": 0})
      window.push_event(win, window.EVENT_KEY_PRESSED, {"key": key.KEY_R, "mods": 0})
      window.push_event(win, window.EVENT_KEY_CHAR, {"char": 114, "mods": 0})
      window.push_event(win, window.EVENT_KEY_PRESSED, {"key": key.KEY_C, "mods": key.MOD_CONTROL})
      window.push_event(win, window.EVENT_KEY_PRESSED, {"key": key.KEY_R, "mods": 0})
      window.push_event(win, window.EVENT_KEY_CHAR, {"char": 114, "mods": 0})
      window.push_event(win, window.EVENT_KEY_PRESSED, {"key": key.KEY_R, "mods": 0})
      window.push_event(win, window.EVENT_KEY_CHAR, {"char": 114, "mods": 0})
   }
   if SCROLLBAR_PROBE && frame_count == 0 {
      def sx = float(lay.get("edit_x", 0.0)) + float(lay.get("edit_w", 0.0)) - 8.0
      def sy1 = float(lay.get("edit_y", 0.0)) + float(lay.get("edit_h", 0.0)) - 18.0
      window.push_event(win, window.EVENT_MOUSE_BUTTON_PRESSED, {"button": 0, "x": sx, "y": sy1})
      window.push_event(win, window.EVENT_MOUSE_POS_CHANGED, {"x": sx, "y": sy1})
      window.push_event(win, window.EVENT_MOUSE_BUTTON_RELEASED, {"button": 0, "x": sx, "y": sy1})
   }
   _process_events(lay)
   def t_events = FRAME_TRACE ? ticks() : 0
   if INPUT_PROBE && !TERMINAL_PROBE && frame_count == 0 {
      if INPUT_TRACE {
         print("[editor:probe] line=" + to_str(int(st.get("cursor_line", -1))) + " col=" + to_str(int(st.get("cursor_col", -1))) + " scroll=" + to_str(int(st.get("scroll", -1))) + " follow=" + to_str(bool(st.get("scroll_follow", true))))
      }
      assert(int(st.get("cursor_line", -1)) == 1 && int(st.get("cursor_col", -1)) == 1 && int(st.get("scroll", 0)) > 0 && !bool(st.get("scroll_follow", true)), "editor input movement")
      window.set_should_close(win, true)
   }
   if CONTEXT_PROBE && frame_count == 0 {
      assert(prompt.context_is_open(context_state), "editor right-click context")
      assert(to_str(prompt.context_entry(context_state).get("scope", "")) == "editor", "editor context scope")
      window.set_should_close(win, true)
   }
   if NAV_PROBE {
      if nav_probe_phase == 0 {
         assert(int(st.get("active", -1)) == 0, "mouse back navigation")
         nav_probe_phase = 1
         window.push_event(win, window.EVENT_MOUSE_BUTTON_PRESSED, {"button": MOUSE_FORWARD, "x": float(lay.get("edit_x", 0.0)) + 30.0, "y": float(lay.get("edit_y", 0.0)) + 30.0})
      } elif nav_probe_phase == 1 {
         assert(int(st.get("active", -1)) == 1, "mouse forward navigation")
         window.set_should_close(win, true)
      }
   }
   if TAB_PROBE && frame_count == 0 {
      assert(int(st.get("active", -1)) == 1, "top tab selects second buffer")
      assert(force_full_redraw > 0, "top tab switch requests redraw")
   }
   if TAB_PROBE && frame_count == 1 {
      assert(int(st.get("active", -1)) == 0, "top tab selects first buffer")
      window.set_should_close(win, true)
   }
   if GIT_PROBE && frame_count == 0 {
      assert(_rail_tab() == "git", "git rail tab selected")
      assert(bool(st.get("git_loaded", false)), "git tab status loaded")
      assert(force_full_redraw > 0, "git tab requests redraw")
      window.set_should_close(win, true)
   }
   if UNDO_PROBE && frame_count == 0 {
      assert(_current_text() == "a", "ctrl-z undo")
      window.set_should_close(win, true)
   }
   if ESCAPE_PROBE && frame_count == 0 {
      assert(bool(st.get("quit_prompt", false)) && !window.should_close(win), "first esc prompts")
   }
   if ESCAPE_PROBE && frame_count == 1 {
      assert(window.should_close(win), "esc esc closes editor")
   }
   if CHORD_PROBE && frame_count == 0 {
      assert(_current_text() == "", "command chord chars do not insert")
      window.set_should_close(win, true)
   }
   if SCROLLBAR_PROBE && frame_count == 0 {
      assert(int(st.get("scroll", 0)) > 0 && !bool(st.get("scroll_follow", true)), "editor scrollbar drag scrolls")
      window.set_should_close(win, true)
   }
   if TERMINAL_PROBE && frame_count >= 18 { window.set_should_close(win, true) }
   if TERMINAL_DUMP_PROBE && frame_count >= max(12, auto_dump_delay + 3) { window.set_should_close(win, true) }
   def mp = window.mouse_pos(win)
   def mpx = float(mp.get(0, 0.0))
   def mpy = float(mp.get(1, 0.0))
   if _dock_resize_hit(lay, mpx, mpy) || st.get("drag_terminal", false) {
      _set_cursor_kind("resize-ns")
   } elif ed.divider_hit(lay, mpx, mpy) || st.get("drag_divider", false) {
      _set_cursor_kind("resize-ew")
   } else {
      _set_cursor_kind("text")
   }
   frame_events_seen = frame_events_seen || st.get("drag_terminal", false) || st.get("drag_divider", false) || st.get("drag_select", false) || st.get("drag_scrollbar", false)
   reuse_state = ui_reuse.note_events_seen(reuse_state, frame_events_seen)
   def reuse_opts = _editor_reuse_opts(sw, sh)
   if ui_reuse.try_present(reuse_state, reuse_opts) {
      reuse_present_count += 1
      if BENCH_PROBE {
         frame_count += 1
         if frame_count >= BENCH_FRAME_LIMIT { window.set_should_close(win, true) }
      } else {
         frame_count += 1
      }
      ui_runtime.close_on_timeout(win, int(fps_state.get("start", 0)), timeout_limit)
      continue
   }
   if !gfx.begin_frame_clear(C_BG) {
      begin_fail_count += 1
      if BENCH_PROBE {
         frame_count += 1
         if frame_count >= BENCH_FRAME_LIMIT { window.set_should_close(win, true) }
      } else {
         _status("renderer begin failed on " + gfx.get_active_backend_name() + "; retrying")
         if begin_fail_count >= BEGIN_FAIL_LIMIT {
            _status("renderer begin failed repeatedly; closing safely")
            window.set_should_close(win, true)
         }
      }
      ui_runtime.close_on_timeout(win, int(fps_state.get("start", 0)), timeout_limit)
      continue
   }
   def t_begin = FRAME_TRACE ? ticks() : 0
   begin_fail_count = 0
   gfx.set_ortho_2d(0.0, sw, 0.0, sh)
   def caret = int(gfx.get_time() / CARET_BLINK_SEC) % 2 == 0
   if bool(st.get("show_titlebar", true)) { _draw_top(lay, fps) }
   def t_top = FRAME_TRACE ? ticks() : 0
   if bool(st.get("show_hierarchy", true)) { _draw_rail(lay) }
   def t_rail = FRAME_TRACE ? ticks() : 0
   _draw_editor(lay, caret)
   def t_editor = FRAME_TRACE ? ticks() : 0
   _draw_terminal(lay)
   def t_terminal = FRAME_TRACE ? ticks() : 0
   if bool(st.get("show_status", true)) { _draw_status(lay) }
   def t_status = FRAME_TRACE ? ticks() : 0
   if st.get("quit_prompt", false) {
      def pw = 240.0
      def ph = 72.0
      def px = sw * 0.5 - pw * 0.5
      def py = sh * 0.5 - ph * 0.5
      _fill_rect(px, py, pw, ph, gfx.color_alpha(C_RAIL, 0.96))
      _stroke_rect(px, py, pw, ph, C_ACCENT, 2.0)
      _text_center(font_body, "Quit? Esc again", px + pw * 0.5, py + 26.0, C_TEXT)
   }
   _draw_context_menu(sw, sh)
   _draw_rename_prompt(sw, sh)
   _draw_completion(lay, sw, sh)
   _draw_color_picker(lay, sw, sh)
   _draw_find_bar(sw, sh)
   _draw_palette(sw, sh)
   _draw_which_key(sw, sh)
   def t_draw = FRAME_TRACE ? ticks() : 0
   _trace_draw(frame_count, t_begin, t_top, t_rail, t_editor, t_terminal, t_status, t_draw)
   _flush_rects()
   def t_flush = FRAME_TRACE ? ticks() : 0
   ui_dump.auto_dump_pre_frame(auto_dump_done, frame_count, auto_dump_delay)
   gfx.end_frame()
   def t_end = FRAME_TRACE ? ticks() : 0
   reuse_state = ui_reuse.note_full_draw(reuse_state, reuse_opts)
   if force_full_redraw > 0 { force_full_redraw -= 1 }
   _trace_frame(frame_count, t_frame0, t_update, t_events, t_begin, t_draw, t_flush, t_end)
   frame_count += 1
   if BENCH_PROBE && BENCH_WARMUP_FRAMES > 0 && frame_count == BENCH_WARMUP_FRAMES {
      fps_state = ui_runtime.fps_begin()
   }
   if BENCH_PROBE && frame_count >= BENCH_FRAME_LIMIT { window.set_should_close(win, true) }
   if !BENCH_PROBE && gfx.get_time() >= next_cache_save_time {
      _save_ui_cache()
      next_cache_save_time = gfx.get_time() + float(CACHE_SAVE_SECONDS)
   }
   auto_dump_done = ui_dump.auto_dump_post_frame(win, auto_dump_done, frame_count, auto_dump_delay, "build/cache/fb/ui/editor.png")
   ui_runtime.close_on_timeout(win, int(fps_state.get("start", 0)), timeout_limit)
}

_bench_finish()
ui_runtime.fps_finish("editor", fps_state)
_save_ui_cache()
term_state = termpane.close(term_state)
_release_image_preview_cache()
gfx.close_window()
