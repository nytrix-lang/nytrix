#!/usr/bin/env ny

;; Keywords: ui editor text panes syntax highlight example
;; Full editor testbed: buffers, highlighting, pane drag, mouse editing, clipboard, save, and drop-open.
use core
use core.common as common
use core.str as str
use core.math (max, min, ceil)
use core.math.crypto.encoding.base as b64
use core.os (exit, file_exists, file_read, file_write, ticks, msleep, getcwd)
use core.os.args as cli
use core.os.subprocess (run_capture)
use core.os.fs as osfs
use core.os.path as ospath
use core.os.ui.window.consts as key
use core.os.ui.render as gfx
use core.os.ui.render.shared (color_u32 as _pack, color_component as _color_at)
use core.os.ui.render.diag as ui_diag
use core.os.ui.render.viewer.trace as ui_trace
use core.os.ui.render.dump as ui_dump
use core.os.ui.render.reuse as ui_reuse
use core.os.ui.render.viewer.batch as ui_batch
use core.os.ui.render.viewer.editor as ed
use core.os.ui.render.viewer.editor.commands as cmd
use core.os.ui.render.viewer.editor.colorpicker as colorpicker
use core.os.ui.render.viewer.editor.diff as eddiff
use core.os.ui.render.viewer.editor.find as find
use core.os.ui.render.viewer.editor.history as history
use core.os.ui.render.viewer.editor.interaction as interact
use core.os.ui.render.viewer.editor.keychord as chord
use core.os.ui.render.viewer.editor.lsp as lsp
use core.os.ui.render.viewer.editor.dap as dap
use core.os.ui.render.viewer.editor.outline as outline
use core.os.ui.render.viewer.editor.palette as pal
use core.os.ui.render.viewer.editor.project as project
use core.os.ui.render.viewer.editor.prompt as prompt
use core.os.ui.render.viewer.editor.runner as runner
use core.os.ui.render.viewer.editor.session as session
use core.os.ui.render.viewer.editor.terminal as termpane
use core.os.ui.render.viewer.editor.tools as tools
use core.os.ui.render.viewer.icons as icons
use core.os.ui.render.viewer.gui as gui
use core.os.ui.render.viewer.runtime as ui_runtime
use core.os.ui.render.viewer.widgets
use core.os.ui.window
use core.math.parse.img.svg as svg_img
use core.math.parse.syntax

fn _editor_color(str code, str text) str {
   if common.env_truthy("NO_COLOR") { return text }
   def esc = chr(27)
   esc + "[" + code + "m" + text + esc + "[0m"
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
   print("  ./make ny etc/labs/editor.ny " + _editor_color("36", "[options]") + " " + _editor_color("90", "[file-or-folder]") )
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
   _editor_help_line("NY_EDITOR_PROJECT_ROOT=path", "root project tree and fallback README.md")
   _editor_help_line("NY_EDITOR_INPUT_TRACE=1", "editor input trace")
   _editor_help_line("NY_EDITOR_OMNI=1", "print compact WM/renderer/editor state for early frames")
   _editor_help_line("NY_EDITOR_FRAME_TRACE=1", "print per-frame update/draw/present timings")
   _editor_help_line("NY_EDITOR_FRAME_HASH=1", "print framebuffer hash for render lock tests")
   _editor_help_line("NY_EDITOR_FAST_PRESENT=1", "use immediate present instead of the default stable vsync")
   _editor_help_line("NY_UI_EDITOR_IDLE_REUSE=1", "enable present-only idle reuse for bench/debug")
   _editor_help_line("NY_UI_HEADLESS=1", "headless/mock mode")
   _editor_help_line("NY_VK_SAFE_TEXT=1", "force slow Vulkan text fallback for driver debugging")
   _editor_help_line("NY_VK_FAST_TEXT=0", "disable Vulkan atlas fast text")
   _editor_help_line("NY_VK_TEXTURE_TEXT=1", "debug Vulkan texture-atlas TTF path")
   print("")
   print(_editor_color("1;33", "Examples"))
   print("  ./make ny etc/labs/editor.ny -v -gl src/main.ny")
   print("  ./make ny etc/labs/editor.ny -vv -vk")
   0
}

def int START_W = common.env_int_clamped("NY_EDITOR_START_W", 1220, 240, 8192)
def int START_H = common.env_int_clamped("NY_EDITOR_START_H", 760, 180, 8192)
def START_FLAGS = key.WINDOW_CENTER | key.WINDOW_FOCUS_ON_SHOW | key.WINDOW_ALLOW_DND
def EDITOR_PRESENT_MODE = common.env_truthy("NY_EDITOR_FAST_PRESENT") ? "immediate" : "fifo"
def UI_FONT_CANDIDATES = borrow([
   "etc/assets/fonts/jetbrains.ttf",
   "etc/assets/fonts/maplemono.ttf",
   "/usr/share/fonts/TTF/JetBrainsMonoNerdFontMono-Regular.ttf",
   "/usr/share/fonts/TTF/JetBrainsMonoNLNerdFontMono-Regular.ttf",
   "/usr/share/fonts/OTF/FiraMonoNerdFontMono-Regular.otf",
   "/usr/share/fonts/TTF/MesloLGSNerdFontMono-Regular.ttf",
   "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
   "etc/assets/fonts/monocraft.ttf",
])

def TITLE_FONT_CANDIDATES = borrow([
   "etc/assets/fonts/jetbrains.ttf",
   "etc/assets/fonts/maplemono.ttf",
   "/usr/share/fonts/TTF/JetBrainsMonoNerdFontMono-Regular.ttf",
   "etc/assets/fonts/monocraft.ttf",
])

def MODELINE_FONT_CANDIDATES = borrow([
   "etc/assets/fonts/jetbrains.ttf",
   "/usr/share/fonts/TTF/JetBrainsMonoNerdFontMono-Regular.ttf",
   "/usr/share/fonts/TTF/JetBrainsMonoNLNerdFontMono-Regular.ttf",
   "/usr/share/fonts/TTF/MesloLGSNerdFontMono-Regular.ttf",
   "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
   "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
])

fn _env_float_between(str name, f64 fallback, f64 lo, f64 hi) f64 {
   def raw = common.env_trim(name)
   if raw.len == 0 { return fallback }
   min(max(str.atof(raw), lo), hi)
}

def UI_DENSITY = _env_float_between("NY_EDITOR_DENSITY", 0.82, 0.54, 1.25)
def int FONT_TITLE_SIZE = common.env_int_clamped("NY_EDITOR_TITLE_FONT_SIZE", 24, 8, 96)
def int FONT_BODY_SIZE = common.env_int_clamped("NY_EDITOR_FONT_SIZE", 18, 8, 96)
def int FONT_SMALL_SIZE = common.env_int_clamped("NY_EDITOR_SMALL_FONT_SIZE", 15, 8, 96)
def int FONT_MODELINE_SIZE = common.env_int_clamped("NY_EDITOR_MODELINE_FONT_SIZE", 13, 8, 96)
def TOP_H = max(21.0, 30.0 * UI_DENSITY)
def RAIL_MIN_W = 38.0
def EDIT_MIN_W = 56.0
def DOCK_MIN_H = 34.0
def EDIT_MIN_H = 28.0
def PANE_MAX = 4096.0

if cli.flag("-h") || cli.flag("--help") || cli.flag("help") { _editor_print_help() exit(0) }
ui_dump.apply_verbose_argv()

if common.env_truthy("NY_UI_HEADLESS") { gfx.set_backend_type(gfx.BACKEND_MOCK) }
else {
   gfx.apply_backend_env()
   gfx.apply_backend_argv()
}

def int RENDER_BACKEND_MOCK = int(gfx.BACKEND_MOCK)
struct EditorState { dict data }
def EditorState editor_state = EditorState(borrow(dict(128)))
;; Mutable borrowed view: keeps writes visible to the record owner across calls.
mut dict _editor_values = borrow(editor_state.data)
fn _editor_get(str name) any { borrow(_editor_values.get(name, nil)) }
_editor_values["win"] = gfx.init_window(START_W, START_H, "Nytrix Editor", START_FLAGS, EDITOR_PRESENT_MODE, false, 4)

if !_editor_get("win") { panic("window init failed") }
window.set_exit_key(_editor_get("win"), key.KEY_NULL)
if is_dict(_editor_get("win")) && !!_editor_get("win").get("_mock", false) && !common.env_truthy("NYTRIX_UI_REPLAY") {
   if gfx.begin_frame_clear([0.015, 0.015, 0.018, 1.0]) { gfx.end_frame() }
   gfx.close_window()
   exit(0)
}

if gfx.get_active_backend_name() == "opengl" {
   if gfx.begin_frame_clear([0.015, 0.015, 0.018, 1.0]) {
      gfx.set_ortho_2d(0.0, float(START_W), 0.0, float(START_H))
      gfx.draw_rect_fast(0.0, 0.0, float(START_W), 2.0, 0xffd6c8ff)
      gfx.end_frame()
   }
}

def FONT_PRIME = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_/#[]{}().,:;+-=<>!?$%@&|\\\"'`~*" + chr(0x2022) + chr(0x2026) + chr(0x2190) + chr(0x2191) + chr(0x2192) + chr(0x2193) + chr(0x2713) + chr(0x2717) + chr(0x03BB)

fn _load_editor_font(list candidates, int size) any {
   def f = gfx.font_load_first(candidates, size, gfx.FONT_FILTER_LINEAR)
   gfx.font_allow_color_fallback(f, true)
   gfx.font_prepare(f, FONT_PRIME)
   f
}

_editor_values["font_title_size"] = FONT_TITLE_SIZE
_editor_values["font_body_size"] = FONT_BODY_SIZE
_editor_values["font_small_size"] = FONT_SMALL_SIZE
_editor_values["font_modeline_size"] = FONT_MODELINE_SIZE
_editor_values["font_title"] = _load_editor_font(TITLE_FONT_CANDIDATES, int(_editor_get("font_title_size")))
_editor_values["font_body"] = _load_editor_font(UI_FONT_CANDIDATES, int(_editor_get("font_body_size")))
_editor_values["font_small"] = _load_editor_font(UI_FONT_CANDIDATES, int(_editor_get("font_small_size")))
_editor_values["font_modeline"] = _load_editor_font(MODELINE_FONT_CANDIDATES, int(_editor_get("font_modeline_size")))
def cursor_text = window.create_standard_cursor(window.IBEAM_CURSOR)
def cursor_resize = window.create_standard_cursor(window.RESIZE_EW_CURSOR)
def cursor_resize_ns = window.create_standard_cursor(window.RESIZE_NS_CURSOR)
def body_advance = _measure_ui(int(_editor_get("font_body")), "0", 0.0)
def small_advance = _measure_ui(int(_editor_get("font_small")), "0", 0.0)
def modeline_advance = _measure_ui(int(_editor_get("font_modeline")), "0", 0.0)
_editor_values["FONT_BODY_ADV"] = body_advance >= 2.0 && body_advance <= float(int(_editor_get("font_body_size"))) * 2.0 ? body_advance : float(int(_editor_get("font_body_size"))) * 0.6
_editor_values["FONT_SMALL_ADV"] = small_advance >= 2.0 && small_advance <= float(int(_editor_get("font_small_size"))) * 2.0 ? small_advance : float(int(_editor_get("font_small_size"))) * 0.6
_editor_values["FONT_MODELINE_ADV"] = modeline_advance >= 2.0 && modeline_advance <= float(int(_editor_get("font_modeline_size"))) * 2.0 ? modeline_advance : float(int(_editor_get("font_modeline_size"))) * 0.6
;; The modeline font is monospace and its advance is measured, so clipping
;; needs only a small margin. A 35% margin cut text a third too early.
_editor_values["FONT_MODELINE_CLIP_ADV"] = max(4.0, float(_editor_get("FONT_MODELINE_ADV")) * 1.06)
def SECTION_H = project.TREE_HEADER_H
def EDITOR_TEXT_TOP = 6.0
def INPUT_TRACE = common.env_truthy("NY_EDITOR_INPUT_TRACE")
def SHOW_LAST_KEY = common.env_truthy("NY_EDITOR_SHOW_LAST_KEY")
def int BIG_FILE_LINES = 5000
def int HIGHLIGHT_MAX_LINE_BYTES = 900
def OUTLINE_MAX_LINES = 5000
def int LINE_DRAW_EXTRA_COLS = 96
def RAIL_TABS_H = 28.0
def CONTEXT_W = 330.0
def CONTEXT_ROW_H = 24.0
def FIND_BAR_H = 48.0
def f64 PALETTE_HEADER_H = 44.0
def f64 PALETTE_ROW_H = 26.0
def f64 PALETTE_DETAIL_H = 44.0
def int PALETTE_MAX_ROWS = 12
def f64 WHICH_KEY_ROW_H = 22.0
def int WHICH_KEY_MAX_ROWS = 14
def TOP_TAB_MIN_W = 96.0
def TOP_TAB_MAX_W = 208.0
def MOUSE_BACK = 3
def MOUSE_FORWARD = 4
def NAV_LIMIT = 96
def EDITOR_CACHE_PATH = "build/cache/editor_state.cfg"
def TERM_TAB_H = 23.0
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
;; Syntax palette: one hue per role, all readable on the black canvas.
def C_KEYWORD = gfx.color_hex("#c099ff")
def C_TYPE = gfx.color_hex("#4fd6be")
def C_STRING = gfx.color_hex("#c3e88d")
def C_NUMBER = gfx.color_hex("#ff966c")
def C_COMMENT = gfx.color_hex("#6e76a6")
def C_FUNC = gfx.color_hex("#82aaff")
def C_OPERATOR = gfx.color_hex("#89ddff")
def C_PUNCT = gfx.color_hex("#8790b8")
def C_PREPROC = gfx.color_hex("#fca7ea")
def C_PROPERTY = gfx.color_hex("#ffc777")
def C_PARAM = gfx.color_hex("#ff98a4")
def C_VAR = gfx.color_hex("#d6ddf8")
def C_WARN = gfx.color_hex("#ff9580")
def C_SELECT = gfx.color_hex("#b6a0ff")

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

fn _text_center(int font, any text, f64 x, f64 y, any color) int {
   _flush_rects()
   widgets.text_center(font, text, x, y, color)
   0
}

fn _font_visual_h(int font, f64 fallback) f64 {
   mut h = float(gfx.font_line_height(font))
   if h < 8.0 { h = fallback }
   ;; Use the actual rasterized font line box.  The old helper clamped the
   ;; measured height back down to the row fallback, which made baselines and
   ;; icon/text limits pretend oversized glyphs were shorter than they really
   ;; were.
   min(max(8.0, h), 96.0)
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

fn _snap_px(f64 v) f64 {
   float(int(v + 0.5))
}

fn _text_center_y_for(int font, f64 y, f64 h) f64 {
   _center_y(y, h, _font_visual_h(font, h))
}

fn _row_text_y(int font, f64 row_y, f64 row_h) f64 {
   _text_center_y_for(font, row_y, row_h)
}

fn _font_ascent_px(int font, f64 fallback) f64 {
   mut a = float(gfx.font_ascent(font))
   if a <= 0.0 { a = fallback * 0.78 }
   a
}

fn _row_baseline_y(int ref_font, f64 row_y, f64 row_h) f64 {
   def text_h = _font_visual_h(ref_font, row_h)
   _snap_px(row_y + float(max(0.0, row_h - text_h)) * 0.5 + float(_font_ascent_px(ref_font, text_h)))
}

fn _row_text_y_from_baseline(int font, f64 baseline_y, f64 row_h) f64 {
   _snap_px(baseline_y - _font_ascent_px(font, _font_visual_h(font, row_h)))
}

fn _editor_row_baseline_y(f64 row_y) f64 {
   _row_baseline_y(int(_editor_get("font_body")), row_y, ed.LINE_H)
}

fn _editor_row_y(f64 panel_y, int row) f64 {
   _snap_px(panel_y + EDITOR_TEXT_TOP + float(row) * float(ed.LINE_H))
}

fn _draw_tex_rect(int tex, f64 x, f64 y, f64 w, f64 h, any tint=gfx.WHITE) int {
   if tex <= 0 || w <= 0.0 || h <= 0.0 { return 0 }
   def sz = gfx.texture_size(tex)
   if !is_list(sz) || int(sz.get(0, 0)) <= 0 || int(sz.get(1, 0)) <= 0 { return 0 }
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
def U_WARN = _pack(C_WARN)
def U_KEYWORD = _pack(C_KEYWORD)
def TERM_BG = 0xff020101
def TERM_FG = 0xfff6f5f5
_editor_values["st"] = dict(0)
_editor_values["hist"] = borrow(history.new())
_editor_values["find_state"] = borrow(find.state())
_editor_values["find_content_rev"] = -1
_editor_values["palette_state"] = borrow(pal.state())
_editor_values["chord_state"] = borrow(chord.empty_state())
_editor_values["command_cfg"] = borrow(cmd.config())
_editor_values["lsp_state"] = borrow(lsp.new())
_editor_values["lsp_popup"] = dict(0)
_editor_values["dap_state"] = borrow(dap.new())
_editor_values["dap_panel"] = true
_editor_values["lsp_dirty_at"] = 0
_editor_values["semantic_path"] = ""
_editor_values["semantic_requested_path"] = ""
_editor_values["semantic_lines"] = dict(0)
_editor_values["inlay_path"] = ""
_editor_values["inlay_requested_path"] = ""
_editor_values["inlay_lines"] = dict(0)
_editor_values["code_lens_path"] = ""
_editor_values["code_lens_requested_path"] = ""
_editor_values["code_lens_lines"] = dict(0)
_editor_values["code_lens_rects"] = []
_editor_values["fold_path"] = ""
_editor_values["fold_requested_path"] = ""
_editor_values["fold_ranges"] = []
_editor_values["fold_collapsed"] = dict(0)
_editor_values["fold_rects"] = []
_editor_values["blame_on"] = false
_editor_values["blame_path"] = ""
_editor_values["blame_lines"] = dict(0)
_editor_values["blame_hunks"] = dict(0)
_editor_values["git_inline_runs"] = []
_editor_values["lsp_auto"] = ""
_editor_values["lsp_auto_at"] = 0
_editor_values["ui_state"] = borrow(interact.new())
_editor_values["term_state"] = borrow(termpane.new(int(_editor_get("font_body")), TERM_BG, TERM_FG))
_editor_values["completion_state"] = dict(6)
_editor_get("completion_state")["open"] = false
_editor_get("completion_state")["items"] = []
_editor_get("completion_state")["index"] = 0
_editor_get("completion_state")["prefix"] = ""
_editor_get("completion_state")["x"] = 0.0
_editor_get("completion_state")["y"] = 0.0
_editor_values["peek_state"] = dict(6)
_editor_get("peek_state")["open"] = false
_editor_get("peek_state")["title"] = ""
_editor_get("peek_state")["body"] = ""
_editor_get("peek_state")["path"] = ""
_editor_get("peek_state")["line"] = 0
_editor_get("peek_state")["col"] = 0
_editor_values["color_state"] = borrow(colorpicker.state())
_editor_values["project_drag"] = dict(5)
_editor_get("project_drag")["active"] = false
_editor_get("project_drag")["armed"] = false
_editor_get("project_drag")["entry"] = dict(8)
_editor_get("project_drag")["x"] = 0.0
_editor_get("project_drag")["y"] = 0.0
_editor_values["project_selection"] = borrow([])
_editor_values["project_selection_anchor"] = ""
_editor_values["section_drag"] = dict(4)
_editor_get("section_drag")["active"] = false
_editor_get("section_drag")["section"] = ""
_editor_get("section_drag")["x"] = 0.0
_editor_get("section_drag")["y"] = 0.0
;; Process-lifetime frame caches: these globals are reused across frames and
;; reset as editor state changes. They are not function-scoped owned slots.
_editor_values["image_preview_cache"] = borrow(dict(32))
_editor_values["nav_back"] = borrow([])
_editor_values["nav_forward"] = borrow([])
_editor_values["nav_live_loc"] = borrow([])
_editor_values["nav_live_idx"] = 0
_editor_values["nav_prev_idx"] = -1
_editor_values["nav_next_idx"] = -1
_editor_values["nav_probe_phase"] = 0
_editor_values["content_rev"] = 0
_editor_values["syntax_cache_rev"] = -1
_editor_values["syntax_run_cache"] = borrow(dict(256))
_editor_values["syntax_flat_cache"] = borrow(dict(256))
_editor_values["syntax_lang_cache"] = borrow(dict(32))
_editor_values["visible_runs_cache_key"] = ""
_editor_values["visible_line_runs_cache"] = borrow([])
_editor_values["visible_text_runs_cache"] = borrow([])
_editor_values["width_cache_rev"] = -1
_editor_values["width_cache"] = borrow(dict(512))
_editor_values["ui_measure_cache"] = borrow(dict(512))
_editor_values["modeline_cache_key"] = ""
_editor_values["modeline_cache"] = borrow(dict(8))
_editor_values["outline_cache_rev"] = -1
_editor_values["outline_cache_name"] = ""
_editor_values["outline_cache"] = borrow([])
_editor_values["definition_cache_rev"] = -1
_editor_values["definition_cache_name"] = ""
_editor_values["definition_cache"] = borrow(dict(64))
_editor_values["project_definition_cache_key"] = ""
_editor_values["project_definition_cache"] = borrow(dict(512))
_editor_values["project_definition_scanned"] = borrow(dict(256))
_editor_values["project_definition_cache_complete"] = false
_editor_values["context_state"] = borrow(prompt.context_state())
_editor_values["rename_state"] = borrow(prompt.rename_state())
_editor_values["fps_state"] = borrow(ui_runtime.fps_begin())
_editor_values["reuse_state"] = borrow(ui_reuse.make())
_editor_values["frame_count"] = 0
_editor_values["reuse_present_count"] = 0
_editor_values["begin_fail_count"] = 0
_editor_values["frame_events_seen"] = false
_editor_values["next_cache_save_time"] = 0.0
_editor_values["force_full_redraw"] = 0
_editor_values["last_escape_ns"] = 0
_editor_values["auto_dump_done"] = false
_editor_values["cursor_kind"] = ""
_editor_values["perf_advisor_reports"] = 0
_editor_values["frame_hash_done"] = false
_editor_values["edit_scroll_accum"] = 0.0
_editor_values["palette_scroll_accum"] = 0.0
_editor_values["which_key_scroll_accum"] = 0.0
_editor_values["which_key_scroll"] = 0
_editor_values["which_key_prefix"] = ""
_editor_values["which_key_rows_cache"] = borrow([])
_editor_values["suppress_char_cp"] = 0
_editor_values["tree_scroll_accum"] = 0.0
_editor_values["outline_scroll_accum"] = 0.0
_editor_values["git_scroll_accum"] = 0.0
_editor_values["timeline_scroll_accum"] = 0.0
_editor_values["project_state"] = project.new(common.env_trim("NY_EDITOR_PROJECT_ROOT"))
def auto_dump_delay = ui_dump.auto_dump_delay_frames(8)
def timeout_limit = ui_runtime.timeout_ns(0)

fn _editor_probe(str name) bool {
   common.env_truthy("NY_EDITOR_PROBE_" + name) ||
   common.env_truthy("NY_EDITOR_" + name + "_PROBE") ||
   common.env_truthy("NY_EDITOR_" + name + "_SELFTEST")
}

def probe_flags = {
   "INPUT": _editor_probe("INPUT"),
   "CONTEXT": _editor_probe("CONTEXT"),
   "TERMINAL": _editor_probe("TERMINAL"),
   "TERMINAL_DUMP": _editor_probe("TERMINAL_DUMP"),
   "SYNTAX": _editor_probe("SYNTAX"),
   "INLAY_LIVE": _editor_probe("INLAY_LIVE"),
   "LSP": _editor_probe("LSP"),
   "DAP": _editor_probe("DAP"),
   "NAV": _editor_probe("NAV"),
   "UNDO": _editor_probe("UNDO"),
   "ESCAPE": _editor_probe("ESCAPE"),
   "MODELINE": _editor_probe("MODELINE"),
   "CHORD": _editor_probe("CHORD"),
   "FIND": _editor_probe("FIND"),
   "BUFFER_SEARCH": _editor_probe("BUFFER_SEARCH"),
   "PALETTE": _editor_probe("PALETTE"),
   "WHICH_KEY": _editor_probe("WHICH_KEY"),
   "MULTICURSOR": _editor_probe("MULTICURSOR"),
   "ZOOM": _editor_probe("ZOOM"),
   "SCROLLBAR": _editor_probe("SCROLLBAR"),
   "GOTO": _editor_probe("GOTO"),
   "GIT": _editor_probe("GIT"),
   "GIT_INLINE": _editor_probe("GIT_INLINE"),
   "TAB": _editor_probe("TAB"),
   "SELECT": _editor_probe("SELECT"),
   "TEXTGRID": _editor_probe("TEXTGRID"),
   "IMAGE": _editor_probe("IMAGE"),
   "BENCH": _editor_probe("BENCH"),
   "POPUPS": _editor_probe("POPUPS")
}
def bool MOCK_BENCH_FAST = probe_flags.get("BENCH", false) && common.env_truthy("NY_UI_HEADLESS") && !common.env_truthy("NYTRIX_AUTO_DUMP")
def int BENCH_FRAME_LIMIT = int(common.env_int_clamped("NY_EDITOR_BENCH_FRAMES", 240, 1, 100000))
def int BENCH_WARMUP_FRAMES = int(common.env_int_clamped("NY_EDITOR_BENCH_WARMUP_FRAMES", 1, 0, 10000))
def bool FRAME_TRACE = common.env_truthy("NY_EDITOR_FRAME_TRACE")
def int FRAME_TRACE_LIMIT = int(common.env_int_clamped("NY_EDITOR_FRAME_TRACE_FRAMES", 5, 1, 100000))
def bool FRAME_HASH = common.env_truthy("NY_EDITOR_FRAME_HASH") || common.env_truthy("NY_UI_FRAME_HASH_LOCK") || common.env_truthy("NYTRIX_UI_REPLAY")
def int FRAME_HASH_FRAME = int(common.env_int_clamped("NY_EDITOR_FRAME_HASH_FRAME", 2, 0, 1000000))
def str FRAME_HASH_EXPECT = common.env_trim("NY_EDITOR_FRAME_HASH_EXPECT")
def bool OMNI_TRACE = common.env_truthy("NY_EDITOR_OMNI") || common.env_truthy("NY_EDITOR_STATE_DUMP")
def int OMNI_TRACE_FRAMES = int(common.env_int_clamped("NY_EDITOR_OMNI_FRAMES", 8, 1, 100000))
def int OMNI_TRACE_EVERY = int(common.env_int_clamped("NY_EDITOR_OMNI_EVERY", 1, 1, 100000))
def bool PERF_ADVISOR = FRAME_TRACE || common.env_truthy("NY_EDITOR_PERF_ADVISOR")
def int PERF_ADVISOR_MIN_MS = int(common.env_int_clamped("NY_EDITOR_PERF_ADVISOR_MIN_MS", 18, 1, 1000))
def int PERF_ADVISOR_LIMIT = int(common.env_int_clamped("NY_EDITOR_PERF_ADVISOR_FRAMES", 12, 1, 10000))
def int CACHE_SAVE_SECONDS = int(common.env_int_clamped("NY_EDITOR_CACHE_SAVE_SECONDS", 5, 1, 300))
def int PROJECT_OPEN_CACHE_LIMIT = int(common.env_int_clamped("NY_EDITOR_PROJECT_OPEN_CACHE_LIMIT", 1024, 0, 10000))
def int BEGIN_FAIL_LIMIT = int(common.env_int_clamped("NY_EDITOR_BEGIN_FAIL_LIMIT", 180, 1, 10000))
def int EVENT_FRAME_LIMIT = int(common.env_int_clamped("NY_EDITOR_MAX_EVENTS_PER_FRAME", 384, 32, 8192))
def int COMPLETION_SCAN_RADIUS = int(common.env_int_clamped("NY_EDITOR_COMPLETION_SCAN_RADIUS", 900, 64, 20000))
def int COMPLETION_SCAN_MAX_LINES = int(common.env_int_clamped("NY_EDITOR_COMPLETION_SCAN_MAX_LINES", 4000, 128, 1000000))
def int IDLE_REUSE_WARMUP = int(common.env_int_clamped("NY_EDITOR_IDLE_REUSE_WARMUP", 1, 0, 128))
def int IDLE_REUSE_INTERVAL = int(common.env_int_clamped("NY_EDITOR_IDLE_REUSE_INTERVAL", 1200, 1, 100000))
def int ESC_QUIT_WINDOW_NS = 900000000
_editor_get("palette_state")["cfg"] = _editor_get("command_cfg")

fn _trace_ms(int a, int b) str {
   def int delta = b - a
   def int us = delta > 0 ? delta / 1000 : 0
   to_str(us / 1000) + "." + to_str((us / 100) % 10) + to_str((us / 10) % 10) + "ms"
}

fn _measure_ui(int font, str text, f64 fallback=0.0) f64 {
   "Returns cached UI text width for stable chrome labels.  Editor line-prefix widths keep their own content-revision cache."
   if text.len <= 0 { return 0.0 }
   if !is_dict(_editor_get("ui_measure_cache")) { _editor_values["ui_measure_cache"] = borrow(dict(512)) }
   def key = to_str(font) + "\t" + text
   if _editor_get("ui_measure_cache").contains(key) { return float(_editor_get("ui_measure_cache").get(key, 0)) }
   def measured = gfx.measure_text_fast(font, text)
   def w = is_list(measured) && measured.len > 0 ? float(measured.get(0, fallback)) : fallback
   if text.len <= 96 && _editor_get("ui_measure_cache").len < 2048 { _editor_get("ui_measure_cache")[key] = w }
   w
}

fn _perf_advisor_frame(int frame, int t0, int t_update, int t_events, int t_begin, int t_draw, int t_flush, int t_end) int {
   if !PERF_ADVISOR || int(_editor_get("perf_advisor_reports")) >= PERF_ADVISOR_LIMIT { return 0 }
   def total_ms = (t_end - t0) / 1000000
   if total_ms < PERF_ADVISOR_MIN_MS { return 0 }
   _editor_values["perf_advisor_reports"] = int(_editor_get("perf_advisor_reports")) + (1)
   def draw_ms = (t_draw - t_begin) / 1000000
   def event_ms = (t_events - t_update) / 1000000
   def flush_ms = (t_end - t_flush) / 1000000
   mut hint = ""
   if event_ms > draw_ms && event_ms >= 4 { hint = "input queue: lower NY_EDITOR_MAX_EVENTS_PER_FRAME or check terminal paste/mouse spam" }
   elif flush_ms >= 4 { hint = "present/driver: try -vk/-gl switch, disable vsync, or inspect renderer stats" }
   elif draw_ms >= 4 { hint = "draw path: enable idle reuse, shrink visible rows, or inspect text/rect batches" }
   else { hint = "mixed frame: run NY_EDITOR_FRAME_TRACE=1 NY_EDITOR_PERF_ADVISOR=1" }
   ui_trace.emit("editor.perf", frame, "[editor:perf] frame=" + to_str(frame) + " total=" + to_str(total_ms) + "ms update=" + _trace_ms(t0, t_update) + " events=" + _trace_ms(t_update, t_events) + " draw=" + _trace_ms(t_begin, t_draw) + " flush=" + _trace_ms(t_flush, t_end) + " hint=" + hint)
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
   ui_trace.emit("editor.frame", frame, "[editor:frame] #" + to_str(frame) +
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
   ui_trace.emit("editor.draw", frame, "[editor:draw] #" + to_str(frame) +
      " top=" + _trace_ms(t0, t_top) +
      " rail=" + _trace_ms(t_top, t_rail) +
      " editor=" + _trace_ms(t_rail, t_editor) +
      " terminal=" + _trace_ms(t_editor, t_terminal) +
      " status=" + _trace_ms(t_terminal, t_status) +
   " popups=" + _trace_ms(t_status, t_popups))
   0
}

fn _editor_popup_summary() str {
   "palette=" + common.yn(pal.is_open(_editor_get("palette_state"))) +
   " find=" + common.yn(find.is_open(_editor_get("find_state"))) +
   " context=" + common.yn(prompt.context_is_open(_editor_get("context_state"))) +
   " rename=" + common.yn(prompt.rename_is_open(_editor_get("rename_state"))) +
   " color=" + common.yn(colorpicker.is_open(_editor_get("color_state"))) +
   " completion=" + common.yn(bool(_editor_get("completion_state").get("open", false))) +
   " chord=" + common.yn(chord.pending(_editor_get("chord_state"))) +
   " quit=" + common.yn(bool(_editor_get("st").get("quit_prompt", false)))
}

fn _editor_omni_dump(dict lay, f64 sw, f64 sh) int {
   if !OMNI_TRACE { return 0 }
   if int(_editor_get("frame_count")) >= OMNI_TRACE_FRAMES { return 0 }
   if int(_editor_get("frame_count")) % OMNI_TRACE_EVERY != 0 { return 0 }
   def b = ed.current_buffer(_editor_get("st"))
   def lines = ed.current_lines(_editor_get("st"))
   def rs = gfx.renderer_frame_stats()
   print("[editor:omni] frame=" + to_str(int(_editor_get("frame_count"))) +
      " fps=" + to_str(int(_editor_get("st").get("fps", 0))) +
      " frame_ms=" + to_str(float(_editor_get("st").get("frame_ms", 0.0))) +
      " focus=" + to_str(_editor_get("st").get("focus", "")) +
      " active=" + to_str(int(_editor_get("st").get("active", 0))) +
      " buffers=" + to_str(_editor_get("st").get("buffers", []).len) +
      " cursor=" + to_str(int(_editor_get("st").get("cursor_line", 0))) + ":" + to_str(int(_editor_get("st").get("cursor_col", 0))) +
      " scroll=" + to_str(int(_editor_get("st").get("scroll", 0))) +
      " mark=" + common.yn(bool(_editor_get("st").get("mark_active", false))) +
      " sel=" + (bool(_editor_get("st").get("sel_active", false)) ? to_str(int(_editor_get("st").get("sel_a_line", 0))) + ":" + to_str(int(_editor_get("st").get("sel_a_col", 0))) + "-" + to_str(int(_editor_get("st").get("sel_b_line", 0))) + ":" + to_str(int(_editor_get("st").get("sel_b_col", 0))) : "none") +
      " palette=" + (pal.is_open(_editor_get("palette_state")) ? "open:" + pal.items_mode(_editor_get("palette_state")) : "closed") +
      " file=" + to_str(b.get("name", "")) +
      " kind=" + to_str(b.get("kind", "text")) +
      " path=" + to_str(b.get("path", "")) +
      " lines=" + to_str(lines.len) +
      " layout=" + to_str(int(sw)) + "x" + to_str(int(sh)) +
      " edit=" + to_str(int(float(lay.get("edit_x", 0.0)))) + "," + to_str(int(float(lay.get("edit_y", 0.0)))) + "," + to_str(int(float(lay.get("edit_w", 0.0)))) + "," + to_str(int(float(lay.get("edit_h", 0.0)))) +
      " rail=" + common.yn(bool(_editor_get("st").get("show_hierarchy", true))) + ":" + _rail_tab() +
      " project_loaded=" + common.yn(bool(_editor_get("st").get("project_loaded", false))) +
      " git_loaded=" + common.yn(bool(_editor_get("st").get("git_loaded", false))) +
      " terminal=" + common.yn(termpane.is_open(_editor_get("term_state"))) +
      " term_tabs=" + to_str(termpane.tab_count(_editor_get("term_state"))) +
      " term_hot=" + common.yn(bool(_editor_get("term_state").get("hot", false))) +
      " term_bytes=" + to_str(int(_editor_get("term_state").get("last_update_bytes", 0))) +
      " popups={" + _editor_popup_summary() + "}" +
      " draws=" + to_str(int(rs.get("draws", 0))) +
      " text=" + to_str(int(rs.get("prim_text_calls", 0))) + "/" + to_str(int(rs.get("prim_text_glyphs", 0))) +
      " " + ui_diag.snapshot_text(_editor_get("win")))
   0
}

fn _request_full_redraw(int frames=2) int {
   if frames > int(_editor_get("force_full_redraw")) { _editor_values["force_full_redraw"] = frames }
   0
}

fn _clear_pending_destructive_prompts() int {
   _editor_get("st")["quit_prompt"] = false
   _editor_get("st")["kill_confirm"] = false
   _editor_get("st")["kill_confirm_buf"] = -1
   _editor_get("st")["reload_confirm"] = false
   _editor_get("st")["reload_confirm_buf"] = -1
   _editor_values["last_escape_ns"] = 0
   0
}

fn _editor_terminal_active() bool {
   termpane.is_open(_editor_get("term_state")) &&
   (bool(_editor_get("term_state").get("hot", false)) || int(_editor_get("term_state").get("last_update_bytes", 0)) > 0)
}

fn _editor_overlay_active() bool {
   if _editor_get("st").get("quit_prompt", false) { return true }
   if chord.pending(_editor_get("chord_state")) { return true }
   if pal.is_open(_editor_get("palette_state")) { return true }
   if find.is_open(_editor_get("find_state")) { return true }
   if prompt.rename_is_open(_editor_get("rename_state")) { return true }
   if prompt.context_is_open(_editor_get("context_state")) { return true }
   if colorpicker.is_open(_editor_get("color_state")) { return true }
   if bool(_editor_get("completion_state").get("open", false)) { return true }
   false
}

fn _editor_drag_active() bool {
   _editor_get("st").get("drag_terminal", false) || _editor_get("st").get("drag_divider", false) || _editor_get("st").get("drag_select", false) ||
   _editor_get("st").get("drag_scrollbar", false) || _editor_get("project_drag").get("active", false) || _editor_get("project_drag").get("armed", false) ||
   _editor_get("section_drag").get("active", false)
}

fn _editor_dynamic_ui_active() bool {
   int(_editor_get("force_full_redraw")) > 0 || _editor_terminal_active() ||
   _editor_overlay_active() || _editor_drag_active()
}

@returns_owned
fn _editor_reuse_opts(f64 sw, f64 sh) dict {
   mut dict opts = dict(12)
   def reuse_enabled = common.env_truthy("NY_UI_EDITOR_IDLE_REUSE")
   opts["enabled"] = reuse_enabled
   opts["gui_frame"] = true
   opts["win_w"] = int(sw)
   opts["win_h"] = int(sh)
   opts["terminal_open"] = termpane.is_open(_editor_get("term_state"))
   opts["first_frame_done"] = int(_editor_get("frame_count")) > 1
   opts["scene_active"] = true
   opts["input_active"] = bool(_editor_get("frame_events_seen"))
   ;; Dynamic state probing is unnecessary when present-only reuse is disabled.
   if reuse_enabled {
      opts["dynamic_active"] = _editor_dynamic_ui_active()
   } else {
      opts["dynamic_active"] = false
   }
   opts["capture_active"] = probe_flags.get("TERMINAL_DUMP", false) || common.env_truthy("NYTRIX_AUTO_DUMP")
   opts["warmup"] = max(IDLE_REUSE_WARMUP, gfx.get_swapchain_image_count() + 2)
   opts["redraw_interval"] = IDLE_REUSE_INTERVAL
   opts
}

fn _bench_finish() int {
   if !probe_flags.get("BENCH", false) { return 0 }
   def int start_ns = int(_editor_get("fps_state").get("start", ticks()))
   def int elapsed_ns = ticks() - start_ns
   def f64 elapsed = elapsed_ns > 0 ? float(elapsed_ns) / 1000000000.0 : 0.000001
   def int total = int(_editor_get("fps_state").get("total", 0))
   def int avg = int(float(total) / elapsed)
   print("[editor:bench] avg_fps=" + to_str(avg) +
      " frames=" + to_str(total) +
      " reused=" + to_str(int(_editor_get("reuse_present_count"))) +
      " reused_streak=" + to_str(ui_reuse.reused_frames(_editor_get("reuse_state"))) +
   " full_ready=" + to_str(ui_reuse.ready_frames(_editor_get("reuse_state"))))
   0
}

fn _set_cursor_kind(str kind) int {
   if to_str(_editor_get("cursor_kind")) == kind { return 0 }
   _editor_values["cursor_kind"] = kind
   window.set_cursor(_editor_get("win"), kind == "resize-ew" ? cursor_resize : (kind == "resize-ns" ? cursor_resize_ns : cursor_text))
   0
}

fn _next_event() any {
   window.check_event(window.get_win(_editor_get("win")))
}

def int EDITOR_ICON_RASTER = 192

fn _editor_icon_tex(str name) int {
   if gfx.get_backend_type() == RENDER_BACKEND_MOCK { return -1 }
   def int tex = icons.icon_tex(name, EDITOR_ICON_RASTER)
   if tex > 0 { return tex }
   name == "file" ? -1 : icons.icon_tex("file", EDITOR_ICON_RASTER)
}

fn _draw_icon(str name, f64 x, f64 y, f64 size, any tint=C_TEXT) int {
   if size <= 0.0 { return 0 }
   if gfx.get_backend_type() == RENDER_BACKEND_MOCK {
      if !MOCK_BENCH_FAST { _stroke_rect(x, y, size, size, tint, 1.0) }
      return 0
   }
   def int tex = _editor_icon_tex(name)
   if tex <= 0 { return 0 }
   def sz = gfx.texture_size(tex)
   def f64 tw = float(sz.get(0, 0))
   def f64 th = float(sz.get(1, 0))
   if tw <= 0.0 || th <= 0.0 { return 0 }
   def f64 scale = size / float(max(tw, th))
   def f64 w = tw * scale
   def f64 h = th * scale
   _draw_tex_rect(tex, _snap_px(x + (size - w) * 0.5), _snap_px(y + (size - h) * 0.5), w, h, gfx.WHITE)
}

fn _editor_file_icon(dict e) str {
   if bool(e.get("dir", false)) { return "folder" }
   def name = to_str(e.get("name", ""))
   def kind = to_str(e.get("kind", project.file_kind(name)))
   def str ext = to_str(str.lower(ospath.extname(name)))
   if kind == "term" || kind == "terminal" { return "terminal" }
   if kind == "diff" { return "git_branch" }
   if ext == ".ny" || ext == ".nyt" { return "nytrix" }
   if ext == ".c" || ext == ".h" { return "nyc" }
   case kind {
      "code" -> "file_code"
      "doc" -> "text_file"
      "data" -> "dict"
      "image" -> "image"
      "model" -> "file_3d"
      "shader" -> "asset_material"
      "font" -> "str"
      "audio" -> "sound"
      "video" -> "play"
      "archive" -> "package"
      "binary" -> "compiler"
      _ -> "file"
   }
}

fn _editor_symbol_icon(str kind) str {
   case kind {
      "fn" -> "callable"
      "struct" -> "collections"
      "enum" -> "dict"
      "module" -> "mod"
      "main" -> "play"
      "def", "mut" -> "tag"
      _ -> "atom"
   }
}

fn _fit_chrome_text(int font, str text, f64 width) str {
   if width <= 0.0 || text.len == 0 { return "" }
   if _measure_ui(font, text, float(str.utf8_len(text)) * 8.0) <= width { return text }
   if _measure_ui(font, "...", 24.0) > width { return "" }
   mut int lo = 0
   mut int hi = str.utf8_len(text)
   while lo < hi {
      def int mid = (lo + hi + 1) / 2
      def candidate = str.utf8_slice(text, 0, mid) + "..."
      if _measure_ui(font, candidate, float(mid + 3) * 8.0) <= width { lo = mid }
      else { hi = mid - 1 }
   }
   str.utf8_slice(text, 0, lo) + "..."
}

fn _layout_sw(dict lay) f64 {
   float(lay.get("edit_x", 0.0)) + float(lay.get("edit_w", 0.0))
}

fn _layout_sh(dict lay) f64 {
   float(lay.get("status_y", 0.0)) + float(lay.get("status_h", ed.STATUS_H))
}

fn _fit_rect(f64 x, f64 y, f64 w, f64 h, f64 sw, f64 sh, f64 margin=8.0) list {
   def f64 max_w = float(max(1.0, sw - margin * 2.0))
   def f64 max_h = float(max(1.0, sh - margin * 2.0))
   w = float(min(max(1.0, w), max_w))
   h = float(min(max(1.0, h), max_h))
   def f64 max_x = float(max(margin, sw - w - margin))
   def f64 max_y = float(max(margin, sh - h - margin))
   x = float(min(max(margin, x), max_x))
   y = float(min(max(margin, y), max_y))
   [x, y, w, h]
}

fn _palette_rect(f64 sw, f64 sh, int matches_len) list {
   def margin = 8.0
   mut f64 w = float(min(680.0, max(250.0, sw - 80.0)))
   w = float(min(w, max(1.0, sw - margin * 2.0)))
   mut f64 y = float(max(12.0, sh * 0.070))
   y = float(min(y, max(margin, sh - margin - PALETTE_HEADER_H - PALETTE_DETAIL_H - PALETTE_ROW_H - 6.0)))
   mut int max_rows = int((sh - y - margin - PALETTE_HEADER_H - PALETTE_DETAIL_H - 6.0) / PALETTE_ROW_H)
   if max_rows < 1 { max_rows = 1 }
   mut int rows = matches_len
   if rows < 1 { rows = 1 }
   if rows > max_rows { rows = max_rows }
   if rows > PALETTE_MAX_ROWS { rows = PALETTE_MAX_ROWS }
   def f64 h = PALETTE_HEADER_H + float(rows) * PALETTE_ROW_H + PALETTE_DETAIL_H + 6.0
   def f64 x0 = sw * 0.5 - w * 0.5
   def fit = _fit_rect(x0, y, w, h, sw, sh, margin)
   def slide = gui.overlay_offset("ed_palette", true)
   [float(fit.get(0, x0)), float(fit.get(1, y)) + slide, float(fit.get(2, w)), float(fit.get(3, h)), rows]
}

fn _palette_contains(dict lay, f64 mx, f64 my) bool {
   if !pal.is_open(_editor_get("palette_state")) { return false }
   def r = _palette_rect(_layout_sw(lay), _layout_sh(lay), _palette_matches().len)
   widgets.hit(mx, my, float(r.get(0, 0.0)), float(r.get(1, 0.0)), float(r.get(2, 0.0)), float(r.get(3, 0.0)))
}

fn _which_key_rect(f64 sw, f64 sh, int total) list {
   mut f64 w = float(min(660.0, max(260.0, sw * 0.62)))
   w = float(min(w, max(1.0, sw - 16.0)))
   def int cols = w > 520.0 ? 2 : 1
   mut int max_lines = int((sh - 84.0) / WHICH_KEY_ROW_H)
   if max_lines < 1 { max_lines = 1 }
   mut int max_visible = max_lines * cols
   if max_visible > WHICH_KEY_MAX_ROWS { max_visible = WHICH_KEY_MAX_ROWS }
   if max_visible < 1 { max_visible = 1 }
   mut int visible = min(max(1, total), max_visible)
   mut int line_count = int((visible + cols - 1) / cols)
   def max_h = max(1.0, sh - 16.0)
   mut f64 h = min(max_h, 42.0 + float(line_count) * WHICH_KEY_ROW_H + 8.0)
   mut int fit_lines = int((h - 50.0) / WHICH_KEY_ROW_H)
   if fit_lines < 1 { fit_lines = 1 }
   if fit_lines < line_count {
      line_count = fit_lines
      visible = min(visible, line_count * cols)
      h = 42.0 + float(line_count) * WHICH_KEY_ROW_H + 8.0
   }
   def fit = _fit_rect(10.0, sh - h - 10.0, w, h, sw, sh, 8.0)
   def slide = gui.overlay_offset("ed_which_key", true)
   [float(fit.get(0, 10.0)), float(fit.get(1, sh - h - 10.0)) + slide, float(fit.get(2, w)), float(fit.get(3, h)), cols, visible]
}

fn _keymap_context() any {
   if !cmd.has_context_bindings() { return nil }
   def b = ed.current_buffer(_editor_get("st"))
   def bool readonly = bool(b.get("readonly", false))
   def bool file = to_str(b.get("path", "")).len > 0
   {"editor": true, "selection": _selection_any_valid(), "readonly": readonly, "writable": !readonly, "file": file, "scratch": !file}
}

fn _which_key_rows(str prefix) list {
   def context = _keymap_context()
   def int context_bits = !is_dict(context) ? 0 : 1 | (context.get("selection", false) ? 2 : 0) | (context.get("readonly", false) ? 4 : 0) | (context.get("file", false) ? 8 : 0)
   if prefix == to_str(_editor_get("which_key_prefix")) && int(_editor_values.get("which_key_context", 0)) == context_bits && is_list(_editor_get("which_key_rows_cache")) { return _editor_get("which_key_rows_cache") }
   _editor_values["which_key_prefix"] = prefix
   _editor_values["which_key_context"] = context_bits
   _editor_values["which_key_scroll"] = 0
   _editor_values["which_key_scroll_accum"] = 0.0
   _editor_values["which_key_rows_cache"] = cmd.which_key(prefix, context)
   _editor_get("which_key_rows_cache")
}

fn _which_key_contains(f64 sw, f64 sh, f64 mx, f64 my) bool {
   if !chord.pending(_editor_get("chord_state")) { return false }
   def rows = _which_key_rows(chord.describe(_editor_get("chord_state")))
   if rows.len <= 0 { return false }
   def r = _which_key_rect(sw, sh, rows.len)
   widgets.hit(mx, my, float(r.get(0, 0.0)), float(r.get(1, 0.0)), float(r.get(2, 0.0)), float(r.get(3, 0.0)))
}

fn _clamp_which_key_scroll(int total, int visible) int {
   def int visible_rows = visible > 0 ? visible : 1
   def int max_scroll = total > visible_rows ? total - visible_rows : 0
   _clamp_int(int(_editor_get("which_key_scroll")), 0, max_scroll)
}

fn _touch_content() int {
   _request_full_redraw(2)
   _editor_values["content_rev"] = int(_editor_get("content_rev")) + (1)
   _editor_values["semantic_path"] = ""
   _editor_values["semantic_requested_path"] = ""
   _editor_values["semantic_lines"] = dict(0)
   _editor_values["inlay_path"] = ""
   _editor_values["inlay_requested_path"] = ""
   _editor_values["inlay_lines"] = dict(0)
   _editor_values["code_lens_path"] = ""
   _editor_values["code_lens_requested_path"] = ""
   _editor_values["code_lens_lines"] = dict(0)
   _editor_values["fold_path"] = ""
   _editor_values["fold_requested_path"] = ""
   _editor_values["fold_ranges"] = []
   _editor_values["fold_collapsed"] = dict(0)
   _editor_values["blame_path"] = ""
   _editor_values["blame_lines"] = dict(0)
   _editor_values["blame_hunks"] = dict(0)
   _editor_values["git_inline_runs"] = []
   _editor_values["syntax_cache_rev"] = -1
   _editor_values["width_cache_rev"] = -1
   _editor_values["outline_cache_rev"] = -1
   _editor_values["definition_cache_rev"] = -1
   _editor_values["syntax_run_cache"] = borrow(dict(256))
   _editor_values["syntax_flat_cache"] = borrow(dict(256))
   _editor_values["width_cache"] = borrow(dict(512))
   _editor_values["find_content_rev"] = -1
   _editor_values["color_state"] = colorpicker.dismiss(_editor_get("color_state"))
   0
}

fn _invalidate_project_definitions() int {
   _editor_values["project_definition_cache_key"] = ""
   _editor_values["project_definition_cache"] = borrow(dict(512))
   _editor_values["project_definition_scanned"] = borrow(dict(256))
   _editor_values["project_definition_cache_complete"] = false
   0
}

fn _invalidate_text_metrics() int {
   _editor_values["syntax_cache_rev"] = -1
   _editor_values["width_cache_rev"] = -1
   _editor_values["modeline_cache_key"] = ""
   _editor_values["visible_runs_cache_key"] = ""
   _editor_values["syntax_run_cache"] = borrow(dict(256))
   _editor_values["syntax_flat_cache"] = borrow(dict(256))
   _editor_values["width_cache"] = borrow(dict(512))
   0
}

def int FONT_ZOOM_MIN = 8
def int FONT_ZOOM_MAX = 42

fn _apply_editor_fonts() int {
   _editor_values["font_title"] = _load_editor_font(TITLE_FONT_CANDIDATES, int(_editor_get("font_title_size")))
   _editor_values["font_body"] = _load_editor_font(UI_FONT_CANDIDATES, int(_editor_get("font_body_size")))
   _editor_values["font_small"] = _load_editor_font(UI_FONT_CANDIDATES, int(_editor_get("font_small_size")))
   _editor_values["font_modeline"] = _load_editor_font(MODELINE_FONT_CANDIDATES, int(_editor_get("font_modeline_size")))
   def body_advance = _measure_ui(int(_editor_get("font_body")), "0", 0.0)
   def small_advance = _measure_ui(int(_editor_get("font_small")), "0", 0.0)
   def modeline_advance = _measure_ui(int(_editor_get("font_modeline")), "0", 0.0)
   _editor_values["FONT_BODY_ADV"] = body_advance >= 2.0 && body_advance <= float(int(_editor_get("font_body_size"))) * 2.0 ? body_advance : float(int(_editor_get("font_body_size"))) * 0.6
   _editor_values["FONT_SMALL_ADV"] = small_advance >= 2.0 && small_advance <= float(int(_editor_get("font_small_size"))) * 2.0 ? small_advance : float(int(_editor_get("font_small_size"))) * 0.6
   _editor_values["FONT_MODELINE_ADV"] = modeline_advance >= 2.0 && modeline_advance <= float(int(_editor_get("font_modeline_size"))) * 2.0 ? modeline_advance : float(int(_editor_get("font_modeline_size"))) * 0.6
   _editor_values["FONT_MODELINE_CLIP_ADV"] = _clamp_f64(float(_editor_get("FONT_MODELINE_ADV")) * 1.06, 4.0, 10000.0)
   _editor_values["term_state"] = termpane.set_font(_editor_get("term_state"), int(_editor_get("font_body")))
   _invalidate_text_metrics()
   0
}

fn _set_editor_font_size(int body_size, bool persist=true) int {
   def int next = _clamp_int(body_size, FONT_ZOOM_MIN, FONT_ZOOM_MAX)
   if next == int(_editor_get("font_body_size")) {
      if persist { _status("font " + to_str(int(_editor_get("font_body_size"))) + "px") }
      return 0
   }
   def int delta = next - FONT_BODY_SIZE
   _editor_values["font_body_size"] = next
   _editor_values["font_title_size"] = _clamp_int(FONT_TITLE_SIZE + delta, 8, 96)
   _editor_values["font_small_size"] = _clamp_int(FONT_SMALL_SIZE + delta, 8, 96)
   _editor_values["font_modeline_size"] = _clamp_int(FONT_MODELINE_SIZE + delta, 8, 96)
   _apply_editor_fonts()
   if persist { _save_ui_cache() }
   if persist { _status("font " + to_str(int(_editor_get("font_body_size"))) + "px") }
   0
}

fn _zoom_editor_font(int delta) int {
   _set_editor_font_size(int(_editor_get("font_body_size")) + delta, true)
}

fn _reset_editor_font() int {
   _set_editor_font_size(FONT_BODY_SIZE, true)
}

fn _line_limit(dict lay) int {
   mut f64 body_advance = float(float(_editor_get("FONT_BODY_ADV")))
   if body_advance < 1.0 { body_advance = 1.0 }
   def f64 limit = float(lay.get("edit_w", 0.0)) / body_advance + float(LINE_DRAW_EXTRA_COLS)
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
   _editor_get("st")["scroll_follow"] = true
   0
}

fn _buffer_source_path(dict b) str {
   def path = to_str(b.get("path", ""))
   path.len > 0 ? path : to_str(b.get("source_path", ""))
}

@returns_owned
fn _editor_context_entry() dict {
   def b = ed.current_buffer(_editor_get("st"))
   def path = _buffer_source_path(b)
   mut dict entry = dict(6)
   entry["scope"] = "editor"
   entry["name"] = to_str(b.get("name", "buffer"))
   entry["path"] = path
   entry["rel"] = path.len > 0 ? path : to_str(b.get("name", "buffer"))
   entry["dir"] = false
   entry["kind"] = to_str(b.get("kind", "file"))
   entry
}

fn _open_context(f64 x, f64 y, dict entry, str scope) int {
   mut e = is_dict(entry) ? clone(entry) : dict(8)
   e["scope"] = scope
   _editor_values["context_state"] = prompt.context_open(_editor_get("context_state"), x, y, e)
   _editor_values["palette_state"] = pal.dismiss(_editor_get("palette_state"))
   0
}

@returns_owned
fn _timeline_context_entry(any row) dict {
   def typ = to_str(row.get(0, ""))
   if typ == "command" {
      mut dict command_entry = dict(6)
      command_entry["type"] = "command"
      command_entry["name"] = to_str(row.get(1, "command"))
      command_entry["id"] = to_str(row.get(2, ""))
      command_entry["path"] = ""
      command_entry["rel"] = to_str(row.get(2, ""))
      command_entry["dir"] = false
      return command_entry
   }
   def path = to_str(row.get(2, ""))
   mut dict file_entry = dict(6)
   file_entry["type"] = "file"
   file_entry["name"] = to_str(row.get(1, ospath.basename(path)))
   file_entry["path"] = path
   file_entry["rel"] = path
   file_entry["dir"] = false
   file_entry["kind"] = session.file_kind(path)
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

@returns_owned
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
   if _editor_get("image_preview_cache").contains(path) {
      def cached = int(_editor_get("image_preview_cache").get(path, -1))
      if cached > 0 {
         def sz = gfx.texture_size(cached)
         if is_list(sz) && int(sz.get(0, 0)) > 0 && int(sz.get(1, 0)) > 0 { return cached }
         _editor_values["image_preview_cache"] = _editor_get("image_preview_cache").delete(path)
      }
      if cached == -2 { return -1 }
   }
   def im = svg_img.load_path(path)
   if !im || !is_dict(im) {
      return -1
   }
   def tex = icons.stable_texture_id(gfx.texture_upload_image_ex(im, path, 37, false, false, 1, 33071, 33071, "editor_svg:" + path, true))
   _editor_get("image_preview_cache")[path] = tex
   tex
}

fn _image_preview_tex(str path) int {
   if path.len <= 0 { return -1 }
   if _editor_get("image_preview_cache").contains(path) {
      def cached = int(_editor_get("image_preview_cache").get(path, -1))
      if cached > 0 {
         def sz = gfx.texture_size(cached)
         if is_list(sz) && int(sz.get(0, 0)) > 0 && int(sz.get(1, 0)) > 0 { return cached }
         _editor_values["image_preview_cache"] = _editor_get("image_preview_cache").delete(path)
      }
      if cached == -2 { return -1 }
   }
   if _is_svg_path(path) { return _svg_preview_tex(path) }
   def tex = gfx.texture_load_ex(path, 37, false, true, 1, 33071, 33071, true)
   _editor_get("image_preview_cache")[path] = tex
   tex
}

fn _release_image_preview_cache() int {
   def keys = dict_keys(_editor_get("image_preview_cache"))
   mut i = 0
   while i < keys.len {
      def tex = int(_editor_get("image_preview_cache").get(keys.get(i), -1))
      if tex > 0 { gfx.texture_destroy(tex) }
      i += 1
   }
   _editor_values["image_preview_cache"] = borrow(dict(32))
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
   _fill_rect(x + pad, y + pad, w - pad * 2.0, h - pad * 2.0, C_PANE_2)
   _stroke_rect(x + pad, y + pad, w - pad * 2.0, h - pad * 2.0, C_LINE_2, 1.0)
   if tex > 0 && iw > 0.0 && ih > 0.0 {
      def f64 max_w = _clamp_f64(w * 0.46, 64.0, 1000000.0)
      def f64 max_h = _clamp_f64(h - 92.0, 64.0, 1000000.0)
      mut f64 scale = max_w / iw
      def f64 scale_h = max_h / ih
      if scale_h < scale { scale = scale_h }
      _draw_tex_rect(tex, x + pad + 12.0, y + pad + 14.0, iw * scale, ih * scale, gfx.WHITE)
   } else {
      _draw_icon("image", x + pad + 24.0, y + pad + 24.0, 44.0, C_WARN)
      _text(int(_editor_get("font_body")), "SVG preview unavailable", x + pad + 82.0, y + pad + 24.0, C_WARN)
   }
   mut lines = _svg_meta_lines(path, 0)
   if tex > 0 { lines = lines.append("texture: " + to_str(int(iw)) + "x" + to_str(int(ih))) }
   def f64 mx = x + (tex > 0 ? _clamp_f64(w * 0.53, 220.0, 1000000.0) : pad + 82.0)
   mut i = 0
   while i < lines.len && y + pad + 50.0 + float(i) * 20.0 < y + h - pad {
      def int text_cols = int(_clamp_f64((x + w - pad - mx) / 7.2, 12.0, 1000000.0))
      _text(i == 0 ? int(_editor_get("font_body")) : int(_editor_get("font_small")), widgets.preview(to_str(lines.get(i, "")), text_cols), mx, y + pad + 24.0 + float(i) * 20.0, i == 0 ? C_ACCENT_2 : C_MUTED)
      i += 1
   }
   true
}

fn _draw_svg_preview(dict lay, dict b, int tex, f64 iw, f64 ih) bool {
   def path = _buffer_source_path(b)
   if !_is_svg_path(path) { return false }
   if tex <= 0 || iw <= 0.0 || ih <= 0.0 { return _draw_svg_metadata(lay, b, tex, iw, ih) }
   def x = float(lay.get("edit_x", 0.0))
   def y = float(lay.get("edit_y", 0.0))
   def w = float(lay.get("edit_w", 0.0))
   def h = float(lay.get("edit_h", 0.0))
   def f64 pad = 12.0
   def f64 label_h = 46.0
   def f64 meta_h = 38.0
   def f64 avail_w = _clamp_f64(w - pad * 2.0, 16.0, 1000000.0)
   def f64 avail_h = _clamp_f64(h - pad * 2.0 - label_h - meta_h, 16.0, 1000000.0)
   mut f64 scale = avail_w / iw
   def f64 scale_h = avail_h / ih
   if scale_h < scale { scale = scale_h }
   def f64 draw_w = max(1.0, iw * scale)
   def f64 draw_h = max(1.0, ih * scale)
   def f64 ix = x + (w - draw_w) * 0.5
   def f64 iy = y + pad + (avail_h - draw_h) * 0.5
   _fill_rect(x + pad, y + pad, w - pad * 2.0, h - pad * 2.0, C_PANE_2)
   _stroke_rect(x + pad, y + pad, w - pad * 2.0, h - pad * 2.0, C_LINE_2, 1.0)
   _draw_tex_rect(tex, ix, iy, draw_w, draw_h, gfx.WHITE)
   def name = to_str(b.get("name", ospath.basename(path)))
   def lines = _svg_meta_lines(path, 0)
   def declared = lines.len > 4 ? to_str(lines.get(4, "")) : "SVG vector"
   def info = "rendered " + to_str(int(iw)) + "x" + to_str(int(ih)) + "  " + declared + "  " + path
   _text(int(_editor_get("font_body")), widgets.preview(name, int(_clamp_f64((w - pad * 2.0) / 9.0, 8.0, 1000000.0))), x + pad + 12.0, y + h - pad - 42.0, C_ACCENT_2)
   _text(int(_editor_get("font_small")), widgets.preview(info, int(_clamp_f64((w - pad * 2.0) / 7.0, 8.0, 1000000.0))), x + pad + 12.0, y + h - pad - 20.0, C_DIM)
   true
}

fn _image_probe_path() str {
   "build/cache/editor_image_probe.svg"
}

fn _write_image_probe_fixture() str {
   def path = _image_probe_path()
   def src = "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"96\" height=\"64\" viewBox=\"0 0 96 64\"><rect width=\"96\" height=\"64\" fill=\"#101014\"/><circle cx=\"32\" cy=\"32\" r=\"20\" fill=\"#8bdc9a\"/><path d=\"M52 14 L82 50 H52 Z\" fill=\"#ffd166\"/></svg>"
   file_write(path, src)
   path
}

fn _draw_image_preview(dict lay, dict b) bool {
   if to_str(b.get("kind", "")) != "image" { return false }
   def path = _buffer_source_path(b)
   def tex = _image_preview_tex(path)
   if _is_svg_path(path) {
      def sz = tex > 0 ? gfx.texture_size(tex) : [0, 0]
      return _draw_svg_preview(lay, b, tex, float(sz.get(0, 0)), float(sz.get(1, 0)))
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
   _text(int(_editor_get("font_body")), widgets.preview(name, int(_clamp_f64((w - pad * 2.0) / 9.0, 8.0, 1000000.0))), x + pad + 12.0, y + h - pad - 38.0, C_ACCENT_2)
   _text(int(_editor_get("font_small")), widgets.preview(info, int(_clamp_f64((w - pad * 2.0) / 7.0, 8.0, 1000000.0))), x + pad + 12.0, y + h - pad - 18.0, C_DIM)
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
   _editor_values["st"] = session.set_status(_editor_get("st"), msg)
   _editor_values["ui_state"] = interact.message(_editor_get("ui_state"), msg)
   0
}

@returns_owned
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
   mut dict op = dict(7)
   op["action"] = action
   op["src_rel"] = _cache_decode(raw, p + "src_rel")
   op["src_path"] = _cache_decode(raw, p + "src_path")
   op["trash_path"] = _cache_decode(raw, p + "trash_path")
   op["name"] = _cache_decode(raw, p + "name")
   op["dir"] = _cache_bool(raw, p + "dir", false)
   op["time"] = _cache_value(raw, p + "time", "0")
   op
}

fn _restore_project_ops(any raw) int {
   def int undo_count = _cache_int(raw, "project_op_undo_count", 0, 0, 128)
   def int redo_count = _cache_int(raw, "project_op_redo_count", 0, 0, 128)
   mut undo = []
   mut redo = []
   mut int i = 0
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
   if undo.len > 0 || redo.len > 0 { _editor_values["project_state"] = project.set_file_history(_editor_get("project_state"), undo, redo) }
   0
}

fn _cache_append_project_ops(list rows, str prefix, list ops) list {
   def int count = int(min(ops.len, 128))
   rows = rows.append(prefix + "_count=" + to_str(count))
   mut int i = 0
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
   if common.env_truthy("NYTRIX_UI_REPLAY") || probe_flags.get("BUFFER_SEARCH", false) { return false }
   cli.args().len <= 1 && !probe_flags.get("INPUT", false) && !probe_flags.get("CONTEXT", false) && !probe_flags.get("TERMINAL", false) && !probe_flags.get("TERMINAL_DUMP", false) && !probe_flags.get("SYNTAX", false) && !probe_flags.get("NAV", false) && !probe_flags.get("UNDO", false) && !probe_flags.get("ESCAPE", false) && !probe_flags.get("MODELINE", false) && !probe_flags.get("CHORD", false) && !probe_flags.get("FIND", false) && !probe_flags.get("PALETTE", false) && !probe_flags.get("POPUPS", false) && !probe_flags.get("WHICH_KEY", false) && !probe_flags.get("MULTICURSOR", false) && !probe_flags.get("ZOOM", false) && !probe_flags.get("SCROLLBAR", false) && !probe_flags.get("GOTO", false) && !probe_flags.get("GIT", false) && !probe_flags.get("TAB", false) && !probe_flags.get("SELECT", false) && !probe_flags.get("TEXTGRID", false) && !probe_flags.get("IMAGE", false) && !probe_flags.get("BENCH", false)
}

@returns_owned
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
   def bs = _editor_get("st").get("buffers", [])
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
   _cache_int(raw, "active", int(_editor_get("st").get("active", 0)), 0, bs.len - 1)
}

fn _valid_focus(str pane) bool {
   pane == "editor" || pane == "project" || pane == "terminal"
}

@returns_owned
fn _session_paths() list {
   mut out = []
   def bs = _editor_get("st").get("buffers", [])
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
      _editor_get("project_state")["open"] = open
      _editor_values["project_state"] = project.refresh(_editor_get("project_state"))
      _editor_get("st")["project_loaded"] = true
   }
   0
}

fn _restore_terminal(any raw) int {
   if !_cache_bool(raw, "term_open", false) { return 0 }
   def m = _cache_value(raw, "term_mode", "terminal")
   if m == "repl" { _editor_values["term_state"] = termpane.open_repl(_editor_get("term_state")) }
   else { _editor_values["term_state"] = termpane.open_shell(_editor_get("term_state")) }
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
         if restored.len > 0 { _editor_values["st"] = ed.state(restored) }
         _editor_get("st")["show_project"] = _cache_bool(cache, "show_project", bool(_editor_get("st").get("show_project", true)))
         _editor_get("st")["show_outline"] = _cache_bool(cache, "show_outline", bool(_editor_get("st").get("show_outline", true)))
         _editor_get("st")["show_hierarchy"] = _cache_bool(cache, "show_hierarchy", bool(_editor_get("st").get("show_hierarchy", true)))
         _editor_get("st")["show_status"] = _cache_bool(cache, "show_status", bool(_editor_get("st").get("show_status", true)))
         _editor_get("st")["show_titlebar"] = _cache_bool(cache, "show_titlebar", bool(_editor_get("st").get("show_titlebar", true)))
         _editor_get("st")["show_line_numbers"] = _cache_bool(cache, "show_line_numbers", bool(_editor_get("st").get("show_line_numbers", true)))
         _editor_get("st")["show_indent_guides"] = _cache_bool(cache, "show_indent_guides", bool(_editor_get("st").get("show_indent_guides", false)))
         _editor_get("st")["rail_w"] = _cache_float(cache, "rail_w", float(_editor_get("st").get("rail_w", 250.0)), RAIL_MIN_W, PANE_MAX)
         _editor_get("st")["rail_split"] = _cache_float(cache, "rail_split", float(_editor_get("st").get("rail_split", 0.0)), 0.0, 0.95)
         _editor_get("st")["term_h"] = _cache_float(cache, "term_h", float(_editor_get("st").get("term_h", 220.0)), DOCK_MIN_H, PANE_MAX)
         _set_editor_font_size(_cache_int(cache, "font_body_size", int(_editor_get("font_body_size")), FONT_ZOOM_MIN, FONT_ZOOM_MAX), false)
         def tab = _cache_value(cache, "rail_tab", to_str(_editor_get("st").get("rail_tab", "files")))
         if tab == "files" || tab == "git" || tab == "timeline" { _editor_get("st")["rail_tab"] = tab }
         _editor_values["st"] = ed.select_buffer(_editor_get("st"), _select_cached_active(cache))
         _editor_get("st")["cursor_line"] = _cache_int(cache, "cursor_line", int(_editor_get("st").get("cursor_line", 0)), 0, 100000000)
         _editor_get("st")["cursor_col"] = _cache_int(cache, "cursor_col", int(_editor_get("st").get("cursor_col", 0)), 0, 100000000)
         _editor_get("st")["scroll"] = _cache_int(cache, "scroll", int(_editor_get("st").get("scroll", 0)), 0, 100000000)
         _editor_get("st")["tree_scroll"] = _cache_int(cache, "tree_scroll", int(_editor_get("st").get("tree_scroll", 0)), 0, 1000000)
         _editor_get("st")["outline_scroll"] = _cache_int(cache, "outline_scroll", int(_editor_get("st").get("outline_scroll", 0)), 0, 1000000)
         _editor_get("st")["git_scroll"] = _cache_int(cache, "git_scroll", int(_editor_get("st").get("git_scroll", 0)), 0, 1000000)
         _editor_get("st")["timeline_scroll"] = _cache_int(cache, "timeline_scroll", int(_editor_get("st").get("timeline_scroll", 0)), 0, 1000000)
         def focus = _cache_value(cache, "focus", to_str(_editor_get("st").get("focus", "editor")))
         if _valid_focus(focus) { _editor_get("st")["focus"] = focus }
         _restore_terminal(cache)
         if to_str(_editor_get("st").get("focus", "editor")) != "terminal" { _editor_get("term_state")["focus"] = false }
         _editor_values["st"] = ed.clamp_cursor(_editor_get("st"))
         _editor_get("st")["scroll_follow"] = false
      }
      err(_) -> { }
   }
   0
}

fn _save_ui_cache() int {
   if !_restore_session_allowed() { return 0 }
   mut rows = [
      "show_project=" + (bool(_editor_get("st").get("show_project", true)) ? "1" : "0"),
      "show_outline=" + (bool(_editor_get("st").get("show_outline", true)) ? "1" : "0"),
      "show_hierarchy=" + (bool(_editor_get("st").get("show_hierarchy", true)) ? "1" : "0"),
      "show_status=" + (bool(_editor_get("st").get("show_status", true)) ? "1" : "0"),
      "show_titlebar=" + (bool(_editor_get("st").get("show_titlebar", true)) ? "1" : "0"),
      "show_line_numbers=" + (bool(_editor_get("st").get("show_line_numbers", true)) ? "1" : "0"),
      "show_indent_guides=" + (bool(_editor_get("st").get("show_indent_guides", false)) ? "1" : "0"),
      "rail_w=" + to_str(float(_editor_get("st").get("rail_w", 250.0))),
      "rail_split=" + to_str(float(_editor_get("st").get("rail_split", 0.0))),
      "term_h=" + to_str(float(_editor_get("st").get("term_h", 220.0))),
      "font_body_size=" + to_str(int(_editor_get("font_body_size"))),
      "rail_tab=" + to_str(_editor_get("st").get("rail_tab", "files")),
      "active=" + to_str(int(_editor_get("st").get("active", 0))),
      "active_path=" + _cache_encode(_current_path()),
      "cursor_line=" + to_str(int(_editor_get("st").get("cursor_line", 0))),
      "cursor_col=" + to_str(int(_editor_get("st").get("cursor_col", 0))),
      "scroll=" + to_str(int(_editor_get("st").get("scroll", 0))),
      "tree_scroll=" + to_str(int(_editor_get("st").get("tree_scroll", 0))),
      "outline_scroll=" + to_str(int(_editor_get("st").get("outline_scroll", 0))),
      "git_scroll=" + to_str(int(_editor_get("st").get("git_scroll", 0))),
      "timeline_scroll=" + to_str(int(_editor_get("st").get("timeline_scroll", 0))),
      "focus=" + to_str(_editor_get("st").get("focus", "editor")),
      "term_open=" + (termpane.is_open(_editor_get("term_state")) ? "1" : "0"),
      "term_mode=" + termpane.mode(_editor_get("term_state"))
   ]
   def paths = _session_paths()
   rows = rows.append("buffer_count=" + to_str(paths.len))
   mut int i = 0
   while i < paths.len {
      rows = rows.append("buffer_" + to_str(i) + "=" + _cache_encode(to_str(paths.get(i, ""))))
      i += 1
   }
   def open_map_raw = _editor_get("project_state").get("open", dict(8))
   def open_map = is_dict(open_map_raw) ? open_map_raw : dict(8)
   def open_keys = dict_keys(open_map)
   def int open_count = int(min(open_keys.len, PROJECT_OPEN_CACHE_LIMIT))
   rows = rows.append("project_open_count=" + to_str(open_count))
   i = 0
   while i < open_count {
      def rel = to_str(open_keys.get(i, ""))
      rows = rows.append("project_open_" + to_str(i) + "=" + _cache_encode(rel))
      rows = rows.append("project_open_val_" + to_str(i) + "=" + (bool(open_map.get(rel, true)) ? "1" : "0"))
      i += 1
   }
   rows = _cache_append_project_ops(rows, "project_op_undo", project.file_undo_stack(_editor_get("project_state")))
   rows = _cache_append_project_ops(rows, "project_op_redo", project.file_redo_stack(_editor_get("project_state")))
   def body = str.join(rows, "\n") + "\n"
   match file_write(EDITOR_CACHE_PATH, body) { ok(_) -> { } err(_) -> { } }
   0
}

fn _current_path() str { _buffer_source_path(ed.current_buffer(_editor_get("st"))) }

fn _current_text() str { ed.join_lines(ed.current_lines(_editor_get("st"))) }

fn _find_seed() str {
   def selected = _selected_text_all()
   if selected.len > 0 && selected.len <= 160 && !str.str_contains(selected, "\n") { return selected }
   _word_at_cursor()
}

fn _find_refresh_now() int {
   _editor_values["find_state"] = find.refresh(_editor_get("find_state"), ed.current_lines(_editor_get("st")), int(_editor_get("st").get("cursor_line", 0)), int(_editor_get("st").get("cursor_col", 0)))
   _editor_values["find_content_rev"] = int(_editor_get("content_rev"))
   0
}

fn _find_refresh_if_needed() int {
   if find.is_open(_editor_get("find_state")) && int(_editor_get("find_content_rev")) != int(_editor_get("content_rev")) { _find_refresh_now() }
   0
}

fn _find_message() str {
   def err = find.error(_editor_get("find_state"))
   err.len > 0 ? "find: bad regex" : "find " + find.summary(_editor_get("find_state"))
}

fn _find_open() int {
   def old = find.query(_editor_get("find_state"))
   def seed = old.len > 0 ? old : _find_seed()
   if seed.len > 0 { _editor_values["find_state"] = find.set_query(_editor_get("find_state"), seed) }
   _editor_values["find_state"] = find.open(_editor_get("find_state"))
   _editor_values["context_state"] = prompt.context_close(_editor_get("context_state"))
   _editor_values["palette_state"] = pal.dismiss(_editor_get("palette_state"))
   _completion_close()
   _find_refresh_now()
   _status(seed.len > 0 ? _find_message() : "find")
   0
}

fn _find_replace_open() int {
   _find_open()
   _editor_values["find_state"] = find.show_replace(_editor_get("find_state"))
   _status("replace")
   0
}

fn _find_close() int {
   _editor_values["find_state"] = find.dismiss(_editor_get("find_state"))
   _status("find closed")
   0
}

fn _find_jump_current(bool announce=true) int {
   _find_refresh_if_needed()
   if find.count(_editor_get("find_state")) <= 0 {
      if announce { _status(_find_message()) }
      return 0
   }
   def cur = find.current(_editor_get("find_state"))
   _editor_get("st")["cursor_line"] = int(cur.get("line", 0))
   _editor_get("st")["cursor_col"] = int(cur.get("start", 0))
   _clear_selections()
   _editor_values["st"] = ed.clamp_cursor(_editor_get("st"))
   _follow_cursor()
   if announce { _status(_find_message()) }
   0
}

fn _find_step(int dir) int {
   if !find.is_open(_editor_get("find_state")) { _find_open() }
   if find.query(_editor_get("find_state")).len <= 0 { _status("find") return 0 }
   _find_refresh_if_needed()
   _editor_values["find_state"] = dir < 0 ? find.prev(_editor_get("find_state")) : find.next(_editor_get("find_state"))
   _find_jump_current(true)
}

fn _find_toggle(str what) int {
   if what == "regex" { _editor_values["find_state"] = find.toggle_regex(_editor_get("find_state")) }
   elif what == "case" { _editor_values["find_state"] = find.toggle_case(_editor_get("find_state")) }
   elif what == "word" { _editor_values["find_state"] = find.toggle_word(_editor_get("find_state")) }
   elif what == "replace" { _editor_values["find_state"] = find.toggle_replace(_editor_get("find_state")) }
   _find_refresh_now()
   _status(_find_message())
   0
}

fn _find_apply_replace(dict res, bool all=false) int {
   _editor_values["find_state"] = res.get("st", _editor_get("find_state"))
   def err = to_str(res.get("error", ""))
   if err.len > 0 || !bool(res.get("ok", false)) {
      _status("replace: " + (err.len > 0 ? err : "failed"))
      return 0
   }
   def n = int(res.get("count", 0))
   if n <= 0 { _status("replace: no matches") return 0 }
   _editor_values["hist"] = history.push(_editor_get("hist"), _editor_get("st"))
   _editor_values["st"] = ed.set_lines(_editor_get("st"), res.get("lines", ed.current_lines(_editor_get("st"))))
   _editor_get("st")["cursor_line"] = int(res.get("line", int(_editor_get("st").get("cursor_line", 0))))
   _editor_get("st")["cursor_col"] = int(res.get("col", int(_editor_get("st").get("cursor_col", 0))))
   _clear_selections()
   _editor_values["st"] = ed.clamp_cursor(_editor_get("st"))
   _touch_content()
   _find_refresh_now()
   _follow_cursor()
   _status(all ? ("replaced " + to_str(n)) : "replaced")
   0
}

fn _find_replace_current() int {
   if !find.is_open(_editor_get("find_state")) { _find_replace_open() }
   _find_refresh_now()
   _find_apply_replace(find.replace_current(_editor_get("find_state"), ed.current_lines(_editor_get("st"))), false)
}

fn _find_replace_all() int {
   if !find.is_open(_editor_get("find_state")) { _find_replace_open() }
   _find_refresh_now()
   _find_apply_replace(find.replace_all(_editor_get("find_state"), ed.current_lines(_editor_get("st"))), true)
}

fn _project_relative_path(str path) str {
   if path.len <= 0 { return "" }
   def root = str.str_replace(ospath.normalize(project.root(_editor_get("project_state"))), "\\", "/")
   def p = str.str_replace(ospath.normalize(path), "\\", "/")
   def prefix = root + "/"
   str.startswith(p, prefix) ? str.str_slice(p, prefix.len, p.len) : p
}

fn _copy_relative_path(str path) int {
   def rel = _project_relative_path(path)
   if rel.len <= 0 { _status("no path") return 0 }
   window.set_clipboard(_editor_get("win"), rel)
   _status("copied relative path")
   0
}

fn _lsp_sync_current(bool force=false) int {
   if !lsp.enabled_by_default() { return 0 }
   def b = ed.current_buffer(_editor_get("st"))
   if b.get("readonly", false) {
      if _editor_get("lsp_state").get("active", false) { _editor_values["lsp_state"] = lsp.stop(_editor_get("lsp_state")) }
      return 0
   }
   def path = _current_path()
   if path.len <= 0 { return 0 }
   def cur_path = to_str(_editor_get("lsp_state").get("path", ""))
   if !force && _editor_get("lsp_state").get("active", false) && cur_path == path { return 0 }
   _editor_values["lsp_state"] = lsp.start(_editor_get("lsp_state"), path, _current_text())
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
   if !bool(_editor_get("st").get("project_loaded", false)) {
      _editor_values["project_state"] = project.refresh(_editor_get("project_state"))
      _editor_get("st")["project_loaded"] = true
      _invalidate_project_definitions()
   }
   0
}

fn _ensure_project_git() int {
   if !bool(_editor_get("st").get("git_loaded", false)) {
      _editor_values["project_state"] = project.refresh_status(_editor_get("project_state"))
      _editor_get("st")["git_loaded"] = true
   }
   0
}

fn _nav_location() list {
   [
      "",
      "",
      int(_editor_get("st").get("active", 0)),
      int(_editor_get("st").get("cursor_line", 0)),
      int(_editor_get("st").get("cursor_col", 0)),
      int(_editor_get("st").get("scroll", 0))
   ]
}

fn _nav_record_current() int {
   _editor_values["nav_live_loc"] = _nav_location()
   _editor_values["nav_live_idx"] = int(_editor_get("st").get("active", 0))
   0
}

fn _nav_push_current() int {
   _editor_values["nav_prev_idx"] = int(_editor_get("nav_live_idx"))
   _editor_values["nav_next_idx"] = -1
   0
}

fn _nav_push_current_if_target_diff(str target_path) int {
   _nav_push_current()
}

fn _nav_go(int dir) int {
   if dir < 0 {
      if int(_editor_get("nav_prev_idx")) < 0 { _status("no previous location") return 0 }
      def target = int(_editor_get("nav_prev_idx"))
      _editor_values["nav_next_idx"] = int(_editor_get("nav_live_idx"))
      _editor_values["nav_prev_idx"] = -1
      _editor_values["st"] = ed.select_buffer(_editor_get("st"), target)
      _buffer_changed(true)
      _editor_values["nav_live_idx"] = target
      _status("back")
      return 0
   }
   if int(_editor_get("nav_next_idx")) < 0 { _status("no next location") return 0 }
   def target2 = int(_editor_get("nav_next_idx"))
   _editor_values["nav_prev_idx"] = int(_editor_get("nav_live_idx"))
   _editor_values["nav_next_idx"] = -1
   _editor_values["st"] = ed.select_buffer(_editor_get("st"), target2)
   _buffer_changed(true)
   _editor_values["nav_live_idx"] = target2
   _status("forward")
   0
}

fn _extra_cursors() list { _editor_get("st").get("extra_cursors", []) }

fn _extra_cursor_count() int { _extra_cursors().len }

fn _cursor_row(any c) int { is_list(c) ? int(c.get(0, 0)) : int(c.get("line", 0)) }

fn _cursor_col(any c) int { is_list(c) ? int(c.get(1, 0)) : int(c.get("col", 0)) }

fn _cursor_make(int row, int col, bool primary=false) dict {
   mut dict cursor = dict(3)
   cursor["line"] = row
   cursor["col"] = col
   cursor["primary"] = primary
   cursor
}

fn _cursor_primary(any c) bool { is_dict(c) && bool(c.get("primary", false)) }

@returns_owned
fn _cursor_sorted_desc(list cursors) list {
   if cursors.len <= 1 { return cursors }
   mut keyed = []
   mut i = 0
   while i < cursors.len {
      def c = cursors.get(i)
      keyed = keyed.append([-_cursor_row(c), -_cursor_col(c), i, c])
      i += 1
   }
   sort(keyed)
   mut out = []
   i = 0
   while i < keyed.len {
      out = out.append(keyed.get(i, [0, 0, 0, []]).get(3, []))
      i += 1
   }
   out
}

@returns_owned
fn _all_cursors_desc() list {
   def lines = ed.current_lines(_editor_get("st"))
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
   def prow = min(max(int(_editor_get("st").get("cursor_line", 0)), 0), max_row)
   def pcol = min(max(int(_editor_get("st").get("cursor_col", 0)), 0), to_str(lines.get(prow, "")).len)
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
         _editor_get("st")["cursor_line"] = row
         _editor_get("st")["cursor_col"] = col
         saw_primary = true
      } else {
         extras = extras.append([row, col])
      }
      i += 1
   }
   if !saw_primary && cursors.len > 0 {
      def first = cursors.get(0)
      _editor_get("st")["cursor_line"] = max(0, _cursor_row(first))
      _editor_get("st")["cursor_col"] = max(0, _cursor_col(first))
   }
   _editor_get("st")["extra_cursors"] = extras
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
   def lines = ed.current_lines(_editor_get("st"))
   mut out = []
   def extras = _extra_cursors()
   mut i = 0
   while i < extras.len {
      def old = extras.get(i)
      mut c = [_cursor_row(old) + drow, _cursor_col(old) + dcol]
      c = _clamp_extra_cursor(c, lines)
      if _cursor_row(c) != int(_editor_get("st").get("cursor_line", 0)) && !_cursor_at_row(out, _cursor_row(c)) {
         out = out.append(c)
      }
      i += 1
   }
   _editor_get("st")["extra_cursors"] = out
   0
}

@returns_owned
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

@returns_owned
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

@returns_owned
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
   mut row = int(_editor_get("st").get("cursor_line", 0))
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
   _editor_get("st")["extra_cursors"] = []
   _editor_get("st")["multi_selects"] = []
   if announce { _status("single cursor") }
   0
}

fn _add_extra_cursor(int dir) int {
   def lines = ed.current_lines(_editor_get("st"))
   if lines.len <= 0 { return 0 }
   def step = dir < 0 ? -1 : 1
   def primary_row = int(_editor_get("st").get("cursor_line", 0))
   mut row = _cursor_stack_edge(step) + step
   def cursors = _extra_cursors()
   while row >= 0 && row < lines.len && (row == primary_row || _cursor_at_row(cursors, row)) {
      row += step
   }
   if row < 0 || row >= lines.len {
      _status("no more lines")
      return 0
   }
   def col = min(max(int(_editor_get("st").get("cursor_col", 0)), 0), to_str(lines.get(row, "")).len)
   _add_extra_cursor_at(row, col)
}

fn _add_extra_cursor_at(int row, int col) int {
   def lines = ed.current_lines(_editor_get("st"))
   if lines.len <= 0 { return 0 }
   def int target_row = int(min(max(row, 0), lines.len - 1))
   def int target_col = int(min(max(col, 0), to_str(lines.get(target_row, "")).len))
   mut cursors = _extra_cursors()
   if target_row == int(_editor_get("st").get("cursor_line", 0)) || _cursor_at_row(cursors, target_row) {
      _status("cursor already on line")
      return 0
   }
   cursors = cursors.append([target_row, target_col])
   _editor_get("st")["extra_cursors"] = cursors
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

@returns_owned
fn _ranges_sorted_desc(list ranges) list {
   if ranges.len <= 1 { return ranges }
   mut keyed = []
   mut int i = 0
   while i < ranges.len {
      def r = _range_norm(ranges.get(i, []))
      keyed = keyed.append([-int(r.get(0, 0)), -int(r.get(1, 0)), i, r])
      i += 1
   }
   sort(keyed)
   mut out = []
   i = 0
   while i < keyed.len {
      out = out.append(keyed.get(i, [0, 0, 0, []]).get(3, []))
      i += 1
   }
   out
}

fn _multi_selection_valid() bool {
   def ranges = _editor_get("st").get("multi_selects", [])
   mut i = 0
   while i < ranges.len {
      if _range_valid(ranges.get(i, [])) { return true }
      i += 1
   }
   false
}

fn _selection_any_valid() bool {
   _multi_selection_valid() || ed.selection_valid(_editor_get("st"))
}

@returns_owned
fn _selection_ranges_active() list {
   if _multi_selection_valid() {
      mut out = []
      def ranges = _editor_get("st").get("multi_selects", [])
      mut i = 0
      while i < ranges.len {
         def r = ranges.get(i, [])
         if _range_valid(r) { out = out.append(_range_norm(r)) }
         i += 1
      }
      return out
   }
   if ed.selection_valid(_editor_get("st")) {
      return [_range_norm(_range_make(
               int(_editor_get("st").get("sel_a_line", 0)), int(_editor_get("st").get("sel_a_col", 0)),
               int(_editor_get("st").get("sel_b_line", 0)), int(_editor_get("st").get("sel_b_col", 0)),
               true
      ))]
   }
   []
}

fn _multi_selection_anchor_if_needed() int {
   if _editor_get("st").get("multi_selects", []).len > 0 { return 0 }
   mut ranges = [_range_make(
         int(_editor_get("st").get("cursor_line", 0)), int(_editor_get("st").get("cursor_col", 0)),
         int(_editor_get("st").get("cursor_line", 0)), int(_editor_get("st").get("cursor_col", 0)),
         true
   )]
   def extras = _extra_cursors()
   mut i = 0
   while i < extras.len {
      def c = extras.get(i, [])
      ranges = ranges.append(_range_make(_cursor_row(c), _cursor_col(c), _cursor_row(c), _cursor_col(c), false))
      i += 1
   }
   _editor_get("st")["multi_selects"] = ranges
   _editor_get("st")["sel_a_line"] = int(_editor_get("st").get("cursor_line", 0))
   _editor_get("st")["sel_a_col"] = int(_editor_get("st").get("cursor_col", 0))
   _editor_get("st")["sel_b_line"] = int(_editor_get("st").get("cursor_line", 0))
   _editor_get("st")["sel_b_col"] = int(_editor_get("st").get("cursor_col", 0))
   _editor_get("st")["sel_active"] = false
   _editor_get("st")["drag_select"] = false
   0
}

fn _multi_selection_update() int {
   def ranges = _editor_get("st").get("multi_selects", [])
   if ranges.len <= 0 { return 0 }
   mut out = []
   mut idx = 0
   if ranges.len > 0 {
      def r = ranges.get(0, [])
      out = out.append(_range_make(
            int(r.get(0, 0)), int(r.get(1, 0)),
            int(_editor_get("st").get("cursor_line", 0)), int(_editor_get("st").get("cursor_col", 0)),
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
   _editor_get("st")["multi_selects"] = out
   if out.len > 0 {
      def p = out.get(0, [])
      _editor_get("st")["sel_a_line"] = int(p.get(0, 0))
      _editor_get("st")["sel_a_col"] = int(p.get(1, 0))
      _editor_get("st")["sel_b_line"] = int(p.get(2, 0))
      _editor_get("st")["sel_b_col"] = int(p.get(3, 0))
      _editor_get("st")["sel_active"] = _range_valid(p)
   }
   0
}

;; Emacs mark/region (transient-mark-mode): `set-mark` drops the mark at
;; point; while it is active the region is mark..point and follows every
;; cursor motion; C-g, copy, cut, and typing deactivate it.
fn _set_mark() int {
   def st = _editor_get("st")
   st["mark_line"] = int(st.get("cursor_line", 0))
   st["mark_col"] = int(st.get("cursor_col", 0))
   st["mark_buffer"] = int(st.get("active", 0))
   st["mark_active"] = true
   st["multi_selects"] = []
   _sync_mark_region()
   _status("Mark set")
   0
}

fn _exchange_mark() int {
   def st = _editor_get("st")
   if int(st.get("mark_buffer", -1)) != int(st.get("active", 0)) { _status("No mark set in this buffer") return 0 }
   def ml = int(st.get("mark_line", 0))
   def mc = int(st.get("mark_col", 0))
   st["mark_line"] = int(st.get("cursor_line", 0))
   st["mark_col"] = int(st.get("cursor_col", 0))
   st["cursor_line"] = ml
   st["cursor_col"] = mc
   st["mark_active"] = true
   _sync_mark_region()
   0
}

fn _deactivate_mark() int {
   def st = _editor_get("st")
   if bool(st.get("mark_active", false)) {
      st["mark_active"] = false
      st["sel_active"] = false
   }
   0
}

fn _sync_mark_region() int {
   def st = _editor_get("st")
   if !bool(st.get("mark_active", false)) { return 0 }
   ;; The mark belongs to one buffer; switching away deactivates it.
   if int(st.get("mark_buffer", -1)) != int(st.get("active", 0)) {
      st["mark_active"] = false
      return 0
   }
   st["sel_a_line"] = int(st.get("mark_line", 0))
   st["sel_a_col"] = int(st.get("mark_col", 0))
   st["sel_b_line"] = int(st.get("cursor_line", 0))
   st["sel_b_col"] = int(st.get("cursor_col", 0))
   st["sel_active"] = true
   0
}

fn _clear_selections() int {
   _editor_get("st")["mark_active"] = false
   _editor_get("st")["sel_active"] = false
   _editor_get("st")["drag_select"] = false
   _editor_get("st")["multi_selects"] = []
   0
}

fn _range_text(list lines, any raw) str {
   def r = _range_norm(raw)
   def int sl = int(min(max(int(r.get(0, 0)), 0), max(0, lines.len - 1)))
   def int el = int(min(max(int(r.get(2, 0)), sl), max(0, lines.len - 1)))
   def int sc = int(max(0, int(r.get(1, 0))))
   def int ec = int(max(0, int(r.get(3, 0))))
   mut out = []
   mut int i = sl
   while i <= el && i < lines.len {
      def line = to_str(lines.get(i, ""))
      def int a = i == sl ? min(sc, line.len) : 0
      def int b = i == el ? min(ec, line.len) : line.len
      out = out.append(str.str_slice(line, a, max(a, b)))
      i += 1
   }
   ed.join_lines(out)
}

fn _selected_text_all() str {
   def ranges = _selection_ranges_active()
   if ranges.len <= 0 { return "" }
   def lines = ed.current_lines(_editor_get("st"))
   mut out = []
   mut int i = 0
   while i < ranges.len {
      out = out.append(_range_text(lines, ranges.get(i, [])))
      i += 1
   }
   str.join(out, "\n")
}

fn _delete_range_from_lines(list lines, any raw) dict {
   def r = _range_norm(raw)
   def int sl = int(min(max(int(r.get(0, 0)), 0), max(0, lines.len - 1)))
   def int el = int(min(max(int(r.get(2, 0)), sl), max(0, lines.len - 1)))
   def first = to_str(lines.get(sl, ""))
   def last = to_str(lines.get(el, ""))
   def int sc = int(min(max(int(r.get(1, 0)), 0), first.len))
   def int ec = int(min(max(int(r.get(3, 0)), 0), last.len))
   mut out = []
   mut int i = 0
   while i < sl { out = out.append(lines.get(i, "")) i += 1 }
   out = out.append(str.str_slice(first, 0, sc) + str.str_slice(last, ec, last.len))
   i = el + 1
   while i < lines.len { out = out.append(lines.get(i, "")) i += 1 }
   if out.len <= 0 { out = [""] }
   mut dict result = dict(3)
   result["lines"] = out
   result["row"] = sl
   result["col"] = sc
   result
}

fn _insert_text_in_lines(list lines, int row, int col, str text) dict {
   row = int(min(max(row, 0), max(0, lines.len - 1)))
   def line = to_str(lines.get(row, ""))
   col = int(min(max(col, 0), line.len))
   def list parts = ed.split_lines(text)
   if parts.len <= 1 {
      lines[row] = ed.line_insert(line, col, text)
      mut dict result = dict(3)
      result["lines"] = lines
      result["row"] = row
      result["col"] = col + text.len
      return result
   }
   def before = str.str_slice(line, 0, col)
   def after = str.str_slice(line, col, line.len)
   mut out = []
   mut int i = 0
   while i < row { out = out.append(lines.get(i, "")) i += 1 }
   out = out.append(before + to_str(parts.get(0, "")))
   i = 1
   while i < parts.len - 1 { out = out.append(parts.get(i, "")) i += 1 }
   def tail = to_str(parts.get(parts.len - 1, ""))
   out = out.append(tail + after)
   i = row + 1
   while i < lines.len { out = out.append(lines.get(i, "")) i += 1 }
   mut dict result = dict(3)
   result["lines"] = out
   result["row"] = row + parts.len - 1
   result["col"] = tail.len
   result
}

fn _replace_multi_selections(str text) int {
   def ranges = _ranges_sorted_desc(_selection_ranges_active())
   if ranges.len <= 0 { return 0 }
   _editor_values["hist"] = history.push(_editor_get("hist"), _editor_get("st"))
   mut lines = ed.current_lines(_editor_get("st"))
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
   _editor_values["st"] = ed.set_lines(_editor_get("st"), lines)
   _set_processed_cursors(cursors)
   _clear_selections()
   _editor_values["st"] = ed.clamp_cursor(_editor_get("st"))
   _follow_cursor()
   _touch_content()
   _lsp_mark_dirty()
   0
}

fn _edit_insert_extra(str text) int {
   if text.len <= 0 { return 0 }
   if _selection_any_valid() { return _replace_multi_selections(text) }
   if _extra_cursor_count() <= 0 {
      return _apply_hist(history.insert_text(_editor_get("hist"), _editor_get("st"), text))
   }
   _editor_values["hist"] = history.push(_editor_get("hist"), _editor_get("st"))
   mut lines = ed.current_lines(_editor_get("st"))
   def cursors = _all_cursors_desc()
   mut updated = []
   mut int i = 0
   while i < cursors.len {
      def int row = int(min(max(_cursor_row(cursors.get(i)), 0), max(0, lines.len - 1)))
      def line = to_str(lines.get(row, ""))
      def int col = int(min(max(_cursor_col(cursors.get(i)), 0), line.len))
      def list parts = ed.split_lines(text)
      if parts.len <= 1 {
         lines[row] = ed.line_insert(line, col, text)
         updated = _adjust_processed_insert(updated, row, col, 0, 0, text.len)
         updated = updated.append(_cursor_make(row, col + text.len, _cursor_primary(cursors.get(i))))
      } else {
         def before = str.str_slice(line, 0, col)
         def after = str.str_slice(line, col, line.len)
         mut out = []
         mut int j = 0
         while j < row { out = out.append(lines.get(j, "")) j += 1 }
         out = out.append(before + to_str(parts.get(0, "")))
         j = 1
         while j < parts.len - 1 { out = out.append(parts.get(j, "")) j += 1 }
         def tail = to_str(parts.get(parts.len - 1, ""))
         out = out.append(tail + after)
         j = row + 1
         while j < lines.len { out = out.append(lines.get(j, "")) j += 1 }
         lines = out
         def int delta = parts.len - 1
         updated = _adjust_processed_insert(updated, row, col, delta, tail.len, text.len)
         updated = updated.append(_cursor_make(row + delta, tail.len, _cursor_primary(cursors.get(i))))
      }
      i += 1
   }
   _editor_values["st"] = ed.set_lines(_editor_get("st"), lines)
   _set_processed_cursors(updated)
   _editor_values["st"] = ed.clamp_cursor(_editor_get("st"))
   _follow_cursor()
   _touch_content()
   0
}

fn _edit_backspace_extra() int {
   if _selection_any_valid() { return _replace_multi_selections("") }
   if _extra_cursor_count() <= 0 { return _apply_hist(history.backspace(_editor_get("hist"), _editor_get("st"))) }
   _editor_values["hist"] = history.push(_editor_get("hist"), _editor_get("st"))
   mut lines = ed.current_lines(_editor_get("st"))
   def cursors = _all_cursors_desc()
   mut updated = []
   mut int i = 0
   while i < cursors.len {
      def int row = int(min(max(_cursor_row(cursors.get(i)), 0), max(0, lines.len - 1)))
      def line = to_str(lines.get(row, ""))
      def int col = int(min(max(_cursor_col(cursors.get(i)), 0), line.len))
      if col > 0 {
         lines[row] = ed.line_delete_before(line, col)
         updated = _adjust_processed_backspace(updated, row, col, 0, 0)
         updated = updated.append(_cursor_make(row, col - 1, _cursor_primary(cursors.get(i))))
      } elif row > 0 {
         def prev = to_str(lines.get(row - 1, ""))
         mut out = []
         mut int j = 0
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
   _editor_values["st"] = ed.set_lines(_editor_get("st"), lines)
   _set_processed_cursors(updated)
   _editor_values["st"] = ed.clamp_cursor(_editor_get("st"))
   _follow_cursor()
   _touch_content()
   0
}

fn _edit_delete_extra() int {
   if _selection_any_valid() { return _replace_multi_selections("") }
   if _extra_cursor_count() <= 0 { return _apply_hist(history.delete_char(_editor_get("hist"), _editor_get("st"))) }
   _editor_values["hist"] = history.push(_editor_get("hist"), _editor_get("st"))
   mut lines = ed.current_lines(_editor_get("st"))
   def cursors = _all_cursors_desc()
   mut updated = []
   mut int i = 0
   while i < cursors.len {
      def int row = int(min(max(_cursor_row(cursors.get(i)), 0), max(0, lines.len - 1)))
      def line = to_str(lines.get(row, ""))
      def int col = int(min(max(_cursor_col(cursors.get(i)), 0), line.len))
      if col < line.len {
         lines[row] = str.str_slice(line, 0, col) + str.str_slice(line, col + 1, line.len)
         updated = _adjust_processed_delete(updated, row, col, 0, 0)
      } elif row + 1 < lines.len {
         mut out = []
         mut int j = 0
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
   _editor_values["st"] = ed.set_lines(_editor_get("st"), lines)
   _set_processed_cursors(updated)
   _editor_values["st"] = ed.clamp_cursor(_editor_get("st"))
   _follow_cursor()
   _touch_content()
   0
}

fn _apply_hist(dict res) int {
   _editor_values["hist"] = res.get("hist", _editor_get("hist"))
   _editor_values["st"] = res.get("st", _editor_get("st"))
   _follow_cursor()
   _touch_content()
   _lsp_mark_dirty()
   def msg = to_str(res.get("status", ""))
   if msg.len > 0 { _status(msg) }
   0
}

fn _undo() int { _apply_hist(history.undo(_editor_get("hist"), _editor_get("st"))) }

fn _redo() int { _apply_hist(history.redo(_editor_get("hist"), _editor_get("st"))) }

fn _edit_insert(str text) int { _edit_insert_extra(text) }

fn _edit_newline() int {
   if _selection_any_valid() { return _replace_multi_selections("\n") }
   if _extra_cursor_count() <= 0 { return _apply_hist(history.newline(_editor_get("hist"), _editor_get("st"))) }
   _edit_insert_extra("\n")
}

fn _edit_backspace() int { _edit_backspace_extra() }

fn _edit_delete() int { _edit_delete_extra() }

fn _open_file(str path) int {
   _nav_push_current_if_target_diff(path)
   _editor_values["st"] = session.open_file(_editor_get("st"), path)
   _buffer_changed(true)
   _nav_record_current()
   0
}

fn _project_entry_at(dict lay, f64 mx, f64 my) dict {
   if !bool(_editor_get("st").get("show_hierarchy", true)) { return dict(0) }
   if _rail_tab() != "files" || !bool(_editor_get("st").get("show_project", true)) { return dict(0) }
   _ensure_project_tree()
   def rail_x = float(lay.get("rail_x", 0.0))
   def rail_y = float(lay.get("rail_y", 0.0))
   def rail_w = float(lay.get("rail_w", 0.0))
   def rail_h = float(lay.get("rail_h", 0.0))
   if rail_w <= 1.0 { return dict(0) }
   def body_y = rail_y + RAIL_TABS_H
   def body_h = rail_h - RAIL_TABS_H
   def rows = project.tree(_editor_get("project_state"))
   def show_outline = bool(_editor_get("st").get("show_outline", true))
   def syms = show_outline ? _outline_symbols() : []
   def tree_h = show_outline ? _rail_tree_height(body_h, rows.len, syms.len) : body_h
   project.hit_entry(_editor_get("project_state"), rail_x, body_y, rail_w, tree_h, int(_editor_get("st").get("tree_scroll", 0)), mx, my)
}

fn _project_rel(dict entry) str { to_str(entry.get("rel", "")) }

fn _project_selection_contains_rel(str rel) bool {
   if rel.len <= 0 { return false }
   mut i = 0
   while i < _editor_get("project_selection").len {
      if to_str(_editor_get("project_selection").get(i, "")) == rel { return true }
      i += 1
   }
   false
}

fn _project_selection_contains(dict entry) bool {
   _project_selection_contains_rel(_project_rel(entry))
}

fn _project_selection_status() int {
   def n = _editor_get("project_selection").len
   _status(n <= 0 ? "selection cleared" : (to_str(n) + " selected"))
}

@consumes(xs)
@forgets(xs)
fn _project_selection_replace(list xs) int {
   _editor_values["project_selection"] = borrow(xs)
   forget(xs)
   0
}

fn _project_selection_set(dict entry) int {
   def rel = _project_rel(entry)
   if rel.len <= 0 { return 0 }
   _project_selection_replace([rel])
   _editor_values["project_selection_anchor"] = rel
   0
}

fn _project_selection_toggle(dict entry) int {
   def rel = _project_rel(entry)
   if rel.len <= 0 { return 0 }
   mut out = []
   mut found = false
   mut i = 0
   while i < _editor_get("project_selection").len {
      def cur = to_str(_editor_get("project_selection").get(i, ""))
      if cur == rel { found = true }
      else { out = out.append(cur) }
      i += 1
   }
   if !found { out = out.append(rel) _editor_values["project_selection_anchor"] = rel }
   _project_selection_replace(out)
   _project_selection_status()
}

fn _project_tree_index(str rel) int {
   def rows = project.tree(_editor_get("project_state"))
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
   if to_str(_editor_get("project_selection_anchor")).len <= 0 { _editor_values["project_selection_anchor"] = rel }
   def a = _project_tree_index(to_str(_editor_get("project_selection_anchor")))
   def b = _project_tree_index(rel)
   if a < 0 || b < 0 {
      _project_selection_set(entry)
      _project_selection_status()
      return 0
   }
   def int lo = int(min(a, b))
   def int hi = int(max(a, b))
   def rows = project.tree(_editor_get("project_state"))
   mut out = []
   mut int i = lo
   while i <= hi && i < rows.len {
      def r = _project_rel(rows.get(i, {}))
      if r.len > 0 { out = out.append(r) }
      i += 1
   }
   _project_selection_replace(out)
   _project_selection_status()
}

@returns_owned
fn _project_selected_entries() list {
   def rows = project.tree(_editor_get("project_state"))
   mut out = []
   mut i = 0
   while i < rows.len {
      def e = rows.get(i, {})
      if is_dict(e) && _project_selection_contains(e) { out = out.append(e) }
      i += 1
   }
   out
}

@returns_owned
fn _project_action_entries(dict entry) list {
   def e = _project_entry_or_current(entry)
   if !is_dict(e) || e.len <= 0 { return [] }
   def scope = to_str(e.get("scope", "file"))
   if _editor_get("project_selection").len > 0 && (scope == "file" || scope.len <= 0 || _project_selection_contains(e)) {
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

@returns_owned
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
   window.set_clipboard(_editor_get("win"), str.join(vals, "\n"))
   _status(vals.len == 1 ? (relative ? "copied relative path" : "copied path") : ("copied " + to_str(vals.len) + " paths"))
}

fn _open_or_toggle_project_row(dict entry) int {
   def res = session.open_or_toggle_project_row(_editor_get("st"), _editor_get("project_state"), entry)
   _editor_values["st"] = res.get("st", _editor_get("st"))
   _editor_values["project_state"] = res.get("project", _editor_get("project_state"))
   _buffer_changed(!entry.get("dir", false))
   if !entry.get("dir", false) { _nav_record_current() }
   0
}

fn _new_file_path(dict entry, str name) str {
   def base = to_str(entry.get("path", project.root(_editor_get("project_state"))))
   def parent = entry.get("dir", false) ? base : ospath.dirname(base)
   ospath.join(parent, str.strip(name))
}

fn _start_new_file(dict entry) int {
   if !is_dict(entry) || entry.len <= 0 {
      entry = session.project_entry_for_path(_editor_get("project_state"), _current_path())
   }
   if !is_dict(entry) || entry.len <= 0 {
      mut dict root_entry = dict(4)
      root_entry["name"] = ospath.basename(project.root(_editor_get("project_state")))
      root_entry["path"] = project.root(_editor_get("project_state"))
      root_entry["rel"] = "."
      root_entry["dir"] = true
      entry = root_entry
   }
   entry["action"] = "new-file"
   entry["name"] = "untitled.ny"
   _editor_values["rename_state"] = prompt.rename_start(_editor_get("rename_state"), entry)
   _editor_values["palette_state"] = pal.dismiss(_editor_get("palette_state"))
   _editor_values["context_state"] = prompt.context_close(_editor_get("context_state"))
   _status("new file")
   0
}

fn _start_rename(dict entry) int {
   if !is_dict(entry) || entry.len <= 0 { entry = session.project_entry_for_path(_editor_get("project_state"), to_str(ed.current_buffer(_editor_get("st")).get("path", ""))) }
   if !is_dict(entry) || to_str(entry.get("rel", "")).len <= 0 || to_str(entry.get("rel", "")) == "." {
      _status("select a file first")
      return 0
   }
   _editor_values["rename_state"] = prompt.rename_start(_editor_get("rename_state"), entry)
   _editor_values["palette_state"] = pal.dismiss(_editor_get("palette_state"))
   _editor_values["context_state"] = prompt.context_close(_editor_get("context_state"))
   _status("rename")
   0
}

fn _finish_rename() int {
   if !prompt.rename_is_open(_editor_get("rename_state")) { return 0 }
   def entry = prompt.rename_entry(_editor_get("rename_state"))
   def action = to_str(entry.get("action", "rename"))
   def text = prompt.rename_text(_editor_get("rename_state"))
   if action == "symbol" {
      _editor_values["rename_state"] = prompt.rename_close(_editor_get("rename_state"))
      if text.len > 0 && text != to_str(entry.get("name", "")) { _lsp_rename(text) }
      return 0
   }
   def created_path = action == "new-file" ? _new_file_path(entry, text) : ""
   if action == "new-file" {
      _editor_values["project_state"] = project.create_file(_editor_get("project_state"), prompt.rename_rel(_editor_get("rename_state")), text)
   } else {
      _editor_values["project_state"] = project.rename_entry(_editor_get("project_state"), prompt.rename_rel(_editor_get("rename_state")), text)
   }
   _invalidate_project_definitions()
   def err = to_str(_editor_get("project_state").get("last_error", ""))
   _editor_values["rename_state"] = prompt.rename_close(_editor_get("rename_state"))
   if err.len > 0 { _status(err) }
   else {
      if action == "new-file" && created_path.len > 0 { _open_file(created_path) }
      _status(action == "new-file" ? "created file" : "renamed")
   }
   0
}

fn _context_action(str id) int {
   def entry = prompt.context_entry(_editor_get("context_state"))
   _editor_values["context_state"] = prompt.context_close(_editor_get("context_state"))
   def scope = to_str(entry.get("scope", "file"))
   def path = to_str(entry.get("path", ""))
   if id == "open" {
      if scope == "editor" {
         if path.len > 0 { _open_file(path) }
      } elif scope == "outline" {
         _editor_get("st")["cursor_line"] = int(entry.get("line", 0))
         _editor_get("st")["cursor_col"] = 0
         _editor_values["st"] = ed.clamp_cursor(_editor_get("st"))
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
         def hit = session.project_entry_for_path(_editor_get("project_state"), _current_path())
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
      _dap_start()
   }
   elif id == "debug-break_point" {
      if scope != "editor" {
         if entry.get("dir", false) { _status("select a file for break_point") return 0 }
         _open_file(path)
      }
      _dap_toggle_breakpoint()
   }
   elif id == "diff-clipboard" {
      if scope != "editor" {
         if entry.get("dir", false) { _status("select a file to diff") return 0 }
         _open_file(path)
      }
      _diff_clipboard()
   }
   elif id == "rename" { _start_rename(entry) }
   elif id == "refresh" { _editor_values["project_state"] = project.refresh_git(_editor_get("project_state")) _editor_get("st")["project_loaded"] = true _editor_get("st")["git_loaded"] = true _invalidate_project_definitions() _status("project refreshed") }
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
   elif id == "terminal-shell" { _editor_values["term_state"] = termpane.show_shell(_editor_get("term_state")) _set_focus("terminal") }
   elif id == "terminal-repl" { _editor_values["term_state"] = termpane.show_repl(_editor_get("term_state")) _set_focus("terminal") }
   elif id == "terminal-next" { _editor_values["term_state"] = termpane.next_tab(_editor_get("term_state")) _set_focus("terminal") }
   elif id == "terminal-prev" { _editor_values["term_state"] = termpane.prev_tab(_editor_get("term_state")) _set_focus("terminal") }
   elif id == "terminal-close" { _editor_values["term_state"] = termpane.close_tab(_editor_get("term_state")) _set_focus(termpane.is_open(_editor_get("term_state")) ? "terminal" : "editor") }
   elif id == "pane-editor" { _set_focus("editor") }
   0
}

fn _run_current_file() int {
   def path = _current_path()
   def selected = _selected_text_all()
   if selected.len > 0 {
      _editor_values["st"] = session.run_text(_editor_get("st"), path, selected, "selection")
   } else {
      def sec = runner.section_at(ed.current_lines(_editor_get("st")), int(_editor_get("st").get("cursor_line", 0)))
      if sec.get("found", false) && str.strip(to_str(sec.get("text", ""))).len > 0 {
         _editor_values["st"] = session.run_text(_editor_get("st"), path, to_str(sec.get("text", "")), "section")
      } else {
         _editor_values["st"] = session.run_current_file(_editor_get("st"))
      }
   }
   _follow_cursor()
   _touch_content()
   0
}

fn _diff_clipboard() int {
   def clip = window.get_clipboard(_editor_get("win"))
   if clip.len <= 0 { _status("clipboard empty") return 0 }
   def b = ed.current_buffer(_editor_get("st"))
   def name = to_str(b.get("name", "buffer"))
   def text = ed.join_lines(ed.current_lines(_editor_get("st")))
   def out = eddiff.side_by_side(text, clip, name, "clipboard", 68)
   _editor_values["st"] = session.set_output_buffer(_editor_get("st"), "*diff: " + name + " clipboard*", out)
   _mark_current_output("diff", _buffer_source_path(b))
   _follow_cursor()
   _touch_content()
   _status("diff clipboard")
   0
}

fn _mark_current_output(str kind, str source_path="") int {
   mut bs = _editor_get("st").get("buffers", [])
   mut b = ed.current_buffer(_editor_get("st"))
   b["readonly"] = true
   b["kind"] = kind
   if source_path.len > 0 { b["source_path"] = source_path }
   bs[int(_editor_get("st").get("active", 0))] = b
   _editor_get("st")["buffers"] = bs
   0
}

fn _open_git_diff(dict entry) int {
   if !is_dict(entry) || entry.len <= 0 { _status("select changed file") return 0 }
   def rel = to_str(entry.get("rel", entry.get("name", "")))
   def body = project.diff_text(_editor_get("project_state"), entry)
   _editor_values["st"] = session.set_output_buffer(_editor_get("st"), "*git diff: " + rel + "*", body)
   _mark_current_output("diff", to_str(entry.get("path", "")))
   _follow_cursor()
   _touch_content()
   _status("git diff " + rel)
   0
}

fn _check_current_file() int {
   _editor_values["st"] = session.check_current_file(_editor_get("st"))
   _follow_cursor()
   _touch_content()
   0
}

fn _format_current_file() int {
   _editor_values["st"] = session.format_current_file(_editor_get("st"))
   _follow_cursor()
   _touch_content()
   0
}

@returns_owned
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
   if !ed.current_buffer(_editor_get("st")).get("readonly", false) && ed.current_buffer(_editor_get("st")).get("dirty", false) { _editor_values["st"] = session.save(_editor_get("st")) }
   def argv = tools.debug_command_for(path)
   if argv.len <= 0 { _status("debug command missing") return 0 }
   _editor_values["term_state"] = termpane.open_command(_editor_get("term_state"), "debug", to_str(argv.get(0, "")), _argv_tail(argv), "$ " + _debug_command_text(argv) + "\n")
   _set_focus("terminal", false)
   _status("debug " + ospath.basename(path))
   0
}

fn _debug_send(str command) int {
   if !termpane.is_open(_editor_get("term_state")) || termpane.mode(_editor_get("term_state")) != "debug" { _debug_start_current() }
   if termpane.is_open(_editor_get("term_state")) {
      _editor_values["term_state"] = termpane.send_text(_editor_get("term_state"), command + "\n")
      _set_focus("terminal", false)
      _status("debug " + command)
   }
   0
}

fn _debug_line_target() str {
   def path = _current_path()
   if path.len <= 0 { return "" }
   path + ":" + to_str(int(_editor_get("st").get("cursor_line", 0)) + 1)
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

@returns_owned
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
   def lines = ed.current_lines(_editor_get("st"))
   if lines.len <= 0 { _status("empty line") return 0 }
   def int row = int(min(max(0, int(_editor_get("st").get("cursor_line", 0))), lines.len - 1))
   def syms = _debug_line_symbols(to_str(lines.get(row, "")))
   if syms.len <= 0 { _status("no symbols on line") return 0 }
   _debug_start_current()
   mut int i = 0
   while i < syms.len {
      _editor_values["term_state"] = termpane.send_text(_editor_get("term_state"), "print " + to_str(syms.get(i, "")) + "\n")
      i += 1
   }
   _set_focus("terminal", false)
   _status("inspect line " + to_str(row + 1))
   0
}

fn _debug_load_coredump() int {
   _editor_values["term_state"] = termpane.open_command(_editor_get("term_state"), "debug", "coredumpctl", ["-1", "gdb"], "$ coredumpctl -1 gdb\n")
   _set_focus("terminal", false)
   _status("latest coredump")
   0
}

fn _debug_breakpoint() int {
   def path = _current_path()
   if path.len <= 0 { _status("save file before break_point") return 0 }
   _debug_send("break " + path + ":" + to_str(int(_editor_get("st").get("cursor_line", 0)) + 1))
}

fn _save() int {
   def path = _current_path()
   def was_dirty = bool(ed.current_buffer(_editor_get("st")).get("dirty", false))
   _editor_values["st"] = session.save(_editor_get("st"))
   def now_dirty = bool(ed.current_buffer(_editor_get("st")).get("dirty", false))
   def save_status = to_str(_editor_get("st").get("status", ""))
   def save_ok = path.len > 0 && !str.startswith(save_status, "save failed") && save_status != "read-only preview" && save_status != "memory buffer"
   if _editor_get("lsp_state").get("active", false) && save_ok && (!now_dirty || !was_dirty) {
      _lsp_flush_change()
      _editor_values["lsp_state"] = lsp.did_save(_editor_get("lsp_state"), path)
   }
   0
}

fn _copy_buffer() int {
   window.set_clipboard(_editor_get("win"), ed.join_lines(ed.current_lines(_editor_get("st"))))
   _status("copied buffer")
   0
}

fn _paste_clipboard() int {
   def text = window.get_clipboard(_editor_get("win"))
   if text.len <= 0 { _status("clipboard empty") return 0 }
   _edit_insert(text)
   _status("pasted")
   0
}

fn _copy_selection_or_buffer() int {
   def selected = _selected_text_all()
   if selected.len > 0 {
      window.set_clipboard(_editor_get("win"), selected)
      _status("copied selection")
   } else {
      _copy_buffer()
   }
   0
}

fn _cut_selection() int {
   if !_selection_any_valid() { _status("no selection") return 0 }
   window.set_clipboard(_editor_get("win"), _selected_text_all())
   _replace_multi_selections("")
   _status("cut")
}

fn _mark_whole_buffer() int {
   def lines = ed.current_lines(_editor_get("st"))
   _editor_get("st")["sel_a_line"] = 0
   _editor_get("st")["sel_a_col"] = 0
   _editor_get("st")["sel_b_line"] = max(0, lines.len - 1)
   _editor_get("st")["sel_b_col"] = to_str(lines.get(max(0, lines.len - 1), "")).len
   _editor_get("st")["cursor_line"] = int(_editor_get("st").get("sel_b_line", 0))
   _editor_get("st")["cursor_col"] = int(_editor_get("st").get("sel_b_col", 0))
   _editor_get("st")["sel_active"] = true
   _editor_get("st")["multi_selects"] = []
   _status("marked buffer")
   0
}

fn _kill_line() int {
   def lines = ed.current_lines(_editor_get("st"))
   def row = min(max(int(_editor_get("st").get("cursor_line", 0)), 0), max(0, lines.len - 1))
   def line = to_str(lines.get(row, ""))
   def col = min(max(int(_editor_get("st").get("cursor_col", 0)), 0), line.len)
   _editor_get("st")["sel_a_line"] = row
   _editor_get("st")["sel_a_col"] = col
   _editor_get("st")["sel_b_line"] = row
   _editor_get("st")["sel_b_col"] = line.len
   _editor_get("st")["sel_active"] = true
   _editor_get("st")["multi_selects"] = []
   _cut_selection()
}

fn _kill_buffer() int {
   mut bs = _editor_get("st").get("buffers", [])
   if bs.len <= 1 {
      _editor_values["st"] = ed.state([ed.buffer("untitled.ny", "", "")])
      _buffer_changed(true)
      _nav_record_current()
      _status("new empty buffer")
      return 0
   }
   def active = int(_editor_get("st").get("active", 0))
   ;; Guard against accidentally discarding unsaved edits: require a second
   ;; kill-buffer invocation within a short window when the active buffer is
   ;; dirty. The first call only emits a status hint.
   def cur = bs.get(active, {})
   if bool(cur.get("dirty", false)) && !bool(cur.get("readonly", false)) {
      if !bool(_editor_get("st").get("kill_confirm", false)) || int(_editor_get("st").get("kill_confirm_buf", -1)) != active {
         _editor_get("st")["kill_confirm"] = true
         _editor_get("st")["kill_confirm_buf"] = active
         _status("unsaved changes — run kill-buffer again to discard")
         return 0
      }
      _editor_get("st")["kill_confirm"] = false
      _editor_get("st")["kill_confirm_buf"] = -1
   }
   mut out = []
   mut i = 0
   while i < bs.len {
      if i != active { out = out.append(bs.get(i)) }
      i += 1
   }
   _editor_get("st")["buffers"] = out
   _editor_get("st")["active"] = min(max(0, active), out.len - 1)
   _editor_get("st")["cursor_line"] = 0
   _editor_get("st")["cursor_col"] = 0
   _editor_get("st")["scroll"] = 0
   _clear_selections()
   _buffer_changed(true)
   _nav_record_current()
   _status("killed buffer")
   0
}

fn _new_scratch() int {
   mut bs = _editor_get("st").get("buffers", [])
   def idx = bs.len + 1
   mut b = ed.buffer("*scratch " + to_str(idx) + "*", "", "")
   b["kind"] = "scratch"
   b["dirty"] = false
   bs = bs.append(b)
   _editor_get("st")["buffers"] = bs
   _editor_get("st")["active"] = bs.len - 1
   _editor_get("st")["cursor_line"] = 0
   _editor_get("st")["cursor_col"] = 0
   _editor_get("st")["scroll"] = 0
   _clear_selections()
   _buffer_changed(true)
   _nav_record_current()
   _status("new scratch")
   0
}

fn _mutate_lines(list lines, str msg) int {
   _editor_values["hist"] = history.push(_editor_get("hist"), _editor_get("st"))
   _editor_values["st"] = ed.set_lines(_editor_get("st"), lines)
   _editor_values["st"] = ed.clamp_cursor(_editor_get("st"))
   _follow_cursor()
   _touch_content()
   if msg.len > 0 { _status(msg) }
   0
}

fn _save_all() int {
   mut bs = _editor_get("st").get("buffers", [])
   mut saved = 0
   mut failed = 0
   mut i = 0
   while i < bs.len {
      mut b = bs.get(i, {})
      def path = to_str(b.get("path", ""))
      if path.len > 0 && !b.get("readonly", false) {
         if session.write_text(path, ed.join_lines(b.get("lines", [""]))) {
            b["dirty"] = false
            bs[i] = b
            saved += 1
         } else {
            ;; write failed (disk full, permissions, etc.) — keep dirty=true
            ;; and surface the failure in the status line so the user knows
            ;; the modeline "MOD" did not actually clear.
            failed += 1
         }
      }
      i += 1
   }
   _editor_get("st")["buffers"] = bs
   if failed > 0 {
      _status("saved " + to_str(saved) + ", " + to_str(failed) + " failed")
   } else {
      _status("saved " + to_str(saved) + " buffer" + (saved == 1 ? "" : "s"))
   }
   0
}

fn _reload_from_disk() int {
   def path = to_str(ed.current_buffer(_editor_get("st")).get("path", ""))
   if path.len <= 0 || !file_exists(path) { _status("no file to reload") return 0 }
   ;; Guard against clobbering unsaved in-memory edits: same double-tap pattern
   ;; as _kill_buffer. The first invocation only warns; the second one within
   ;; the same editor session actually reloads.
   if bool(ed.current_buffer(_editor_get("st")).get("dirty", false)) {
      def active = int(_editor_get("st").get("active", 0))
      if !bool(_editor_get("st").get("reload_confirm", false)) || int(_editor_get("st").get("reload_confirm_buf", -1)) != active {
         _editor_get("st")["reload_confirm"] = true
         _editor_get("st")["reload_confirm_buf"] = active
         _status("buffer dirty — run reload again to discard edits")
         return 0
      }
      _editor_get("st")["reload_confirm"] = false
      _editor_get("st")["reload_confirm_buf"] = -1
   }
   mut bs = _editor_get("st").get("buffers", [])
   bs[int(_editor_get("st").get("active", 0))] = session.read_buffer(path)
   _editor_get("st")["buffers"] = bs
   _editor_get("st")["cursor_line"] = 0
   _editor_get("st")["cursor_col"] = 0
   _editor_get("st")["scroll"] = 0
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
   window.set_clipboard(_editor_get("win"), path)
   _status("copied path")
   0
}

fn _copy_line_info() int {
   def b = ed.current_buffer(_editor_get("st"))
   def path = _buffer_source_path(b)
   def info = (path.len > 0 ? path : to_str(b.get("name", "buffer"))) + ":" +
   to_str(int(_editor_get("st").get("cursor_line", 0)) + 1) + ":" + to_str(int(_editor_get("st").get("cursor_col", 0)) + 1)
   window.set_clipboard(_editor_get("win"), info)
   _status("copied line info")
   0
}

fn _word_byte(int c) bool {
   (c >= 65 && c <= 90) || (c >= 97 && c <= 122) || (c >= 48 && c <= 57) || c == 95
}

fn _word_at_cursor() str {
   def lines = ed.current_lines(_editor_get("st"))
   def int row = int(min(max(int(_editor_get("st").get("cursor_line", 0)), 0), max(0, lines.len - 1)))
   def line = to_str(lines.get(row, ""))
   if line.len <= 0 { return "" }
   mut int col = int(min(max(int(_editor_get("st").get("cursor_col", 0)), 0), line.len - 1))
   if !_word_byte(load8(line, col)) && col > 0 { col -= 1 }
   if !_word_byte(load8(line, col)) { return "" }
   mut int a = col
   while a > 0 && _word_byte(load8(line, a - 1)) { a -= 1 }
   mut int b = col + 1
   while b < line.len && _word_byte(load8(line, b)) { b += 1 }
   str.str_slice(line, a, b)
}

fn _word_prefix_before_cursor() str {
   def lines = ed.current_lines(_editor_get("st"))
   def int row = int(min(max(int(_editor_get("st").get("cursor_line", 0)), 0), max(0, lines.len - 1)))
   def line = to_str(lines.get(row, ""))
   mut int col = int(min(max(int(_editor_get("st").get("cursor_col", 0)), 0), line.len))
   mut int a = col
   while a > 0 && _word_byte(load8(line, a - 1)) { a -= 1 }
   str.str_slice(line, a, col)
}

fn _completion_close() int {
   _editor_get("completion_state")["open"] = false
   _editor_get("completion_state")["items"] = []
   _editor_get("completion_state")["index"] = 0
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
   mut dict item = dict(4)
   item["label"] = label
   item["detail"] = detail
   item["kind"] = kind
   item["insert"] = insert.len > 0 ? insert : label
   return items.append(item)
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

fn _completion_scan_line(list out, str prefix, list lines, int li) list {
   if li < 0 || li >= lines.len || out.len >= 22 { return out }
   def line = to_str(lines.get(li, ""))
   mut int i = 0
   while i < line.len && out.len < 22 {
      if !_word_byte(load8(line, i)) { i += 1 continue }
      def start = i
      while i < line.len && _word_byte(load8(line, i)) { i += 1 }
      def word = str.str_slice(line, start, i)
      if word.len > prefix.len && (prefix.len == 0 || str.startswith(word, prefix)) {
         out = _completion_push(out, word, "buffer line " + to_str(li + 1), "symbol", word)
      }
   }
   out
}

fn _completion_items(str prefix, bool lsp_hint=false) list {
   mut out = _completion_seed_items(prefix)
   def lines = ed.current_lines(_editor_get("st"))
   def int cursor = int(min(max(int(_editor_get("st").get("cursor_line", 0)), 0), max(0, lines.len - 1)))
   def int radius = int(min(COMPLETION_SCAN_RADIUS, max(lines.len, 1)))
   def int lo = int(max(0, cursor - radius))
   def int hi = int(min(lines.len, cursor + radius + 1))
   mut int li = lo
   while li < hi && out.len < 22 {
      out = _completion_scan_line(out, prefix, lines, li)
      li += 1
   }
   if out.len < 12 && lines.len <= COMPLETION_SCAN_MAX_LINES {
      li = 0
      while li < lines.len && out.len < 22 {
         if li < lo || li >= hi { out = _completion_scan_line(out, prefix, lines, li) }
         li += 1
      }
   }
   if lsp_hint {
      out = _completion_push(out, "LSP completion requested", to_str(_editor_get("lsp_state").get("last", "waiting for language server")), "lsp", prefix)
   }
   out
}

fn _completion_open(bool lsp_hint=false) int {
   def prefix = _word_prefix_before_cursor()
   def items = _completion_items(prefix, lsp_hint)
   _editor_get("completion_state")["open"] = items.len > 0
   _editor_get("completion_state")["items"] = items
   _editor_get("completion_state")["index"] = 0
   _editor_get("completion_state")["prefix"] = prefix
   if items.len <= 0 { _status("no completions") }
   0
}

fn _completion_help(str title, str detail) int {
   _editor_get("completion_state")["open"] = true
   mut dict item = dict(4)
   item["label"] = title
   item["detail"] = detail
   item["kind"] = "lsp"
   item["insert"] = ""
   _editor_get("completion_state")["items"] = [item]
   _editor_get("completion_state")["index"] = 0
   _editor_get("completion_state")["prefix"] = ""
   0
}

fn _completion_apply() int {
   if !bool(_editor_get("completion_state").get("open", false)) { return 0 }
   def items = _editor_get("completion_state").get("items", [])
   if items.len <= 0 { return _completion_close() }
   def idx = min(max(0, int(_editor_get("completion_state").get("index", 0))), items.len - 1)
   def item = items.get(idx, {})
   def insert = to_str(item.get("insert", ""))
   def prefix = to_str(_editor_get("completion_state").get("prefix", ""))
   if insert.len > prefix.len && str.startswith(insert, prefix) {
      _edit_insert(str.str_slice(insert, prefix.len, insert.len))
   } elif insert.len > 0 && insert != prefix {
      ;; The server's text replaces the typed prefix (case or fuzzy match).
      def int row = int(_editor_get("st").get("cursor_line", 0))
      def int col = int(_editor_get("st").get("cursor_col", 0))
      def int start = int(max(0, col - str.utf8_len(prefix)))
      def lines = ed.current_lines(_editor_get("st"))
      def line = to_str(lines.get(row, ""))
      _editor_values["hist"] = history.push(_editor_get("hist"), _editor_get("st"))
      mut nl = lines
      nl[row] = str.str_slice(line, 0, start) + str.str_slice(line, col, str.utf8_len(line))
      _editor_values["st"] = ed.set_lines(_editor_get("st"), nl)
      _editor_get("st")["cursor_col"] = start
      _edit_insert(insert)
   } else {
      _status(to_str(item.get("detail", item.get("label", ""))))
   }
   _completion_close()
}

fn _completion_handle_key(any data) bool {
   if !bool(_editor_get("completion_state").get("open", false)) {
      if _editor_get("lsp_popup").len > 0 && window.event_key_is(data, key.KEY_ESCAPE) { _lsp_popup_close() return true }
      return false
   }
   def items = _editor_get("completion_state").get("items", [])
   if window.event_key_is(data, key.KEY_ESCAPE) { _completion_close() return true }
   if window.event_key_is(data, key.KEY_DOWN) {
      _editor_get("completion_state")["index"] = items.len <= 0 ? 0 : (int(_editor_get("completion_state").get("index", 0)) + 1) % items.len
      return true
   }
   if window.event_key_is(data, key.KEY_UP) {
      _editor_get("completion_state")["index"] = items.len <= 0 ? 0 : (int(_editor_get("completion_state").get("index", 0)) + items.len - 1) % items.len
      return true
   }
   if window.event_key_is(data, key.KEY_ENTER) || window.event_key_is(data, key.KEY_TAB) { _completion_apply() return true }
   false
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
   def int col = int(max(0, str.find(raw, name)))
   mut dict definition = dict(5)
   definition["kind"] = kind
   definition["name"] = name
   definition["path"] = path
   definition["line"] = line
   definition["col"] = col
   idx[name] = definition
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

fn _current_definition_index() dict {
   def b = ed.current_buffer(_editor_get("st"))
   def name = to_str(int(_editor_get("st").get("active", 0))) + ":" + to_str(b.get("name", ""))
   if int(_editor_get("definition_cache_rev")) == int(_editor_get("content_rev")) && to_str(_editor_get("definition_cache_name")) == name { return _editor_get("definition_cache") }
   _editor_values["definition_cache"] = _definition_index_from_lines(ed.current_lines(_editor_get("st")), _current_path())
   _editor_values["definition_cache_rev"] = int(_editor_get("content_rev"))
   _editor_values["definition_cache_name"] = name
   _editor_get("definition_cache")
}

fn _current_definition(str word) dict {
   def idx = _current_definition_index()
   idx.contains(word) ? idx.get(word, dict(0)) : dict(0)
}

fn _source_like_path(str path) bool {
   def str ext = str.lower(ospath.extname(path))
   ext == ".ny" || ext == ".c" || ext == ".h" || ext == ".cpp" || ext == ".hpp" || ext == ".cc" || ext == ".cxx" || ext == ".py" || ext == ".js" || ext == ".ts"
}

fn _goto_max_files() int { common.env_int_clamped("NY_EDITOR_GOTO_MAX_FILES", 160, 16, 2000) }

fn _goto_max_bytes() int { common.env_int_clamped("NY_EDITOR_GOTO_MAX_BYTES", 524288, 4096, 8388608) }

fn _project_definition_key(str active_path, int max_files, int max_bytes) str {
   project.root(_editor_get("project_state")) + "\t" + to_str(project.tree(_editor_get("project_state")).len) + "\t" + ospath.normalize(active_path) + "\t" + to_str(max_files) + "\t" + to_str(max_bytes)
}

fn _prepare_project_definition_cache(str active_path, int max_files, int max_bytes) int {
   def key = _project_definition_key(active_path, max_files, max_bytes)
   if to_str(_editor_get("project_definition_cache_key")) == key { return 0 }
   _editor_values["project_definition_cache_key"] = key
   _editor_values["project_definition_cache"] = borrow(dict(512))
   _editor_values["project_definition_scanned"] = borrow(dict(256))
   _editor_values["project_definition_cache_complete"] = false
   0
}

fn _project_cache_file_defs(str path, str active_path, int max_bytes) int {
   if path.len <= 0 || _editor_get("project_definition_scanned").contains(path) { return 0 }
   _editor_get("project_definition_scanned")[path] = true
   if ospath.normalize(path) == ospath.normalize(active_path) { return 0 }
   def text = session.read_text(path)
   if text.len <= 0 || text.len > max_bytes { return 0 }
   def defs = _definition_index_from_lines(ed.split_lines(text), path)
   def keys = dict_keys(defs)
   mut i = 0
   while i < keys.len {
      def k = to_str(keys.get(i, ""))
      if k.len > 0 && !_editor_get("project_definition_cache").contains(k) {
         _editor_get("project_definition_cache")[k] = defs.get(k, dict(0))
      }
      i += 1
   }
   0
}

fn _find_project_definition(str word) dict {
   if word.len <= 0 { return dict(0) }
   _ensure_project_tree()
   def active_path = ospath.normalize(_current_path())
   def rows = project.tree(_editor_get("project_state"))
   def max_files = _goto_max_files()
   def max_bytes = _goto_max_bytes()
   _prepare_project_definition_cache(active_path, max_files, max_bytes)
   if _editor_get("project_definition_cache").contains(word) { return _editor_get("project_definition_cache").get(word, dict(0)) }
   if bool(_editor_get("project_definition_cache_complete")) { return dict(0) }
   mut checked = _editor_get("project_definition_scanned").len
   mut i = 0
   while i < rows.len && checked < max_files {
      def entry = rows.get(i, {})
      def path = to_str(entry.get("path", ""))
      if !entry.get("dir", false) && path.len > 0 && _source_like_path(path) {
         if !_editor_get("project_definition_scanned").contains(path) {
            checked += 1
            _project_cache_file_defs(path, active_path, max_bytes)
            if _editor_get("project_definition_cache").contains(word) { return _editor_get("project_definition_cache").get(word, dict(0)) }
         }
      }
      i += 1
   }
   if i >= rows.len { _editor_values["project_definition_cache_complete"] = true }
   dict(0)
}

fn _goto_definition() int {
   def word = _word_at_cursor()
   if word.len <= 0 { _status("no symbol") return 0 }
   def here = _current_definition(word)
   if is_dict(here) && here.len > 0 {
      _nav_push_current()
      _editor_get("st")["cursor_line"] = int(here.get("line", 0))
      _editor_get("st")["cursor_col"] = int(here.get("col", 0))
      _editor_values["st"] = ed.clamp_cursor(_editor_get("st"))
      _follow_cursor()
      _status("definition " + word)
      return 0
   }
   def found = _find_project_definition(word)
   if is_dict(found) && found.len > 0 {
      _nav_push_current()
      _editor_values["st"] = session.open_file(_editor_get("st"), to_str(found.get("path", "")))
      _editor_get("st")["cursor_line"] = int(found.get("line", 0))
      _editor_get("st")["cursor_col"] = int(found.get("col", 0))
      _editor_values["st"] = ed.clamp_cursor(_editor_get("st"))
      _buffer_changed(true)
      _follow_cursor()
      _status("definition " + word)
      return 0
   }
   _lsp_request("definition")
   0
}

fn _select_line() int {
   def lines = ed.current_lines(_editor_get("st"))
   if _extra_cursor_count() > 0 {
      mut ranges = []
      def prow = min(max(int(_editor_get("st").get("cursor_line", 0)), 0), max(0, lines.len - 1))
      ranges = ranges.append(_range_make(prow, 0, prow, to_str(lines.get(prow, "")).len, true))
      def extras = _extra_cursors()
      mut i = 0
      while i < extras.len {
         def row2 = min(max(_cursor_row(extras.get(i)), 0), max(0, lines.len - 1))
         ranges = ranges.append(_range_make(row2, 0, row2, to_str(lines.get(row2, "")).len, false))
         i += 1
      }
      _editor_get("st")["multi_selects"] = ranges
      def p = ranges.get(0, [])
      _editor_get("st")["sel_a_line"] = int(p.get(0, 0))
      _editor_get("st")["sel_a_col"] = int(p.get(1, 0))
      _editor_get("st")["sel_b_line"] = int(p.get(2, 0))
      _editor_get("st")["sel_b_col"] = int(p.get(3, 0))
      _editor_get("st")["sel_active"] = true
      _status("selected " + to_str(ranges.len) + " lines")
      return 0
   }
   def int row = int(min(max(int(_editor_get("st").get("cursor_line", 0)), 0), max(0, lines.len - 1)))
   _editor_get("st")["cursor_line"] = row
   _editor_get("st")["cursor_col"] = to_str(lines.get(row, "")).len
   _editor_get("st")["sel_a_line"] = row
   _editor_get("st")["sel_a_col"] = 0
   _editor_get("st")["sel_b_line"] = row
   _editor_get("st")["sel_b_col"] = int(_editor_get("st").get("cursor_col", 0))
   _editor_get("st")["sel_active"] = true
   _editor_get("st")["multi_selects"] = []
   _status("selected line")
   0
}

fn _duplicate_line() int {
   def lines = ed.current_lines(_editor_get("st"))
   def int row = int(min(max(int(_editor_get("st").get("cursor_line", 0)), 0), max(0, lines.len - 1)))
   mut out = []
   mut int i = 0
   while i < lines.len {
      out = out.append(lines.get(i, ""))
      if i == row { out = out.append(lines.get(i, "")) }
      i += 1
   }
   _editor_get("st")["cursor_line"] = row + 1
   _mutate_lines(out, "duplicated line")
}

fn _delete_line() int {
   def lines = ed.current_lines(_editor_get("st"))
   if lines.len <= 1 {
      _editor_get("st")["cursor_line"] = 0
      _editor_get("st")["cursor_col"] = 0
      return _mutate_lines([""], "deleted line")
   }
   def int row = int(min(max(int(_editor_get("st").get("cursor_line", 0)), 0), lines.len - 1))
   mut out = []
   mut int i = 0
   while i < lines.len {
      if i != row { out = out.append(lines.get(i, "")) }
      i += 1
   }
   _editor_get("st")["cursor_line"] = min(row, out.len - 1)
   _editor_get("st")["cursor_col"] = min(int(_editor_get("st").get("cursor_col", 0)), to_str(out.get(int(_editor_get("st").get("cursor_line", 0)), "")).len)
   _mutate_lines(out, "deleted line")
}

fn _move_line(int dir) int {
   def lines = ed.current_lines(_editor_get("st"))
   if lines.len <= 1 { return 0 }
   def int row = int(min(max(int(_editor_get("st").get("cursor_line", 0)), 0), lines.len - 1))
   def int other = row + dir
   if other < 0 || other >= lines.len { return 0 }
   mut out = lines
   def a = out.get(row, "")
   out[row] = out.get(other, "")
   out[other] = a
   _editor_get("st")["cursor_line"] = other
   _mutate_lines(out, dir < 0 ? "moved line up" : "moved line down")
}

fn _join_line(bool tight=false) int {
   def lines = ed.current_lines(_editor_get("st"))
   def int row = int(min(max(int(_editor_get("st").get("cursor_line", 0)), 0), max(0, lines.len - 1)))
   if row + 1 >= lines.len { return 0 }
   def a = to_str(lines.get(row, ""))
   def b = to_str(lines.get(row + 1, ""))
   def mid = tight || a.len == 0 || b.len == 0 ? "" : " "
   mut out = []
   mut int i = 0
   while i < lines.len {
      if i == row { out = out.append(a + mid + str.strip(b)) }
      elif i != row + 1 { out = out.append(lines.get(i, "")) }
      i += 1
   }
   _editor_get("st")["cursor_col"] = a.len
   _mutate_lines(out, "joined lines")
}

fn _selection_line_span() list {
   def ranges = _selection_ranges_active()
   if ranges.len <= 0 { return [int(_editor_get("st").get("cursor_line", 0)), int(_editor_get("st").get("cursor_line", 0))] }
   mut int lo = 1000000000
   mut int hi = 0
   mut int i = 0
   while i < ranges.len {
      def r = _range_norm(ranges.get(i, []))
      lo = min(lo, int(r.get(0, 0)))
      hi = max(hi, int(r.get(2, 0)))
      i += 1
   }
   [lo, hi]
}

fn _sort_lines() int {
   def lines = ed.current_lines(_editor_get("st"))
   if lines.len <= 1 { return 0 }
   def span = _selection_line_span()
   def int start = int(min(max(int(span.get(0, 0)), 0), lines.len - 1))
   def int end = int(min(max(int(span.get(1, start)), start), lines.len - 1))
   mut part = []
   mut int i = start
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
   def lines = ed.current_lines(_editor_get("st"))
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
   def str ext = str.lower(ospath.extname(to_str(ed.current_buffer(_editor_get("st")).get("name", ""))))
   if ext == ".py" || ext == ".sh" || ext == ".rb" || ext == ".pl" || ext == ".yaml" || ext == ".yml" { return "# " }
   if ext == ".lua" { return "-- " }
   "// "
}

fn _toggle_comment() int {
   def lines = ed.current_lines(_editor_get("st"))
   if lines.len <= 0 { return 0 }
   def span = _selection_line_span()
   def int start = int(min(max(int(span.get(0, 0)), 0), lines.len - 1))
   def int end = int(min(max(int(span.get(1, start)), start), lines.len - 1))
   def prefix = _comment_prefix()
   mut all_commented = true
   mut int i = start
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
   _apply_hist(history.insert_pair(_editor_get("hist"), _editor_get("st"), open, close))
}

fn _mods_live(any data) int {
   int(data.get("mods", data.get("mod", window.get_modifiers(_editor_get("win")))))
}

fn _ctrl(any data) bool {
   (_mods_live(data) & key.MOD_CONTROL) != 0 || window.key_down(_editor_get("win"), key.KEY_LEFT_CONTROL) || window.key_down(_editor_get("win"), key.KEY_RIGHT_CONTROL) || window.key_down(_editor_get("win"), key.KEY_CTRL)
}

fn _cmd_mod(any data) bool {
   (_mods_live(data) & (key.MOD_CONTROL | key.MOD_SUPER | key.MOD_META)) != 0 || _ctrl(data) || window.key_down(_editor_get("win"), key.KEY_LEFT_SUPER) || window.key_down(_editor_get("win"), key.KEY_RIGHT_SUPER) || window.key_down(_editor_get("win"), key.KEY_SUPER)
}

fn _shift(any data) bool {
   (_mods_live(data) & key.MOD_SHIFT) != 0 || window.key_down(_editor_get("win"), key.KEY_LEFT_SHIFT) || window.key_down(_editor_get("win"), key.KEY_RIGHT_SHIFT) || window.key_down(_editor_get("win"), key.KEY_SHIFT)
}

fn _alt(any data) bool {
   (_mods_live(data) & key.MOD_ALT) != 0 || window.key_down(_editor_get("win"), key.KEY_LEFT_ALT) || window.key_down(_editor_get("win"), key.KEY_RIGHT_ALT) || window.key_down(_editor_get("win"), key.KEY_ALT)
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
      _editor_values["suppress_char_cp"] = cp
      if INPUT_TRACE { print("[editor:key] suppress-char cp=" + to_str(cp)) }
   }
   0
}

fn _consume_suppressed_char(any data) bool {
   def cp = int(data.get("char", 0))
   if int(_editor_get("suppress_char_cp")) <= 0 || cp <= 0 { return false }
   def expected = int(_editor_get("suppress_char_cp"))
   _editor_values["suppress_char_cp"] = 0
   if _same_char_fold(cp, expected) {
      if INPUT_TRACE { print("[editor:char] suppressed cp=" + to_str(cp)) }
      return true
   }
   false
}

fn _line_text_x(dict lay) f64 {
   float(lay.get("edit_x", 0.0)) + (bool(_editor_get("st").get("show_line_numbers", true)) ? 58.0 : 16.0)
}

fn _layout_with_terminal(dict lay, f64 sh) dict {
   lay = ed.with_bottom_dock(lay, sh, termpane.is_open(_editor_get("term_state")), float(_editor_get("st").get("term_h", 0.0)))
   if termpane.is_open(_editor_get("term_state")) {
      _editor_get("st")["term_h"] = float(lay.get("dock_h", _editor_get("st").get("term_h", 0.0)))
      _editor_values["term_state"] = termpane.resize(
         _editor_get("term_state"),
         float(lay.get("dock_x", 0.0)) + 8.0,
         float(lay.get("dock_y", 0.0)) + TERM_TAB_H,
         max(1.0, float(lay.get("dock_w", 1.0)) - 16.0),
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
   if !termpane.is_open(_editor_get("term_state")) { return _tab_hit_result("", -1, false) }
   def x = float(lay.get("dock_x", 0.0))
   def y = float(lay.get("dock_y", 0.0))
   def w = float(lay.get("dock_w", 0.0))
   if my < y + 2.0 || my > y + TERM_TAB_H - 2.0 || mx < x + 6.0 || mx > x + w - 6.0 { return _tab_hit_result("", -1, false) }
   def n = termpane.tab_count(_editor_get("term_state"))
   def tw = _term_tab_w(w, n)
   def start_x = x + 6.0
   mut i = 0
   while i < n {
      def f64 tx = _snap_px(start_x + float(i) * tw)
      def f64 right = tx + float(min(tw - 2.0, x + w - tx - 40.0))
      if right - tx > 36.0 && mx >= tx && mx < right && my >= y + 3.0 && my < y + TERM_TAB_H - 2.0 {
         return _tab_hit_result(mx >= right - 22.0 ? "close" : "select", i, true)
      }
      i += 1
   }
   def f64 plus_x = _snap_px(start_x + float(n) * tw + 3.0)
   if plus_x + 24.0 < x + w - 8.0 && mx >= plus_x && mx < plus_x + 24.0 { return _tab_hit_result("new", n, true) }
   _tab_hit_result("", -1, false)
}

fn _tab_hit_result(str action, int idx, bool has_index) dict {
   mut dict result = dict(has_index ? 2 : 1)
   result["action"] = action
   if has_index { result["idx"] = idx }
   result
}

fn _scrollbar_value_from_mouse(f64 track_y, f64 track_h, int total, int visible, f64 my) int {
   def int max_scroll = max(0, total - visible)
   if max_scroll <= 0 || track_h <= 1.0 { return 0 }
   def f64 thumb_h = max(18.0, track_h * float(visible) / float(max(visible, total)))
   def f64 range = max(1.0, track_h - thumb_h)
   def f64 pos = min(max((my - track_y - thumb_h * 0.5) / range, 0.0), 1.0)
   int(min(max_scroll, max(0, int(pos * float(max_scroll) + 0.5))))
}

fn _scrollbar_track_hit(f64 mx, f64 my, f64 x, f64 y, f64 w, f64 h) bool {
   h > 8.0 && mx >= x && mx <= x + w && my >= y && my <= y + h
}

fn _editor_scrollbar_target(dict lay, f64 mx, f64 my) dict {
   def lines = ed.current_lines(_editor_get("st"))
   def visible = ed.visible_rows(float(lay.get("edit_h", 0.0)))
   if lines.len <= visible { return dict(0) }
   def x = float(lay.get("edit_x", 0.0)) + float(lay.get("edit_w", 0.0)) - 18.0
   def y = float(lay.get("edit_y", 0.0)) + 10.0
   def h = max(1.0, float(lay.get("edit_h", 0.0)) - 20.0)
   if !_scrollbar_track_hit(mx, my, x, y, 18.0, h) { return dict(0) }
   mut dict target = dict(5)
   target["kind"] = "editor"
   target["track_y"] = y
   target["track_h"] = h
   target["total"] = lines.len
   target["visible"] = visible
   target
}

fn _scrollbar_target(str kind, f64 y, f64 h, int total, int visible) dict {
   mut dict target = dict(5)
   target["kind"] = kind
   target["track_y"] = y
   target["track_h"] = h
   target["total"] = total
   target["visible"] = visible
   target
}

fn _rail_scrollbar_target(dict lay, f64 mx, f64 my) dict {
   if !bool(_editor_get("st").get("show_hierarchy", true)) { return dict(0) }
   def rail_x = float(lay.get("rail_x", 0.0))
   def rail_y = float(lay.get("rail_y", 0.0))
   def rail_w = float(lay.get("rail_w", 0.0))
   def rail_h = float(lay.get("rail_h", 0.0))
   if rail_w <= 1.0 || mx < rail_x + rail_w - 16.0 || mx > rail_x + rail_w { return dict(0) }
   def body_y = rail_y + RAIL_TABS_H
   def body_h = rail_h - RAIL_TABS_H
   def tab = _rail_tab()
   if tab == "git" {
      def rows_git = project.changes(_editor_get("project_state"))
      def visible_git = project.visible_count(body_h)
      def ty = body_y + project.TREE_HEADER_H
      def th = max(1.0, body_h - project.TREE_HEADER_H)
      if rows_git.len > visible_git && _scrollbar_track_hit(mx, my, rail_x + rail_w - 16.0, ty, 16.0, th) {
         return _scrollbar_target("git", ty, th, rows_git.len, visible_git)
      }
      return dict(0)
   }
   if tab == "timeline" {
      def rows_time = _timeline_rows()
      def visible_time = project.visible_count(body_h)
      def ty = body_y + project.TREE_HEADER_H
      def th = max(1.0, body_h - project.TREE_HEADER_H)
      if rows_time.len > visible_time && _scrollbar_track_hit(mx, my, rail_x + rail_w - 16.0, ty, 16.0, th) {
         return _scrollbar_target("timeline", ty, th, rows_time.len, visible_time)
      }
      return dict(0)
   }
   def show_project = bool(_editor_get("st").get("show_project", true))
   def show_outline = bool(_editor_get("st").get("show_outline", true))
   if show_project { _ensure_project_tree() }
   def rows = project.tree(_editor_get("project_state"))
   def syms = show_outline ? _outline_symbols() : []
   def tree_h = (!show_project) ? 0.0 : (show_outline ? _rail_tree_height(body_h, rows.len, syms.len) : body_h)
   if show_project {
      def visible_tree = project.visible_count(tree_h)
      def ty_tree = body_y + project.TREE_HEADER_H
      def th_tree = max(1.0, tree_h - project.TREE_HEADER_H)
      if rows.len > visible_tree && _scrollbar_track_hit(mx, my, rail_x + rail_w - 16.0, ty_tree, 16.0, th_tree) {
         return _scrollbar_target("tree", ty_tree, th_tree, rows.len, visible_tree)
      }
   }
   if show_outline {
      def f64 outline_y = float(body_y) + (show_project ? float(tree_h) + 1.0 : 0.0)
      def f64 outline_h = float(body_h) - (outline_y - float(body_y))
      def visible_outline = _outline_visible(outline_h)
      def ty_outline = outline_y + SECTION_H
      def th_outline = max(1.0, outline_h - SECTION_H)
      if syms.len > visible_outline && _scrollbar_track_hit(mx, my, rail_x + rail_w - 16.0, ty_outline, 16.0, th_outline) {
         return _scrollbar_target("outline", ty_outline, th_outline, syms.len, visible_outline)
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
      "editor" -> { _editor_get("st")["scroll"] = value _editor_get("st")["scroll_follow"] = false }
      "tree" -> { _editor_get("st")["tree_scroll"] = value }
      "outline" -> { _editor_get("st")["outline_scroll"] = value }
      "git" -> { _editor_get("st")["git_scroll"] = value }
      "timeline" -> { _editor_get("st")["timeline_scroll"] = value }
      _ -> {}
   }
   0
}

fn _rail_width_from_mouse(dict lay, f64 mx) f64 {
   def sw = float(lay.get("edit_x", 0.0)) + float(lay.get("edit_w", 0.0))
   ;; Upper bound is the editor-edge position; let it fall to 0 when the window
   ;; is narrower than EDIT_MIN_W so the layout can collapse the rail instead
   ;; of starving the editor pane below EDIT_MIN_W.
   min(max(mx, RAIL_MIN_W), max(0.0, sw - EDIT_MIN_W))
}

fn _place_cursor(dict lay, f64 x, f64 y) int {
   def lines = ed.current_lines(_editor_get("st"))
   if lines.len <= 0 { return 0 }
   def top = float(lay.get("edit_y", 0.0)) + 14.0
   def bottom = float(lay.get("edit_y", 0.0)) + float(lay.get("edit_h", 0.0)) - 8.0
   def view = _fold_visible_indices(lines)
   def int start = _fold_first_visible_at_or_after(view, int(_editor_get("st").get("scroll", 0)))
   def int screen_row = max(0, ed.row_at(lay, min(max(y, top), bottom), 0))
   def int view_row = min(max(0, start + screen_row), max(0, view.len - 1))
   def int row = int(view.get(view_row, 0))
   def line = to_str(lines.get(row, ""))
   _editor_get("st")["cursor_line"] = row
   _editor_get("st")["cursor_col"] = _visual_col_at(line, row, x - _line_text_x(lay))
   _editor_values["st"] = ed.clamp_cursor(_editor_get("st"))
   _follow_cursor()
   _editor_get("st")["quit_prompt"] = false
   _editor_values["last_escape_ns"] = 0
   0
}

fn _run_code_lens(any lens) int {
   if !is_dict(lens) { return 0 }
   def cmd = lens.get("command", dict(0))
   if !is_dict(cmd) || to_str(cmd.get("command", "")).len <= 0 { _status("code lens has no command") return 0 }
   _editor_values["lsp_state"] = lsp.execute_command(_lsp(), to_str(cmd.get("command", "")), cmd.get("arguments", []))
   _status("code lens: " + to_str(lens.get("title", "")))
   0
}

fn _code_lens_click(f64 x, f64 y) bool {
   def rects = _editor_get("code_lens_rects")
   mut int i = 0
   while i < rects.len {
      def r = rects.get(i, [])
      if is_list(r) && r.len >= 5 && widgets.hit(x, y, float(r.get(0)), float(r.get(1)), float(r.get(2)), float(r.get(3))) {
         _run_code_lens(r.get(4, dict(0)))
         return true
      }
      i += 1
   }
   false
}

fn _click_editor(dict lay, f64 x, f64 y) int {
   def lines = ed.current_lines(_editor_get("st"))
   def view = _fold_visible_indices(lines)
   def int start = _fold_first_visible_at_or_after(view, int(_editor_get("st").get("scroll", 0)))
   def int screen_row = max(0, ed.row_at(lay, y, 0))
   def row = int(view.get(start + screen_row, -1))
   if row < 0 || row >= lines.len { return 0 }
   ;; The gutter (line numbers) toggles breakpoints.
   if x >= float(lay.get("edit_x", 0.0)) && x < _line_text_x(lay) - 6.0 { return _dap_toggle_breakpoint(row) }
   _place_cursor(lay, x, y)
}

fn _palette_matches() list {
   pal.state_matches(_editor_get("palette_state"))
}

fn _palette_open() int {
   _editor_get("palette_state")["cfg"] = _editor_get("command_cfg")
   _editor_values["palette_state"] = pal.open(_editor_get("palette_state"))
   _editor_values["context_state"] = prompt.context_close(_editor_get("context_state"))
   _editor_values["find_state"] = find.dismiss(_editor_get("find_state"))
   _editor_values["rename_state"] = prompt.rename_close(_editor_get("rename_state"))
   _request_full_redraw(3)
   _status("command palette")
   0
}

fn _set_ui_toggle(str key_name) bool {
   def k = "show_" + key_name
   def on = !bool(_editor_get("st").get(k, true))
   _editor_get("st")[k] = on
   _request_full_redraw(2)
   _save_ui_cache()
   on
}

fn _set_focus(str pane, bool announce=true) int {
   _editor_get("st")["focus"] = pane
   _editor_get("term_state")["focus"] = pane == "terminal" && termpane.is_open(_editor_get("term_state"))
   _request_full_redraw(2)
   if announce { _status("focus " + pane) }
   0
}

fn _focus_next() int {
   _set_focus(ed.focus_next(to_str(_editor_get("st").get("focus", "editor")), bool(_editor_get("st").get("show_hierarchy", true)), termpane.is_open(_editor_get("term_state"))))
}

fn _pane_resize(str dir) int {
   if dir == "left" {
      _editor_get("st")["rail_w"] = max(RAIL_MIN_W, float(_editor_get("st").get("rail_w", 250.0)) - 24.0)
   } elif dir == "right" {
      _editor_get("st")["show_hierarchy"] = true
      _editor_get("st")["rail_w"] = min(PANE_MAX, float(_editor_get("st").get("rail_w", 250.0)) + 24.0)
   } elif dir == "up" {
      _editor_get("st")["term_h"] = min(PANE_MAX, float(_editor_get("st").get("term_h", 0.0)) + 24.0)
   } elif dir == "down" {
      _editor_get("st")["term_h"] = max(DOCK_MIN_H, float(_editor_get("st").get("term_h", 0.0)) - 24.0)
   }
   _request_full_redraw(2)
   _save_ui_cache()
   _status("resize " + dir)
   0
}

fn _close_focused_pane() int {
   def focus = to_str(_editor_get("st").get("focus", "editor"))
   if focus == "terminal" {
      _editor_values["term_state"] = termpane.dismiss(_editor_get("term_state"))
      _set_focus("editor")
   } elif focus == "project" {
      _editor_get("st")["show_hierarchy"] = false
      _save_ui_cache()
      _set_focus("editor")
   } else {
      _status("editor stays open")
   }
   0
}

fn _lsp_output(str title, str body) int {
   def path = _current_path()
   _editor_values["st"] = session.set_output_buffer(_editor_get("st"), lsp.output_name(path), title + "\n\n" + body)
   0
}

fn _lsp_start() int {
   if ed.current_buffer(_editor_get("st")).get("readonly", false) { _status("lsp skipped for preview") return 0 }
   if _current_path().len <= 0 { _status("save before lsp") return 0 }
   _editor_values["lsp_state"] = lsp.start(_editor_get("lsp_state"), _current_path(), _current_text())
   _status(lsp.status(_editor_get("lsp_state")))
   0
}

;; ---- language server ---------------------------------------------------
;; Requests are asynchronous: _lsp_request sends, _lsp_pump (once per frame)
;; routes the answers. Text changes are debounced into one didChange.

def int LSP_CHANGE_DEBOUNCE_NS = 150000000
def int LSP_COMPLETION_LIMIT = 80

fn _lsp() dict { _editor_get("lsp_state") }

fn _lsp_line_text(int line) str {
   def lines = ed.current_lines(_editor_get("st"))
   line >= 0 && line < lines.len ? to_str(lines.get(line, "")) : ""
}

fn _lsp_cursor() list {
   def int line = int(_editor_get("st").get("cursor_line", 0))
   def int col = int(_editor_get("st").get("cursor_col", 0))
   [line, lsp.lsp_col(_lsp(), _lsp_line_text(line), col)]
}

fn _lsp_mark_dirty() int {
   if _editor_get("lsp_popup").len > 0 && to_str(_editor_get("lsp_popup").get("kind", "")) == "hover" { _lsp_popup_close() }
   if !bool(_lsp().get("active", false)) || _current_path().len <= 0 { return 0 }
   _editor_values["lsp_dirty_at"] = ticks()
   0
}

fn _lsp_flush_change() int {
   if int(_editor_get("lsp_dirty_at")) == 0 { return 0 }
   _editor_values["lsp_dirty_at"] = 0
   def path = _current_path()
   if path.len <= 0 || !bool(_lsp().get("active", false)) { return 0 }
   _editor_values["lsp_state"] = lsp.did_change(_lsp(), path, _current_text())
   _lsp_request_semantic(path)
   _lsp_request_inlay(path)
   _lsp_request_code_lens(path)
   _lsp_request_folding(path)
   0
}

fn _lsp_request_semantic(str path) int {
   if path.len == 0 || !lsp.ready(_lsp()) || !_lsp().get("docs", dict(0)).contains(lsp.uri_of(path)) { return 0 }
   if !_lsp().get("caps", dict(0)).contains("semanticTokensProvider") { return 0 }
   _editor_values["semantic_requested_path"] = path
   _editor_values["lsp_state"] = lsp.semantic_tokens(_lsp(), path)
   0
}

fn _lsp_request_inlay(str path) int {
   if path.len == 0 || !lsp.ready(_lsp()) || !_lsp().get("docs", dict(0)).contains(lsp.uri_of(path)) { return 0 }
   if !_lsp().get("caps", dict(0)).contains("inlayHintProvider") { return 0 }
   _editor_values["inlay_requested_path"] = path
   _editor_values["lsp_state"] = lsp.inlay_hints(_lsp(), path, 0, ed.current_lines(_editor_get("st")).len)
   0
}

fn _lsp_request_code_lens(str path) int {
   if path.len == 0 || !lsp.ready(_lsp()) || !_lsp().get("docs", dict(0)).contains(lsp.uri_of(path)) { return 0 }
   if !_lsp().get("caps", dict(0)).contains("codeLensProvider") { return 0 }
   _editor_values["code_lens_requested_path"] = path
   _editor_values["lsp_state"] = lsp.code_lens(_lsp(), path)
   0
}

fn _lsp_request_folding(str path) int {
   if path.len == 0 || !lsp.ready(_lsp()) || !_lsp().get("docs", dict(0)).contains(lsp.uri_of(path)) { return 0 }
   if !_lsp().get("caps", dict(0)).contains("foldingRangeProvider") { return 0 }
   _editor_values["fold_requested_path"] = path
   _editor_values["lsp_state"] = lsp.folding_ranges(_lsp(), path)
   0
}

fn _lsp_probe_at(str spec) int {
   ;; "line:col", 1-based
   def parts = str.split(spec, ":")
   _editor_get("st")["cursor_line"] = int(max(0, str.atoi(to_str(parts.get(0, "1"))) - 1))
   _editor_get("st")["cursor_col"] = int(max(0, str.atoi(to_str(parts.get(1, "1"))) - 1))
   _editor_values["st"] = ed.clamp_cursor(_editor_get("st"))
   0
}

fn _lsp_probe_step() int {
   ;; NY_EDITOR_PROBE_LSP=1 with NY_EDITOR_PROBE_LSP_HOVER / _DEF / _COMPLETE
   ;; positions ("line:col"): drives the real request paths and prints results.
   def int phase = int(_editor_values.get("lsp_probe_phase", 0))
   def int frame = int(_editor_get("frame_count"))
   if frame > 1500 { print("LSP_PROBE timeout phase=" + to_str(phase)) window.set_should_close(_editor_get("win"), true) return 0 }
   if phase == 99 { return 0 }
   if phase == 0 { _lsp_start() _editor_values["lsp_probe_phase"] = 1 return 0 }
   if phase == 1 {
      if !lsp.ready(_lsp()) { return 0 }
      print("LSP_PROBE ready " + to_str(_lsp().get("server", "")))
      _lsp_probe_at(common.env_trim("NY_EDITOR_PROBE_LSP_HOVER"))
      _lsp_request("hover")
      _editor_values["lsp_probe_phase"] = 2
      return 0
   }
   if phase == 2 {
      if _editor_get("lsp_popup").len <= 0 { return 0 }
      print("LSP_PROBE hover " + str.str_replace(to_str(_editor_get("lsp_popup").get("text", "")), "\n", " | "))
      if common.env_trim("NY_EDITOR_PROBE_LSP_HOLD") == "hover" { _editor_values["lsp_probe_phase"] = 99 return 0 }
      _lsp_probe_at(common.env_trim("NY_EDITOR_PROBE_LSP_DEF"))
      _editor_values["lsp_probe_line0"] = int(_editor_get("st").get("cursor_line", 0))
      _lsp_request("definition")
      _editor_values["lsp_probe_phase"] = 3
      return 0
   }
   if phase == 3 {
      if int(_editor_get("st").get("cursor_line", 0)) == int(_editor_values.get("lsp_probe_line0", -1)) { return 0 }
      print("LSP_PROBE definition line=" + to_str(int(_editor_get("st").get("cursor_line", 0)) + 1) + " col=" + to_str(int(_editor_get("st").get("cursor_col", 0)) + 1))
      _lsp_probe_at(common.env_trim("NY_EDITOR_PROBE_LSP_COMPLETE"))
      _lsp_request("completion")
      _editor_values["lsp_probe_phase"] = 4
      return 0
   }
   if phase == 4 {
      if to_str(_editor_get("completion_state").get("source", "")) != "lsp" { return 0 }
      def items = _editor_get("completion_state").get("items", [])
      print("LSP_PROBE completion prefix=" + to_str(_editor_get("completion_state").get("prefix", "")) + " items=" + to_str(items.len) + " first=" + to_str(items.get(0, dict(0)).get("label", "")))
      _completion_close()
      _lsp_probe_at(common.env_trim("NY_EDITOR_PROBE_LSP_DEF"))
      _lsp_request("references")
      _editor_values["lsp_probe_phase"] = 5
      return 0
   }
   if phase == 5 {
      if !str.startswith(to_str(ed.current_buffer(_editor_get("st")).get("name", "")), "lsp:") { return 0 }
      def lines = ed.current_lines(_editor_get("st"))
      print("LSP_PROBE references " + to_str(lines.get(0, "")))
      print("LSP_PROBE diagnostics " + to_str(lsp.diagnostics_count(_lsp(), "")))
      print("LSP_PROBE_OK")
      _editor_values["lsp_probe_phase"] = 6
      window.set_should_close(_editor_get("win"), true)
   }
   0
}

@effects(all)
fn _git_inline_probe(dict lay) int {
   def int phase = int(_editor_values.get("git_inline_probe_phase", 0))
   if int(_editor_get("frame_count")) > 240 { panic("git inline probe timed out") }
   if phase == 0 {
      _git_inline_toggle()
      assert(bool(_editor_get("blame_on")), "git blame loads from the repository")
      _editor_values["git_inline_probe_phase"] = 1
   } elif phase == 1 {
      def info = _editor_get("blame_lines").get("0", dict(0))
      assert(_editor_get("blame_lines").len > 0 && to_str(info.get("author", "")).len > 0, "porcelain blame parsed")
      assert(_editor_get("git_inline_runs").len > 0, "blame annotation rendered")
      if _editor_get("blame_hunks").len > 0 { assert(int(_editor_get("git_hunk_bars")) > 0, "hunk bars rendered") }
      print("EDITOR-GIT-INLINE-OK")
      _editor_values["git_inline_probe_phase"] = 2
      window.set_should_close(_editor_get("win"), true)
   }
   0
}

@effects(all)
fn _inlay_live_probe(dict lay) int {
   def int phase = int(_editor_values.get("inlay_probe_phase", 0))
   if int(_editor_get("frame_count")) > 1200 { panic("live inlay probe timed out") }
   if phase == 1 && (_inlays_for_line(0).len == 0 || _editor_get("code_lens_rects").len == 0 || _editor_get("fold_ranges").len == 0 || _editor_get("fold_rects").len == 0) { msleep(5) return 0 }
   def fb = gfx.read_framebuffer()
   def int width = int(fb.get("width", 0))
   def int height = int(fb.get("height", 0))
   def ptr pixels = __ffi_untag_ptr(fb.get("data", 0))
   assert(width > 0 && height > 0 && !!pixels, "inlay probe reads framebuffer")
   def int x0 = int(_line_text_x(lay) + _measure_prefix("def answer = 43", 10))
   def int y0 = int(_editor_row_y(float(lay.get("edit_y", 0.0)), 0)) + 2
   assert(x0 >= 0 && x0 + 90 < width && y0 >= 0 && y0 + 18 < height, "inlay probe region visible")
   mut samples = []
   mut int changed = 0
   def baseline = _editor_values.get("inlay_probe_pixels", [])
   mut int y = 0
   while y < 18 {
      mut int x = 0
      while x < 90 {
         def int pixel = int(load32(pixels, ((y0 + y) * width + x0 + x) * 4))
         if phase == 0 { samples = samples.append(pixel) }
         elif pixel != int(baseline.get(y * 90 + x, pixel)) { changed += 1 }
         x += 1
      }
      y += 1
   }
   free(pixels)
   if phase == 0 {
      _editor_values["inlay_probe_pixels"] = samples
      _editor_values["inlay_probe_phase"] = 1
      _lsp_start()
   } elif phase == 1 {
      def hints = _inlays_for_line(0)
      def hint = hints.get(0, dict(0))
      assert(to_str(hint.get("label", "")) == ": int" && int(hint.get("col", -1)) == 10, "live server delivered the expected hint")
      def runs = _editor_get("visible_text_runs_cache")
      mut bool displayed = false
      mut int i = 0
      while i + 3 < runs.len {
         if to_str(runs.get(i, "")) == " : int " && int(runs.get(i + 3, 0)) == 10006 &&
            abs(float(runs.get(i + 1, 0.0)) - float(x0)) < 1.0 { displayed = true }
         i += 4
      }
      assert(displayed, "live hint appears in the drawn text runs at its source position")
      def lens_rects = _editor_get("code_lens_rects")
      def lens0 = lens_rects.get(0, [])
      def lens1 = lens_rects.get(1, [])
      assert(lens_rects.len == 2 && to_str(lens0.get(4, dict(0)).get("title", "")) == "▶ Run" && to_str(lens1.get(4, dict(0)).get("title", "")) == "Debug", "live code lenses rendered and hit-testable")
      assert(float(lens0.get(2, 0.0)) > 0.0 && float(lens0.get(1, -1.0)) >= _editor_row_y(float(lay.get("edit_y", 0.0)), 0) && float(lens0.get(1, 0.0)) < _editor_row_y(float(lay.get("edit_y", 0.0)), 1), "code lens is placed on its source row")
      assert(changed > 10 && _visual_prefix("def answer = 43", 0, 10) > _measure_prefix("def answer = 43", 10), "live hint changes the rendered line")
      def folds = _editor_get("fold_ranges")
      def markers = _editor_get("fold_rects")
      def marker = markers.get(0, [])
      assert(folds.len == 1 && int(folds.get(0).get("start", -1)) == 1 && int(folds.get(0).get("end", -1)) == 3 && markers.len == 1, "live folding range rendered in the gutter")
      assert(_fold_rect_click(float(marker.get(0, 0.0)) + 2.0, float(marker.get(1, 0.0)) + 2.0), "fold marker routes a click to its range")
      _editor_values["inlay_probe_phase"] = 10
   } elif phase == 10 {
      def lines = ed.current_lines(_editor_get("st"))
      def view = _fold_visible_indices(lines)
      assert(lines.len == 5 && view.len == 3 && _editor_get("fold_collapsed").contains("1"), "collapsed fold hides its body lines")
      _fold_open_all()
      _editor_values["inlay_probe_phase"] = 11
   } elif phase == 11 {
      def lines = ed.current_lines(_editor_get("st"))
      def view = _fold_visible_indices(lines)
      assert(view.len == lines.len && _editor_get("fold_collapsed").len == 0, "opening all folds restores every line")
      def lens0 = _editor_get("code_lens_rects").get(0, [])
      assert(_code_lens_click(float(lens0.get(0, 0.0)) + 2.0, float(lens0.get(1, 0.0)) + 2.0), "code lens rect routes a click to its command")
      _editor_values["inlay_probe_phase"] = 2
   } elif phase == 2 {
      if !termpane.is_open(_editor_get("term_state")) { msleep(5) return 0 }
      assert(int(_editor_get("term_state").get("active", -1)) >= 0, "code lens process action opens a terminal")
      print("EDITOR-INLAY-LIVE-OK")
      _editor_values["inlay_probe_phase"] = 3
      _editor_values["lsp_state"] = lsp.stop(_lsp())
      window.set_should_close(_editor_get("win"), true)
   }
   0
}

fn _dap_probe_step() int {
   ;; NY_EDITOR_PROBE_DAP=1 NY_EDITOR_PROBE_DAP_BP=<1-based line>: breakpoint,
   ;; start, report the stop, step once, continue to the end.
   def int phase = int(_editor_values.get("dap_probe_phase", 0))
   if int(_editor_get("frame_count")) > 3000 { print("DAP_PROBE timeout phase=" + to_str(phase)) window.set_should_close(_editor_get("win"), true) return 0 }
   if phase == 99 { return 0 }
   if phase == 0 {
      _dap_toggle_breakpoint(int(max(0, str.atoi(common.env_trim("NY_EDITOR_PROBE_DAP_BP")) - 1)))
      _dap_start()
      _editor_values["dap_probe_phase"] = 1
      return 0
   }
   if phase == 1 {
      if !dap.stopped(_dap()) || dap.stack(_dap()).len <= 0 { return 0 }
      def loc = dap.stop_location(_dap())
      print("DAP_PROBE stopped " + to_str(loc.get("name", "")) + " line=" + to_str(int(loc.get("line", 0)) + 1) + " cursor=" + to_str(int(_editor_get("st").get("cursor_line", 0)) + 1) + " frames=" + to_str(dap.stack(_dap()).len))
      if common.env_trim("NY_EDITOR_PROBE_DAP_HOLD") == "stop" { _editor_values["dap_probe_phase"] = 99 return 0 }
      _editor_values["dap_probe_line"] = int(loc.get("line", 0))
      _dap_step("next")
      _editor_values["dap_probe_phase"] = 2
      return 0
   }
   if phase == 2 {
      if !dap.stopped(_dap()) || dap.stack(_dap()).len <= 0 { return 0 }
      print("DAP_PROBE stepped line=" + to_str(int(dap.stop_location(_dap()).get("line", 0)) + 1))
      _dap_toggle_breakpoint(int(max(0, str.atoi(common.env_trim("NY_EDITOR_PROBE_DAP_BP")) - 1)))
      _dap_step("continue")
      _editor_values["dap_probe_phase"] = 3
      return 0
   }
   if phase == 3 {
      if dap.running(_dap()) { return 0 }
      print("DAP_PROBE ended " + dap.status(_dap()))
      print("DAP_PROBE output " + str.str_replace(str.strip(dap.output_text(_dap())), "\n", " | "))
      print("DAP_PROBE_OK")
      _editor_values["dap_probe_phase"] = 99
      window.set_should_close(_editor_get("win"), true)
   }
   0
}

fn _lsp_auto(str kind) int {
   ;; Typing-driven request: only when a server is ready, coalesced per frame.
   if !lsp.ready(_lsp()) || !bool(_lsp().get("popups", true)) { return 0 }
   _editor_values["lsp_auto"] = kind
   _editor_values["lsp_auto_at"] = ticks()
   0
}

fn _lsp_pump() int {
   if !is_dict(_lsp().get("conn", nil)) { return 0 }
   def path = _current_path()
   if lsp.ready(_lsp()) && path.len > 0 && !ed.current_buffer(_editor_get("st")).get("readonly", false) {
      if !_lsp().get("docs", dict(0)).contains(lsp.uri_of(path)) {
         _editor_values["lsp_state"] = lsp.did_open(_lsp(), path, _current_text())
      }
      if to_str(_editor_values.get("semantic_requested_path", "")) != path && int(_editor_get("lsp_dirty_at")) == 0 { _lsp_request_semantic(path) }
      if to_str(_editor_values.get("inlay_requested_path", "")) != path && int(_editor_get("lsp_dirty_at")) == 0 { _lsp_request_inlay(path) }
      if to_str(_editor_values.get("code_lens_requested_path", "")) != path && int(_editor_get("lsp_dirty_at")) == 0 { _lsp_request_code_lens(path) }
      if to_str(_editor_values.get("fold_requested_path", "")) != path && int(_editor_get("lsp_dirty_at")) == 0 { _lsp_request_folding(path) }
   }
   def auto = to_str(_editor_get("lsp_auto"))
   if auto.len > 0 && ticks() - int(_editor_get("lsp_auto_at")) >= 60000000 {
      _editor_values["lsp_auto"] = ""
      _lsp_flush_change()
      def pos = _lsp_cursor()
      if auto == "completion" { _editor_values["lsp_state"] = lsp.completion(_lsp(), _current_path(), int(pos.get(0, 0)), int(pos.get(1, 0))) }
      else { _editor_values["lsp_state"] = lsp.signature_help(_lsp(), _current_path(), int(pos.get(0, 0)), int(pos.get(1, 0))) }
   }
   def int dirty = int(_editor_get("lsp_dirty_at"))
   if dirty > 0 && ticks() - dirty >= LSP_CHANGE_DEBOUNCE_NS { _lsp_flush_change() }
   def events = lsp.pump(_lsp())
   mut int i = 0
   while i < events.len {
      _lsp_event(events.get(i, dict(0)))
      i += 1
   }
   0
}

def LSP_TRACE = common.env_truthy("NY_EDITOR_LSP_TRACE")

fn _lsp_event(dict e) int {
   def kind = to_str(e.get("kind", ""))
   if LSP_TRACE { eprint("[lsp] event " + kind + " " + to_str(e.get("text", e.get("request", "")))) }
   if kind == "ready" { _editor_values["semantic_requested_path"] = "" _editor_values["inlay_requested_path"] = "" _editor_values["code_lens_requested_path"] = "" _editor_values["fold_requested_path"] = "" _status(to_str(_lsp().get("last", "lsp ready"))) }
   elif kind == "diagnostics" { _request_full_redraw(1) }
   elif kind == "semantic_tokens" {
      def path = to_str(e.get("path", ""))
      if path == _current_path() && int(_editor_get("lsp_dirty_at")) == 0 {
         mut dict by_line = dict(128)
         for token in e.get("items", []) {
            def key = to_str(int(token.get("line", 0)))
            by_line[key] = by_line.get(key, []).append(token)
         }
         _editor_values["semantic_path"] = path
         _editor_values["semantic_lines"] = by_line
         _editor_values["syntax_cache_rev"] = -1
         _editor_values["visible_runs_cache_key"] = ""
         _request_full_redraw(1)
      }
   }
   elif kind == "inlay_hints" {
      def path = to_str(e.get("path", ""))
      if path == _current_path() && int(_editor_get("lsp_dirty_at")) == 0 {
         mut dict by_line = dict(128)
         for hint in e.get("items", []) {
            def key = to_str(int(hint.get("line", 0)))
            by_line[key] = by_line.get(key, []).append(hint)
         }
         _editor_values["inlay_path"] = path
         _editor_values["inlay_lines"] = by_line
         _editor_values["visible_runs_cache_key"] = ""
         _request_full_redraw(1)
      }
   }
   elif kind == "folding_ranges" {
      def path = to_str(e.get("path", ""))
      if path == _current_path() && int(_editor_get("lsp_dirty_at")) == 0 {
         def ranges = e.get("items", [])
         mut dict keep = dict(0)
         def old = _editor_get("fold_collapsed")
         for r in ranges { def key = to_str(int(r.get("start", 0))) if old.contains(key) { keep[key] = true } }
         _editor_values["fold_path"] = path
         _editor_values["fold_ranges"] = ranges
         _editor_values["fold_collapsed"] = keep
         _request_full_redraw(1)
      }
   }
   elif kind == "code_lenses" {
      def path = to_str(e.get("path", ""))
      if path == _current_path() && int(_editor_get("lsp_dirty_at")) == 0 {
         mut dict by_line = dict(32)
         for lens in e.get("items", []) {
            def key = to_str(int(lens.get("line", 0)))
            by_line[key] = by_line.get(key, []).append(lens)
         }
         _editor_values["code_lens_path"] = path
         _editor_values["code_lens_lines"] = by_line
         _request_full_redraw(1)
      }
   }
   elif kind == "command" { _lsp_command_result(e) }
   elif kind == "completion" { _lsp_show_completion(e) }
   elif kind == "hover" { _lsp_show_hover(e) }
   elif kind == "signature" { _lsp_show_signature(e) }
   elif kind == "definition" { _lsp_jump(e.get("locations", []), "definition") }
   elif kind == "references" { _lsp_show_locations(e.get("locations", []), "references") }
   elif kind == "rename" { _lsp_apply_workspace(e.get("edits", dict(0)), "rename") }
   elif kind == "apply_edit" { _lsp_apply_workspace(e.get("edits", dict(0)), "edit") }
   elif kind == "format" { _lsp_apply_current(e.get("edits", []), "format") }
   elif kind == "symbols" { _lsp_show_symbols(e) }
   elif kind == "actions" { _lsp_show_actions(e) }
   elif kind == "message" || kind == "error" { _status(to_str(e.get("text", ""))) }
   0
}

fn _lsp_request(str kind) int {
   if ed.current_buffer(_editor_get("st")).get("readonly", false) { _status("lsp skipped for preview") return 0 }
   def path = _current_path()
   if path.len <= 0 { _status("save before lsp") return 0 }
   if !is_dict(_lsp().get("conn", nil)) { _editor_values["lsp_state"] = lsp.start(_lsp(), path, _current_text()) }
   elif !_lsp().get("docs", dict(0)).contains(lsp.uri_of(path)) { _editor_values["lsp_state"] = lsp.did_open(_lsp(), path, _current_text()) }
   _lsp_flush_change()
   def pos = _lsp_cursor()
   def int line = int(pos.get(0, 0))
   def int col = int(pos.get(1, 0))
   if LSP_TRACE { eprint("[lsp] request " + kind + " " + path + " " + to_str(line) + ":" + to_str(col) + " ready=" + to_str(lsp.ready(_lsp()))) }
   if kind == "completion" {
      _completion_open(false)
      _editor_values["lsp_state"] = lsp.completion(_lsp(), path, line, col)
   } elif kind == "hover" {
      _editor_values["lsp_state"] = lsp.hover(_lsp(), path, line, col)
   } elif kind == "signature" {
      _editor_values["lsp_state"] = lsp.signature_help(_lsp(), path, line, col)
   } elif kind == "definition" {
      _editor_values["lsp_state"] = lsp.definition(_lsp(), path, line, col)
   } elif kind == "references" {
      _editor_values["lsp_state"] = lsp.references(_lsp(), path, line, col)
   } elif kind == "format" {
      _editor_values["lsp_state"] = lsp.formatting(_lsp(), path)
   } elif kind == "symbols" {
      _editor_values["lsp_state"] = lsp.document_symbols(_lsp(), path)
   } elif kind == "actions" {
      _editor_values["lsp_state"] = lsp.code_actions(_lsp(), path, line, col)
   }
   _status(to_str(_lsp().get("last", "lsp")))
   0
}

fn _lsp_command_result(dict e) int {
   def action = e.get("result", nil)
   if !is_dict(action) { _status("lsp command complete") return 0 }
   def kind = to_str(action.get("kind", ""))
   def save_uri = to_str(action.get("save", ""))
   if save_uri.len > 0 && save_uri == lsp.uri_of(_current_path()) && bool(ed.current_buffer(_editor_get("st")).get("dirty", false)) { _save() }
   if kind == "message" {
      _status(to_str(action.get("text", "lsp command")))
   } elif kind == "process" {
      def cmd = to_str(action.get("command", ""))
      mut argv = []
      for arg in action.get("args", []) { argv = argv.append(to_str(arg)) }
      if cmd.len > 0 {
         def title = to_str(action.get("title", "command"))
         _editor_values["term_state"] = termpane.open_command(_editor_get("term_state"), title, cmd, argv, "$ " + str.join([cmd].extend(argv), " ") + "\n")
         _set_focus("terminal", false)
         _status(title)
      }
   } elif kind == "debug" {
      def cfg = action.get("configuration", dict(0))
      def program = to_str(cfg.get("program", ""))
      if program.len > 0 {
         _editor_values["dap_state"] = dap.launch(_dap(), program, [], to_str(cfg.get("cwd", "")))
         _editor_values["dap_panel"] = true
         _status(dap.status(_dap()))
      }
   } else { _status("lsp command complete") }
   0
}

fn _lsp_rename(str new_name) int {
   def path = _current_path()
   if path.len <= 0 || new_name.len <= 0 { return 0 }
   _lsp_flush_change()
   def pos = _lsp_cursor()
   _editor_values["lsp_state"] = lsp.rename(_lsp(), path, int(pos.get(0, 0)), int(pos.get(1, 0)), new_name)
   _status("rename to " + new_name)
   0
}

fn _lsp_diagnostics() int {
   def path = _current_path()
   if path.len <= 0 { _status("save before diagnostics") return 0 }
   if lsp.ready(_lsp()) {
      _editor_get("lsp_state")["diagnostics"] = lsp.diagnostics_for(_lsp(), path)
   } else {
      def res = tools.check_file(path)
      def ds = lsp.diagnostics_from_check(res)
      _editor_get("lsp_state")["diagnostics"] = ds
      _lsp().get("diag_by_path", dict(8))[path] = ds
   }
   _lsp_output("LSP diagnostics", lsp.diagnostics_text(_lsp()))
   _status(to_str(lsp.diagnostics_count(_lsp(), path)) + " diagnostics")
   0
}

;; ---- completion / hover / signature ----

fn _lsp_show_completion(dict e) int {
   if !bool(_lsp().get("popups", true)) { return 0 }
   if lsp.norm_path(to_str(e.get("path", ""))) != lsp.norm_path(_current_path()) || int(e.get("line", -1)) != int(_editor_get("st").get("cursor_line", 0)) { return 0 }
   def prefix = _word_prefix_before_cursor()
   def lp = str.lower(prefix)
   def items = e.get("items", [])
   mut exact = []
   mut loose = []
   mut int i = 0
   while i < items.len && exact.len < LSP_COMPLETION_LIMIT {
      def it = items.get(i, dict(0))
      i += 1
      def label = to_str(it.get("label", ""))
      def f = to_str(it.get("filter", label))
      if prefix.len > 0 {
         if str.startswith(f, prefix) { exact = exact.append(it) }
         elif loose.len < LSP_COMPLETION_LIMIT && str.startswith(str.lower(f), lp) { loose = loose.append(it) }
         continue
      }
      exact = exact.append(it)
   }
   mut out = exact
   mut int j = 0
   while j < loose.len && out.len < LSP_COMPLETION_LIMIT { out = out.append(loose.get(j)) j += 1 }
   if out.len <= 0 {
      if !bool(_editor_get("completion_state").get("open", false)) { _status("no completions") }
      return 0
   }
   _editor_get("completion_state")["open"] = true
   _editor_get("completion_state")["items"] = out
   _editor_get("completion_state")["index"] = 0
   _editor_get("completion_state")["top"] = 0
   _editor_get("completion_state")["prefix"] = prefix
   _editor_get("completion_state")["source"] = "lsp"
   0
}

fn _lsp_popup_open(str kind, str title, str text, int line, int col, int hl_start=-1, int hl_end=-1) int {
   _editor_values["lsp_popup"] = {"kind": kind, "title": title, "text": text, "line": line, "col": col, "path": _current_path(), "hl_start": hl_start, "hl_end": hl_end}
   _request_full_redraw(1)
   0
}

fn _lsp_popup_close() int {
   if _editor_get("lsp_popup").len > 0 { _editor_values["lsp_popup"] = dict(0) _request_full_redraw(1) }
   0
}

fn _lsp_show_hover(dict e) int {
   def text = str.strip(to_str(e.get("text", "")))
   if text.len <= 0 { _status("no hover information") return 0 }
   if !bool(_lsp().get("popups", true)) { _lsp_output("hover", text) return 0 }
   _lsp_popup_open("hover", "", text, int(_editor_get("st").get("cursor_line", 0)), int(_editor_get("st").get("cursor_col", 0)))
}

fn _lsp_show_signature(dict e) int {
   def sig = e.get("signature", dict(0))
   def label = to_str(sig.get("label", ""))
   if label.len <= 0 { _lsp_popup_close() return 0 }
   def doc = str.strip(to_str(sig.get("doc", "")))
   _lsp_popup_open("signature", label, doc, int(_editor_get("st").get("cursor_line", 0)), int(_editor_get("st").get("cursor_col", 0)), int(sig.get("param_start", -1)), int(sig.get("param_end", -1)))
}

;; ---- locations ----

fn _lsp_goto(dict loc) int {
   def path = to_str(loc.get("path", ""))
   if path.len <= 0 { return 0 }
   if lsp.norm_path(path) != lsp.norm_path(_current_path()) { _open_file(path) } else { _nav_push_current() }
   def lines = ed.current_lines(_editor_get("st"))
   def int line = int(min(max(int(loc.get("line", 0)), 0), max(0, lines.len - 1)))
   _editor_get("st")["cursor_line"] = line
   _editor_get("st")["cursor_col"] = lsp.editor_col(_lsp(), to_str(lines.get(line, "")), int(loc.get("col", 0)))
   _editor_values["st"] = ed.clamp_cursor(_editor_get("st"))
   _follow_cursor()
   _nav_record_current()
   0
}

fn _lsp_jump(list locs, str what) int {
   if locs.len <= 0 { _status("no " + what + " found") return 0 }
   _lsp_goto(locs.get(0, dict(0)))
   if locs.len > 1 { _lsp_show_locations(locs, what) }
   _status(what + (locs.len > 1 ? " (" + to_str(locs.len) + " candidates)" : ""))
   0
}

fn _lsp_file_line(str path, int line) str {
   if lsp.norm_path(path) == lsp.norm_path(_current_path()) { return _lsp_line_text(line) }
   def r = file_read(path)
   if !is_ok(r) { return "" }
   to_str(ed.split_lines(unwrap(r)).get(line, ""))
}

fn _lsp_show_locations(list locs, str what) int {
   if locs.len <= 0 { _status("no " + what + " found") return 0 }
   mut rows = []
   mut int i = 0
   while i < locs.len && i < 500 {
      def l = locs.get(i, dict(0))
      def p = to_str(l.get("path", ""))
      def int ln = int(l.get("line", 0))
      rows = rows.append(p + ":" + to_str(ln + 1) + ":" + to_str(int(l.get("col", 0)) + 1) + ":  " + str.strip(_lsp_file_line(p, ln)))
      i += 1
   }
   _lsp_output(what + " (" + to_str(locs.len) + ")", str.join(rows, "\n"))
   _status(to_str(locs.len) + " " + what)
   0
}

fn _lsp_show_symbols(dict e) int {
   def syms = e.get("symbols", [])
   if syms.len <= 0 { _status("no symbols") return 0 }
   def path = to_str(e.get("path", _current_path()))
   mut rows = []
   mut int i = 0
   while i < syms.len {
      def s = syms.get(i, dict(0))
      def p = to_str(s.get("path", path))
      rows = rows.append(p + ":" + to_str(int(s.get("line", 0)) + 1) + ":" + to_str(int(s.get("col", 0)) + 1) + ":  " + str.repeat("  ", int(s.get("depth", 0))) + to_str(s.get("kind", "")) + " " + to_str(s.get("name", "")) + "  " + to_str(s.get("detail", "")))
      i += 1
   }
   _lsp_output("symbols (" + to_str(syms.len) + ")", str.join(rows, "\n"))
   0
}

fn _lsp_show_actions(dict e) int {
   def acts = e.get("actions", [])
   if acts.len <= 0 { _status("no code actions here") return 0 }
   ;; Apply the preferred (first) quick fix; list the rest.
   def first = acts.get(0, dict(0))
   def edits = first.get("edit", dict(0))
   if is_dict(edits) && edits.len > 0 { _lsp_apply_workspace(edits, to_str(first.get("title", "action"))) }
   def c = first.get("command", nil)
   if is_dict(c) { _editor_values["lsp_state"] = lsp.execute_command(_lsp(), to_str(c.get("command", "")), c.get("arguments", [])) }
   if acts.len > 1 {
      mut rows = []
      mut int i = 0
      while i < acts.len { rows = rows.append(to_str(i + 1) + ". " + to_str(acts.get(i, dict(0)).get("title", ""))) i += 1 }
      _lsp_output("code actions", str.join(rows, "\n"))
   }
   0
}

;; ---- edits ----

fn _lsp_edit_before(dict a, dict b) bool {
   def int al = int(a.get("line", 0))
   def int bl = int(b.get("line", 0))
   al > bl || (al == bl && int(a.get("col", 0)) > int(b.get("col", 0)))
}

fn _lsp_apply_lines(list lines, list edits) list {
   ;; Apply from the end of the document backwards so earlier ranges stay valid.
   mut order = []
   mut int i = 0
   while i < edits.len {
      def e = edits.get(i, dict(0))
      mut int k = 0
      while k < order.len && !_lsp_edit_before(e, order.get(k, dict(0))) { k += 1 }
      order = slice(order, 0, k).append(e).extend(slice(order, k, order.len))
      i += 1
   }
   mut out = lines
   i = 0
   while i < order.len {
      def e = order.get(i, dict(0))
      def int sl = int(max(0, int(e.get("line", 0))))
      def int el = int(max(sl, int(e.get("end_line", sl))))
      def sline = sl < out.len ? to_str(out.get(sl, "")) : ""
      def eline = el < out.len ? to_str(out.get(el, "")) : ""
      def int sc = lsp.editor_col(_lsp(), sline, int(e.get("col", 0)))
      def int ec = lsp.editor_col(_lsp(), eline, int(e.get("end_col", 0)))
      def merged = str.str_slice(sline, 0, sc) + to_str(e.get("text", "")) + str.str_slice(eline, ec, str.utf8_len(eline))
      def mid = ed.split_lines(merged)
      def int hi = int(min(out.len, el + 1))
      out = slice(out, 0, int(min(sl, out.len))).extend(mid).extend(slice(out, hi, out.len))
      i += 1
   }
   out.len > 0 ? out : [""]
}

fn _lsp_apply_current(list edits, str what) int {
   if edits.len <= 0 { _status(what + ": no changes") return 0 }
   _editor_values["hist"] = history.push(_editor_get("hist"), _editor_get("st"))
   _editor_values["st"] = ed.set_lines(_editor_get("st"), _lsp_apply_lines(ed.current_lines(_editor_get("st")), edits))
   _buffer_changed(true)
   _status(what + ": " + to_str(edits.len) + " edit" + (edits.len == 1 ? "" : "s"))
   0
}

fn _lsp_apply_workspace(dict by_path, str what) int {
   if by_path.len <= 0 { _status(what + ": no changes") return 0 }
   def cur = _current_path()
   mut int files = 0
   mut int total = 0
   for p in by_path.keys() {
      def path = to_str(p)
      def edits = by_path.get(p, [])
      if edits.len <= 0 { continue }
      total += edits.len
      files += 1
      if lsp.norm_path(path) == lsp.norm_path(cur) {
         _editor_values["hist"] = history.push(_editor_get("hist"), _editor_get("st"))
         _editor_values["st"] = ed.set_lines(_editor_get("st"), _lsp_apply_lines(ed.current_lines(_editor_get("st")), edits))
         continue
      }
      ;; Another open buffer: edit it in place; otherwise rewrite the file.
      mut bool done = false
      mut bs = _editor_get("st").get("buffers", [])
      mut int bi = 0
      while bi < bs.len {
         mut b = bs.get(bi, dict(0))
         if is_dict(b) && lsp.norm_path(_buffer_source_path(b)) == lsp.norm_path(path) {
            b["lines"] = _lsp_apply_lines(b.get("lines", [""]), edits)
            b["dirty"] = true
            bs[bi] = b
            done = true
            break
         }
         bi += 1
      }
      if done { _editor_get("st")["buffers"] = bs continue }
      def r = file_read(path)
      if is_ok(r) {
         def text = ed.join_lines(_lsp_apply_lines(ed.split_lines(unwrap(r)), edits))
         file_write(path, text)
      }
   }
   _buffer_changed(true)
   _status(what + ": " + to_str(total) + " edits in " + to_str(files) + " file" + (files == 1 ? "" : "s"))
   0
}

;; ---- drawing ----

def C_DIAG_ERROR = gfx.color_hex("#ff6b7a")
def C_DIAG_WARN = gfx.color_hex("#f2c56b")
def C_DIAG_INFO = gfx.color_hex("#6bb8ff")
def C_DIAG_HINT = gfx.color_hex("#8a8f98")

fn _diag_color(int severity) any {
   severity == 1 ? C_DIAG_ERROR : (severity == 2 ? C_DIAG_WARN : (severity == 3 ? C_DIAG_INFO : C_DIAG_HINT))
}

fn _draw_squiggle(f64 x0, f64 x1, f64 y, any c) int {
   ;; A 2px zig-zag drawn as alternating dashes: cheap and crisp at any scale.
   mut f64 x = x0
   mut int k = 0
   while x < x1 {
      def f64 w = float(min(3.0, x1 - x))
      _fill_rect(x, y + (k % 2 == 0 ? 0.0 : 1.5), w, 1.5, c)
      x += 3.0
      k += 1
   }
   0
}

fn _draw_diagnostics(dict lay) int {
   def path = _current_path()
   if path.len <= 0 { return 0 }
   def ds = lsp.diagnostics_for(_lsp(), path)
   if ds.len <= 0 { return 0 }
   def lines = ed.current_lines(_editor_get("st"))
   def int scroll = int(_editor_get("st").get("scroll", 0))
   def int rows = ed.visible_rows(float(lay.get("edit_h", 0.0)))
   def view = _fold_visible_indices(lines)
   def int start = _fold_first_visible_at_or_after(view, scroll)
   def f64 text_x = _line_text_x(lay)
   def f64 edit_x = float(lay.get("edit_x", 0.0))
   def f64 right = edit_x + float(lay.get("edit_w", 0.0)) - 12.0
   def f64 top = float(lay.get("edit_y", 0.0))
   ;; Strongest diagnostic per line wins the gutter mark and the inline message.
   mut dict worst = dict(16)
   mut int i = 0
   while i < ds.len {
      def d = ds.get(i, dict(0))
      i += 1
      def int sl = int(d.get("line", 0))
      def int el = int(max(sl, int(d.get("end_line", sl))))
      def int sev = int(d.get("severity", 1))
      def c = _diag_color(sev)
      mut int ln = int(max(sl, scroll))
      while ln <= el && ln < lines.len {
         def int sr = _fold_visible_index(view, ln) - start
         if sr < 0 || sr >= rows { ln += 1 continue }
         def text = to_str(lines.get(ln, ""))
         def int a = ln == sl ? lsp.editor_col(_lsp(), text, int(d.get("col", 0))) : 0
         mut int b = ln == el ? lsp.editor_col(_lsp(), text, int(d.get("end_col", 0))) : str.utf8_len(text)
         if b <= a { b = a + 1 }
         def f64 y = float(_editor_row_y(top, sr)) + float(ed.LINE_H) - 3.0
         def f64 x0 = text_x + _visual_prefix(text, ln, a)
         def f64 x1 = float(min(right, float(max(x0 + 6.0, text_x + _visual_prefix(text, ln, b)))))
         _draw_squiggle(x0, x1, y, c)
         ln += 1
      }
      def key = to_str(sl)
      if !worst.contains(key) || int(worst.get(key, dict(0)).get("severity", 9)) > sev { worst[key] = d }
   }
   mut runs = []
   def int sf = int(_editor_get("font_small"))
   for k in worst.keys() {
      def d = worst.get(k, dict(0))
      def int ln = int(d.get("line", 0))
      def int sr = _fold_visible_index(view, ln) - start
      if sr < 0 || sr >= rows { continue }
      def int sev = int(d.get("severity", 1))
      def c = _diag_color(sev)
      def f64 row_y = float(_editor_row_y(top, sr))
      ;; gutter dot
      _fill_rect(edit_x + 4.0, row_y + float(ed.LINE_H) * 0.5 - 3.0, 6.0, 6.0, c)
      ;; inline message after the code, faded, truncated to the pane
      def text = to_str(lines.get(ln, ""))
      def f64 mx = text_x + _visual_prefix(text, ln, str.utf8_len(text)) + 28.0
      if mx < right - 40.0 {
         def msg = _fit_chrome_text(sf, str.str_replace(to_str(d.get("message", "")), "\n", "  "), right - mx)
         _fill_rect(mx - 10.0, row_y + 3.0, _measure_ui(sf, msg, 0.0) + 20.0, float(ed.LINE_H) - 6.0, gfx.color_alpha(c, 0.10))
         runs = _append_text_run(runs, msg, mx, _row_text_y(sf, row_y, float(ed.LINE_H)), _pack(gfx.color_alpha(c, 0.92)))
      }
   }
   _flush_rects()
   if runs.len > 0 { gfx.draw_text_runs_flat_colors(sf, runs) }
   0
}

fn _lsp_plain(str line) str {
   ;; Markdown to plain text for popups: drop emphasis and code markers,
   ;; turn headings and list markers into clean prefixes.
   mut t = str.str_replace(str.str_replace(str.str_replace(line, "**", ""), "__", ""), "`", "")
   def lead = str.strip(t)
   if str.startswith(lead, "- ") || str.startswith(lead, "* ") { t = "•  " + str.str_slice(lead, 2, str.utf8_len(lead)) }
   elif str.startswith(lead, "#") {
      mut int i = 0
      while i < lead.len && load8(lead, i) == 35 { i += 1 }
      t = str.strip(str.str_slice(lead, i, str.utf8_len(lead)))
   }
   t
}

fn _lsp_wrap(int font, str text, f64 width, int max_lines) list {
   mut out = []
   def paras = str.split(text, "\n")
   mut int p = 0
   mut bool last_blank = true
   while p < paras.len && out.len < max_lines {
      def raw = to_str(paras.get(p, ""))
      p += 1
      if str.startswith(str.strip(raw), "```") { continue }
      def para = _lsp_plain(raw)
      if str.strip(para).len == 0 {
         if !last_blank { out = out.append("") }
         last_blank = true
         continue
      }
      last_blank = false
      def words = str.split(para, " ")
      mut line = ""
      mut int w = 0
      while w < words.len {
         def word = to_str(words.get(w, ""))
         def cand = line.len > 0 ? line + " " + word : word
         if line.len > 0 && _measure_ui(font, cand, 0.0) > width {
            out = out.append(line)
            if out.len >= max_lines { break }
            line = word
         } else { line = cand }
         w += 1
      }
      if out.len < max_lines { out = out.append(line) }
   }
   out
}

fn _draw_lsp_popup(dict lay, f64 sw, f64 sh) int {
   def pop = _editor_get("lsp_popup")
   if pop.len <= 0 { return 0 }
   if to_str(pop.get("path", "")) != _current_path() || int(pop.get("line", -1)) != int(_editor_get("st").get("cursor_line", 0)) {
      _editor_values["lsp_popup"] = dict(0)
      return 0
   }
   if bool(_editor_get("completion_state").get("open", false)) { return 0 }
   def int bf = int(_editor_get("font_body"))
   def int sf = int(_editor_get("font_small"))
   def lines = ed.current_lines(_editor_get("st"))
   def int row = int(pop.get("line", 0))
   def int scroll = int(_editor_get("st").get("scroll", 0))
   def line = to_str(lines.get(int(min(max(row, 0), max(0, lines.len - 1))), ""))
   def f64 ax = _line_text_x(lay) + _visual_prefix(line, row, int(pop.get("col", 0)))
   def f64 row_y = float(_editor_row_y(float(lay.get("edit_y", 0.0)), int(max(0, row - scroll))))
   def f64 w = float(min(620.0, max(320.0, sw * 0.46)))
   def f64 pad = 12.0
   def title = to_str(pop.get("title", ""))
   def body = _lsp_wrap(sf, to_str(pop.get("text", "")), w - pad * 2.0, 14)
   def f64 lh = 18.0
   def f64 title_h = title.len > 0 ? 28.0 : 0.0
   def f64 h = pad * 2.0 + title_h + float(body.len) * lh
   ;; Prefer above the cursor line, like a tooltip; fall below when no room.
   mut f64 y = row_y - h - 6.0
   if y < float(lay.get("edit_y", 0.0)) { y = row_y + float(ed.LINE_H) + 6.0 }
   def fit = _fit_rect(ax - pad, y, w, h, sw, sh - float(ed.STATUS_H), 8.0)
   def f64 x = float(fit.get(0, ax))
   y = float(fit.get(1, y))
   _fill_rect(x + 2.0, y + 3.0, w, h, gfx.color_alpha(C_BG, 0.45))
   _fill_rect(x, y, w, h, gfx.color_alpha(C_PANE_2, 0.985))
   _stroke_rect(x, y, w, h, C_LINE_2, 1.0)
   _fill_rect(x, y, 2.0, h, to_str(pop.get("kind", "")) == "signature" ? C_CYAN : C_ACCENT)
   mut body_runs = []
   mut small_runs = []
   if title.len > 0 {
      def ty = _row_text_y(bf, y + pad - 4.0, 28.0)
      def int hs = int(pop.get("hl_start", -1))
      def int he = int(pop.get("hl_end", -1))
      def t = _fit_chrome_text(bf, title, w - pad * 2.0)
      if hs >= 0 && he > hs && he <= t.len {
         def before = str.str_slice(t, 0, hs)
         def mid = str.str_slice(t, hs, he)
         def f64 bx = x + pad + _measure_ui(bf, before, 0.0)
         _fill_rect(bx - 2.0, y + pad, _measure_ui(bf, mid, 0.0) + 4.0, 22.0, gfx.color_alpha(C_CYAN, 0.18))
         body_runs = _append_text_run(body_runs, t, x + pad, ty, U_TEXT)
      } else {
         body_runs = _append_text_run(body_runs, t, x + pad, ty, U_TEXT)
      }
      _fill_rect(x + pad, y + pad + title_h - 3.0, w - pad * 2.0, 1.0, C_LINE)
   }
   mut int i = 0
   while i < body.len {
      def by = y + pad + title_h + float(i) * lh
      small_runs = _append_text_run(small_runs, to_str(body.get(i, "")), x + pad, _row_text_y(sf, by, lh), U_MUTED)
      i += 1
   }
   _flush_rects()
   if body_runs.len > 0 { gfx.draw_text_runs_flat_colors(bf, body_runs) }
   if small_runs.len > 0 { gfx.draw_text_runs_flat_colors(sf, small_runs) }
   0
}

;; ---- debugger (DAP) -------------------------------------------------------
;; F8 starts (or F5 continues), F9 toggles a breakpoint (also a gutter click),
;; C-SPC d n/s/f step over/in/out, C-SPC d q stops. Execution stops jump to
;; the source line; a panel shows the stack, variables and program output.

def C_BREAK = gfx.color_hex("#ff5f6d")
def C_EXEC = gfx.color_hex("#ffc777")

fn _dap() dict { _editor_get("dap_state") }

fn _dap_start() int {
   if dap.running(_dap()) {
      if dap.stopped(_dap()) { _editor_values["dap_state"] = dap.continue_run(_dap()) }
      _status(dap.status(_dap()))
      return 0
   }
   def path = _current_path()
   if path.len <= 0 || !file_exists(path) { _status("save the file before debugging") return 0 }
   if !ed.current_buffer(_editor_get("st")).get("readonly", false) && ed.current_buffer(_editor_get("st")).get("dirty", false) { _save() }
   _editor_values["dap_state"] = dap.launch(_dap(), path, [], project.root(_editor_get("project_state")))
   _editor_values["dap_panel"] = true
   _status(dap.status(_dap()))
   0
}

fn _dap_toggle_breakpoint(int line=-1) int {
   def path = _current_path()
   if path.len <= 0 { _status("save the file before adding breakpoints") return 0 }
   def int row = line >= 0 ? line : int(_editor_get("st").get("cursor_line", 0))
   _editor_values["dap_state"] = dap.toggle_breakpoint(_dap(), path, row)
   _status((dap.has_breakpoint(_dap(), path, row) ? "breakpoint at " : "removed breakpoint at ") + ospath.basename(path) + ":" + to_str(row + 1))
   _request_full_redraw(1)
   0
}

fn _dap_step(str how) int {
   if !dap.running(_dap()) { _status("no debug session (F8 starts one)") return 0 }
   if how == "pause" { _editor_values["dap_state"] = dap.pause(_dap()) }
   elif !dap.stopped(_dap()) { _status("program is running") return 0 }
   elif how == "continue" { _editor_values["dap_state"] = dap.continue_run(_dap()) }
   elif how == "next" { _editor_values["dap_state"] = dap.step_over(_dap()) }
   elif how == "step" { _editor_values["dap_state"] = dap.step_in(_dap()) }
   elif how == "finish" { _editor_values["dap_state"] = dap.step_out(_dap()) }
   _status(dap.status(_dap()))
   0
}

fn _dap_stop() int {
   if !dap.running(_dap()) { _status("no debug session") return 0 }
   _editor_values["dap_state"] = dap.stop(_dap())
   _status("debug session stopped")
   _request_full_redraw(1)
   0
}

fn _dap_evaluate() int {
   if !dap.stopped(_dap()) { _status("evaluate needs a stopped program") return 0 }
   def sel = ed.selection_valid(_editor_get("st")) ? ed.selection_text(_editor_get("st")) : _word_at_cursor()
   if sel.len <= 0 { _status("nothing to evaluate") return 0 }
   _editor_values["dap_state"] = dap.evaluate(_dap(), sel)
   0
}

fn _dap_show_location() int {
   def loc = dap.stop_location(_dap())
   def path = to_str(loc.get("path", ""))
   if path.len <= 0 || !file_exists(path) { return 0 }
   if lsp.norm_path(path) != lsp.norm_path(_current_path()) { _open_file(path) }
   def lines = ed.current_lines(_editor_get("st"))
   _editor_get("st")["cursor_line"] = int(min(max(int(loc.get("line", 0)), 0), max(0, lines.len - 1)))
   _editor_get("st")["cursor_col"] = 0
   _editor_values["st"] = ed.clamp_cursor(_editor_get("st"))
   _follow_cursor()
   _request_full_redraw(1)
   0
}

fn _dap_pump() int {
   if !is_dict(_dap().get("conn", nil)) { return 0 }
   def events = dap.pump(_dap())
   mut int i = 0
   while i < events.len {
      def e = events.get(i, dict(0))
      def kind = to_str(e.get("kind", ""))
      if kind == "frames" { _dap_show_location() _status(dap.status(_dap())) }
      elif kind == "evaluate" { _status(to_str(e.get("expr", "")) + " = " + to_str(e.get("value", ""))) }
      elif kind == "exited" || kind == "terminated" || kind == "error" { _status(dap.status(_dap())) _request_full_redraw(1) }
      elif kind == "output" || kind == "continued" || kind == "variables" { _request_full_redraw(1) }
      i += 1
   }
   0
}

fn _draw_debug_gutter(dict lay) int {
   def path = _current_path()
   if path.len <= 0 { return 0 }
   def bps = dap.breakpoints_for(_dap(), path)
   def loc = dap.stopped(_dap()) ? dap.stop_location(_dap()) : dict(0)
   def bool here = loc.len > 0 && lsp.norm_path(to_str(loc.get("path", ""))) == lsp.norm_path(path)
   if bps.len <= 0 && !here { return 0 }
   def int scroll = int(_editor_get("st").get("scroll", 0))
   def int rows = ed.visible_rows(float(lay.get("edit_h", 0.0)))
   def lines = ed.current_lines(_editor_get("st"))
   def view = _fold_visible_indices(lines)
   def int start = _fold_first_visible_at_or_after(view, scroll)
   def f64 top = float(lay.get("edit_y", 0.0))
   def f64 gx = float(lay.get("edit_x", 0.0))
   def f64 lh = float(ed.LINE_H)
   if here {
      def int ln = int(loc.get("line", 0))
      def int sr = _fold_visible_index(view, ln) - start
      if sr >= 0 && sr < rows {
         def f64 y = float(_editor_row_y(top, sr))
         _fill_rect(gx, y, float(lay.get("edit_w", 0.0)), lh, gfx.color_alpha(C_EXEC, 0.10))
         _fill_rect(gx, y, 3.0, lh, C_EXEC)
      }
   }
   mut int i = 0
   while i < bps.len {
      def int ln = int(bps.get(i, -1))
      i += 1
      def int sr = _fold_visible_index(view, ln) - start
      if sr < 0 || sr >= rows { continue }
      def f64 y = float(_editor_row_y(top, sr))
      def f64 d = float(min(9.0, lh - 8.0))
      ;; A rounded dot left of the line numbers: a cross of rects reads as a
      ;; circle at this size.
      def f64 cx = gx + 4.0
      def f64 cy = y + (lh - d) * 0.5
      _fill_rect(cx + 2.0, cy, d - 4.0, d, C_BREAK)
      _fill_rect(cx, cy + 2.0, d, d - 4.0, C_BREAK)
      _fill_rect(cx + 1.0, cy + 1.0, d - 2.0, d - 2.0, C_BREAK)
   }
   _flush_rects()
   0
}

fn _draw_debug_panel(dict lay, f64 sw, f64 sh) int {
   if !bool(_editor_values.get("dap_panel", false)) { return 0 }
   def st = _dap()
   if !dap.running(st) && dap.output_text(st).len <= 0 { return 0 }
   def int bf = int(_editor_get("font_body"))
   def int sf = int(_editor_get("font_small"))
   def f64 w = float(min(440.0, max(300.0, float(lay.get("edit_w", 0.0)) * 0.36)))
   def f64 x = float(lay.get("edit_x", 0.0)) + float(lay.get("edit_w", 0.0)) - w - 14.0
   def f64 y = float(lay.get("edit_y", 0.0)) + 12.0
   mut rows = []
   def frames = dap.stack(st)
   mut int i = 0
   while i < frames.len && i < 6 {
      def f = frames.get(i, dict(0))
      rows = rows.append([(i == int(st.get("frame", 0)) ? "▸ " : "  ") + to_str(f.get("name", "?")), ospath.basename(to_str(f.get("path", ""))) + ":" + to_str(int(f.get("line", 0)) + 1), i == 0 ? U_TEXT : U_MUTED])
      i += 1
   }
   for s in dap.variables(st) {
      def vars = s.get("vars", [])
      if vars.len <= 0 { continue }
      rows = rows.append([to_str(s.get("name", "")), "", U_ACCENT])
      mut int j = 0
      while j < vars.len && j < 8 {
         def v = vars.get(j, dict(0))
         rows = rows.append(["  " + to_str(v.get("name", "")), to_str(v.get("value", "")), U_MUTED])
         j += 1
      }
   }
   def out_lines = str.split(str.strip(dap.output_text(st)), "\n")
   mut tail = []
   mut int k = int(max(0, out_lines.len - 5))
   while k < out_lines.len { def l = to_str(out_lines.get(k, "")) if l.len > 0 && !str.startswith(l, "ny-dap: building") { tail = tail.append(l) } k += 1 }
   def f64 rh = 20.0
   def f64 head_h = 32.0
   def f64 h = head_h + float(rows.len) * rh + (tail.len > 0 ? 10.0 + float(tail.len) * 18.0 : 0.0) + 10.0
   _fill_rect(x + 2.0, y + 3.0, w, h, gfx.color_alpha(C_BG, 0.45))
   _fill_rect(x, y, w, h, gfx.color_alpha(C_PANE_2, 0.97))
   _stroke_rect(x, y, w, h, C_LINE_2, 1.0)
   def phase = dap.running(st) ? (dap.stopped(st) ? "Paused" : "Running") : "Ended"
   def pc = dap.stopped(st) ? C_EXEC : (dap.running(st) ? C_OK : C_DIM)
   _fill_rect(x + 12.0, y + 13.0, 7.0, 7.0, pc)
   mut body = []
   mut small = []
   body = _append_text_run(body, phase, x + 26.0, _row_text_y(bf, y + 2.0, 28.0), U_TEXT)
   small = _append_right_run(small, sf, _fit_chrome_text(sf, dap.status(st), w * 0.6), x + w - 12.0, _row_text_y(sf, y + 2.0, 28.0), U_DIM)
   _fill_rect(x + 10.0, y + head_h - 2.0, w - 20.0, 1.0, C_LINE)
   mut int r = 0
   while r < rows.len {
      def row = rows.get(r, [])
      def f64 ry = y + head_h + float(r) * rh
      def left = _fit_chrome_text(sf, to_str(row.get(0, "")), w * 0.48)
      small = _append_text_run(small, left, x + 14.0, _row_text_y(sf, ry, rh), int(row.get(2, U_MUTED)))
      def right = _fit_chrome_text(sf, to_str(row.get(1, "")), w * 0.46)
      small = _append_right_run(small, sf, right, x + w - 14.0, _row_text_y(sf, ry, rh), U_DIM)
      r += 1
   }
   if tail.len > 0 {
      def f64 oy = y + head_h + float(rows.len) * rh + 6.0
      _fill_rect(x + 10.0, oy, w - 20.0, 1.0, C_LINE)
      mut int t = 0
      while t < tail.len {
         small = _append_text_run(small, _fit_chrome_text(sf, to_str(tail.get(t, "")), w - 28.0), x + 14.0, _row_text_y(sf, oy + 4.0 + float(t) * 18.0, 18.0), U_ACCENT_2)
         t += 1
      }
   }
   _flush_rects()
   if body.len > 0 { gfx.draw_text_runs_flat_colors(bf, body) }
   if small.len > 0 { gfx.draw_text_runs_flat_colors(sf, small) }
   0
}

fn _run_command(str id) int {
   _editor_values["ui_state"] = interact.history_add(_editor_get("ui_state"), id)
   _request_full_redraw(2)
   if id != "quit" {
      _editor_get("st")["quit_prompt"] = false
      _editor_values["last_escape_ns"] = 0
   }
   if id == "save" { _save() }
   elif id == "save-all" { _save_all() }
   elif id == "reload" { _reload_from_disk() }
   elif id == "copy-path" { _copy_file_path() }
   elif id == "copy-relative-path" { _copy_relative_path(_current_path()) }
   elif id == "copy-line-info" { _copy_line_info() }
   elif id == "open-file" { _status("use project tree or drag-drop to open") }
   elif id == "new-file" { _start_new_file(session.project_entry_for_path(_editor_get("project_state"), _current_path())) }
   elif id == "delete-file" { _delete_project_entry(session.project_entry_for_path(_editor_get("project_state"), _current_path())) }
   elif id == "undo-file-op" { _undo_file_op() }
   elif id == "redo-file-op" { _redo_file_op() }
   elif id == "new-scratch" { _new_scratch() }
   elif id == "kill-buffer" { _kill_buffer() }
   elif id == "diff-clipboard" { _diff_clipboard() }
   elif id == "run" { _run_current_file() }
   elif id == "check" { _check_current_file() }
   elif id == "format" { _format_current_file() }
   elif id == "git-blame" { _git_inline_toggle() }
   elif id == "fold-toggle" { _fold_toggle_at_cursor() }
   elif id == "fold-close" { _fold_close_at_cursor() }
   elif id == "fold-open-all" { _fold_open_all() }
   elif id == "debug" || id == "dap-debug" || id == "debug-start" { _dap_start() }
   elif id == "debug-stop" { _dap_stop() }
   elif id == "debug-pause" { _dap_step("pause") }
   elif id == "debug-eval" { _dap_evaluate() }
   elif id == "debug-panel" { _editor_values["dap_panel"] = !bool(_editor_values.get("dap_panel", true)) _request_full_redraw(1) }
   elif id == "debug-terminal" { _debug_start_current() }
   elif id == "debug-break_point" { _dap_toggle_breakpoint() }
   elif id == "debug-continue" { if dap.running(_dap()) { _dap_step("continue") } else { _dap_start() } }
   elif id == "debug-next" { _dap_step("next") }
   elif id == "debug-step" { _dap_step("step") }
   elif id == "debug-finish" { _dap_step("finish") }
   elif id == "debug-backtrace" { _lsp_output("debugger", dap.state_text(_dap()) + "\n\nOutput\n" + dap.output_text(_dap()) + "\n\nDebugger\n" + dap.console_text(_dap())) }
   elif id == "debug-until-line" { _debug_until_current_line() }
   elif id == "debug-jump-line" { _debug_jump_current_line() }
   elif id == "debug-inspect-line" { _debug_inspect_line() }
   elif id == "debug-reverse-continue" { _debug_send("reverse-continue") }
   elif id == "debug-reverse-next" { _debug_send("reverse-next") }
   elif id == "debug-reverse-step" { _debug_send("reverse-step") }
   elif id == "debug-coredump" { _debug_load_coredump() }
   elif id == "refresh" { _editor_values["project_state"] = project.refresh_git(_editor_get("project_state")) _editor_get("st")["project_loaded"] = true _editor_get("st")["git_loaded"] = true _invalidate_project_definitions() _status("project refreshed") }
   elif id == "rename" { _start_rename(prompt.context_entry(_editor_get("context_state"))) }
   elif id == "find" { _find_open() }
   elif id == "find-buffers" { _find_buffers_open() }
   elif id == "replace" { _find_replace_open() }
   elif id == "find-next" { _find_step(1) }
   elif id == "find-prev" { _find_step(-1) }
   elif id == "replace-current" { _find_replace_current() }
   elif id == "replace-all" { _find_replace_all() }
   elif id == "find-toggle-regex" { _find_toggle("regex") }
   elif id == "find-toggle-case" { _find_toggle("case") }
   elif id == "find-toggle-word" { _find_toggle("word") }
   elif id == "palette" { _palette_open() }
   elif id == "switch-buffer" { _switch_buffer_open() }
   elif id == "describe-key" {
      _editor_values["describe_key"] = true
      _status("Describe key: press a key or chord")
   }
   elif id == "describe-bindings" { _palette_open() }
   elif id == "describe-prefix" { _open_chord_prefix("C-h") }
   elif id == "toggle-keymap" {
      def next_map = cmd.keymap() == "emacs" ? "default" : "emacs"
      cmd.set_keymap(next_map)
      _status("keymap: " + next_map)
   }
   elif id == "quit" {
      if _editor_get("st").get("quit_prompt", false) { window.set_should_close(_editor_get("win"), true) }
      else { _editor_get("st")["quit_prompt"] = true _status("quit? run quit again") }
   }
   elif id == "undo" { _undo() }
   elif id == "redo" { _redo() }
   elif id == "copy" {
      _copy_selection_or_buffer()
      _deactivate_mark()
   }
   elif id == "cut" {
      _cut_selection()
      _editor_get("st")["mark_active"] = false
   }
   elif id == "paste" { _paste_clipboard() }
   elif id == "mark-whole" { _mark_whole_buffer() }
   elif id == "set-mark" { _set_mark() }
   elif id == "exchange-mark" { _exchange_mark() }
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
   elif id == "next" { _nav_push_current() _editor_values["st"] = ed.switch_buffer(_editor_get("st"), 1) _buffer_changed(true) _nav_record_current() _status("next buffer") }
   elif id == "prev" { _nav_push_current() _editor_values["st"] = ed.switch_buffer(_editor_get("st"), -1) _buffer_changed(true) _nav_record_current() _status("previous buffer") }
   elif id == "go-back" { _nav_go(-1) }
   elif id == "go-forward" { _nav_go(1) }
   elif id == "go-definition" { _goto_definition() }
   elif id == "toggle-sidebar" {
      def on = !bool(_editor_get("st").get("show_hierarchy", true))
      _editor_get("st")["show_hierarchy"] = on
      if on && !bool(_editor_get("st").get("show_project", true)) && !bool(_editor_get("st").get("show_outline", true)) { _editor_get("st")["show_project"] = true }
      if !on && to_str(_editor_get("st").get("focus", "editor")) == "project" { _set_focus("editor", false) }
      _save_ui_cache()
      _status(on ? "hierarchy on" : "hierarchy off")
   }
   elif id == "toggle-titlebar" { _status(_set_ui_toggle("titlebar") ? "title bar on" : "title bar off") }
   elif id == "zoom-in" { _zoom_editor_font(1) }
   elif id == "zoom-out" { _zoom_editor_font(-1) }
   elif id == "zoom-reset" { _reset_editor_font() }
   elif id == "pane-next" { _focus_next() }
   elif id == "pane-project" { _editor_get("st")["show_hierarchy"] = true _save_ui_cache() _set_focus("project") }
   elif id == "pane-editor" { _set_focus("editor") }
   elif id == "pane-terminal" { if termpane.is_open(_editor_get("term_state")) { _set_focus("terminal") } else { _status("terminal closed") } }
   elif id == "pane-close" { _close_focused_pane() }
   elif id == "pane-only" { _editor_get("st")["show_hierarchy"] = false _editor_values["term_state"] = termpane.dismiss(_editor_get("term_state")) _save_ui_cache() _set_focus("editor") }
   elif id == "pane-split-below" { _editor_values["term_state"] = termpane.open_shell(_editor_get("term_state")) _set_focus("terminal") }
   elif id == "pane-split-right" { _editor_get("st")["show_hierarchy"] = true _editor_get("st")["show_project"] = true _editor_get("st")["show_outline"] = true _save_ui_cache() _set_focus("project") }
   elif id == "pane-balance" { _editor_get("st")["rail_w"] = 240.0 _editor_get("st")["rail_split"] = 0.0 _editor_get("st")["term_h"] = 200.0 _save_ui_cache() _status("panes balanced") }
   elif id == "toggle-fullscreen" { window.toggle_window_fullscreen(_editor_get("win")) _status("fullscreen") }
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
   elif id == "toggle-terminal" { _editor_values["term_state"] = termpane.toggle_shell(_editor_get("term_state")) _set_focus(termpane.is_open(_editor_get("term_state")) ? "terminal" : "editor") }
   elif id == "toggle-repl" { _editor_values["term_state"] = termpane.toggle_repl(_editor_get("term_state")) _set_focus(termpane.is_open(_editor_get("term_state")) ? "terminal" : "editor") }
   elif id == "terminal-shell" { _editor_values["term_state"] = termpane.show_shell(_editor_get("term_state")) _set_focus("terminal") }
   elif id == "terminal-repl" { _editor_values["term_state"] = termpane.show_repl(_editor_get("term_state")) _set_focus("terminal") }
   elif id == "terminal-next" { _editor_values["term_state"] = termpane.next_tab(_editor_get("term_state")) _set_focus("terminal") }
   elif id == "terminal-prev" { _editor_values["term_state"] = termpane.prev_tab(_editor_get("term_state")) _set_focus("terminal") }
   elif id == "terminal-close" { _editor_values["term_state"] = termpane.close_tab(_editor_get("term_state")) _set_focus(termpane.is_open(_editor_get("term_state")) ? "terminal" : "editor") }
   elif id == "toggle-lsp-popups" { _editor_values["lsp_state"] = lsp.toggle_popups(_editor_get("lsp_state")) _status(to_str(_editor_get("lsp_state").get("last", "lsp"))) }
   elif id == "lsp-status" { _lsp_output("LSP status", lsp.status(_editor_get("lsp_state")) + "\n\n" + lsp.diagnostics_text(_editor_get("lsp_state"))) _status("lsp status") }
   elif id == "lsp-start" { _lsp_start() }
   elif id == "lsp-restart" { _editor_values["lsp_state"] = lsp.restart(_editor_get("lsp_state"), _current_path(), _current_text()) _lsp_output("LSP status", lsp.status(_editor_get("lsp_state"))) _status(lsp.status(_editor_get("lsp_state"))) }
   elif id == "lsp-stop" { _editor_values["lsp_state"] = lsp.stop(_editor_get("lsp_state")) _status(lsp.status(_editor_get("lsp_state"))) }
   elif id == "lsp-diagnostics" { _lsp_diagnostics() }
   elif id == "lsp-complete" { _lsp_request("completion") }
   elif id == "lsp-hover" { _lsp_request("hover") }
   elif id == "lsp-definition" { _lsp_request("definition") }
   elif id == "lsp-references" { _lsp_request("references") }
   elif id == "lsp-format" { _lsp_request("format") }
   elif id == "lsp-symbols" { _lsp_request("symbols") }
   elif id == "lsp-actions" { _lsp_request("actions") }
   elif id == "lsp-signature" { _lsp_request("signature") }
   elif id == "lsp-rename" {
      def word = _word_at_cursor()
      if word.len <= 0 { _status("no symbol under cursor") }
      else { _editor_values["rename_state"] = prompt.rename_start(_editor_get("rename_state"), {"name": word, "action": "symbol", "rel": word}) }
   }
   else { _status("unknown command " + id) }
   if id != "palette" && id != "switch-buffer" && id != "find-buffers" { _editor_values["palette_state"] = pal.dismiss(_editor_get("palette_state")) }
   0
}

fn _buffer_switch_rows() list {
   ;; Most recent first: the buffer you came from is one RET away, as in
   ;; Emacs `switch-to-buffer`.
   def bs = _editor_get("st").get("buffers", [])
   def active = int(_editor_get("st").get("active", 0))
   mut rows = []
   mut pass = 0
   while pass < 2 {
      mut i = 0
      while i < bs.len {
         if (pass == 0) == (i != active) {
            def any b = bs.get(i, {})
            def name = to_str(b.get("name", "buffer"))
            def path = to_str(b.get("path", ""))
            def flags = (bool(b.get("dirty", false)) ? "* " : "") + (bool(b.get("readonly", false)) ? "% " : "")
            rows = rows.append([flags + name, "buffer:" + to_str(i), i == active ? "current" : "", path.len > 0 ? path : name, "buffer"])
         }
         i += 1
      }
      pass += 1
   }
   rows
}

fn _switch_buffer_open() int {
   _editor_get("palette_state")["cfg"] = _editor_get("command_cfg")
   _editor_values["palette_state"] = pal.open_items(_editor_get("palette_state"), _buffer_switch_rows(), "buffers")
   _editor_values["context_state"] = prompt.context_close(_editor_get("context_state"))
   _editor_values["find_state"] = find.dismiss(_editor_get("find_state"))
   _request_full_redraw(3)
   _status("switch to buffer")
   0
}

@inline
fn _ascii_fold(int byte_value) int {
   byte_value >= 65 && byte_value <= 90 ? byte_value + 32 : byte_value
}

fn _find_buffers_next(str line, str needle, int start) int {
   def last = line.len - needle.len
   mut i = start
   while i <= last {
      mut j = 0
      while j < needle.len && _ascii_fold(load8(line, i + j)) == load8(needle, j) { j += 1 }
      if j == needle.len { return i }
      i += 1
   }
   -1
}

fn _find_buffers_column(str line, int byte_at) int {
   "Maps a UTF-8 byte offset from the literal search to the editor's codepoint column."
   mut int col = 0
   mut int i = 0
   def end = min(max(byte_at, 0), line.len)
   while i < end {
      def int b = load8(line, i) & 255
      if b < 128 || b >= 192 { col += 1 }
      i += 1
   }
   col
}

fn _find_buffers_refresh() int {
   def ps = _editor_get("palette_state")
   def q = to_str(str.lower(pal.query(ps)))
   mut rows = []
   def limit = 1000
   if q.len > 0 {
      def bs = _editor_get("st").get("buffers", [])
      mut bi = 0
      while bi < bs.len && rows.len < limit {
         def b = bs.get(bi, {})
         def name = to_str(b.get("name", "buffer"))
         def path = _buffer_source_path(b)
         def lines = b.get("lines", [])
         mut li = 0
         while li < lines.len && rows.len < limit {
            def line = to_str(lines.get(li, ""))
            if line.len >= q.len {
               mut at = _find_buffers_next(line, q, 0)
               while at >= 0 && rows.len < limit {
                  def end = at + q.len
                  def loc = to_str(li + 1) + ":" + to_str(_find_buffers_column(line, at) + 1)
                  mut first = max(0, at - 12)
                  while first < at && (load8(line, first) & 192) == 128 { first += 1 }
                  mut stop = min(line.len, first + 76)
                  while stop < line.len && (load8(line, stop) & 192) == 128 { stop -= 1 }
                  def preview = str.strip(str.str_slice(line, _find_buffers_column(line, first), _find_buffers_column(line, stop))) + (stop < line.len ? "…" : "")
                  rows = rows.append([name + ":" + loc + "  " + preview, "", "", path.len > 0 ? path : name, "search", bi, li, at, end])
                  at = _find_buffers_next(line, q, end)
               }
            }
            li += 1
         }
         bi += 1
      }
   }
   _editor_values["palette_state"] = pal.replace_results(ps, rows)
   0
}

fn _find_buffers_open() int {
   _editor_values["palette_state"] = pal.open_items(_editor_get("palette_state"), [], "find-buffers")
   _editor_values["context_state"] = prompt.context_close(_editor_get("context_state"))
   _editor_values["find_state"] = find.dismiss(_editor_get("find_state"))
   _completion_close()
   _request_full_redraw(3)
   _status("find in open buffers")
   0
}

fn _find_buffers_choose(list row) int {
   def st = _editor_get("st")
   def bi = int(row.get(5, -1))
   def li = int(row.get(6, -1))
   def first, last = int(row.get(7, 0)), int(row.get(8, 0))
   def buffers = st.get("buffers", [])
   if bi < 0 || bi >= buffers.len { return _find_buffers_refresh() }
   def lines = buffers.get(bi, {}).get("lines", [])
   if li < 0 || li >= lines.len { return _find_buffers_refresh() }
   def line = to_str(lines.get(li, ""))
   def ps = _editor_get("palette_state")
   def first_col = _find_buffers_column(line, first)
   def last_col = _find_buffers_column(line, last)
   if last > line.len || to_str(str.lower(str.str_slice(line, first_col, last_col))) != to_str(str.lower(pal.query(ps))) { return _find_buffers_refresh() }
   _editor_values["palette_state"] = pal.dismiss(ps)
   if bi == int(st.get("active", 0)) { _nav_push_current() }
   _select_top_tab(bi)
   _clear_selections()
   mut dict active = _editor_get("st")
   active["cursor_line"] = li
   active["cursor_col"] = last_col
   active["sel_a_line"] = li
   active["sel_a_col"] = first_col
   active["sel_b_line"] = li
   active["sel_b_col"] = last_col
   active["sel_active"] = true
   _editor_values["st"] = active
   _follow_cursor()
   _nav_record_current()
   _status(to_str(row.get(3, "")) + ":" + to_str(li + 1) + ":" + to_str(first + 1))
   0
}

fn _palette_choose() int {
   def ps = _editor_get("palette_state")
   def mode = pal.items_mode(ps)
   def row = pal.selected(ps)
   if row.len <= 0 { _status(mode == "find-buffers" ? "no matches in open buffers" : "no command") return 0 }
   if mode == "find-buffers" { return _find_buffers_choose(row) }
   def id = to_str(row.get(1, ""))
   if mode == "buffers" && str.startswith(id, "buffer:") {
      _editor_values["palette_state"] = pal.dismiss(_editor_get("palette_state"))
      _select_top_tab(int(str.atof(str.str_slice(id, 7, id.len))))
      _status("buffer " + to_str(ed.current_buffer(_editor_get("st")).get("name", "")))
      return 0
   }
   _run_command(id)
}

fn _handle_palette_key(any data) int {
   def ps = _editor_get("palette_state")
   def searching = pal.items_mode(ps) == "find-buffers"
   def previous = searching ? pal.query(ps) : ""
   def next = pal.handle_key(ps, data)
   _editor_values["palette_state"] = next
   if searching && pal.query(next) != previous { _find_buffers_refresh() }
   if pal.wants_choose(next) { _palette_choose() }
   0
}

fn _handle_palette_char(any data) int {
   def ps = _editor_get("palette_state")
   def searching = pal.items_mode(ps) == "find-buffers"
   def previous = searching ? pal.query(ps) : ""
   def next = pal.handle_char(ps, data)
   _editor_values["palette_state"] = next
   if searching && pal.query(next) != previous { _find_buffers_refresh() }
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
   def res = find.handle_key(_editor_get("find_state"), data)
   _editor_values["find_state"] = res.get("st", _editor_get("find_state"))
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
   def before = find.query(_editor_get("find_state"))
   def repl_before = find.replacement(_editor_get("find_state"))
   _editor_values["find_state"] = find.handle_char(_editor_get("find_state"), data)
   if find.query(_editor_get("find_state")) != before {
      _find_refresh_now()
      _status(_find_message())
   } elif find.replacement(_editor_get("find_state")) != repl_before {
      _status("replace")
   }
   0
}

fn _handle_rename_key(any data) int {
   def res = prompt.rename_handle_key(_editor_get("rename_state"), data)
   _editor_values["rename_state"] = res.get("st", _editor_get("rename_state"))
   if res.get("submit", false) { _finish_rename() }
   0
}

fn _handle_rename_char(any data) int {
   _editor_values["rename_state"] = prompt.rename_handle_char(_editor_get("rename_state"), data)
   0
}

fn _selection_anchor_if_needed() int {
   if _extra_cursor_count() > 0 {
      _multi_selection_anchor_if_needed()
      return 0
   }
   if !ed.selection_valid(_editor_get("st")) && !bool(_editor_get("st").get("sel_active", false)) {
      _editor_values["st"] = ed.set_selection_anchor(_editor_get("st"))
      _editor_get("st")["multi_selects"] = []
      _editor_get("st")["drag_select"] = false
   }
   0
}

fn _move_cursor_ext(str dir, bool jump, bool extend=false) int {
   _follow_cursor()
   if extend { _selection_anchor_if_needed() }
   def old_row = int(_editor_get("st").get("cursor_line", 0))
   def old_col = int(_editor_get("st").get("cursor_col", 0))
   def lines = ed.current_lines(_editor_get("st"))
   mut row = int(_editor_get("st").get("cursor_line", 0))
   mut col = int(_editor_get("st").get("cursor_col", 0))
   if dir == "left" {
      mut steps = jump ? 4 : 1
      while steps > 0 {
         if col > 0 { col = ed.char_prev(to_str(lines.get(row, "")), col) }
         elif row > 0 {
            row -= 1
            col = str.utf8_len(to_str(lines.get(row, "")))
         }
         steps -= 1
      }
      _editor_get("st")["cursor_line"] = row
      _editor_get("st")["cursor_col"] = col
   } elif dir == "right" {
      mut steps2 = jump ? 4 : 1
      while steps2 > 0 {
         def line = to_str(lines.get(row, ""))
         if col < str.utf8_len(line) { col = ed.char_next(line, col) }
         elif row + 1 < lines.len {
            row += 1
            col = 0
         }
         steps2 -= 1
      }
      _editor_get("st")["cursor_line"] = row
      _editor_get("st")["cursor_col"] = col
   } elif dir == "up" {
      _editor_get("st")["cursor_line"] = row - (jump ? 8 : 1)
   } elif dir == "down" {
      _editor_get("st")["cursor_line"] = row + (jump ? 8 : 1)
   }
   _editor_values["st"] = ed.clamp_cursor(_editor_get("st"))
   _move_extra_cursors_by(int(_editor_get("st").get("cursor_line", 0)) - old_row, int(_editor_get("st").get("cursor_col", 0)) - old_col)
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
      if _editor_get("st").get("multi_selects", []).len > 0 { _multi_selection_update() }
      else { _editor_values["st"] = ed.update_selection(_editor_get("st")) }
      _editor_get("st")["drag_select"] = false
   } elif bool(_editor_get("st").get("mark_active", false)) {
      ;; Emacs transient mark: motion extends the region from the mark.
      _sync_mark_region()
   } else {
      _clear_selections()
   }
   0
}

fn _page_cursor(int dir, bool extend=false) int {
   _follow_cursor()
   if extend { _selection_anchor_if_needed() }
   def old_row = int(_editor_get("st").get("cursor_line", 0))
   def old_col = int(_editor_get("st").get("cursor_col", 0))
   _editor_get("st")["cursor_line"] = int(_editor_get("st").get("cursor_line", 0)) + dir * 12
   _editor_values["st"] = ed.clamp_cursor(_editor_get("st"))
   _move_extra_cursors_by(int(_editor_get("st").get("cursor_line", 0)) - old_row, int(_editor_get("st").get("cursor_col", 0)) - old_col)
   _finish_selection_move(extend)
}

fn _line_edge(str edge, bool extend=false) int {
   _follow_cursor()
   if extend { _selection_anchor_if_needed() }
   def old_row = int(_editor_get("st").get("cursor_line", 0))
   def old_col = int(_editor_get("st").get("cursor_col", 0))
   if edge == "start" {
      _editor_get("st")["cursor_col"] = 0
   } else {
      _editor_get("st")["cursor_col"] = to_str(ed.current_lines(_editor_get("st")).get(int(_editor_get("st").get("cursor_line", 0)), "")).len
   }
   _editor_values["st"] = ed.clamp_cursor(_editor_get("st"))
   _move_extra_cursors_by(int(_editor_get("st").get("cursor_line", 0)) - old_row, int(_editor_get("st").get("cursor_col", 0)) - old_col)
   _finish_selection_move(extend)
}

fn _buffer_edge(str edge, bool extend=false) int {
   _follow_cursor()
   if extend { _selection_anchor_if_needed() }
   def old_row = int(_editor_get("st").get("cursor_line", 0))
   def old_col = int(_editor_get("st").get("cursor_col", 0))
   def lines = ed.current_lines(_editor_get("st"))
   if edge == "start" {
      _editor_get("st")["cursor_line"] = 0
      _editor_get("st")["cursor_col"] = 0
   } else {
      def last = max(0, lines.len - 1)
      _editor_get("st")["cursor_line"] = last
      _editor_get("st")["cursor_col"] = to_str(lines.get(last, "")).len
   }
   _editor_values["st"] = ed.clamp_cursor(_editor_get("st"))
   _move_extra_cursors_by(int(_editor_get("st").get("cursor_line", 0)) - old_row, int(_editor_get("st").get("cursor_col", 0)) - old_col)
   _finish_selection_move(extend)
}

fn _handle_command(str ch, any context) bool {
   if ch.len <= 0 {
      _status(chord.describe(_editor_get("chord_state")) + "-")
      return true
   }
   if ch == "cancel" { _editor_values["chord_state"] = chord.clear(_editor_get("chord_state")) _run_command("cancel") return true }
   def id = cmd.id_for_chord(ch, context)
   if id.len <= 0 { return false }
   _run_command(id)
   true
}

fn _open_chord_prefix(str prefix) int {
   _editor_get("chord_state")["prefix"] = prefix
   _editor_get("chord_state")["timer"] = chord.PREFIX_TIMEOUT
   _editor_values["which_key_prefix"] = ""
   _editor_values["which_key_rows_cache"] = borrow([])
   _editor_values["which_key_scroll"] = 0
   _editor_values["which_key_scroll_accum"] = 0.0
   _editor_values["ui_state"] = interact.key_seen(_editor_get("ui_state"), prefix)
   _request_full_redraw(2)
   _status(prefix + "-")
   0
}

fn _is_ctrl_space_event(any data) bool {
   if !_ctrl(data) { return false }
   if window.event_key_is(data, key.KEY_SPACE) { return true }
   def k = int(data.get("key", key.KEY_NULL))
   if k == key.KEY_SPACE { return true }
   def sc = int(data.get("scancode", data.get("scan", -1)))
   if sc == 65 { return true }
   def cp = int(data.get("char", -1))
   cp == 0 || cp == 32
}

fn _handle_ctrl_space(any data) bool {
   if !_is_ctrl_space_event(data) { return false }
   ;; Emacs profile: C-SPC sets the mark; the leader chords live on C-c.
   if cmd.keymap() == "emacs" {
      _run_command("set-mark")
      _suppress_char_from_key(data)
      return true
   }
   if chord.pending(_editor_get("chord_state")) && chord.describe(_editor_get("chord_state")) == "C-SPC" {
      _editor_values["chord_state"] = chord.clear(_editor_get("chord_state"))
      _run_command("palette")
   } else {
      _open_chord_prefix("C-SPC")
   }
   _suppress_char_from_key(data)
   true
}

fn _direct_key_fallback(any data) bool {
   if _ctrl(data) && _shift(data) && window.event_key_is(data, key.KEY_P) { _run_command("palette") return true }
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
   if int(_editor_get("suppress_char_cp")) > 0 { _editor_values["suppress_char_cp"] = 0 }
   if INPUT_TRACE {
      print("[editor:key] enter rename=" + to_str(prompt.rename_is_open(_editor_get("rename_state"))) + " palette=" + to_str(pal.is_open(_editor_get("palette_state"))))
   }
   if _handle_escape_key(data) { return 0 }
   if prompt.rename_is_open(_editor_get("rename_state")) { _handle_rename_key(data) return 0 }
   if pal.is_open(_editor_get("palette_state")) { _handle_palette_key(data) return 0 }
   if find.is_open(_editor_get("find_state")) { _handle_find_key(data) return 0 }
   if _ctrl(data) && _shift(data) && window.event_key_is(data, key.KEY_P) {
      _run_command("palette")
      _suppress_char_from_key(data)
      return 0
   }
   if _handle_ctrl_space(data) { return 0 }
   if _completion_handle_key(data) { return 0 }
   def key_chord = chord.event_chord(data)
   if INPUT_TRACE {
      print("[editor:key] chord=" + key_chord + " key=" + to_str(int(data.get("key", -1))) + " mods=" + to_str(int(data.get("mods", data.get("mod", 0)))))
   }
   def had_prefix = chord.pending(_editor_get("chord_state"))
   def context = _keymap_context()
   if SHOW_LAST_KEY || had_prefix { _editor_values["ui_state"] = interact.key_seen(_editor_get("ui_state"), key_chord) }
   ;; In the default keymap C-c copies at once; in Emacs it is a prefix.
   def res = chord.command_chord(_editor_get("chord_state"), data, context)
   _editor_values["chord_state"] = res.get(0)
   def resolved = to_str(res.get(1, ""))
   if resolved == "" && key_chord == "C-c" && !had_prefix && cmd.keymap() != "emacs" && cmd.id_for_chord(key_chord, context) == "copy" { _copy_selection_or_buffer() }
   ;; C-h k: the next complete chord is described instead of run.
   if resolved.len > 0 && resolved != "cancel" && bool(_editor_values.get("describe_key", false)) {
      _editor_values["describe_key"] = false
      _status(cmd.describe_chord(resolved, context))
      _suppress_char_from_key(data)
      return 0
   }
   if INPUT_TRACE { print("[editor:key] resolved=" + resolved + " prefix=" + chord.describe(_editor_get("chord_state"))) }
   if _handle_command(resolved, context) {
      _suppress_char_from_key(data)
      return 0
   }
   if had_prefix {
      _suppress_char_from_key(data)
      return 0
   }
   if INPUT_TRACE { print("[editor:key] fallback") }
   def fallback_handled = _direct_key_fallback(data)
   if fallback_handled && _editor_get("st").get("quit_prompt", false) { _clear_pending_destructive_prompts() }
   0
}

fn _handle_char(any data) int {
   if _consume_suppressed_char(data) { return 0 }
   if prompt.rename_is_open(_editor_get("rename_state")) { _handle_rename_char(data) return 0 }
   if pal.is_open(_editor_get("palette_state")) { _handle_palette_char(data) return 0 }
   if find.is_open(_editor_get("find_state")) { _handle_find_char(data) return 0 }
   if _ctrl(data) { return 0 }
   def cp = int(data.get("char", 0))
   if cp >= 32 && cp != 127 {
      _editor_get("st")["quit_prompt"] = false
      _editor_values["last_escape_ns"] = 0
      ;; Typing deactivates an Emacs region rather than replacing it.
      _deactivate_mark()
   }
   if cp == 40 { _insert_pair("(", ")") _lsp_auto("signature") return 0 }
   if cp == 91 { _insert_pair("[", "]") return 0 }
   if cp == 123 { _insert_pair("{", "}") return 0 }
   if cp == 34 { _insert_pair("\"", "\"") return 0 }
   if cp == 39 { _insert_pair("'", "'") return 0 }
   if cp >= 32 && cp != 127 {
      _edit_insert(chr(cp))
      if bool(_editor_get("lsp_state").get("popups", true)) && _word_byte(cp) && _word_prefix_before_cursor().len >= 2 {
         _completion_open(false)
         _lsp_auto("completion")
      } elif cp == 46 {
         _completion_close()
         _lsp_auto("completion")
      } else { _completion_close() }
      if cp == 44 { _lsp_auto("signature") }
      elif cp == 41 { _lsp_popup_close() }
   }
   0
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
   if prompt.rename_is_open(_editor_get("rename_state")) { _editor_values["rename_state"] = prompt.rename_close(_editor_get("rename_state")) cleared = true }
   if pal.is_open(_editor_get("palette_state")) { _editor_values["palette_state"] = pal.dismiss(_editor_get("palette_state")) cleared = true }
   if find.is_open(_editor_get("find_state")) { _editor_values["find_state"] = find.dismiss(_editor_get("find_state")) cleared = true }
   if colorpicker.is_open(_editor_get("color_state")) { _editor_values["color_state"] = colorpicker.dismiss(_editor_get("color_state")) cleared = true }
   if prompt.context_is_open(_editor_get("context_state")) { _editor_values["context_state"] = prompt.context_close(_editor_get("context_state")) cleared = true }
   if bool(_editor_get("completion_state").get("open", false)) { _completion_close() cleared = true }
   if bool(_editor_get("peek_state").get("open", false)) { _editor_get("peek_state")["open"] = false cleared = true }
   if chord.pending(_editor_get("chord_state")) { _editor_values["chord_state"] = chord.empty_state() cleared = true }
   if _extra_cursor_count() > 0 { _clear_extra_cursors(false) cleared = true }
   if _selection_any_valid() || bool(_editor_get("st").get("sel_active", false)) { _clear_selections() cleared = true }
   if _editor_get("project_selection").len > 0 { _project_selection_replace([]) _editor_values["project_selection_anchor"] = "" cleared = true }
   _editor_get("st")["drag_select"] = false
   _editor_get("st")["drag_divider"] = false
   _editor_get("st")["drag_terminal"] = false
   if cleared {
      _clear_pending_destructive_prompts()
      _request_full_redraw(2)
      _status("cleared")
      return true
   }
   def now = ticks()
   if _editor_get("st").get("quit_prompt", false) || (int(_editor_get("last_escape_ns")) > 0 && now - int(_editor_get("last_escape_ns")) <= ESC_QUIT_WINDOW_NS) {
      window.set_should_close(_editor_get("win"), true)
      return true
   }
   _editor_values["last_escape_ns"] = now
   _editor_get("st")["quit_prompt"] = true
   _request_full_redraw(2)
   _status("Esc again quits")
   true
}

fn _handle_mouse_press(dict lay, any data) int {
   def p = window.mouse_pos(_editor_get("win"))
   def mx = float(data.get("x", p.get(0, 0.0)))
   def my = float(data.get("y", p.get(1, 0.0)))
   def button = int(data.get("button", 0))
   ;; The palette is modal: a click inside it belongs to its toolkit rows,
   ;; a click outside dismisses it. A click inside the context menu or the
   ;; find bar belongs to the toolkit too.
   if pal.is_open(_editor_get("palette_state")) {
      if !_palette_contains(lay, mx, my) { _editor_values["palette_state"] = pal.dismiss(_editor_get("palette_state")) }
      return 0
   }
   if _context_contains(lay, mx, my) { return 0 }
   if _gui_overlay_live && gui.window_rect("ed_find").len >= 4 && find.is_open(_editor_get("find_state")) {
      def fr = gui.window_rect("ed_find")
      if widgets.hit(mx, my, float(fr.get(0, 0.0)), float(fr.get(1, 0.0)), float(fr.get(2, 0.0)), float(fr.get(3, 0.0))) { return 0 }
   }
   if _gui_overlay_live && colorpicker.is_open(_editor_get("color_state")) {
      def cr = gui.window_rect("ed_color")
      if cr.len >= 4 && widgets.hit(mx, my, float(cr.get(0, 0.0)), float(cr.get(1, 0.0)), float(cr.get(2, 0.0)), float(cr.get(3, 0.0))) { return 0 }
   }
   def tab_hit_top = _top_tab_hit(lay, mx, my)
   def top_action = to_str(tab_hit_top.get("action", ""))
   if top_action.len > 0 {
      def idx_top = int(tab_hit_top.get("idx", -1))
      if top_action == "close" { _close_top_tab(idx_top) }
      else { _select_top_tab(idx_top) }
      return 0
   }
   def top_h = float(lay.get("top", 0.0))
   if button == 0 && top_h > 1.0 && my >= 0.0 && my < top_h {
      if window.start_window_move(_editor_get("win")) { return 0 }
   }
   def term_hit = _term_tab_hit(lay, mx, my)
   def term_action = to_str(term_hit.get("action", ""))
   if term_action.len > 0 {
      if term_action == "new" { _editor_values["term_state"] = termpane.open_shell(_editor_get("term_state")) }
      elif term_action == "close" { _editor_values["term_state"] = termpane.close_tab(_editor_get("term_state"), int(term_hit.get("idx", -1))) }
      elif term_action == "select" { _editor_values["term_state"] = termpane.select_tab(_editor_get("term_state"), int(term_hit.get("idx", 0))) }
      _request_full_redraw(3)
      _set_focus(termpane.is_open(_editor_get("term_state")) ? "terminal" : "editor", false)
      return 0
   }
   def pane = ed.pane_at(lay, mx, my)
   if pane == "editor" || pane == "project" || pane == "terminal" { _set_focus(pane, false) }
   _editor_get("st")["quit_prompt"] = false
   _editor_values["last_escape_ns"] = 0
   def rail_x = float(lay.get("rail_x", 0.0))
   def rail_y = float(lay.get("rail_y", 0.0))
   def rail_w = float(lay.get("rail_w", 0.0))
   def rail_h = float(lay.get("rail_h", 0.0))
   def tab_hit = (bool(_editor_get("st").get("show_hierarchy", true)) && rail_w > 1.0) ? _rail_tab_at(rail_x, rail_y, rail_w, mx, my) : ""
   if tab_hit.len > 0 {
      def old_tab = _rail_tab()
      _editor_get("st")["rail_tab"] = tab_hit
      if tab_hit != old_tab {
         _request_full_redraw(3)
         if tab_hit == "git" { _ensure_project_git() }
      }
      _set_focus("project", false)
      return 0
   }
   _editor_values["context_state"] = prompt.context_close(_editor_get("context_state"))
   if (button == 0 || button == 1) && _dock_resize_hit(lay, mx, my) {
      _editor_get("st")["drag_terminal"] = true
      return 0
   }
   if button == 0 {
      def scroll_target = _scrollbar_target_at(lay, mx, my)
      if scroll_target.len > 0 {
         _editor_get("st")["drag_scrollbar"] = true
         _editor_get("st")["scrollbar_target"] = scroll_target
         _apply_scrollbar_target(scroll_target, my)
         return 0
      }
   }
   def section_hit = _rail_section_at(lay, mx, my)
   if section_hit.len > 0 && button == 0 {
      _editor_get("section_drag")["active"] = true
      _editor_get("section_drag")["section"] = section_hit
      _editor_get("section_drag")["x"] = mx
      _editor_get("section_drag")["y"] = my
      _set_focus("project", false)
      return 0
   }
   if ed.divider_hit(lay, mx, my) {
      _editor_get("st")["drag_divider"] = true
      return 0
   }
   if pane == "editor" {
      if button == 0 && _fold_rect_click(mx, my) { return 0 }
      if button == 0 && _code_lens_click(mx, my) { return 0 }
      if button == 0 && _alt(data) && _shift(data) {
         def lines_alt = ed.current_lines(_editor_get("st"))
         if lines_alt.len > 0 {
            def row_alt = min(max(ed.row_at(lay, my, int(_editor_get("st").get("scroll", 0))), 0), lines_alt.len - 1)
            def line_alt = to_str(lines_alt.get(row_alt, ""))
            _add_extra_cursor_at(row_alt, _visual_col_at(line_alt, row_alt, mx - _line_text_x(lay)))
         }
         _editor_get("st")["drag_select"] = false
         return 0
      }
      _click_editor(lay, mx, my)
      if button == 0 && bool(_editor_get("st").get("show_line_numbers", true)) && mx < _line_text_x(lay) - 8.0 {
         _debug_breakpoint()
         _editor_get("st")["drag_select"] = false
         return 0
      }
      if button == 1 {
         _open_context(mx, my, _editor_context_entry(), "editor")
         _editor_get("st")["drag_select"] = false
         return 0
      }
      _editor_get("st")["drag_select"] = button == 0
      if _shift(data) {
         _editor_values["st"] = ed.update_selection(_editor_get("st"))
      } else {
         _editor_values["st"] = ed.set_selection_anchor(_editor_get("st"))
      }
      return 0
   }
   if pane != "project" || rail_w <= 1.0 || !bool(_editor_get("st").get("show_hierarchy", true)) { return 0 }
   def tab = _rail_tab()
   def body_y = rail_y + RAIL_TABS_H
   def body_h = rail_h - RAIL_TABS_H
   if pane == "project" && tab == "git" {
      def rows_git = project.changes(_editor_get("project_state"))
      def visible_git = project.visible_count(body_h)
      def idx_git = project.row_at(rail_x, body_y, rail_w, body_h, mx, my, min(visible_git, rows_git.len - int(_editor_get("st").get("git_scroll", 0))))
      if idx_git >= 0 {
         def entry = rows_git.get(int(_editor_get("st").get("git_scroll", 0)) + idx_git, {})
         if button == 1 { _open_context(mx, my, entry, "git") }
         else { _open_git_diff(entry) }
         return 0
      }
   }
   if pane == "project" && tab == "timeline" {
      def rows_time = _timeline_rows()
      def visible_time = project.visible_count(body_h)
      def idx_time = project.row_at(rail_x, body_y, rail_w, body_h, mx, my, min(visible_time, rows_time.len - int(_editor_get("st").get("timeline_scroll", 0))))
      if idx_time >= 0 {
         def row = rows_time.get(int(_editor_get("st").get("timeline_scroll", 0)) + idx_time, [])
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
   def show_project = bool(_editor_get("st").get("show_project", true))
   def show_outline = bool(_editor_get("st").get("show_outline", true))
   if tab == "files" && show_project { _ensure_project_tree() }
   def rows = project.tree(_editor_get("project_state"))
   def syms = _outline_symbols()
   def tree_h = (!show_project) ? 0.0 : (show_outline ? _rail_tree_height(body_h, rows.len, syms.len) : body_h)
   if tab == "files" && show_project {
      def entry = project.hit_entry(_editor_get("project_state"), rail_x, body_y, rail_w, tree_h, int(_editor_get("st").get("tree_scroll", 0)), mx, my)
      if is_dict(entry) && entry.len > 0 {
         if button == 1 {
            if !_project_selection_contains(entry) { _project_selection_set(entry) }
            entry["selection_count"] = _editor_get("project_selection").len
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
            _editor_get("project_drag")["active"] = false
            _editor_get("project_drag")["armed"] = true
            _editor_get("project_drag")["entry"] = borrow(clone(entry))
            _editor_get("project_drag")["x"] = mx
            _editor_get("project_drag")["y"] = my
            _open_or_toggle_project_row(entry)
         }
         return 0
      }
   }
   def outline_off = show_project ? tree_h + 1.0 : 0.0
   def outline_y = body_y + outline_off
   def outline_h = body_h - outline_off
   def outline_visible = _outline_visible(outline_h)
   def outline_scroll = _outline_clamp(int(_editor_get("st").get("outline_scroll", 0)), syms.len, outline_visible)
   _editor_get("st")["outline_scroll"] = outline_scroll
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
      _editor_get("st")["cursor_line"] = int(sym.get("line", 0))
      _editor_get("st")["cursor_col"] = 0
      _editor_values["st"] = ed.clamp_cursor(_editor_get("st"))
      _follow_cursor()
      _status("symbol " + to_str(sym.get("name", "")))
      return 0
   }
   0
}

fn _handle_mouse_move(dict lay, any data) int {
   def p = window.mouse_pos(_editor_get("win"))
   def mx = float(data.get("x", p.get(0, 0.0)))
   def my = float(data.get("y", p.get(1, 0.0)))
   if _editor_get("project_drag").get("armed", false) {
      def dx = mx - float(_editor_get("project_drag").get("x", mx))
      def dy = my - float(_editor_get("project_drag").get("y", my))
      if (dx * dx + dy * dy) > 36.0 {
         _editor_get("project_drag")["active"] = true
         if INPUT_TRACE {
            def drag_entry = _editor_get("project_drag").get("entry", dict(0))
            print("[editor:drag] project " + (is_dict(drag_entry) ? to_str(drag_entry.get("rel", "")) : ""))
         }
      }
   }
   if _editor_get("st").get("drag_select", false) {
      _place_cursor(lay, mx, my)
      _editor_values["st"] = ed.update_selection(_editor_get("st"))
      return 0
   }
   if _editor_get("st").get("drag_terminal", false) {
      _editor_get("st")["term_h"] = _dock_height_from_mouse(lay, my)
      return 0
   }
   if _editor_get("st").get("drag_scrollbar", false) {
      _apply_scrollbar_target(_editor_get("st").get("scrollbar_target", dict(0)), my)
      return 0
   }
   if !_editor_get("st").get("drag_divider", false) { return 0 }
   _editor_get("st")["rail_w"] = _rail_width_from_mouse(lay, mx)
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
   mut bs = _editor_get("st").get("buffers", [])
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
   _editor_get("st")["buffers"] = bs
   0
}

fn _drop_open_buffer_path(str path) int {
   if path.len <= 0 { return 0 }
   def want = ospath.normalize(path)
   def active = int(_editor_get("st").get("active", 0))
   def bs = _editor_get("st").get("buffers", [])
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
   _editor_get("st")["buffers"] = out
   _editor_get("st")["active"] = min(max(0, active - removed_before), out.len - 1)
   _editor_get("st")["cursor_line"] = 0
   _editor_get("st")["cursor_col"] = 0
   _editor_get("st")["scroll"] = 0
   _clear_selections()
   _buffer_changed(true)
   _nav_record_current()
   0
}

fn _project_entry_or_current(dict entry) dict {
   if is_dict(entry) && entry.len > 0 && to_str(entry.get("rel", "")).len > 0 { return entry }
   def cur = _current_path()
   if cur.len > 0 {
      def hit = session.project_entry_for_path(_editor_get("project_state"), cur)
      if is_dict(hit) && hit.len > 0 { return hit }
      mut dict current_entry = dict(4)
      current_entry["name"] = ospath.basename(cur)
      current_entry["path"] = cur
      current_entry["rel"] = str.str_replace(ospath.normalize(cur), ospath.normalize(project.root(_editor_get("project_state"))) + ospath.sep(), "")
      current_entry["dir"] = false
      return current_entry
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
      _editor_values["project_state"] = project.trash_entry(_editor_get("project_state"), rel)
      def err = to_str(_editor_get("project_state").get("last_error", ""))
      if err.len > 0 { _status(err) return 0 }
      if path.len > 0 { _drop_open_buffer_path(path) }
      deleted += 1
      i += 1
   }
   _project_selection_replace([])
   _editor_values["project_selection_anchor"] = ""
   _editor_get("st")["project_loaded"] = true
   _editor_get("st")["git_loaded"] = true
   _invalidate_project_definitions()
   _save_ui_cache()
   _status(deleted == 1 ? ("moved to trash " + first_name) : ("moved " + to_str(deleted) + " items to trash"))
   0
}

fn _undo_file_op() int {
   _editor_values["project_state"] = project.undo_file_op(_editor_get("project_state"))
   def err = to_str(_editor_get("project_state").get("last_error", ""))
   if err.len > 0 { _status(err) return 0 }
   _editor_get("st")["project_loaded"] = true
   _editor_get("st")["git_loaded"] = true
   _invalidate_project_definitions()
   _save_ui_cache()
   _status("file op undone")
   0
}

fn _redo_file_op() int {
   _editor_values["project_state"] = project.redo_file_op(_editor_get("project_state"))
   def err = to_str(_editor_get("project_state").get("last_error", ""))
   if err.len > 0 { _status(err) return 0 }
   _editor_get("st")["project_loaded"] = true
   _editor_get("st")["git_loaded"] = true
   _invalidate_project_definitions()
   _save_ui_cache()
   _status("file op redone")
   0
}

fn _finish_project_drag(dict lay, f64 mx, f64 my) int {
   if !_editor_get("project_drag").get("armed", false) { return 0 }
   def src = _editor_get("project_drag").get("entry", dict(8))
   def active = bool(_editor_get("project_drag").get("active", false))
   _editor_get("project_drag")["active"] = false
   _editor_get("project_drag")["armed"] = false
   _editor_get("project_drag")["entry"] = borrow(dict(8))
   _editor_get("project_drag")["x"] = 0.0
   _editor_get("project_drag")["y"] = 0.0
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
      _editor_values["project_state"] = project.move_entry(_editor_get("project_state"), to_str(e.get("rel", "")), dst_rel)
      def err = to_str(_editor_get("project_state").get("last_error", ""))
      if err.len > 0 { _status(err) return 0 }
      _retarget_open_buffers(to_str(e.get("path", "")), new_path)
      moved += 1
      i += 1
   }
   _project_selection_replace([])
   _editor_values["project_selection_anchor"] = ""
   _editor_get("st")["project_loaded"] = true
   _editor_get("st")["git_loaded"] = true
   _invalidate_project_definitions()
   _buffer_changed(false)
   _status(moved == 1 ? ("moved " + first_name) : ("moved " + to_str(moved) + " items"))
   0
}

fn _finish_section_drag(f64 mx, f64 my) int {
   if !_editor_get("section_drag").get("active", false) { return 0 }
   def section = to_str(_editor_get("section_drag").get("section", ""))
   def dx = mx - float(_editor_get("section_drag").get("x", mx))
   def dy = my - float(_editor_get("section_drag").get("y", my))
   _editor_get("section_drag")["active"] = false
   _editor_get("section_drag")["section"] = ""
   _editor_get("section_drag")["x"] = 0.0
   _editor_get("section_drag")["y"] = 0.0
   if section.len <= 0 { return 0 }
   if (dx * dx + dy * dy) > 49.0 { _hide_rail_section(section) }
   else { _toggle_rail_section(section) }
   0
}

fn _handle_mouse_release(dict lay, any data) int {
   def p = window.mouse_pos(_editor_get("win"))
   def mx = is_dict(data) ? float(data.get("x", p.get(0, 0.0))) : float(p.get(0, 0.0))
   def my = is_dict(data) ? float(data.get("y", p.get(1, 0.0))) : float(p.get(1, 0.0))
   _finish_section_drag(mx, my)
   _finish_project_drag(lay, mx, my)
   _editor_get("st")["drag_divider"] = false
   _editor_get("st")["drag_terminal"] = false
   _editor_get("st")["drag_select"] = false
   _editor_get("st")["drag_scrollbar"] = false
   _editor_get("st")["scrollbar_target"] = dict(0)
   0
}

fn _handle_scroll(dict lay, any data) int {
   def p = window.mouse_pos(_editor_get("win"))
   mut f64 mx = float(p.get(0, 0.0))
   mut f64 my = float(p.get(1, 0.0))
   if is_dict(data) {
      def data_x = data.get("x")
      def data_y = data.get("y")
      if !is_nil(data_x) { mx = float(data_x) }
      if !is_nil(data_y) { my = float(data_y) }
   }
   mut dy = 0.0
   if is_dict(data) { dy = float(data.get("dy", 0.0)) }
   elif is_list(data) { dy = float(data.get(1, 0.0)) }
   if dy == 0.0 { return 0 }
   if INPUT_TRACE { print("[editor:scroll] mx=" + to_str(mx) + " my=" + to_str(my) + " dy=" + to_str(dy)) }
   if pal.is_open(_editor_get("palette_state")) {
      if _palette_contains(lay, mx, my) {
         _editor_values["palette_scroll_accum"] = float(_editor_get("palette_scroll_accum")) + (-dy * 3.0)
         def step_pal = int(float(_editor_get("palette_scroll_accum")))
         if step_pal != 0 {
            _editor_values["palette_scroll_accum"] = float(_editor_get("palette_scroll_accum")) - (float(step_pal))
            _editor_values["palette_state"] = pal.scroll_by(_editor_get("palette_state"), step_pal)
         }
      }
      return 0
   }
   if _which_key_contains(_layout_sw(lay), _layout_sh(lay), mx, my) {
      def rows_key = _which_key_rows(chord.describe(_editor_get("chord_state")))
      def rect_key = _which_key_rect(_layout_sw(lay), _layout_sh(lay), rows_key.len)
      def visible_key = int(rect_key.get(5, 1))
      _editor_values["which_key_scroll_accum"] = float(_editor_get("which_key_scroll_accum")) + (-dy * 3.0)
      def step_key = int(float(_editor_get("which_key_scroll_accum")))
      if step_key != 0 {
         _editor_values["which_key_scroll_accum"] = float(_editor_get("which_key_scroll_accum")) - (float(step_key))
         _editor_values["which_key_scroll"] = int(_editor_get("which_key_scroll")) + (step_key)
         _editor_values["which_key_scroll"] = _clamp_which_key_scroll(rows_key.len, visible_key)
      }
      return 0
   }
   if widgets.hit(mx, my, float(lay.get("rail_x", 0.0)), float(lay.get("rail_y", 0.0)), float(lay.get("rail_w", 0.0)), float(lay.get("rail_h", 0.0))) {
      def rail_h = float(lay.get("rail_h", 0.0))
      def body_h = rail_h - RAIL_TABS_H
      def tab = _rail_tab()
      if tab == "git" {
         def rows_git = project.changes(_editor_get("project_state"))
         def visible_git = project.visible_count(body_h)
         _editor_values["git_scroll_accum"] = float(_editor_get("git_scroll_accum")) + (-dy * 3.0)
         def step_git = int(float(_editor_get("git_scroll_accum")))
         if step_git != 0 {
            _editor_values["git_scroll_accum"] = float(_editor_get("git_scroll_accum")) - (float(step_git))
            _editor_get("st")["git_scroll"] = project.clamp_scroll(int(_editor_get("st").get("git_scroll", 0)) + step_git, rows_git.len, visible_git)
         }
         return 0
      }
      if tab == "timeline" {
         def rows_time = _timeline_rows()
         def visible_time = project.visible_count(body_h)
         _editor_values["timeline_scroll_accum"] = float(_editor_get("timeline_scroll_accum")) + (-dy * 3.0)
         def step_time = int(float(_editor_get("timeline_scroll_accum")))
         if step_time != 0 {
            _editor_values["timeline_scroll_accum"] = float(_editor_get("timeline_scroll_accum")) - (float(step_time))
            _editor_get("st")["timeline_scroll"] = project.clamp_scroll(int(_editor_get("st").get("timeline_scroll", 0)) + step_time, rows_time.len, visible_time)
         }
         return 0
      }
      def show_project = bool(_editor_get("st").get("show_project", true))
      def show_outline = bool(_editor_get("st").get("show_outline", true))
      if show_project { _ensure_project_tree() }
      def rows = project.tree(_editor_get("project_state"))
      def syms = _outline_symbols()
      def tree_h = (!show_project) ? 0.0 : (show_outline ? _rail_tree_height(body_h, rows.len, syms.len) : body_h)
      def outline_y = float(lay.get("rail_y", 0.0)) + RAIL_TABS_H + (show_project ? tree_h + 1.0 : 0.0)
      if show_outline && my >= outline_y {
         def visible = _outline_visible(body_h - (outline_y - float(lay.get("rail_y", 0.0)) - RAIL_TABS_H))
         _editor_values["outline_scroll_accum"] = float(_editor_get("outline_scroll_accum")) + (-dy * 3.0)
         def step = int(float(_editor_get("outline_scroll_accum")))
         if step != 0 {
            _editor_values["outline_scroll_accum"] = float(_editor_get("outline_scroll_accum")) - (float(step))
            _editor_get("st")["outline_scroll"] = _outline_clamp(int(_editor_get("st").get("outline_scroll", 0)) + step, syms.len, visible)
         }
         if INPUT_TRACE { print("[editor:scroll] outline step=" + to_str(step) + " scroll=" + to_str(int(_editor_get("st").get("outline_scroll", 0)))) }
         return 0
      }
      if show_project {
         def visible = project.visible_count(tree_h)
         _editor_values["tree_scroll_accum"] = float(_editor_get("tree_scroll_accum")) + (-dy * 3.0)
         def step = int(float(_editor_get("tree_scroll_accum")))
         if step != 0 {
            _editor_values["tree_scroll_accum"] = float(_editor_get("tree_scroll_accum")) - (float(step))
            _editor_get("st")["tree_scroll"] = project.clamp_scroll(int(_editor_get("st").get("tree_scroll", 0)) + step, rows.len, visible)
         }
         if INPUT_TRACE { print("[editor:scroll] tree step=" + to_str(step) + " scroll=" + to_str(int(_editor_get("st").get("tree_scroll", 0)))) }
         return 0
      }
      return 0
   }
   if !widgets.hit(mx, my, float(lay.get("edit_x", 0.0)), float(lay.get("edit_y", 0.0)), float(lay.get("edit_w", 0.0)), float(lay.get("edit_h", 0.0))) {
      if INPUT_TRACE { print("[editor:scroll] miss") }
      return 0
   }
   def lines = ed.current_lines(_editor_get("st"))
   def visible_rows = ed.visible_rows(float(lay.get("edit_h", 0.0)))
   def max_scroll = max(0, lines.len - visible_rows)
   _editor_values["edit_scroll_accum"] = float(_editor_get("edit_scroll_accum")) + (-dy * 5.0)
   def step = int(float(_editor_get("edit_scroll_accum")))
   if step != 0 {
      _editor_values["edit_scroll_accum"] = float(_editor_get("edit_scroll_accum")) - (float(step))
      _editor_get("st")["scroll"] = min(max_scroll, max(0, int(_editor_get("st").get("scroll", 0)) + step))
      _editor_get("st")["scroll_follow"] = false
   }
   if INPUT_TRACE { print("[editor:scroll] edit step=" + to_str(step) + " max=" + to_str(max_scroll) + " scroll=" + to_str(int(_editor_get("st").get("scroll", 0)))) }
   0
}

fn _outline_symbols() list {
   def name = to_str(ed.current_buffer(_editor_get("st")).get("name", ""))
   def cache_name = to_str(int(_editor_get("st").get("active", 0))) + ":" + name
   if int(_editor_get("outline_cache_rev")) == int(_editor_get("content_rev")) && to_str(_editor_get("outline_cache_name")) == cache_name { return _editor_get("outline_cache") }
   _editor_values["outline_cache"] = outline.symbols(ed.current_lines(_editor_get("st")), name, OUTLINE_MAX_LINES)
   _editor_values["outline_cache_rev"] = int(_editor_get("content_rev"))
   _editor_values["outline_cache_name"] = cache_name
   _editor_get("outline_cache")
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
         _editor_values["project_state"] = project.refresh_git(_editor_get("project_state"))
         _editor_get("st")["project_loaded"] = true
         _editor_get("st")["git_loaded"] = true
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
   def p = window.mouse_pos(_editor_get("win"))
   def mx = is_dict(data) ? float(data.get("x", p.get(0, 0.0))) : float(p.get(0, 0.0))
   def my = is_dict(data) ? float(data.get("y", p.get(1, 0.0))) : float(p.get(1, 0.0))
   def target = _project_entry_at(lay, mx, my)
   if is_dict(target) && target.len > 0 {
      mut opened = false
      mut moved = 0
      mut failed = 0
      mut i = 0
      while i < files.len {
         def dst = _drop_move_into(target, to_str(files.get(i, "")))
         if dst.len > 0 {
            moved += 1
            if !opened && !osfs.is_dir(dst) { _open_file(dst) opened = true }
         } else {
            failed += 1
         }
         i += 1
      }
      ;; Preserve per-file failure messages instead of overwriting them with a
      ;; blanket success count.
      if moved > 0 && failed > 0 {
         _status("moved " + to_str(moved) + ", " + to_str(failed) + " failed")
      } elif moved > 0 {
         _status("moved " + to_str(moved) + " dropped item" + (moved == 1 ? "" : "s"))
      } elif failed > 0 {
         _status("drop failed: " + to_str(failed) + " item" + (failed == 1 ? "" : "s"))
      }
      return 0
   }
   _editor_values["st"] = session.append_file(_editor_get("st"), to_str(files.get(0, "")))
   _buffer_changed(true)
   0
}

fn _process_events(dict lay) int {
   _editor_get("term_state")["focus"] = to_str(_editor_get("st").get("focus", "editor")) == "terminal" && termpane.is_open(_editor_get("term_state"))
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
      if is_mouse_press || is_mouse_release || is_mouse_move || is_scroll { _gui_feed_mouse(typ, data) }
      ;; Focus loss / mouse leave: drop any armed chord prefix and the quit
      ;; prompt so the user does not return to a half-armed editor that
      ;; silently runs C-x k on the next keystroke.
      if typ == window.EVENT_FOCUS_OUT || typ == window.EVENT_MOUSE_LEAVE {
         if chord.pending(_editor_get("chord_state")) { _editor_values["chord_state"] = chord.empty_state() }
         _clear_pending_destructive_prompts()
      }
      mut scroll_ev = 0
      if is_scroll {
         if is_dict(data) {
            scroll_ev = data
         } else {
            def p = window.mouse_pos(_editor_get("win"))
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
            if termpane.is_open(_editor_get("term_state")) && to_str(_editor_get("st").get("focus", "editor")) == "terminal" {
               def ch = chord.event_chord(data)
               if ch != "F6" && ch != "F7" && ch != "C-g" {
                  def tres = termpane.handle_event(_editor_get("term_state"), typ, data)
                  _editor_values["term_state"] = tres.get(0, _editor_get("term_state"))
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
            if pal.is_open(_editor_get("palette_state")) || _which_key_contains(_layout_sw(lay), _layout_sh(lay), float(data.get("x", -1.0)), float(data.get("y", -1.0))) {
               if INPUT_TRACE { print("[editor:event] scroll-overlay") }
               _handle_scroll(lay, data)
            } elif termpane.point_contains(_editor_get("term_state"), float(data.get("x", -1.0)), float(data.get("y", -1.0))) {
               if INPUT_TRACE { print("[editor:event] scroll-terminal") }
               def tres = termpane.handle_event(_editor_get("term_state"), window.EVENT_MOUSE_SCROLL, data)
               _editor_values["term_state"] = tres.get(0, _editor_get("term_state"))
            } else {
               if INPUT_TRACE { print("[editor:event] scroll-editor") }
               _handle_scroll(lay, data)
            }
         } else {
            if !is_dict(scroll_ev) { scroll_ev = {"dx": 0.0, "dy": 0.0, "x": 0.0, "y": 0.0} }
            if pal.is_open(_editor_get("palette_state")) || _which_key_contains(_layout_sw(lay), _layout_sh(lay), float(scroll_ev.get("x", -1.0)), float(scroll_ev.get("y", -1.0))) {
               if INPUT_TRACE { print("[editor:event] scroll-overlay") }
               _handle_scroll(lay, scroll_ev)
            } elif termpane.point_contains(_editor_get("term_state"), float(scroll_ev.get("x", -1.0)), float(scroll_ev.get("y", -1.0))) {
               if INPUT_TRACE { print("[editor:event] scroll-terminal") }
               def tres = termpane.handle_event(_editor_get("term_state"), window.EVENT_MOUSE_SCROLL, scroll_ev)
               _editor_values["term_state"] = tres.get(0, _editor_get("term_state"))
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
      if is_mouse_press && is_dict(data) && int(data.get("button", 0)) == 1 && termpane.is_open(_editor_get("term_state")) && termpane.point_contains(_editor_get("term_state"), float(data.get("x", -1.0)), float(data.get("y", -1.0))) {
         mut dict target = dict(3)
         target["name"] = termpane.tab_title(_editor_get("term_state"), termpane.active_tab(_editor_get("term_state")))
         target["path"] = ""
         target["dir"] = false
         _open_context(float(data.get("x", 0.0)), float(data.get("y", 0.0)), target, "terminal")
         e = _next_event()
         continue
      }
      if termpane.is_open(_editor_get("term_state")) {
         def mouse_ev = is_mouse_press || is_mouse_release || is_mouse_move
         def term_mouse = mouse_ev && is_dict(data) && termpane.point_contains(_editor_get("term_state"), float(data.get("x", -1.0)), float(data.get("y", -1.0)))
         if term_mouse || (!mouse_ev && to_str(_editor_get("st").get("focus", "editor")) == "terminal") {
            def tres = termpane.handle_event(_editor_get("term_state"), typ, data)
            _editor_values["term_state"] = tres.get(0, _editor_get("term_state"))
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
      _editor_values["frame_events_seen"] = true
      _editor_values["force_full_redraw"] = max(int(_editor_get("force_full_redraw")), 1)
      _status("input queue throttled; continuing next frame")
   }
   if have_motion {
      mut consumed = false
      def editor_dragging = _editor_get("st").get("drag_terminal", false) || _editor_get("st").get("drag_divider", false) || _editor_get("st").get("drag_select", false) || _editor_get("st").get("drag_scrollbar", false) || _editor_get("project_drag").get("armed", false)
      if termpane.is_open(_editor_get("term_state")) && !editor_dragging {
         def tres = termpane.handle_event(_editor_get("term_state"), window.EVENT_MOUSE_POS_CHANGED, motion_data)
         _editor_values["term_state"] = tres.get(0, _editor_get("term_state"))
         consumed = bool(tres.get(1, false))
      }
      if !consumed { _handle_mouse_move(lay, motion_data) }
   }
   if INPUT_TRACE && ev_count > 0 {
      print("[editor:input] events=" + to_str(ev_count) + " motion=" + to_str(motion_count) + " scroll=" + to_str(scroll_count))
   }
   if !window.mouse_down(_editor_get("win"), 0) && !window.mouse_down(_editor_get("win"), 1) {
      _editor_get("st")["drag_divider"] = false
      _editor_get("st")["drag_terminal"] = false
      _editor_get("st")["drag_select"] = false
      _editor_get("st")["drag_scrollbar"] = false
      _editor_get("st")["scrollbar_target"] = dict(0)
      _editor_get("project_drag")["active"] = false
      _editor_get("project_drag")["armed"] = false
      _editor_get("section_drag")["active"] = false
      _editor_get("section_drag")["section"] = ""
   }
   _editor_values["st"] = ed.clamp_cursor(_editor_get("st"))
   _editor_values["frame_events_seen"] = ev_count > 0 || have_motion || scroll_count > 0
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
      syntax.TOK_OPERATOR -> C_OPERATOR
      syntax.TOK_PUNCT -> C_PUNCT
      syntax.TOK_PREPROC -> C_PREPROC
      syntax.TOK_PROPERTY, syntax.TOK_ATTR, syntax.TOK_KEY -> C_PROPERTY
      syntax.TOK_PARAM, syntax.TOK_LABEL, syntax.TOK_REGISTER -> C_PARAM
      syntax.TOK_VALUE -> C_STRING
      syntax.TOK_VARIABLE -> C_VAR
      _ -> C_VAR
   }
}

fn _syntax_lang(str filename) str {
   if _editor_get("syntax_lang_cache").contains(filename) { return to_str(_editor_get("syntax_lang_cache").get(filename, "text")) }
   def lang = syntax.detect_language(filename)
   _editor_get("syntax_lang_cache")[filename] = lang
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
   def lines = ed.current_lines(_editor_get("st"))
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

;; ---- multi-line syntax state --------------------------------------------
;; The tokenizers see one line at a time. A line that starts inside a block
;; comment or an open string gets that prefix coloured here, and the rest of
;; the line is tokenized normally. States: "" (code), "c" (block comment),
;; "s<delim>" (inside a string closed by <delim>).

def int SYNTAX_STATE_MAX_LINES = 60000

fn _syn_line_comment(str lang) str {
   case lang {
      "nytrix" -> ";"
      "c", "javascript", "typescript" -> "//"
      "python", "bash", "cmake", "yaml" -> "#"
      "lua" -> "--"
      _ -> ""
   }
}

fn _syn_block(str lang) list {
   case lang {
      "c", "javascript", "typescript" -> ["/*", "*/"]
      "lua" -> ["--[[", "]]"]
      "html", "xml", "markdown" -> ["<!--", "-->"]
      _ -> []
   }
}

;; Delimiters whose strings may continue onto the next line.
fn _syn_spanning(str lang, str delim) bool {
   case lang {
      "nytrix" -> true
      "python" -> delim.len == 3
      "javascript", "typescript" -> delim == "`"
      "bash" -> delim == "\"" || delim == "'"
      "lua" -> delim == "]]"
      _ -> false
   }
}

fn _syn_at(str line, int i, str pat) bool {
   if pat.len <= 0 || i + pat.len > line.len { return false }
   mut int k = 0
   while k < pat.len {
      if load8(line, i + k) != load8(pat, k) { return false }
      k += 1
   }
   true
}

;; Byte index just past `delim` closing a string that starts at `i`, or -1.
fn _syn_string_end(str line, int i, str delim) int {
   mut int j = i
   while j < line.len {
      def c = load8(line, j)
      if c == 92 && delim != "]]" { j += 2 continue }
      if _syn_at(line, j, delim) { return j + delim.len }
      j += 1
   }
   -1
}

;; Byte index just past the closer of `state` in `line`, or -1 when the
;; construct continues past this line.
fn _syn_close(str lang, str line, str state) int {
   if state == "c" {
      def blk = _syn_block(lang)
      if blk.len < 2 { return 0 }
      def close = to_str(blk.get(1, ""))
      mut int j = 0
      while j < line.len {
         if _syn_at(line, j, close) { return j + close.len }
         j += 1
      }
      return -1
   }
   _syn_string_end(line, 0, str.str_slice(state, 1, str.utf8_len(state)))
}

;; State at the end of `line` when it starts in `state`.
fn _syn_scan(str lang, str line, str state) str {
   mut st = state
   mut int i = 0
   if st.len > 0 {
      def e = _syn_close(lang, line, st)
      if e < 0 { return st }
      i = e
      st = ""
   }
   def lc = _syn_line_comment(lang)
   def blk = _syn_block(lang)
   def bopen = blk.len >= 2 ? to_str(blk.get(0, "")) : ""
   while i < line.len {
      if bopen.len > 0 && _syn_at(line, i, bopen) {
         ;; search for the closer after the opener only
         mut int j = i + bopen.len
         def close = to_str(blk.get(1, ""))
         mut int found = -1
         while j < line.len {
            if _syn_at(line, j, close) { found = j + close.len break }
            j += 1
         }
         if found < 0 { return "c" }
         i = found
         continue
      }
      if lc.len > 0 && _syn_at(line, i, lc) { return "" }
      def c = load8(line, i)
      if c == 34 || c == 39 || (c == 96 && (lang == "javascript" || lang == "typescript")) {
         mut delim = chr(c)
         if (lang == "python" || lang == "nytrix") && i + 2 < line.len && load8(line, i + 1) == c && load8(line, i + 2) == c { delim = delim + delim + delim }
         def e = _syn_string_end(line, i + delim.len, delim)
         if e < 0 { return _syn_spanning(lang, delim) ? "s" + delim : "" }
         i = e
         continue
      }
      if lang == "lua" && _syn_at(line, i, "[[") {
         def e = _syn_string_end(line, i + 2, "]]")
         if e < 0 { return "s]]" }
         i = e
         continue
      }
      i += 1
   }
   ""
}

fn _syntax_line_state(str lang, int li) str {
   if li <= 0 || _syn_line_comment(lang).len == 0 && _syn_block(lang).len == 0 && lang != "nytrix" { return "" }
   def lines = ed.current_lines(_editor_get("st"))
   if lines.len > SYNTAX_STATE_MAX_LINES || li >= lines.len + 1 { return "" }
   if int(_editor_values.get("syn_state_rev", -1)) != int(_editor_get("content_rev")) || to_str(_editor_values.get("syn_state_lang", "")) != lang {
      _editor_values["syn_state_rev"] = int(_editor_get("content_rev"))
      _editor_values["syn_state_lang"] = lang
      _editor_values["syn_states"] = borrow([""])
   }
   mut states = _editor_values.get("syn_states", [""])
   while states.len <= li && states.len - 1 < lines.len {
      def k = states.len - 1
      states = states.append(_syn_scan(lang, to_str(lines.get(k, "")), to_str(states.get(k, ""))))
   }
   _editor_values["syn_states"] = borrow(states)
   to_str(states.get(li, ""))
}

fn _tokenize_stateful(str lang, str line, int li) list {
   def state = li > 0 ? _syntax_line_state(lang, li) : ""
   if state.len == 0 { return _tokenize_language(lang, line) }
   def int kind = state == "c" ? int(syntax.TOK_COMMENT) : int(syntax.TOK_STRING)
   def int close = _syn_close(lang, line, state)
   if close < 0 { return [[kind, 0, line.len]] }
   mut out = [[kind, 0, close]]
   if close >= line.len { return out }
   ;; Tokenize the remainder (code-point slice) and shift back to bytes.
   def rest = str.str_slice(line, _byte_to_cp(line, close), str.utf8_len(line))
   for t in _tokenize_language(lang, rest) {
      out = out.append([int(t.get(0, 0)), int(t.get(1, 0)) + close, int(t.get(2, 0))])
   }
   out
}

fn _byte_to_cp(str line, int b) int {
   mut int i = 0
   mut int cps = 0
   while i < b && i < line.len {
      def c = load8(line, i)
      i += c < 0x80 ? 1 : (c < 0xE0 ? 2 : (c < 0xF0 ? 3 : 4))
      cps += 1
   }
   cps
}

fn _tokenize_line(str filename, str line, int li=-1) list {
   def lang = _syntax_lang(filename)
   if lang == "markdown" {
      def fenced_lang = _markdown_fenced_lang_at(li)
      if fenced_lang.len > 0 { return _tokenize_language(fenced_lang, line) }
   }
   _tokenize_stateful(lang, line, li)
}

fn _syntax_key(str filename, int li, str line) str {
   filename + ":" + to_str(li) + ":" + to_str(line.len) + ":" + to_str(hash(line))
}

fn _wide_cp(int cp) bool {
   (cp >= 0x1100 && cp <= 0x115F) || (cp >= 0x2E80 && cp <= 0xA4CF) || (cp >= 0xAC00 && cp <= 0xD7A3) ||
   (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0xFE30 && cp <= 0xFE4F) || (cp >= 0xFF00 && cp <= 0xFF60) ||
   (cp >= 0xFFE0 && cp <= 0xFFE6) || (cp >= 0x1F300 && cp <= 0x1FAFF) || (cp >= 0x20000 && cp <= 0x3FFFD)
}

;; Pixel offset of code point column `col`: one cell per character, two for
;; East Asian wide characters and emoji, tabs to the next 4-cell stop.
fn _prefix_x(str line, int col) int {
   if col <= 0 { return 0 }
   mut int advance = int(float(_editor_get("FONT_BODY_ADV")))
   if advance < 1 { advance = 1 }
   def int tab_width = advance * 4
   mut int x = 0
   mut int i = 0
   mut int cps = 0
   while cps < col && i < line.len {
      def int c = load8(line, i)
      if c == 9 {
         x = (x / tab_width + 1) * tab_width
         i += 1
      } elif c < 128 {
         x += advance
         i += 1
      } else {
         def int n = c < 0xE0 ? 2 : (c < 0xF0 ? 3 : 4)
         mut int cp = n == 2 ? (c & 31) : (n == 3 ? (c & 15) : (c & 7))
         mut int k = 1
         while k < n && i + k < line.len { cp = (cp << 6) | (load8(line, i + k) & 63) k += 1 }
         x += _wide_cp(cp) ? advance * 2 : advance
         i += n
      }
      cps += 1
   }
   x
}

fn _syntax_runs(str filename, str line, int li) list {
   if int(_editor_get("syntax_cache_rev")) != int(_editor_get("content_rev")) {
      _editor_values["syntax_cache_rev"] = int(_editor_get("content_rev"))
      _editor_values["syntax_run_cache"] = borrow(dict(256))
      _editor_values["syntax_flat_cache"] = borrow(dict(256))
   }
   def key = _syntax_key(filename, li, line)
   if _editor_get("syntax_run_cache").contains(key) { return _editor_get("syntax_run_cache").get(key, []) }
   mut toks = _tokenize_line(filename, line, li)
   if _current_path() == to_str(_editor_values.get("semantic_path", "")) && filename == to_str(ed.current_buffer(_editor_get("st")).get("name", "")) {
      def semantic = _editor_values.get("semantic_lines", dict(0)).get(to_str(li), [])
      if semantic.len > 0 { toks = lsp.merge_semantic_tokens(_lsp(), line, toks, semantic) }
   }
   mut runs = []
   ;; Tokenizers report byte offsets; slicing and x positions use code points.
   def int cp_len = str.utf8_len(line)
   def bool ascii = cp_len == line.len
   mut cp_at = []
   if !ascii {
      mut int b = 0
      mut int cps = 0
      while b < line.len {
         def c = load8(line, b)
         def int n = c < 0x80 ? 1 : (c < 0xE0 ? 2 : (c < 0xF0 ? 3 : 4))
         mut int k = 0
         while k < n { cp_at = cp_at.append(cps) k += 1 }
         b += n
         cps += 1
      }
      cp_at = cp_at.append(cps)
   }
   if toks.len == 0 {
      if line.len > 0 { runs = runs.append([line, syntax.TOK_TEXT, 0.0]) }
   } else {
      mut int cursor = 0
      mut int i = 0
      while i < toks.len {
         def tok = toks.get(i)
         def int kind = int(tok.get(0, syntax.TOK_TEXT))
         def int bstart = min(max(int(tok.get(1, 0)), 0), line.len)
         def int bend = int(min(line.len, bstart + int(max(0, int(tok.get(2, 0))))))
         def int start = ascii ? bstart : int(cp_at.get(bstart, cp_len))
         def int end = ascii ? bend : int(cp_at.get(bend, cp_len))
         if start > cursor {
            def int xoff = _prefix_x(line, cursor)
            runs = runs.append([str.str_slice(line, cursor, start), syntax.TOK_TEXT, xoff])
         }
         def int seg_start = max(start, cursor)
         if end > seg_start {
            def int xoff = _prefix_x(line, seg_start)
            runs = runs.append([str.str_slice(line, seg_start, end), kind, xoff])
            cursor = end
         }
         i += 1
      }
      if cursor < cp_len {
         def int xoff = _prefix_x(line, cursor)
         runs = runs.append([str.str_slice(line, cursor, cp_len), syntax.TOK_TEXT, xoff])
      }
   }
   _editor_get("syntax_run_cache")[key] = borrow(runs)
   forget(runs)
   _editor_get("syntax_run_cache").get(key, [])
}

fn _append_syntax_flat_run(list flat, str text, f64 x, int kind) list {
   if text.len <= 0 { return flat }
   flat = flat.append(text)
   flat = flat.append(x)
   flat = flat.append(kind)
   flat
}

fn _syntax_rel_runs(str filename, str line, int li) list {
   if int(_editor_get("syntax_cache_rev")) != int(_editor_get("content_rev")) {
      _editor_values["syntax_cache_rev"] = int(_editor_get("content_rev"))
      _editor_values["syntax_run_cache"] = borrow(dict(256))
      _editor_values["syntax_flat_cache"] = borrow(dict(256))
   }
   def key = _syntax_key(filename, li, line)
   if _editor_get("syntax_flat_cache").contains(key) { return _editor_get("syntax_flat_cache").get(key, []) }
   def runs = _syntax_runs(filename, line, li)
   mut flat = []
   mut str pending_text = ""
   mut f64 pending_x = 0.0
   mut f64 pending_end = 0.0
   mut int pending_kind = -1
   mut i = 0
   while i < runs.len {
      def run = runs.get(i)
      def text = to_str(run.get(0, ""))
      if text.len > 0 {
         def xoff = float(run.get(2, 0.0))
         def int kind = int(run.get(1, syntax.TOK_TEXT))
         def width = _measure_ui(int(_editor_get("font_body")), text, float(text.len) * float(_editor_get("FONT_BODY_ADV")))
         def f64 gap = abs(xoff - pending_end)
         if pending_text.len > 0 && pending_kind == kind && gap <= 0.75 {
            pending_text = pending_text + text
            pending_end = max(pending_end, xoff + width)
         } else {
            flat = _append_syntax_flat_run(flat, pending_text, pending_x, pending_kind)
            pending_text = text
            pending_x = xoff
            pending_end = xoff + width
            pending_kind = kind
         }
      }
      i += 1
   }
   flat = _append_syntax_flat_run(flat, pending_text, pending_x, pending_kind)
   _editor_get("syntax_flat_cache")[key] = borrow(flat)
   forget(flat)
   _editor_get("syntax_flat_cache").get(key, [])
}

fn _append_plain_run(list out, str line, f64 x, f64 y) list {
   if line.len <= 0 { return out }
   out = out.append(line)
   out = out.append(x)
   out = out.append(y)
   out = out.append(syntax.TOK_TEXT)
   out
}

fn _diff_buffer_name(str filename) bool {
   str.startswith(filename, "*git diff:") || str.startswith(filename, "*diff:")
}

fn _diff_line_color(str line) int {
   if str.startswith(line, "diff --git") { return 10001 }
   if str.startswith(line, "+++") || str.startswith(line, "---") { return 10001 }
   if str.startswith(line, "@@") { return 10002 }
   if str.startswith(line, "+") { return 10003 }
   if str.startswith(line, "-") { return 10004 }
   if str.startswith(line, ">") { return 10003 }
   if str.startswith(line, "<") { return 10004 }
   if str.startswith(line, "left:") || str.startswith(line, "right:") { return 10005 }
   10006
}

fn _body_run_color(int kind) any {
   if kind == 10001 { return C_CYAN }
   if kind == 10002 { return C_ACCENT }
   if kind == 10003 { return C_OK }
   if kind == 10004 { return C_WARN }
   if kind == 10005 { return C_ACCENT_2 }
   if kind == 10006 { return C_MUTED }
   if kind == 10007 { return C_MUTED }
   if kind == 10008 { return C_CYAN }
   if kind == 10009 { return C_ACCENT }
   if kind == 10010 { return C_ACCENT_2 }
   if kind == 10011 { return C_WARN }
   _tok_color(kind)
}

fn _append_diff_run(list out, str line, f64 x, f64 y) list {
   if line.len <= 0 { return out }
   out = out.append(line)
   out = out.append(x)
   out = out.append(y)
   out = out.append(_diff_line_color(line))
   out
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
   def hints = _inlays_for_line(li)
   if hints.len > 0 {
      mut result = out
      def runs = highlight ? _syntax_runs(filename, line, li) : [[line, syntax.TOK_TEXT, 0.0]]
      mut int col = 0
      mut f64 added = 0.0
      for run in runs {
         def text = to_str(run.get(0, ""))
         def int kind = int(run.get(1, syntax.TOK_TEXT))
         def int count = str.utf8_len(text)
         mut int c = 0
         while c < count {
            mut int next = count
            for hint in hints {
               def int at = _inlay_col(line, hint) - col
               if at >= c && at < next { next = at }
            }
            if next > c {
               result = result.append(str.str_slice(text, c, next)).append(x + _measure_prefix(line, col + c) + added).append(y).append(kind)
               c = next
            }
            if c >= count { break }
            for hint in hints {
               if _inlay_col(line, hint) == col + c {
                  def label = _inlay_label(hint)
                  result = result.append(label).append(x + _measure_prefix(line, col + c) + added).append(y).append(10006)
                  added += _inlay_width(hint)
               }
            }
            result = result.append(str.str_slice(text, c, c + 1)).append(x + _measure_prefix(line, col + c) + added).append(y).append(kind)
            c += 1
         }
         col += count
      }
      for hint in hints {
         if _inlay_col(line, hint) == col {
            def label = _inlay_label(hint)
            result = result.append(label).append(x + _measure_prefix(line, col) + added).append(y).append(10006)
            added += _inlay_width(hint)
         }
      }
      return result
   }
   if !highlight { return _append_plain_run(out, line, x, y) }
   def runs = _syntax_rel_runs(filename, line, li)
   if runs.len == 0 { return _append_plain_run(out, line, x, y) }
   mut int i = 0
   while i + 2 < runs.len {
      out = out.append(to_str(runs.get(i, "")))
      out = out.append(x + float(runs.get(int(i + 1), 0)))
      out = out.append(y)
      out = out.append(int(runs.get(int(i + 2), syntax.TOK_TEXT)))
      i += 3
   }
   out
}

fn _draw_editor_body_runs(int font, list runs) int {
   mut int drew = 0
   mut int i = 0
   while i + 3 < runs.len {
      def str text = to_str(runs.get(i, ""))
      def f64 x = float(runs.get(int(i + 1), 0.0))
      def f64 y = float(runs.get(int(i + 2), 0.0))
      def int kind = int(runs.get(int(i + 3), syntax.TOK_TEXT))
      def color = _body_run_color(kind)
      if gfx.draw_text(font, text, x, y, color) { drew = 1 }
      i += 4
   }
   drew
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
   to_str(int(_editor_get("content_rev"))) + "\t" + filename + "\t" + to_str(scroll) + "\t" + to_str(rows) +
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
   if key == to_str(_editor_get("visible_runs_cache_key")) { return 0 }
   mut line_runs = []
   mut text_runs = []
   mut i = 0
   while i < rows && scroll + i < lines.len {
      def li = scroll + i
      def yy = _editor_row_y(y, i)
      def baseline_y = _editor_row_baseline_y(yy)
      def text_y = _row_text_y_from_baseline(int(_editor_get("font_body")), baseline_y, ed.LINE_H)
      def num_y = _row_text_y_from_baseline(int(_editor_get("font_small")), baseline_y, ed.LINE_H)
      def line = to_str(lines.get(li, ""))
      def draw_line = _draw_line_text(line, draw_limit)
      if show_numbers {
         line_runs = line_runs.append(to_str(li + 1))
         line_runs = line_runs.append(num_x)
         line_runs = line_runs.append(num_y)
      }
      text_runs = _append_highlight_runs(text_runs, filename, draw_line, li, text_x, text_y, _highlight_on(draw_line))
      i += 1
   }
   _editor_values["visible_runs_cache_key"] = key
   _editor_values["visible_line_runs_cache"] = borrow(line_runs)
   _editor_values["visible_text_runs_cache"] = borrow(text_runs)
   forget(line_runs)
   forget(text_runs)
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

fn _measure_prefix(str line, int col) f64 {
   if int(_editor_get("width_cache_rev")) != int(_editor_get("content_rev")) {
      _editor_values["width_cache_rev"] = int(_editor_get("content_rev"))
      _editor_values["width_cache"] = borrow(dict(512))
   }
   def int n = int(min(max(col, 0), line.len))
   if n <= 0 { return 0.0 }
   def key = to_str(n) + ":" + line
   if _editor_get("width_cache").contains(key) { return float(_editor_get("width_cache").get(key, 0.0)) }
   def w = float(_prefix_x(line, n))
   _editor_get("width_cache")[key] = w
   w
}

fn _inlays_for_line(int li) list {
   if _current_path().len == 0 || _current_path() != to_str(_editor_values.get("inlay_path", "")) { return [] }
   _editor_values.get("inlay_lines", dict(0)).get(to_str(li), [])
}

fn _inlay_label(dict hint) str {
   (bool(hint.get("padding_left", false)) ? " " : "") + to_str(hint.get("label", "")) + (bool(hint.get("padding_right", false)) ? " " : "")
}

fn _inlay_col(str line, dict hint) int {
   def int col = int(hint.get("col", -1))
   if col < 0 || col > lsp.lsp_col(_lsp(), line, str.utf8_len(line)) { return -1 }
   lsp.editor_col(_lsp(), line, col)
}

fn _inlay_width(dict hint) f64 {
   def label = _inlay_label(hint)
   _measure_ui(int(_editor_get("font_body")), label, float(str.utf8_len(label)) * float(_editor_get("FONT_BODY_ADV")))
}

fn _visual_prefix(str line, int li, int col) f64 {
   mut f64 x = _measure_prefix(line, col)
   for hint in _inlays_for_line(li) {
      def int at = _inlay_col(line, hint)
      if at >= 0 && at <= col { x += _inlay_width(hint) }
   }
   x
}

fn _visual_col_at(str line, int li, f64 target) int {
   def hints = _inlays_for_line(li)
   if hints.len == 0 { return ed.col_at(int(_editor_get("font_body")), line, target, 0.0) }
   if target <= 0.0 { return 0 }
   def int length = str.utf8_len(line)
   mut int col = 0
   while col <= length {
      def f64 end = _visual_prefix(line, li, col)
      mut f64 hint_width = 0.0
      for hint in hints { if _inlay_col(line, hint) == col { hint_width += _inlay_width(hint) } }
      if hint_width > 0.0 && target >= end - hint_width && target <= end { return col }
      if col == length { return length }
      def f64 next = _visual_prefix(line, li, col + 1)
      mut f64 next_hints = 0.0
      for hint in hints { if _inlay_col(line, hint) == col + 1 { next_hints += _inlay_width(hint) } }
      if target < (end + next - next_hints) * 0.5 { return col }
      col += 1
   }
   length
}

fn _hex_color(str hex) any {
   hex.len >= 7 ? gfx.color_hex(hex) : C_ACCENT
}

fn _color_picker_open() int {
   def hit = colorpicker.at_cursor(ed.current_lines(_editor_get("st")), int(_editor_get("st").get("cursor_line", 0)), int(_editor_get("st").get("cursor_col", 0)))
   if !is_dict(hit) || hit.len <= 0 {
      _editor_values["color_state"] = colorpicker.dismiss(_editor_get("color_state"))
      _status("no hex color")
      return 0
   }
   _editor_values["color_state"] = colorpicker.open(_editor_get("color_state"), hit)
   _status(to_str(hit.get("hex", "")) + " " + colorpicker.rgb_label(to_str(hit.get("hex", ""))))
   0
}

fn _draw_color_swatches_visible(list lines, list view, int start, int rows, f64 text_x, f64 y, int draw_limit) int {
   def scroll = int(view.get(start, 0))
   def swatches = colorpicker.swatches_visible(lines, scroll, max(rows, view.len - start), draw_limit)
   mut i = 0
   while i < swatches.len {
      def hit = swatches.get(i, {})
      def li = int(hit.get("line", -1))
      def int sr = _fold_visible_index(view, li) - start
      if sr >= 0 && sr < rows && li < lines.len {
         def line = _draw_line_text(to_str(lines.get(li, "")), draw_limit)
         def int start = int(min(max(int(hit.get("start", 0)), 0), line.len))
         def int stop = int(min(max(int(hit.get("end", start)), start), line.len))
         def hex = to_str(hit.get("hex", ""))
         if hex.len >= 7 && stop > start {
            def f64 yy = _editor_row_y(y, sr)
            def f64 sx = text_x + _visual_prefix(line, li, start)
            def f64 ex = text_x + _visual_prefix(line, li, stop)
            def col = _hex_color(hex)
            _fill_rect(sx, yy + float(ed.LINE_H) - 4.0, max(12.0, ex - sx), 2.0, gfx.color_alpha(col, 0.85))
            _fill_rect(sx - 13.0, yy + 3.0, 8.0, 8.0, col)
            _stroke_rect(sx - 13.0, yy + 3.0, 8.0, 8.0, C_LINE_2, 1.0)
         }
      }
      i += 1
   }
   0
}

fn _draw_color_picker(dict lay, f64 sw, f64 sh) int {
   ;; The hex literal under the cursor, edited with the toolkit's picker; a
   ;; change is written back into the buffer as the same literal.
   if !colorpicker.is_open(_editor_get("color_state")) { return 0 }
   def hex = colorpicker.hex(_editor_get("color_state"))
   if hex.len < 7 { return 0 }
   def row = colorpicker.line(_editor_get("color_state"))
   def scroll = int(_editor_get("st").get("scroll", 0))
   def x0 = float(lay.get("edit_x", 0.0))
   def y0 = float(lay.get("edit_y", 0.0))
   def rows = ed.visible_rows(float(lay.get("edit_h", 0.0)))
   def f64 w = float(min(300.0, max(1.0, sw - 16.0)))
   def f64 h = float(min(440.0, max(1.0, sh - 16.0)))
   mut fit0 = _fit_rect(x0 + 42.0, y0 + 40.0, w, h, sw, sh, 8.0)
   if row >= scroll && row < scroll + rows {
      def line = to_str(ed.current_lines(_editor_get("st")).get(row, ""))
      def f64 row_y = float(_editor_row_y(y0, row - scroll))
      fit0 = _fit_rect(_line_text_x(lay) + _visual_prefix(line, row, colorpicker.start(_editor_get("color_state"))) + 18.0, row_y + float(ed.LINE_H) + 2.0, w, h, sw, sh, 8.0)
   }
   _gui_overlay_window("ed_color", float(fit0.get(0, 8.0)), float(fit0.get(1, 8.0)), w, h)
   gui.text_colored(hex + "   " + colorpicker.rgb_label(hex), "text")
   def list rgba = colorpicker.rgba(hex)
   def list picked = gui.color_picker4("ed_color_pick", "", rgba)
   gui.text_colored("Esc closes; edits write back to the buffer", "text_dim")
   gui.end_window()
   def str next_hex = colorpicker.rgba_hex(picked)
   def str was_hex = str.lower(str.str_slice(hex, 0, 7))
   def str now_hex = str.lower(next_hex)
   if now_hex != was_hex { _color_picker_write(next_hex + str.str_slice(hex, 7, hex.len)) }
   0
}

fn _color_picker_write(str hex) int {
   def cs = _editor_get("color_state")
   def int li = colorpicker.line(cs)
   def int a = colorpicker.start(cs)
   def int b = colorpicker.end(cs)
   def lines = ed.current_lines(_editor_get("st"))
   if li < 0 || li >= lines.len { return 0 }
   def line = to_str(lines.get(li, ""))
   if a < 0 || b > line.len || b <= a { return 0 }
   _editor_values["hist"] = history.push(_editor_get("hist"), _editor_get("st"))
   mut out = lines
   out[li] = str.str_slice(line, 0, a) + hex + str.str_slice(line, b, line.len)
   _editor_values["st"] = ed.set_lines(_editor_get("st"), out)
   _touch_content()
   def hit = colorpicker.at_cursor(out, li, a)
   if is_dict(hit) && hit.len > 0 { _editor_values["color_state"] = colorpicker.open(cs, hit) }
   _status(hex + " " + colorpicker.rgb_label(hex))
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
   def sx = text_x + _visual_prefix(line, li, a)
   def ex = text_x + _visual_prefix(line, li, b)
   _fill_rect(sx, y - 2.0, max(4.0, ex - sx), ed.LINE_H, gfx.color_alpha(C_SELECT, 0.70))
   0
}

fn _draw_find_visible(list lines, list view, int start, int rows, f64 text_x, f64 y) int {
   if !find.is_open(_editor_get("find_state")) || find.count(_editor_get("find_state")) <= 0 { return 0 }
   def rs = find.results(_editor_get("find_state"))
   def active_idx = find.index(_editor_get("find_state"))
   mut i = 0
   while i < rs.len {
      def r = rs.get(i)
      def row = int(r.get("line", -1))
      def int sr = _fold_visible_index(view, row) - start
      if sr >= 0 && sr < rows && row < lines.len {
         def line = to_str(lines.get(row, ""))
         def int a = int(min(max(int(r.get("start", 0)), 0), line.len))
         def int b = int(min(max(int(r.get("end", a)), a), line.len))
         if b > a {
            def f64 yy = float(_editor_row_y(y, sr))
            def sx = text_x + _visual_prefix(line, row, a)
            def ex = text_x + _visual_prefix(line, row, b)
            _fill_rect(sx, yy - 2.0, max(4.0, ex - sx), float(ed.LINE_H), i == active_idx ? gfx.color_alpha(C_ACCENT, 0.52) : gfx.color_alpha(C_CYAN, 0.24))
         }
      }
      i += 1
   }
   0
}

fn _draw_minimap(dict lay, int rows, int start, int total) int {
   if total <= rows { return 0 }
   def x = float(lay.get("edit_x", 0.0)) + float(lay.get("edit_w", 0.0)) - 10.0
   def y = float(lay.get("edit_y", 0.0)) + 10.0
   def f64 h = float(lay.get("edit_h", 0.0)) - 20.0
   def f64 thumb_h = max(18.0, h * float(rows) / float(total))
   def int max_start = max(1, total - rows)
   def f64 thumb_y = y + (h - thumb_h) * float(start) / float(max_start)
   _fill_rect(x, y, 3.0, h, gfx.color_alpha(C_LINE, 0.65))
   _fill_rect(x - 1.0, thumb_y, 5.0, thumb_h, gfx.color_alpha(C_ACCENT, 0.78))
   0
}

fn _outline_visible(f64 h) int {
   max(0, int((h - float(SECTION_H)) / float(outline.ROW_H)))
}

fn _outline_clamp(int scroll, int total, int visible) int {
   min(max(0, scroll), max(0, total - visible))
}

fn _rail_tree_height(f64 body_h, int tree_rows, int outline_rows) f64 {
   def sep = 1.0
   def f64 avail = max(1.0, body_h - sep)
   if avail < 120.0 { return avail * 0.50 }
   def f64 min_pane = min(150.0, max(64.0, avail * 0.24))
   def f64 tree_need = float(project.TREE_HEADER_H) + float(max(1, tree_rows)) * float(project.TREE_ROW_H)
   def f64 outline_need = float(SECTION_H) + float(max(1, outline_rows)) * float(outline.ROW_H)
   def f64 total_need = float(max(1.0, tree_need + outline_need))
   mut f64 h = avail * tree_need / total_need
   if outline_rows <= 0 { h = max(h, avail * 0.72) }
   if tree_rows <= 3 && outline_rows > 8 { h = min(h, avail * 0.36) }
   min(max(min_pane, h), max(min_pane, avail - min_pane))
}

fn _top_tab_w(f64 sw, int count) f64 {
   min(TOP_TAB_MAX_W, max(TOP_TAB_MIN_W, (sw - 12.0) / float(max(1, count))))
}

fn _top_tab_hit(dict lay, f64 mx, f64 my) dict {
   def top_h = float(lay.get("top", 0.0))
   if !bool(_editor_get("st").get("show_titlebar", true)) || top_h <= 1.0 || my < 0.0 || my > top_h { return _tab_hit_result("", -1, true) }
   def bs = _editor_get("st").get("buffers", [])
   def sw = _layout_sw(lay)
   def tab_w = _top_tab_w(sw, bs.len)
   def start_x = 6.0
   mut i = 0
   while i < bs.len {
      def x = start_x + float(i) * tab_w
      def f64 right = min(x + tab_w - 2.0, sw - 6.0)
      if right - x >= 36.0 && mx >= x && mx < right && my >= 3.0 && my < top_h - 2.0 {
         return _tab_hit_result(mx >= right - 22.0 ? "close" : "select", i, true)
      }
      i += 1
   }
   _tab_hit_result("", -1, true)
}

fn _select_top_tab(int idx) int {
   def bs = _editor_get("st").get("buffers", [])
   if idx < 0 || idx >= bs.len { return 0 }
   _clear_pending_destructive_prompts()
   if idx != int(_editor_get("st").get("active", 0)) { _nav_push_current() }
   _editor_values["st"] = ed.select_buffer(_editor_get("st"), idx)
   _buffer_changed(true)
   _nav_record_current()
   _set_focus("editor", false)
   0
}

fn _close_top_tab(int idx) int {
   def bs = _editor_get("st").get("buffers", [])
   if idx < 0 || idx >= bs.len { return 0 }
   _editor_get("st")["active"] = idx
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
   def bs = _editor_get("st").get("buffers", [])
   def active = int(_editor_get("st").get("active", 0))
   def tab_w = _top_tab_w(sw, bs.len)
   def tab_text_y = _text_center_y_for(int(_editor_get("font_modeline")), 3.0, top_h - 5.0)
   mut runs = []
   mut i = 0
   while i < bs.len {
      def b = bs.get(i, {})
      def x = 6.0 + float(i) * tab_w
      def f64 right = min(x + tab_w - 2.0, sw - 6.0)
      if right - x >= 36.0 {
         def on = i == active
         def dirty = bool(b.get("dirty", false))
         def icon = _editor_file_icon(b)
         _fill_rect(x, 3.0, min(tab_w - 2.0, sw - x - 6.0), top_h - 5.0, on ? C_PANE_2 : C_PANE)
         _stroke_rect(x, 3.0, min(tab_w - 2.0, sw - x - 6.0), top_h - 5.0, on ? C_LINE_2 : C_LINE, 1.0)
         if on { _fill_rect(x, top_h - 2.0, min(tab_w - 2.0, sw - x - 6.0), 2.0, C_ACCENT) }
         _draw_icon(icon, x + 8.0, _center_y(3.0, top_h - 5.0, 14.0), 14.0, on ? C_ACCENT : C_MUTED)
         def tab_label = _modeline_text((dirty ? "* " : "") + to_str(b.get("name", "buffer")))
         runs = _append_text_run(runs, _fit_chrome_text(int(_editor_get("font_modeline")), tab_label, right - x - 52.0), x + 28.0, tab_text_y, on ? U_TEXT : U_MUTED)
         _draw_icon("close", right - 18.0, _center_y(3.0, top_h - 5.0, 12.0), 12.0, on ? C_MUTED : C_DIM)
      }
      i += 1
   }
   _flush_rects()
   if runs.len > 0 { gfx.draw_text_runs_flat_colors(int(_editor_get("font_modeline")), runs) }
   0
}

fn _queue_git_badge(list runs, str code, f64 right_x, f64 y) list {
   def label = project.status_label(code)
   if label.len <= 0 { return runs }
   def col = _git_color(code)
   _fill_rect(right_x - 18.0, y + 3.0, 14.0, 14.0, gfx.color_alpha(col, 0.18))
   _stroke_rect(right_x - 18.0, y + 3.0, 14.0, 14.0, col, 1.0)
   _append_center_run(runs, int(_editor_get("font_small")), label, right_x - 11.0, y + 4.0, _pack(col))
}

fn _draw_section_title(str icon, str title, f64 x, f64 y, f64 w, str right="") int {
   _fill_rect(x, y, w, SECTION_H, C_PANE_2)
   def icon_y = _center_y(y, SECTION_H, 15.0)
   def text_y = _row_text_y(int(_editor_get("font_small")), y, SECTION_H)
   _draw_icon(icon, x + 8.0, icon_y, 15.0, C_MUTED)
   mut runs = []
   def int font = int(_editor_get("font_small"))
   def tail = _fit_chrome_text(font, right, max(0.0, w * 0.40 - 8.0))
   def f64 tail_w = _measure_ui(font, tail, 0.0)
   runs = _append_text_run(runs, _fit_chrome_text(font, title, w - 46.0 - tail_w), x + 30.0, text_y, U_MUTED)
   if tail.len > 0 { runs = _append_right_run(runs, font, tail, x + w - 8.0, text_y, U_MUTED) }
   _flush_rects()
   if runs.len > 0 { gfx.draw_text_runs_flat_colors(int(_editor_get("font_small")), runs) }
   0
}

fn _rail_tab() str { to_str(_editor_get("st").get("rail_tab", "files")) }

fn _rail_tab_at(f64 x, f64 y, f64 w, f64 mx, f64 my) str {
   if !widgets.hit(mx, my, x, y, w, RAIL_TABS_H) { return "" }
   def tab_w = w / 3.0
   def idx = int((mx - x) / tab_w)
   idx <= 0 ? "files" : (idx == 1 ? "timeline" : "git")
}

fn _rail_section_at(dict lay, f64 mx, f64 my) str {
   if !bool(_editor_get("st").get("show_hierarchy", true)) || _rail_tab() != "files" { return "" }
   def x = float(lay.get("rail_x", 0.0))
   def y = float(lay.get("rail_y", 0.0))
   def w = float(lay.get("rail_w", 0.0))
   def h = float(lay.get("rail_h", 0.0))
   if !widgets.hit(mx, my, x, y + RAIL_TABS_H, w, h - RAIL_TABS_H) { return "" }
   def body_y = y + RAIL_TABS_H
   def body_h = h - RAIL_TABS_H
   def show_project = bool(_editor_get("st").get("show_project", true))
   def show_outline = bool(_editor_get("st").get("show_outline", true))
   if show_project { _ensure_project_tree() }
   def rows = project.tree(_editor_get("project_state"))
   def syms = show_outline ? _outline_symbols() : []
   def tree_h = (!show_project) ? 0.0 : (show_outline ? _rail_tree_height(body_h, rows.len, syms.len) : body_h)
   if show_project && my >= body_y && my < body_y + float(project.TREE_HEADER_H) { return "project" }
   def f64 outline_y = body_y + (show_project ? tree_h + 1.0 : 0.0)
   if show_outline && my >= outline_y && my < outline_y + float(SECTION_H) { return "outline" }
   ""
}

fn _hide_rail_section(str section) int {
   if section == "project" { _editor_get("st")["show_project"] = false }
   elif section == "outline" { _editor_get("st")["show_outline"] = false }
   if !bool(_editor_get("st").get("show_project", true)) && !bool(_editor_get("st").get("show_outline", true)) {
      _editor_get("st")["show_hierarchy"] = false
      _set_focus("editor", false)
   }
   _save_ui_cache()
   _status(section + " hidden")
   0
}

fn _toggle_rail_section(str section) int {
   if section == "project" { _editor_get("st")["show_project"] = !bool(_editor_get("st").get("show_project", true)) }
   elif section == "outline" { _editor_get("st")["show_outline"] = !bool(_editor_get("st").get("show_outline", true)) }
   if bool(_editor_get("st").get("show_project", true)) || bool(_editor_get("st").get("show_outline", true)) { _editor_get("st")["show_hierarchy"] = true }
   _save_ui_cache()
   _status(section + (section == "project" ? (bool(_editor_get("st").get("show_project", true)) ? " on" : " off") : (bool(_editor_get("st").get("show_outline", true)) ? " on" : " off")))
   0
}

fn _draw_rail_tabs(f64 x, f64 y, f64 w) int {
   def active = _rail_tab()
   def tabs = [["files", "tree", "Files"], ["timeline", "history", "Time"], ["git", "git_branch", "Git"]]
   def tab_w = w / 3.0
   mut runs = []
   mut i = 0
   while i < tabs.len {
      def t = tabs.get(i)
      def id = to_str(t.get(0, ""))
      def tx = _snap_px(x + float(i) * tab_w)
      def on = id == active
      def text_y = _row_text_y(int(_editor_get("font_small")), y, RAIL_TABS_H)
      def bg_color = on ? _pack(gfx.color_alpha(C_ACCENT, 0.13)) : _pack(C_PANE_2)
      _fill_rect(tx, y, tab_w, RAIL_TABS_H, bg_color)
      _stroke_rect(tx, y, tab_w, RAIL_TABS_H, on ? C_ACCENT : C_LINE, 1.0)
      def int font = int(_editor_get("font_small"))
      def label = to_str(t.get(2, ""))
      def f64 label_w = _measure_ui(font, label, 32.0)
      def bool show_label = tab_w >= label_w + 31.0
      def f64 group_w = show_label ? label_w + 21.0 : 15.0
      def f64 ix = _snap_px(tx + (tab_w - group_w) * 0.5)
      _draw_icon(to_str(t.get(1, "")), ix, _center_y(y, RAIL_TABS_H, 15.0), 15.0, on ? C_ACCENT : C_MUTED)
      if show_label { runs = _append_text_run(runs, label, ix + 21.0, text_y, on ? U_TEXT : U_MUTED) }
      i += 1
   }
   _flush_rects()
   if runs.len > 0 { gfx.draw_text_runs_flat_colors(int(_editor_get("font_small")), runs) }
   0
}

fn _draw_git_panel(f64 x, f64 y, f64 w, f64 h) int {
   _ensure_project_git()
   def rows = project.changes(_editor_get("project_state"))
   def counts = project.counts(_editor_get("project_state"))
   def dirty = int(counts.get("modified", 0)) + int(counts.get("added", 0)) + int(counts.get("deleted", 0)) + int(counts.get("untracked", 0))
   _draw_section_title("git_branch", "CHANGES", x, y, w, project.branch(_editor_get("project_state")) + (dirty > 0 ? " +" + to_str(dirty) : ""))
   def visible = project.visible_count(h)
   def scroll = project.clamp_scroll(int(_editor_get("st").get("git_scroll", 0)), rows.len, visible)
   _editor_get("st")["git_scroll"] = scroll
   mut runs = []
   mut i = 0
   while i < visible && scroll + i < rows.len {
      def yy = project.row_y(y, i)
      def e = rows.get(scroll + i, {})
      def code = to_str(e.get("git", ""))
      def col = _git_color(code)
      def baseline_y = _row_baseline_y(int(_editor_get("font_small")), yy, project.TREE_ROW_H)
      def text_y = _row_text_y_from_baseline(int(_editor_get("font_small")), baseline_y, project.TREE_ROW_H)
      _draw_icon(_editor_file_icon(e), x + 10.0, _center_y(yy, project.TREE_ROW_H, 16.0), 16.0, col)
      runs = _append_text_run(runs, _fit_chrome_text(int(_editor_get("font_small")), to_str(e.get("rel", "")), w - 64.0), x + 34.0, text_y, U_MUTED)
      runs = _queue_git_badge(runs, code, x + w - 8.0, yy)
      i += 1
   }
   if rows.len == 0 { runs = _append_text_run(runs, "working tree clean", x + 12.0, y + 36.0, U_MUTED) }
   _flush_rects()
   if runs.len > 0 { gfx.draw_text_runs_flat_colors(int(_editor_get("font_small")), runs) }
   i
}

@returns_owned
fn _timeline_rows() list {
   mut out = []
   def bs = _editor_get("st").get("buffers", [])
   mut bi = bs.len - 1
   while bi >= 0 && out.len < 12 {
      def b = bs.get(bi, {})
      out = out.append(["buffer", to_str(b.get("name", "buffer")), to_str(b.get("path", b.get("source_path", "")))])
      bi -= 1
   }
   def hist_rows = interact.history(_editor_get("ui_state"))
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
   def scroll = project.clamp_scroll(int(_editor_get("st").get("timeline_scroll", 0)), rows.len, visible)
   _editor_get("st")["timeline_scroll"] = scroll
   mut runs = []
   mut i = 0
   while i < visible && scroll + i < rows.len {
      def yy = project.row_y(y, i)
      def row = rows.get(scroll + i, [])
      def kind = to_str(row.get(0, ""))
      def label = to_str(row.get(1, ""))
      def detail = to_str(row.get(2, ""))
      def baseline_y = _row_baseline_y(int(_editor_get("font_small")), yy, project.TREE_ROW_H)
      def text_y = _row_text_y_from_baseline(int(_editor_get("font_small")), baseline_y, project.TREE_ROW_H)
      _draw_icon(kind == "buffer" ? "text_file" : "info", x + 10.0, _center_y(yy, project.TREE_ROW_H, 16.0), 16.0, kind == "buffer" ? C_ACCENT_2 : C_MUTED)
      def int font = int(_editor_get("font_small"))
      def tail = _fit_chrome_text(font, detail, max(0.0, w * 0.32 - 8.0))
      def f64 tail_w = _measure_ui(font, tail, 0.0)
      runs = _append_text_run(runs, _fit_chrome_text(font, label, w - 50.0 - tail_w), x + 34.0, text_y, kind == "buffer" ? U_TEXT : U_MUTED)
      if tail.len > 0 { runs = _append_right_run(runs, font, tail, x + w - 8.0, text_y, U_DIM) }
      i += 1
   }
   if rows.len == 0 { runs = _append_text_run(runs, "no timeline yet", x + 12.0, y + 36.0, U_MUTED) }
   _flush_rects()
   if runs.len > 0 { gfx.draw_text_runs_flat_colors(int(_editor_get("font_small")), runs) }
   i
}

fn _draw_tree_disclosure(bool open, f64 x, f64 y, any tint) int {
   _draw_icon(open ? "collapse" : "chevron", x, y, 12.0, tint)
}

fn _draw_project_tree(f64 x, f64 y, f64 w, f64 h) int {
   _ensure_project_tree()
   def counts = project.counts(_editor_get("project_state"))
   def dirty = int(counts.get("modified", 0)) + int(counts.get("added", 0)) + int(counts.get("deleted", 0)) + int(counts.get("untracked", 0))
   def branch = project.branch(_editor_get("project_state"))
   def right = (branch.len > 0 ? branch : "cwd") + (dirty > 0 ? " +" + to_str(dirty) : "")
   _draw_section_title("tree", ospath.basename(project.root(_editor_get("project_state"))), x, y, w, right)
   def rows = project.tree(_editor_get("project_state"))
   def visible = project.visible_count(h)
   def scroll = project.clamp_scroll(int(_editor_get("st").get("tree_scroll", 0)), rows.len, visible)
   _editor_get("st")["tree_scroll"] = scroll
   def active_path = _buffer_source_path(ed.current_buffer(_editor_get("st")))
   def active_norm = active_path.len > 0 ? ospath.normalize(active_path) : ""
   mut runs = []
   mut i = 0
   while i < visible && scroll + i < rows.len {
      def f64 row_h = float(project.TREE_ROW_H)
      def f64 yy = float(project.row_y(y, i))
      if yy + row_h > y + h { break }
      def e = rows.get(scroll + i, {})
      def depth = int(e.get("depth", 0))
      def path = to_str(e.get("path", ""))
      def active = path.len > 0 && active_norm.len > 0 && (path == active_path || path == active_norm)
      def selected = _project_selection_contains(e)
      if selected { _fill_rect(x + 4.0, yy, w - 8.0, row_h, gfx.color_alpha(C_ACCENT, active ? 0.25 : 0.14)) }
      elif active { _fill_rect(x + 4.0, yy, w - 8.0, row_h, gfx.color_alpha(C_ACCENT, 0.18)) }
      mut f64 ix = x + 8.0 + float(min(float(depth) * 12.0, max(0.0, w - 86.0)))
      def arrow_slot = 16.0
      if e.get("dir", false) {
         _draw_tree_disclosure(bool(e.get("open", false)), ix + 1.0, _center_y(yy, row_h, 12.0), selected ? C_ACCENT : C_MUTED)
      }
      ix += arrow_slot
      def file_x = ix
      def text_x = file_x + 24.0
      def baseline_y = _row_baseline_y(int(_editor_get("font_small")), yy, row_h)
      def text_y = _row_text_y_from_baseline(int(_editor_get("font_small")), baseline_y, row_h)
      _draw_icon(_editor_file_icon(e), file_x, _center_y(yy, row_h, 16.0), 16.0, (active || selected) ? C_ACCENT : C_MUTED)
      runs = _append_text_run(runs, _fit_chrome_text(int(_editor_get("font_small")), to_str(e.get("name", "")), x + w - text_x - 30.0), text_x, text_y, (active || selected) ? U_TEXT : U_MUTED)
      runs = _queue_git_badge(runs, to_str(e.get("git", "")), x + w - 8.0, yy)
      i += 1
   }
   if rows.len > visible && visible > 0 {
      def f64 track_y = y + float(project.TREE_HEADER_H)
      def f64 track_h = h - float(project.TREE_HEADER_H)
      def f64 thumb_h = float(max(20.0, track_h * float(visible) / float(rows.len)))
      def f64 thumb_y = track_y + (track_h - thumb_h) * float(scroll) / float(max(1, rows.len - visible))
      _fill_rect(x + w - 4.0, track_y, 2.0, track_h, gfx.color_alpha(C_LINE, 0.65))
      _fill_rect(x + w - 5.0, thumb_y, 4.0, thumb_h, gfx.color_alpha(C_ACCENT, 0.55))
   }
   _flush_rects()
   if runs.len > 0 { gfx.draw_text_runs_flat_colors(int(_editor_get("font_small")), runs) }
   i
}

fn _draw_outline(f64 x, f64 y, f64 w, f64 h) int {
   def syms = _outline_symbols()
   _draw_section_title("tree", "OUTLINE", x, y, w, to_str(syms.len))
   def visible = _outline_visible(h)
   def scroll = _outline_clamp(int(_editor_get("st").get("outline_scroll", 0)), syms.len, visible)
   _editor_get("st")["outline_scroll"] = scroll
   mut runs = []
   mut i = 0
   while i < visible && scroll + i < syms.len {
      def f64 row_h = float(outline.ROW_H)
      def f64 yy = float(outline.row_y(y, i))
      if yy + row_h > y + h { break }
      def s = syms.get(scroll + i, {})
      def line = int(s.get("line", 0))
      def active = line == int(_editor_get("st").get("cursor_line", 0))
      if active { _fill_rect(x + 4.0, yy, w - 8.0, row_h, gfx.color_alpha(C_ACCENT, 0.16)) }
      def text_y = _row_text_y(int(_editor_get("font_small")), yy, row_h)
      _draw_icon(_editor_symbol_icon(to_str(s.get("kind", ""))), x + 10.0, _center_y(yy, row_h, 15.0), 15.0, active ? C_ACCENT : C_MUTED)
      runs = _append_text_run(runs, _fit_chrome_text(int(_editor_get("font_small")), to_str(s.get("name", "")), w - 48.0 - _measure_ui(int(_editor_get("font_small")), to_str(line + 1), 0.0)), x + 32.0, text_y, active ? U_TEXT : U_MUTED)
      runs = _append_right_run(runs, int(_editor_get("font_small")), to_str(line + 1), x + w - 8.0, text_y, U_MUTED)
      i += 1
   }
   if syms.len > visible && visible > 0 {
      def f64 track_y = y + float(SECTION_H)
      def f64 track_h = h - float(SECTION_H)
      def f64 thumb_h = float(max(20.0, track_h * float(visible) / float(syms.len)))
      def f64 thumb_y = track_y + (track_h - thumb_h) * float(scroll) / float(max(1, syms.len - visible))
      _fill_rect(x + w - 4.0, track_y, 2.0, track_h, gfx.color_alpha(C_LINE, 0.65))
      _fill_rect(x + w - 5.0, thumb_y, 4.0, thumb_h, gfx.color_alpha(C_ACCENT, 0.55))
   }
   if syms.len == 0 { runs = _append_text_run(runs, "no symbols", x + 12.0, y + 34.0, U_MUTED) }
   _flush_rects()
   if runs.len > 0 { gfx.draw_text_runs_flat_colors(int(_editor_get("font_small")), runs) }
   0
}

fn _draw_rail(dict lay) int {
   def x = float(lay.get("rail_x", 0.0))
   def y = float(lay.get("rail_y", 0.0))
   def w = float(lay.get("rail_w", 0.0))
   def h = float(lay.get("rail_h", 0.0))
   if w <= 1.0 || !bool(_editor_get("st").get("show_hierarchy", true)) { return 0 }
   if MOCK_BENCH_FAST && int(_editor_get("frame_count")) >= BENCH_WARMUP_FRAMES { return 0 }
   _fill_rect(x, y, w, h, C_RAIL)
   _stroke_rect(x, y, w, h, to_str(_editor_get("st").get("focus", "editor")) == "project" ? C_ACCENT : C_LINE, to_str(_editor_get("st").get("focus", "editor")) == "project" ? 2.0 : 1.0)
   _draw_rail_tabs(x, y, w)
   def body_y = y + RAIL_TABS_H
   def body_h = h - RAIL_TABS_H
   def tab = _rail_tab()
   if tab == "git" { _draw_git_panel(x, body_y, w, body_h) return 0 }
   if tab == "timeline" { _draw_timeline_panel(x, body_y, w, body_h) return 0 }
   def show_project = bool(_editor_get("st").get("show_project", true))
   def show_outline = bool(_editor_get("st").get("show_outline", true))
   if !show_project && !show_outline {
      _draw_section_title("box", "PANES", x, body_y, w, "hidden")
      _text(int(_editor_get("font_small")), "C-SPC t p project  C-SPC t o outline", x + 12.0, body_y + 36.0, C_MUTED)
      return 0
   }
   if show_project { _ensure_project_tree() }
   def rows = project.tree(_editor_get("project_state"))
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

fn _fold_collapsed_end(int start) int {
   if !_editor_get("fold_collapsed").contains(to_str(start)) { return -1 }
   for r in _editor_get("fold_ranges") {
      if int(r.get("start", -1)) == start { return int(r.get("end", start)) }
   }
   -1
}

fn _fold_hidden_start(int line) int {
   for r in _editor_get("fold_ranges") {
      def int start = int(r.get("start", -1))
      def int end = int(r.get("end", -1))
      if _editor_get("fold_collapsed").contains(to_str(start)) && line > start && line <= end { return start }
   }
   -1
}

fn _fold_visible_indices(list lines) list {
   mut out = []
   mut int i = 0
   while i < lines.len {
      out = out.append(i)
      def int end = _fold_collapsed_end(i)
      i = end > i ? end + 1 : i + 1
   }
   out
}

fn _fold_visible_index(list view, int doc_line) int {
   mut int i = 0
   while i < view.len {
      if int(view.get(i, -1)) == doc_line { return i }
      if int(view.get(i, -1)) > doc_line { return -1 }
      i += 1
   }
   -1
}

fn _fold_first_visible_at_or_after(list view, int doc_line) int {
   mut int i = 0
   while i < view.len {
      if int(view.get(i, 0)) >= doc_line { return i }
      i += 1
   }
   view.len
}

fn _fold_range_at(int line) dict {
   mut best = dict(0)
   for r in _editor_get("fold_ranges") {
      def int start = int(r.get("start", -1))
      def int end = int(r.get("end", -1))
      if line >= start && line <= end {
         if best.len == 0 || end - start < int(best.get("end", 0)) - int(best.get("start", 0)) { best = r }
      }
   }
   best
}

fn _fold_apply(int start, bool collapse) int {
   def key = to_str(start)
   if collapse {
      _editor_get("fold_collapsed")[key] = true
   } else {
      mut dict next = dict(0)
      for k in _editor_get("fold_collapsed").keys() { if to_str(k) != key { next[k] = true } }
      _editor_values["fold_collapsed"] = next
   }
   def int cursor = int(_editor_get("st").get("cursor_line", 0))
   if collapse && cursor > start { _editor_get("st")["cursor_line"] = start _editor_get("st")["cursor_col"] = 0 }
   _editor_values["st"] = ed.clamp_cursor(_editor_get("st"))
   _follow_cursor()
   _editor_values["visible_runs_cache_key"] = ""
   _request_full_redraw(2)
   0
}

fn _fold_toggle_at_cursor() int {
   def r = _fold_range_at(int(_editor_get("st").get("cursor_line", 0)))
   if r.len == 0 { _status("no fold here") return 0 }
   def int start = int(r.get("start", 0))
   def bool collapsed = _editor_get("fold_collapsed").contains(to_str(start))
   _fold_apply(start, !collapsed)
   _status(collapsed ? "fold opened" : "fold closed")
}

fn _fold_close_at_cursor() int {
   def r = _fold_range_at(int(_editor_get("st").get("cursor_line", 0)))
   if r.len == 0 { _status("no fold here") return 0 }
   _fold_apply(int(r.get("start", 0)), true)
   _status("fold closed")
}

fn _fold_open_all() int {
   _editor_values["fold_collapsed"] = dict(0)
   _editor_values["visible_runs_cache_key"] = ""
   _request_full_redraw(2)
   _status("folds opened")
}

fn _fold_rect_click(f64 x, f64 y) bool {
   def rects = _editor_get("fold_rects")
   mut int i = 0
   while i < rects.len {
      def r = rects.get(i, [])
      if is_list(r) && r.len >= 5 && widgets.hit(x, y, float(r.get(0)), float(r.get(1)), float(r.get(2)), float(r.get(3))) {
         def int start = int(r.get(4, -1))
         _fold_apply(start, !_editor_get("fold_collapsed").contains(to_str(start)))
         return true
      }
      i += 1
   }
   false
}

fn _sync_scroll(int rows, int total, list view=[]) int {
   def cursor = int(_editor_get("st").get("cursor_line", 0))
   mut scroll = int(_editor_get("st").get("scroll", 0))
   if view.len <= 0 {
      def max_scroll = max(0, total - rows)
      if bool(_editor_get("st").get("scroll_follow", true)) {
         if cursor < scroll { scroll = cursor }
         if cursor >= scroll + rows { scroll = cursor - rows + 1 }
      }
      _editor_get("st")["scroll"] = min(max_scroll, max(0, scroll))
      return 0
   }
   mut int ci = _fold_visible_index(view, cursor)
   if ci < 0 {
      def int hidden = _fold_hidden_start(cursor)
      if hidden >= 0 { _editor_get("st")["cursor_line"] = hidden _editor_get("st")["cursor_col"] = 0 ci = _fold_visible_index(view, hidden) }
   }
   if ci < 0 { ci = 0 }
   mut int start = _fold_first_visible_at_or_after(view, scroll)
   def max_start = max(0, view.len - rows)
   if bool(_editor_get("st").get("scroll_follow", true)) {
      if ci < start { start = ci }
      if ci >= start + rows { start = ci - rows + 1 }
   }
   start = min(max_start, max(0, start))
   _editor_get("st")["scroll"] = int(view.get(start, 0))
   0
}

fn _code_lenses_for_line(int li) list {
   if _current_path().len == 0 || _current_path() != to_str(_editor_values.get("code_lens_path", "")) { return [] }
   _editor_values.get("code_lens_lines", dict(0)).get(to_str(li), [])
}

fn _queue_code_lenses(list runs, dict lay, list view, int start, int rows) list {
   _editor_values["code_lens_rects"] = []
   def path = _current_path()
   if path.len == 0 || path != to_str(_editor_values.get("code_lens_path", "")) { return runs }
   def int sf = int(_editor_get("font_small"))
   def f64 top = float(lay.get("edit_y", 0.0))
   def f64 text_x = _line_text_x(lay)
   def f64 right_edge = float(lay.get("edit_x", 0.0)) + float(lay.get("edit_w", 0.0)) - 20.0
   def lines = ed.current_lines(_editor_get("st"))
   def draw_limit = _line_limit(lay)
   mut int i = 0
   while i < rows && start + i < view.len {
      def int li = int(view.get(start + i, -1))
      if li < 0 { break }
      def lenses = _code_lenses_for_line(li)
      if lenses.len > 0 {
         def yy = _editor_row_y(top, i)
         def line = _draw_line_text(to_str(lines.get(li, "")), draw_limit)
         mut f64 x = text_x + _visual_prefix(line, li, str.utf8_len(line)) + 12.0
         for lens in lenses {
            def title = to_str(lens.get("title", ""))
            def label = _fit_chrome_text(sf, title, max(0.0, right_edge - x - 10.0))
            if label.len <= 0 { break }
            def f64 ww = _measure_ui(sf, label, 0.0) + 10.0
            if x + ww > right_edge { break }
            _fill_rect(x, yy + 2.0, ww, float(ed.LINE_H) - 4.0, gfx.color_alpha(C_CHIP, 0.48))
            _stroke_rect(x, yy + 2.0, ww, float(ed.LINE_H) - 4.0, gfx.color_alpha(C_CYAN, 0.45), 1.0)
            runs = _append_text_run(runs, label, x + 5.0, _row_text_y(sf, yy, ed.LINE_H), U_CYAN)
            _editor_values["code_lens_rects"] = _editor_get("code_lens_rects").append([x, yy + 1.0, ww, float(ed.LINE_H) - 2.0, lens])
            x += ww + 6.0
         }
      }
      i += 1
   }
   runs
}

@effects(all)
fn _git_inline_refresh() int {
   def path = _current_path()
   if path.len == 0 || !file_exists(path) { _status("save file before git blame") return 0 }
   def root = project.root(_editor_get("project_state"))
   if root.len == 0 { root = ospath.dirname(path) }
   def int cursor = int(_editor_get("st").get("cursor_line", 0))
   def int lo = max(1, cursor + 1 - 40)
   def cap = run_capture(["git", "-C", root, "blame", "--line-porcelain", "-L", to_str(lo) + ",+160", "--", path], [], nil, false)
   if int(cap.get("code", 1)) != 0 {
      print("[git-inline] blame failed path=" + path + " root=" + root + " code=" + to_str(cap.get("code", -1)) + " err=" + to_str(cap.get("stderr", "")))
      _status("git blame unavailable")
      return 0
   }
   mut dict rows = dict(128)
   mut str author = ""
   mut str summary = ""
   mut int current_line = 0
   def blame_out = to_str(cap.get("stdout", ""))
   mut int bpos = 0
   while bpos <= blame_out.len {
      mut int bend = bpos
      while bend < blame_out.len && int(load8(blame_out, bend)) != 10 { bend += 1 }
      def t = str.str_slice(blame_out, bpos, bend)
      if t.len > 0 {
         if int(load8(t, 0)) == 9 {
            rows[to_str(current_line)] = {"author": author, "summary": summary}
         } else {
            def s1 = str.find(t, " ")
            if s1 == 40 {
               def rest = str.str_slice(t, s1 + 1, t.len)
               def s2 = str.find(rest, " ")
               if s2 > 0 {
                  def rest2 = str.str_slice(rest, s2 + 1, rest.len)
                  def s3 = str.find(rest2, " ")
                  if s3 > 0 {
                     def int n = int(str.str_slice(rest2, 0, s3))
                     if n > 0 { current_line = n - 1 }
                  }
               }
            }
            if str.startswith(t, "author ") { author = str.strip(str.str_slice(t, 7, t.len)) }
            elif str.startswith(t, "summary ") { summary = str.strip(str.str_slice(t, 8, t.len)) }
         }
      }
      if bend >= blame_out.len { break }
      bpos = bend + 1
   }
   if rows.len == 0 {
      print("[git-inline] no blame rows path=" + path + " root=" + root)
      _status("no blame for this file")
      return 0
   }
   def diff = run_capture(["git", "-C", root, "diff", "--unified=0", "--no-color", "--", path], [], nil, false)
   mut dict hunks = dict(32)
   def diff_out = to_str(diff.get("stdout", ""))
   mut int dpos = 0
   while dpos <= diff_out.len {
      mut int dend = dpos
      while dend < diff_out.len && int(load8(diff_out, dend)) != 10 { dend += 1 }
      def t = str.strip(str.str_slice(diff_out, dpos, dend))
      if str.startswith(t, "@@") {
         mut int old_count = 0
         mut int new_start = 0
         mut int new_count = 0
         def minus = str.find(t, "-")
         if minus >= 0 {
            def after = str.str_slice(t, minus + 1, t.len)
            def sp = str.find(after, " ")
            def tok = sp >= 0 ? str.str_slice(after, 0, sp) : after
            def comma = str.find(tok, ",")
            old_count = comma >= 0 ? int(str.str_slice(tok, comma + 1, tok.len)) : 1
         }
         def plus = str.find(t, "+")
         if plus >= 0 {
            def after = str.str_slice(t, plus + 1, t.len)
            def sp = str.find(after, " ")
            def tok = sp >= 0 ? str.str_slice(after, 0, sp) : after
            def comma = str.find(tok, ",")
            new_start = int(comma >= 0 ? str.str_slice(tok, 0, comma) : tok)
            new_count = comma >= 0 ? int(str.str_slice(tok, comma + 1, tok.len)) : 1
         }
         mut int i = 0
         while i < new_count { hunks[to_str(new_start + i)] = old_count > 0 ? "M" : "A" i += 1 }
         if new_count == 0 && old_count > 0 { hunks[to_str(new_start)] = "D" }
      }
      if dend >= diff_out.len { break }
      dpos = dend + 1
   }
   _editor_values["blame_on"] = true
   _editor_values["blame_path"] = path
   _editor_values["blame_lines"] = rows
   _editor_values["blame_hunks"] = hunks
   _request_full_redraw(2)
   0
}

fn _git_inline_toggle() int {
   if bool(_editor_values.get("blame_on", false)) {
      _editor_values["blame_on"] = false
      _editor_values["blame_path"] = ""
      _editor_values["blame_lines"] = dict(0)
      _editor_values["blame_hunks"] = dict(0)
      _editor_values["git_inline_runs"] = []
      _status("git blame off")
      return 0
   }
   _git_inline_refresh()
   if bool(_editor_values.get("blame_on", false)) { _status("git blame on") }
   0
}

fn _queue_git_inline(dict lay, int rows) list {
   _editor_values["git_inline_runs"] = []
   _editor_values["git_hunk_bars"] = 0
   if !bool(_editor_get("blame_on")) || to_str(_editor_values.get("blame_path", "")) != _current_path() { return [] }
   def lines = ed.current_lines(_editor_get("st"))
   def view = _fold_visible_indices(lines)
   def int scroll = int(_editor_get("st").get("scroll", 0))
   mut int start = 0
   while start < view.len && int(view.get(start, 0)) < scroll { start += 1 }
   def f64 top = float(lay.get("edit_y", 0.0))
   def f64 gx = float(lay.get("edit_x", 0.0)) + 1.0
   mut int bars = 0
   mut runs = []
   def hunks = _editor_get("blame_hunks")
   mut int si = 0
   while si < rows && start + si < view.len {
      def code = to_str(hunks.get(to_str(int(view.get(start + si, -1))), ""))
      if code.len > 0 {
         def color = code == "A" ? C_OK : (code == "M" ? C_WARN : C_DIAG_ERROR)
         _fill_rect(gx, _editor_row_y(top, si) + 2.0, 2.0, float(ed.LINE_H) - 4.0, color)
         bars += 1
      }
      si += 1
   }
   def int cursor = int(_editor_get("st").get("cursor_line", 0))
   def int sr = _fold_visible_index(view, cursor) - start
   if sr >= 0 && sr < rows {
      def info = _editor_get("blame_lines").get(to_str(cursor), dict(0))
      if info.len > 0 {
         def author = to_str(info.get("author", ""))
         def summary = to_str(info.get("summary", ""))
         def label = author.len > 0 ? author + (summary.len > 0 ? " · " + summary : "") : "uncommitted"
         def int sf = int(_editor_get("font_small"))
         def fitted = _fit_chrome_text(sf, label, max(80.0, float(lay.get("edit_w", 0.0)) * 0.48))
         if fitted.len > 0 {
            def f64 lw = _measure_ui(sf, fitted, 0.0) + 12.0
            def f64 bx = float(lay.get("edit_x", 0.0)) + float(lay.get("edit_w", 0.0)) - lw - 14.0
            def f64 yy = _editor_row_y(top, sr)
            _fill_rect(bx, yy + 2.0, lw, float(ed.LINE_H) - 4.0, gfx.color_alpha(C_PANE_2, 0.92))
            _stroke_rect(bx, yy + 2.0, lw, float(ed.LINE_H) - 4.0, gfx.color_alpha(C_LINE_2, 0.75), 1.0)
            runs = _append_text_run(runs, fitted, bx + 6.0, _row_text_y(sf, yy, ed.LINE_H), U_DIM)
         }
      }
   }
   _editor_values["git_inline_runs"] = runs
   _editor_values["git_hunk_bars"] = bars
   runs
}

fn _queue_fold_markers(dict lay, list view, int start, int rows) int {
   _editor_values["fold_rects"] = []
   def ranges = _editor_get("fold_ranges")
   if ranges.len <= 0 { return 0 }
   def f64 top = float(lay.get("edit_y", 0.0))
   def f64 gx = _line_text_x(lay) - 14.0
   mut int i = 0
   while i < rows && start + i < view.len {
      def int doc = int(view.get(start + i, -1))
      for r in ranges {
         if int(r.get("start", -1)) != doc { continue }
         def f64 yy = _editor_row_y(top, i)
         def bool collapsed = _editor_get("fold_collapsed").contains(to_str(doc))
         _fill_rect(gx - 4.0, yy + 2.0, 14.0, float(ed.LINE_H) - 4.0, gfx.color_alpha(C_PANE_2, 0.72))
         _fill_rect(gx, yy + float(ed.LINE_H) * 0.5 - 1.0, 8.0, 2.0, C_MUTED)
         if collapsed { _fill_rect(gx + 3.0, yy + float(ed.LINE_H) * 0.5 - 4.0, 2.0, 8.0, C_MUTED) }
         _editor_values["fold_rects"] = _editor_get("fold_rects").append([gx - 6.0, yy + 1.0, 18.0, float(ed.LINE_H) - 2.0, doc])
      }
      i += 1
   }
   0
}

fn _draw_editor(dict lay, bool caret) int {
   def x = float(lay.get("edit_x", 0.0))
   def y = float(lay.get("edit_y", 0.0))
   def w = float(lay.get("edit_w", 0.0))
   def h = float(lay.get("edit_h", 0.0))
   def rows = ed.visible_rows(h)
   def cur_buf = ed.current_buffer(_editor_get("st"))
   def lines = ed.current_lines(_editor_get("st"))
   def view = _fold_visible_indices(lines)
   _sync_scroll(rows, lines.len, view)
   _editor_values["code_lens_rects"] = []
   _editor_values["fold_rects"] = []
   _find_refresh_if_needed()
   _fill_rect(x, y, w, h, C_PANE)
   _stroke_rect(x, y, w, h, C_LINE, 1.0)
   def div_x = float(lay.get("divider_x", x - 9.0))
   _fill_rect(div_x - 1.0, y, 2.0, h, _editor_get("st").get("drag_divider", false) ? C_ACCENT : C_LINE)
   if _draw_image_preview(lay, cur_buf) { return 0 }
   def text_x = _line_text_x(lay)
   def filename = to_str(cur_buf.get("name", ""))
   def scroll = int(_editor_get("st").get("scroll", 0))
   def int start_vis = _fold_first_visible_at_or_after(view, scroll)
   mut line_runs = []
   mut text_runs = []
   def show_numbers = bool(_editor_get("st").get("show_line_numbers", true))
   def show_guides = bool(_editor_get("st").get("show_indent_guides", false))
   def draw_limit = _line_limit(lay)
   def big_file = _buffer_is_big(lines)
   def selection_ranges = _selection_ranges_active()
   def cursor_row = int(_editor_get("st").get("cursor_line", 0))
   def int cursor_screen = _fold_visible_index(view, cursor_row) - start_vis
   if cursor_screen >= 0 && cursor_screen < rows {
      def cy0 = _editor_row_y(y, cursor_screen)
      _fill_rect(x + 1.0, cy0, w - 2.0, ed.LINE_H, gfx.color_alpha(C_ACCENT, 0.10))
   }
   if show_guides || selection_ranges.len > 0 || view.len != lines.len {
      mut i = 0
      while i < rows && start_vis + i < view.len {
         def li = int(view.get(start_vis + i, -1))
         if li < 0 { break }
         def yy = _editor_row_y(y, i)
         def baseline_y = _editor_row_baseline_y(yy)
         def text_y = _row_text_y_from_baseline(int(_editor_get("font_body")), baseline_y, ed.LINE_H)
         def num_y = _row_text_y_from_baseline(int(_editor_get("font_small")), baseline_y, ed.LINE_H)
         def line = to_str(lines.get(li, ""))
         def draw_line = _draw_line_text(line, draw_limit)
         if show_guides {
            mut g = 0
            while g < draw_line.len && load8(draw_line, g) == 32 {
               if g > 0 && g % 3 == 0 { _fill_rect(text_x + float(g) * float(_editor_get("FONT_BODY_ADV")), yy + 1.0, 1.0, float(ed.LINE_H) - 2.0, gfx.color_alpha(C_LINE_2, 0.58)) }
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
            line_runs = line_runs.append(num_y)
         }
         text_runs = _append_highlight_runs(text_runs, filename, draw_line, li, text_x, text_y, _highlight_on(draw_line))
         i += 1
      }
   } else {
      _prepare_visible_runs(lines, filename, scroll, rows, text_x, y, x + 16.0, draw_limit, show_numbers, big_file)
      line_runs = _editor_get("visible_line_runs_cache")
      text_runs = _editor_get("visible_text_runs_cache")
   }
   _draw_find_visible(lines, view, start_vis, rows, text_x, y)
   if !big_file { _draw_color_swatches_visible(lines, view, start_vis, rows, text_x, y, draw_limit) }
   def lens_runs = _queue_code_lenses([], lay, view, start_vis, rows)
   def blame_runs = _queue_git_inline(lay, rows)
   _queue_fold_markers(lay, view, start_vis, rows)
   _flush_rects()
   if line_runs.len > 0 { gfx.draw_text_runs_flat(int(_editor_get("font_small")), line_runs, C_MUTED) }
   if text_runs.len > 0 { _draw_editor_body_runs(int(_editor_get("font_body")), text_runs) }
   if lens_runs.len > 0 { gfx.draw_text_runs_flat_colors(int(_editor_get("font_small")), lens_runs) }
   if blame_runs.len > 0 { gfx.draw_text_runs_flat_colors(int(_editor_get("font_small")), blame_runs) }
   _draw_minimap(lay, rows, start_vis, view.len)
   if caret && cursor_screen >= 0 && cursor_screen < rows {
      def line = to_str(lines.get(cursor_row, ""))
      def cx = text_x + _visual_prefix(line, cursor_row, int(_editor_get("st").get("cursor_col", 0)))
      def cy = _editor_row_y(y, cursor_screen)
      _fill_rect(cx, cy + 1.0, 2.0, float(ed.LINE_H) - 2.0, C_ACCENT)
   }
   if caret && _extra_cursor_count() > 0 {
      def extras = _extra_cursors()
      mut ei = 0
      while ei < extras.len {
         def erow = _cursor_row(extras.get(ei))
         def int escreen = _fold_visible_index(view, erow) - start_vis
         if escreen >= 0 && escreen < rows {
            def eline = to_str(lines.get(erow, ""))
            def ecol = min(max(_cursor_col(extras.get(ei)), 0), eline.len)
            def ecx = text_x + _visual_prefix(eline, erow, ecol)
            def ecy = _editor_row_y(y, escreen)
            _fill_rect(ecx, ecy + 1.0, 2.0, float(ed.LINE_H) - 2.0, C_ACCENT_2)
         }
         ei += 1
      }
   }
   0
}

fn _draw_terminal(dict lay) int {
   if !termpane.is_open(_editor_get("term_state")) { return 0 }
   def x = float(lay.get("dock_x", 0.0))
   def y = float(lay.get("dock_y", 0.0))
   def w = float(lay.get("dock_w", 0.0))
   def h = float(lay.get("dock_h", 0.0))
   _fill_rect(x, y, w, h, C_RAIL)
   _flush_rects()
   _editor_values["term_state"] = termpane.draw(_editor_get("term_state"))
   _fill_rect(x, y, w, TERM_TAB_H, C_PANE_2)
   mut runs = []
   def int n = int(termpane.tab_count(_editor_get("term_state")))
   def int active = int(termpane.active_tab(_editor_get("term_state")))
   def f64 tw = float(_term_tab_w(w, n))
   mut i = 0
   while i < n {
      def tx = _snap_px(x + 6.0 + float(i) * tw)
      def f64 tab_w = float(min(tw - 2.0, x + w - tx - 40.0))
      if tab_w <= 36.0 { break }
      def on = i == active
      def text_y = _row_text_y(int(_editor_get("font_small")), y + 3.0, TERM_TAB_H - 5.0)
      _fill_rect(tx, y + 3.0, tab_w, TERM_TAB_H - 5.0, on ? C_PANE : C_CHIP)
      _stroke_rect(tx, y + 3.0, tab_w, TERM_TAB_H - 5.0, on ? C_ACCENT : C_LINE, 1.0)
      if on { _fill_rect(tx, y + TERM_TAB_H - 2.0, tab_w, 2.0, C_ACCENT) }
      _draw_icon("terminal", tx + 7.0, _center_y(y + 3.0, TERM_TAB_H - 5.0, 14.0), 14.0)
      runs = _append_text_run(runs, _fit_chrome_text(int(_editor_get("font_small")), termpane.tab_title(_editor_get("term_state"), i), tab_w - 50.0), tx + 27.0, text_y, on ? U_TEXT : U_MUTED)
      _draw_icon("close", tx + tab_w - 18.0, _center_y(y + 3.0, TERM_TAB_H - 5.0, 12.0), 12.0)
      i += 1
   }
   def plus_x = _snap_px(x + 6.0 + float(n) * tw + 3.0)
   if plus_x + 24.0 < x + w - 8.0 {
      _fill_rect(plus_x, y + 3.0, 24.0, TERM_TAB_H - 5.0, C_CHIP)
      _stroke_rect(plus_x, y + 3.0, 24.0, TERM_TAB_H - 5.0, C_LINE, 1.0)
      _draw_icon("add", plus_x + 5.0, _center_y(y + 3.0, TERM_TAB_H - 5.0, 14.0), 14.0)
   }
   def int hint_font = int(_editor_get("font_small"))
   def f64 hint_width = _measure_ui(hint_font, "F6 shell  F7 repl", 0.0)
   if plus_x + 34.0 + hint_width <= x + w - 10.0 {
      runs = _append_right_run(runs, hint_font, "F6 shell  F7 repl", x + w - 10.0, _row_text_y(hint_font, y + 3.0, TERM_TAB_H - 5.0), U_DIM)
   }
   _flush_rects()
   if runs.len > 0 { gfx.draw_text_runs_flat_colors(int(_editor_get("font_small")), runs) }
   _stroke_rect(x, y, w, h, to_str(_editor_get("st").get("focus", "editor")) == "terminal" ? C_ACCENT : C_LINE, to_str(_editor_get("st").get("focus", "editor")) == "terminal" ? 2.0 : 1.0)
   0
}

fn _command_icon(str tag, str id) str {
   if id == "save" || id == "save-all" { return "save" }
   if id == "copy" || id == "copy-path" || id == "copy-relative-path" || id == "copy-line-info" || id == "duplicate-line" { return "copy" }
   if id == "cut" || id == "kill-line" { return "cut" }
   if id == "paste" { return "paste" }
   if id == "delete-file" || id == "delete-line" { return "trash" }
   if id == "new-file" || id == "new-scratch" { return "add" }
   if id == "kill-buffer" || id == "terminal-close" || id == "cancel" || id == "quit" { return "close" }
   if id == "undo" || id == "undo-file-op" || id == "go-back" || id == "prev" || id == "find-prev" { return "back" }
   if id == "redo" || id == "redo-file-op" || id == "go-forward" || id == "next" || id == "find-next" { return "forward" }
   if id == "open-file" || id == "open" { return "folder" }
   if id == "refresh" || id == "reload" { return "refresh" }
   if id == "rename" { return "code_edit" }
   if id == "go-definition" { return "callable" }
   if id == "lsp-hover" { return "info" }
   if id == "lsp-complete" { return "list" }
   if id == "run" { return "play" }
   if id == "check" || id == "lsp-diagnostics" { return "check" }
   if id == "format" { return "format" }
   if id == "diff-clipboard" { return "git_branch" }
   if id == "palette" || tag == "search" { return "search" }
   if tag == "file" { return "file" }
   if tag == "diff" { return "git_branch" }
   if tag == "run" { return "script" }
   if tag == "debug" { return "debug" }
   if tag == "project" { return "tree" }
   if tag == "edit" { return "code_edit" }
   if tag == "buffer" { return "text_file" }
   if tag == "pane" { return "box" }
   if tag == "terminal" { return "terminal" }
   if tag == "lsp" { return "globe" }
   if tag == "toggle" { return "check" }
   "info"
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
   def f64 bw = float(max(24.0, _measure_ui(int(_editor_get("font_small")), label, 0.0) + 10.0))
   _fill_rect(x, y, bw, 17.0, gfx.color_alpha(C_CHIP, 0.50))
   _stroke_rect(x, y, bw, 17.0, gfx.color_alpha(col, 0.34), 1.0)
   _append_center_run(runs, int(_editor_get("font_small")), label, x + bw * 0.5, y + 3.0, _pack(col))
}

fn _draw_find_bar(f64 sw, f64 sh) int {
   ;; Typing stays with the find module (it owns the query, the replacement and
   ;; the key bindings); the bar's options and actions are toolkit widgets.
   if !find.is_open(_editor_get("find_state")) { return 0 }
   _find_refresh_if_needed()
   def fs = _editor_get("find_state")
   def replace_on = find.replace_on(fs)
   def f64 bar_h = replace_on ? 102.0 : 84.0
   def f64 w0 = float(min(780.0, max(280.0, sw - 34.0)))
   def fit = _fit_rect(sw * 0.5 - w0 * 0.5, sh - bar_h - float(ed.STATUS_H) - 10.0, w0, bar_h, sw, sh, 8.0)
   def f64 w = float(fit.get(2, w0))
   def slide = gui.overlay_offset("ed_find", true)
   _gui_overlay_window("ed_find", float(fit.get(0, 8.0)), float(fit.get(1, 8.0)) + slide, w, float(fit.get(3, bar_h)))
   def q = find.query(fs)
   def repl = find.replacement(fs)
   def field = find.active_field(fs)
   def bool err = find.error(fs).len > 0
   gui.layout_row([w - 230.0, 190.0], 8.0)
   gui.text_colored((field == "find" ? "> " : "  ") + (q.len > 0 ? q : "find in buffer"), field == "find" ? "accent" : (q.len > 0 ? "text" : "text_dim"))
   gui.text_colored(find.summary(fs), err ? "warn" : "text_dim")
   if replace_on {
      gui.text_colored((field == "replace" ? "> " : "  ") + (repl.len > 0 ? repl : "replace with"), field == "replace" ? "accent" : (repl.len > 0 ? "text" : "text_dim"))
   }
   ;; Options are chips: highlighted while on, a click flips them.
   gui.layout_row([96.0, 86.0, 86.0, 112.0, 64.0, 64.0, 64.0], 6.0)
   mut str act = ""
   if gui.selectable("f_regex", "Regex", find.regex_on(fs), 96.0, 24.0, "", -1, false) { act = "regex" }
   if gui.selectable("f_case", "Case", find.case_on(fs), 86.0, 24.0, "", -1, false) { act = "case" }
   _probe_note_rect("f_case")
   if gui.selectable("f_word", "Word", find.word_on(fs), 86.0, 24.0, "", -1, false) { act = "word" }
   if gui.selectable("f_repl", "Replace", replace_on, 112.0, 24.0, "", -1, false) { act = "replace" }
   if gui.small_button("f_prev", "Prev") { act = "prev" }
   if gui.small_button("f_next", "Next") { act = "next" }
   if replace_on {
      if gui.small_button("f_all", "All") { act = "all" }
   } else {
      if gui.small_button("f_close", "Close") { act = "close" }
   }
   gui.end_window()
   if act == "prev" { _find_step(-1) }
   elif act == "next" { _find_step(1) }
   elif act == "all" { _find_replace_all() }
   elif act == "close" { _find_close() }
   elif act.len > 0 { _find_toggle(act) }
   0
}

fn _draw_palette(f64 sw, f64 sh) int {
   ;; The palette's query and keyboard selection live in the palette module;
   ;; the rows, hover and clicks are toolkit selectables.
   if !pal.is_open(_editor_get("palette_state")) { return 0 }
   def matches = _palette_matches()
   def rect = _palette_rect(sw, sh, matches.len)
   def x = float(rect.get(0, 0.0))
   def y = float(rect.get(1, 0.0))
   def w = float(rect.get(2, 0.0))
   def h = float(rect.get(3, 0.0))
   def rows = int(rect.get(4, 0))
   _editor_values["palette_state"] = pal.set_visible(_editor_get("palette_state"), rows)
   def ps = _editor_get("palette_state")
   def start = pal.scroll(ps)
   def int sel_idx = pal.index(ps)
   _gui_overlay_window("ed_palette", x, y, w, h)
   def q = pal.query(ps)
   def searching = pal.items_mode(ps) == "find-buffers"
   def pos_label = matches.len > 0 ? to_str(min(matches.len, sel_idx + 1)) + "/" + to_str(matches.len) : "0/0"
   gui.layout_row([w - 110.0, 70.0], 8.0)
   gui.text_colored("> " + (q.len > 0 ? q : (searching ? "search open buffers" : "type a command")), q.len > 0 ? "text" : "text_dim")
   gui.text_colored(pos_label, "text_dim")
   gui.separator()
   mut int chosen = -1
   mut i = 0
   while i < rows && start + i < matches.len {
      def row = matches.get(start + i)
      def actual = start + i
      def shortcut = to_str(row.get(2, ""))
      def label = to_str(row.get(0, ""))
      def int tex = _editor_icon_tex(_command_icon(to_str(row.get(4, "")), to_str(row.get(1, ""))))
      if gui.selectable("pal_" + to_str(actual), label + (shortcut.len > 0 ? "   " + shortcut : ""), actual == sel_idx, w - 28.0, float(PALETTE_ROW_H) - 4.0, "", tex, false) { chosen = actual }
      _probe_note_rect("pal_" + to_str(i))
      i += 1
   }
   if matches.len == 0 && (!searching || q.len > 0) { gui.text_colored(searching ? "No matches" : "No commands", "warn") }
   if matches.len > 0 {
      def sel = matches.get(min(max(sel_idx, 0), matches.len - 1), [])
      gui.separator()
      gui.layout_row([w - 230.0, 190.0], 8.0)
      def detail = searching ? to_str(sel.get(3, "")) : to_str(sel.get(1, "")) + "  " + to_str(sel.get(4, "")) + "   " + to_str(sel.get(3, ""))
      gui.text_colored(widgets.preview(detail, int((w - 230.0) / 9.0)), "accent")
      gui.text_colored(searching ? "Enter jump  Tab next" : "Enter run  Tab next", "text_dim")
   }
   gui.end_window()
   if chosen >= 0 {
      _editor_values["palette_state"] = pal.set_index(_editor_get("palette_state"), chosen)
      _palette_choose()
   }
   0
}

fn _completion_color(str kind) vec4 {
   if kind == "keyword" { return C_KEYWORD }
   if kind == "lsp" { return C_CYAN }
   C_ACCENT
}

fn _draw_completion(dict lay, f64 sw, f64 sh) int {
   if !bool(_editor_get("completion_state").get("open", false)) { return 0 }
   def items = _editor_get("completion_state").get("items", [])
   if items.len <= 0 { return 0 }
   def lines = ed.current_lines(_editor_get("st"))
   def row = int(_editor_get("st").get("cursor_line", 0))
   def scroll = int(_editor_get("st").get("scroll", 0))
   def line = to_str(lines.get(min(max(row, 0), max(0, lines.len - 1)), ""))
   def visible_row = row - scroll
   def base_x = _line_text_x(lay) + _visual_prefix(line, row, int(_editor_get("st").get("cursor_col", 0)))
   def f64 base_y = float(_editor_row_y(float(lay.get("edit_y", 0.0)), max(0, visible_row))) + float(ed.LINE_H) + 4.0
   def int rows = int(min(items.len, 9))
   def int sel = int(_editor_get("completion_state").get("index", 0))
   mut int top = int(_editor_get("completion_state").get("top", 0))
   if sel < top { top = sel }
   if sel >= top + rows { top = sel - rows + 1 }
   top = int(max(0, min(top, items.len - rows)))
   _editor_get("completion_state")["top"] = top
   def f64 w0 = float(min(420.0, max(260.0, sw * 0.42)))
   def f64 h0 = 34.0 + float(rows) * 26.0
   def fit = _fit_rect(base_x, base_y, w0, h0, sw, sh - float(ed.STATUS_H), 8.0)
   def x = float(fit.get(0, 8.0))
   def y = float(fit.get(1, 8.0))
   def w = float(fit.get(2, w0))
   def h = float(fit.get(3, h0))
   def prefix = to_str(_editor_get("completion_state").get("prefix", ""))
   _fill_rect(x, y, w, h, gfx.color_alpha(C_RAIL, 0.98))
   _stroke_rect(x, y, w, h, C_ACCENT, 1.0)
   _draw_icon("search", x + 12.0, _center_y(y, 30.0, 16.0), 16.0, C_ACCENT)
   mut body_runs = []
   mut small_runs = []
   def header_body_y = _row_text_y(int(_editor_get("font_body")), y, 30.0)
   def header_small_y = _row_text_y(int(_editor_get("font_small")), y, 30.0)
   def int sf = int(_editor_get("font_small"))
   def hint = _fit_chrome_text(sf, "Enter accept  Esc close", w * 0.48)
   body_runs = _append_text_run(body_runs, _fit_chrome_text(int(_editor_get("font_body")), prefix.len > 0 ? prefix : "completion", w - 56.0 - _measure_ui(sf, hint, 0.0)), x + 36.0, header_body_y, U_TEXT)
   small_runs = _append_right_run(small_runs, sf, hint, x + w - 12.0, header_small_y, U_MUTED)
   _fill_rect(x + 12.0, y + 30.0, w - 24.0, 1.0, C_LINE)
   mut i = 0
   while i < rows {
      def item = items.get(top + i, {})
      def active = top + i == sel
      def yy = y + 40.0 + float(i) * 26.0
      def kind = to_str(item.get("kind", "symbol"))
      def col = _completion_color(kind)
      def body_y = _row_text_y(int(_editor_get("font_body")), yy - 4.0, 24.0)
      def small_y = _row_text_y(int(_editor_get("font_small")), yy - 4.0, 24.0)
      if active { _fill_rect(x + 8.0, yy - 4.0, w - 16.0, 24.0, gfx.color_alpha(C_ACCENT, 0.18)) }
      _draw_icon(kind == "lsp" ? "network" : (kind == "keyword" ? "code_edit" : "callable"), x + 14.0, _center_y(yy - 4.0, 24.0, 15.0), 15.0, active ? C_ACCENT : col)
      def tail = _fit_chrome_text(sf, to_str(item.get("detail", "")), w * 0.37)
      def f64 tail_w = _measure_ui(sf, tail, 0.0)
      body_runs = _append_text_run(body_runs, _fit_chrome_text(int(_editor_get("font_body")), to_str(item.get("label", "")), w - 64.0 - tail_w), x + 38.0, body_y, active ? U_TEXT : U_MUTED)
      small_runs = _append_right_run(small_runs, sf, tail, x + w - 14.0, small_y, active ? U_ACCENT_2 : U_DIM)
      i += 1
   }
   _flush_rects()
   if body_runs.len > 0 { gfx.draw_text_runs_flat_colors(int(_editor_get("font_body")), body_runs) }
   if small_runs.len > 0 { gfx.draw_text_runs_flat_colors(int(_editor_get("font_small")), small_runs) }
   0
}

fn _which_key_command_row(str prefix, str head) list {
   def full = str.strip(prefix + " " + head)
   def id = cmd.id_for_chord(full, _keymap_context())
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
   if pal.is_open(_editor_get("palette_state")) || prompt.rename_is_open(_editor_get("rename_state")) { return 0 }
   if !chord.pending(_editor_get("chord_state")) {
      if !SHOW_LAST_KEY { return 0 }
      def seen = interact.show_key(_editor_get("ui_state"))
      if seen.len <= 0 { return 0 }
      def f64 w0 = float(min(320.0, max(130.0, float(seen.len) * float(_editor_get("FONT_SMALL_ADV")) + 64.0)))
      def f64 x0 = sw - w0 - 10.0
      def f64 y0 = sh - 42.0
      _fill_rect(x0, y0, w0, 28.0, gfx.color_alpha(C_RAIL, 0.88))
      _stroke_rect(x0, y0, w0, 28.0, C_LINE_2, 1.0)
      mut kruns = []
      kruns = _append_text_run(kruns, _preview_small(seen, w0 - 66.0), x0 + 10.0, _row_text_y(int(_editor_get("font_small")), y0, 28.0), U_TEXT)
      kruns = _append_right_run(kruns, int(_editor_get("font_small")), "key", x0 + w0 - 10.0, _row_text_y(int(_editor_get("font_small")), y0, 28.0), U_DIM)
      _flush_rects()
      if kruns.len > 0 { gfx.draw_text_runs_flat_colors(int(_editor_get("font_small")), kruns) }
      return 0
   }
   def prefix = chord.describe(_editor_get("chord_state"))
   def rows = _which_key_rows(prefix)
   if rows.len <= 0 { return 0 }
   def rect = _which_key_rect(sw, sh, rows.len)
   def x = float(rect.get(0, 0.0))
   def y = float(rect.get(1, 0.0))
   def w = float(rect.get(2, 0.0))
   def h = float(rect.get(3, 0.0))
   def cols = int(rect.get(4, 3))
   def visible = int(rect.get(5, 1))
   _editor_values["which_key_scroll"] = _clamp_which_key_scroll(rows.len, visible)
   def start = int(_editor_get("which_key_scroll"))
   _fill_rect(x, y, w, h, gfx.color_alpha(C_RAIL, 0.94))
   _stroke_rect(x, y, w, h, C_ACCENT, 1.0)
   _fill_rect(x + 10.0, y + 34.0, w - 20.0, 1.0, C_LINE)
   mut body_runs = []
   mut small_runs = []
   body_runs = _append_text_run(body_runs, prefix, x + 12.0, y + 8.0, U_TEXT)
   def key_pos = rows.len > 0 ? to_str(start + 1) + "-" + to_str(min(rows.len, start + visible)) + "/" + to_str(rows.len) : "0/0"
   small_runs = _append_right_run(small_runs, int(_editor_get("font_small")), key_pos + "  Esc", x + w - 12.0, y + 10.0, U_MUTED)
   def col_w = (w - 20.0) / float(cols)
   def f64 key_row_h = float(WHICH_KEY_ROW_H)
   mut i = 0
   while i < visible {
      def row = rows.get(start + i, ["", ""])
      def cx = x + 10.0 + float(i % cols) * col_w
      def f64 cy = y + 42.0 + float(i / cols) * key_row_h
      def head = to_str(row.get(0, ""))
      def key_label = _preview_small(head, min(62.0, col_w * 0.22))
      small_runs = _append_text_run(small_runs, key_label, cx + 2.0, cy + 3.0, U_ACCENT)
      body_runs = _append_text_run(body_runs, _preview_small(to_str(row.get(1, "")), col_w - 80.0), cx + 62.0, cy + 1.0, U_TEXT)
      small_runs = _append_text_run(small_runs, _preview_small(to_str(row.get(3, "")), col_w - 80.0), cx + 62.0, cy + 14.0, U_DIM)
      i += 1
   }
   if rows.len > visible {
      def track_x = x + w - 6.0
      def f64 track_y = y + 42.0
      def f64 track_h = h - 50.0
      def f64 thumb_h = float(max(18.0, track_h * float(visible) / float(max(visible, rows.len))))
      def f64 thumb_y = track_y + (track_h - thumb_h) * float(start) / float(max(1, rows.len - visible))
      _fill_rect(track_x, track_y, 2.0, track_h, gfx.color_alpha(C_LINE_2, 0.62))
      _fill_rect(track_x, thumb_y, 2.0, thumb_h, gfx.color_alpha(C_ACCENT, 0.74))
   }
   _flush_rects()
   if body_runs.len > 0 { gfx.draw_text_runs_flat_colors(int(_editor_get("font_body")), body_runs) }
   if small_runs.len > 0 { gfx.draw_text_runs_flat_colors(int(_editor_get("font_small")), small_runs) }
   0
}

fn _draw_context_menu(f64 sw, f64 sh) int {
   if !prompt.context_is_open(_editor_get("context_state")) { return 0 }
   def rect = _context_rect(sw, sh)
   def actions = prompt.context_actions_for(_editor_get("context_state"))
   _gui_overlay_window("ed_context", float(rect.get(0, 8.0)), float(rect.get(1, 8.0)), float(rect.get(2, CONTEXT_W)), float(rect.get(3, 84.0)))
   mut str chosen = ""
   mut i = 0
   while i < actions.len {
      def row = actions.get(i)
      if gui.menu_item("ctx_" + to_str(i), to_str(row.get(0, "")), to_str(row.get(2, ""))) { chosen = to_str(row.get(1, "")) }
      _probe_note_rect("ctx_" + to_str(i))
      def detail = to_str(row.get(4, ""))
      if detail.len > 0 { gui.tooltip(detail) }
      i += 1
   }
   gui.end_window()
   if chosen.len > 0 { _context_action(chosen) }
   0
}

fn _context_rect(f64 sw, f64 sh) list {
   ;; One place for the menu's rectangle: drawing and click routing both use it.
   def f64 w = float(min(CONTEXT_W, max(1.0, sw - 16.0)))
   def actions = prompt.context_actions_for(_editor_get("context_state"))
   def f64 h = float(min(float(actions.len) * CONTEXT_ROW_H + 14.0, max(1.0, sh - 16.0)))
   _editor_values["context_state"] = prompt.context_clamp(_editor_get("context_state"), sw, sh, w, CONTEXT_ROW_H)
   def fit = _fit_rect(prompt.context_x(_editor_get("context_state")), prompt.context_y(_editor_get("context_state")), w, h, sw, sh, 8.0)
   _editor_get("context_state")["x"] = float(fit.get(0, 8.0))
   _editor_get("context_state")["y"] = float(fit.get(1, 8.0))
   def slide = gui.overlay_offset("ed_context", true)
   [float(fit.get(0, 8.0)), float(fit.get(1, 8.0)) + slide, w, float(max(84.0, h))]
}

fn _context_contains(dict lay, f64 mx, f64 my) bool {
   if !prompt.context_is_open(_editor_get("context_state")) { return false }
   def r = _context_rect(_layout_sw(lay), _layout_sh(lay))
   widgets.hit(mx, my, float(r.get(0, 0.0)), float(r.get(1, 0.0)), float(r.get(2, 0.0)), float(r.get(3, 0.0)))
}

;; ---- toolkit overlays ------------------------------------------------------
;; The palette, context menu, find bar and colour picker are drawn with the
;; shared toolkit (viewer/gui.ny) in a gui frame after the editor's own
;; drawing. Mouse events reach the toolkit while one of them is open.

def GUI_OVERLAY_OPTS = {"titlebar": false, "resizable": false, "movable": false, "collapsible": false, "closable": false, "scrollbar": false}
mut bool _gui_overlay_live = false

fn _probe_note_rect(str name) any {
   if probe_flags.get("POPUPS", false) { _editor_values["probe_rect_" + name] = gui.last_item_rect() }
   0
}

fn _probe_click(str name, f64 fx=0.5, f64 fy=0.5) any {
   ;; Clicks the recorded widget through the window's event queue, the same
   ;; path a real pointer takes.
   def r = _editor_get("probe_rect_" + name)
   assert(is_list(r) && r.len >= 4 && float(r.get(2, 0.0)) > 0.0, "popup probe: widget " + name + " was drawn")
   _probe_click_at(float(r.get(0, 0.0)) + float(r.get(2, 0.0)) * fx, float(r.get(1, 0.0)) + float(r.get(3, 0.0)) * fy)
}

fn _probe_click_at(f64 x, f64 y) any {
   window.push_event(_editor_get("win"), window.EVENT_MOUSE_POS_CHANGED, {"x": x, "y": y})
   window.push_event(_editor_get("win"), window.EVENT_MOUSE_BUTTON_PRESSED, {"button": 0, "x": x, "y": y})
   _editor_values["probe_release"] = [x, y]
   0
}

fn _probe_popups_show(int f) any {
   ;; NY_EDITOR_PROBE_POPUPS_SHOW=palette|context|find|color opens that popup
   ;; with sample content and leaves it up for a compositor capture.
   def what = common.env_trim("NY_EDITOR_PROBE_POPUPS_SHOW")
   if f != 1 { return 0 }
   if what == "palette" {
      _handle_key({"key": key.KEY_P, "mods": key.MOD_CONTROL | key.MOD_SHIFT})
      _editor_values["palette_state"] = pal.handle_char(_editor_get("palette_state"), {"char": 102, "mods": 0})
   } elif what == "context" {
      _open_context(260.0, 180.0, _editor_context_entry(), "editor")
   } elif what == "find" {
      _find_replace_open()
      _editor_values["find_state"] = find.set_query(_editor_get("find_state"), "tw")
      _find_refresh_now()
   } elif what == "color" {
      _editor_values["st"] = ed.state([ed.buffer("theme.ny", "", "def accent = \"#9f86d9\"\ndef warn = \"#e0a030\"\n")])
      _editor_values["hist"] = history.new()
      _editor_get("st")["cursor_line"] = 0
      _editor_get("st")["cursor_col"] = 16
      _color_picker_open()
   }
   _request_full_redraw(2)
   0
}

fn _probe_popups_step(int f) any {
   if common.env_trim("NY_EDITOR_PROBE_POPUPS_SHOW").len > 0 { return _probe_popups_show(f) }
   ;; Each popup is drawn by the toolkit and answers a click on its widgets.
   ;; A click's release goes out a frame after its press, as a pointer's does.
   def rel = _editor_get("probe_release")
   if is_list(rel) && rel.len == 2 {
      window.push_event(_editor_get("win"), window.EVENT_MOUSE_BUTTON_RELEASED, {"button": 0, "x": float(rel.get(0, 0.0)), "y": float(rel.get(1, 0.0))})
      _editor_values["probe_release"] = []
   }
   if f == 0 {
      _handle_key({"key": key.KEY_P, "mods": key.MOD_CONTROL | key.MOD_SHIFT})
      assert(pal.is_open(_editor_get("palette_state")), "palette opens")
   } elif f == 1 {
      _editor_values["probe_palette_slide_y"] = float(gui.window_rect("ed_palette").get(1, 0.0))
   } elif f == 2 {
      assert(float(gui.window_rect("ed_palette").get(1, 0.0)) < float(_editor_values.get("probe_palette_slide_y", 0.0)), "the palette slides into place")
   } elif f == 3 {
      assert(float(gui.window_rect("ed_palette").get(2, 0.0)) > 0.0, "palette is a toolkit window")
      _probe_click("pal_1")
   } elif f == 6 {
      assert(!pal.is_open(_editor_get("palette_state")), "clicking a palette row runs it and closes the palette")
      _open_context(200.0, 160.0, _editor_context_entry(), "editor")
      assert(prompt.context_is_open(_editor_get("context_state")), "context menu opens")
   } elif f == 9 {
      assert(float(gui.window_rect("ed_context").get(2, 0.0)) > 0.0, "context menu is a toolkit window")
      _probe_click_at(4.0, float(START_H) - 4.0)
   } elif f == 12 {
      assert(!prompt.context_is_open(_editor_get("context_state")), "a click outside closes the context menu")
      _open_context(200.0, 160.0, _editor_context_entry(), "editor")
   } elif f == 15 {
      _probe_click("ctx_0")
   } elif f == 18 {
      assert(!prompt.context_is_open(_editor_get("context_state")), "clicking a menu item runs it and closes the menu")
      _find_open()
      _editor_values["find_state"] = find.set_query(_editor_get("find_state"), "tw")
      _find_refresh_now()
   } elif f == 21 {
      assert(!find.case_on(_editor_get("find_state")), "case starts off")
      _probe_click("f_case")
   } elif f == 24 {
      assert(find.case_on(_editor_get("find_state")), "the find bar's Case toggle is a toolkit toggle")
      _find_close()
      _editor_values["st"] = ed.state([ed.buffer("color.ny", "", "def c = \"#ff0000\"\n")])
      _editor_values["hist"] = history.new()
      _editor_get("st")["cursor_line"] = 0
      _editor_get("st")["cursor_col"] = 10
      _color_picker_open()
      assert(colorpicker.is_open(_editor_get("color_state")), "colour picker opens on a hex literal")
   } elif f == 27 {
      assert(float(gui.window_rect("ed_color").get(2, 0.0)) > 0.0, "colour picker is a toolkit window")
      ;; Inside the saturation/value plane, under the hex line and swatch row.
      def wr = gui.window_rect("ed_color")
      _probe_click_at(float(wr.get(0, 0.0)) + float(wr.get(2, 0.0)) * 0.3, float(wr.get(1, 0.0)) + 100.0)
   } elif f == 30 {
      def line = to_str(ed.current_lines(_editor_get("st")).get(0, ""))
      print("popup colour line " + line)
      assert(!str.str_contains(line, "#ff0000") && str.str_contains(line, "#"), "picking a colour writes the literal back")
      print("EDITOR-GUI-POPUPS-OK")
      window.set_should_close(_editor_get("win"), true)
   }
   _request_full_redraw(2)
   0
}

fn _gui_overlay_open() bool {
   pal.is_open(_editor_get("palette_state")) || prompt.context_is_open(_editor_get("context_state")) || find.is_open(_editor_get("find_state")) || colorpicker.is_open(_editor_get("color_state"))
}

fn _gui_overlay_window(str id, f64 x, f64 y, f64 w, f64 h) any {
   gui.begin_window(id, "", x, y, w, h, GUI_OVERLAY_OPTS)
   gui.set_window_pos(id, x, y)
   gui.set_window_size(id, w, h)
   0
}

fn _gui_feed_mouse(int typ, any data) bool {
   if !_gui_overlay_live || !is_dict(data) { return false }
   gui.feed_event(typ, data)
}

fn _draw_gui_overlays(dict lay, f64 sw, f64 sh) int {
   if !_gui_overlay_open() {
      _gui_overlay_live = false
      return 0
   }
   if !_gui_overlay_live {
      ;; A press from before the overlay opened must not count as a click.
      gui.feed_event(window.EVENT_MOUSE_BUTTON_RELEASED, {"button": 0, "x": -1.0, "y": -1.0})
      gui.prepare_input(0, sw, sh)
      _gui_overlay_live = true
   }
   _flush_rects()
   gui.set_enabled(true)
   gui.set_fonts(int(_editor_get("font_body")), int(_editor_get("font_title")), int(_editor_get("font_small")))
   gui.prepare_input(_editor_get("win"), sw, sh)
   gfx.set_ortho_2d(0.0, sw, sh, 0.0)
   gui.begin_frame(0, int(_editor_get("font_body")), sw, sh)
   _draw_context_menu(sw, sh)
   _draw_color_picker(lay, sw, sh)
   _draw_find_bar(sw, sh)
   _draw_palette(sw, sh)
   gui.end_frame()
   0
}

fn _draw_rename_prompt(f64 sw, f64 sh) int {
   if !prompt.rename_is_open(_editor_get("rename_state")) { return 0 }
   def f64 w0 = float(min(520.0, max(1.0, sw - 44.0)))
   def f64 h0 = 86.0
   def fit = _fit_rect(sw * 0.5 - w0 * 0.5, max(24.0, sh * 0.18), w0, h0, sw, sh, 8.0)
   def x = float(fit.get(0, 8.0))
   def y = float(fit.get(1, 8.0))
   def w = float(fit.get(2, w0))
   def h = float(fit.get(3, h0))
   _fill_rect(x, y, w, h, gfx.color_alpha(C_RAIL, 0.98))
   _stroke_rect(x, y, w, h, C_ACCENT, 2.0)
   _text(int(_editor_get("font_small")), (to_str(prompt.rename_entry(_editor_get("rename_state")).get("action", "")) == "symbol" ? "rename symbol " + to_str(prompt.rename_entry(_editor_get("rename_state")).get("name", "")) : "rename " + prompt.rename_rel(_editor_get("rename_state"))), x + 16.0, y + 14.0, C_MUTED)
   _fill_rect(x + 14.0, y + 42.0, w - 28.0, 30.0, C_PANE)
   _stroke_rect(x + 14.0, y + 42.0, w - 28.0, 30.0, C_LINE, 1.0)
   def text = prompt.rename_text(_editor_get("rename_state"))
   _text(int(_editor_get("font_body")), text, x + 24.0, y + 49.0, C_TEXT)
   _fill_rect(x + 25.0 + _measure_ui(int(_editor_get("font_body")), text, 0.0), y + 48.0, 2.0, 18.0, C_ACCENT)
   0
}

fn _modeline_path(str path, str fallback) str {
   if path.len <= 0 { return fallback }
   def root = ospath.normalize(project.root(_editor_get("project_state")))
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
   if to_str(_editor_get("modeline_cache_key")) == key { return _editor_get("modeline_cache") }
   def run_cmd = b.get("readonly", false) ? [] : runner.command_for(path)
   _editor_values["modeline_cache_key"] = key
   _editor_values["modeline_cache"] = borrow({
      "name": name,
      "kind": kind,
      "path": path,
      "display_path": _modeline_path(path, name),
      "run_hint": run_cmd.len > 0 ? to_str(run_cmd.get(0, "")) : "no-runner",
      "language": kind.len > 0 && kind != "code" && kind != "doc" && kind != "file" ? kind : syntax.detect_language(name)
   })
   _editor_get("modeline_cache")
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
   def f64 safe_adv = float(max(1.0, adv))
   widgets.preview(text, int(max(4.0, px / safe_adv)))
}

fn _preview_small(str text, f64 px) str { _preview_px(text, px, float(_editor_get("FONT_SMALL_ADV"))) }

fn _preview_body(str text, f64 px) str { _preview_px(text, px, float(_editor_get("FONT_BODY_ADV"))) }

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
   _preview_utf8(text, int(max(4.0, px / float(max(1.0, float(_editor_get("FONT_MODELINE_CLIP_ADV")))))))
}

fn _draw_status(dict lay) int {
   def pad = float(lay.get("chrome_pad", 8.0))
   def y = float(lay.get("status_y", 0.0))
   def h = float(lay.get("status_h", ed.STATUS_H))
   def sw = float(lay.get("edit_x", 0.0)) + float(lay.get("edit_w", 0.0))
   _fill_rect(0.0, y, sw, h, C_PANE_2)
   _stroke_rect(0.0, y, sw, h, C_LINE, 1.0)
   def b = ed.current_buffer(_editor_get("st"))
   def meta = _modeline_meta(b)
   def display_path = _modeline_text(to_str(meta.get("display_path", "buffer")))
   def counts = project.counts(_editor_get("project_state"))
   def mod_n = int(counts.get("modified", 0))
   def add_n = int(counts.get("added", 0))
   def del_n = int(counts.get("deleted", 0))
   def untracked_n = int(counts.get("untracked", 0))
   def dirty = mod_n + add_n + del_n + untracked_n
   def branch = _modeline_text(project.branch(_editor_get("project_state")))
   def run_hint = _modeline_text(to_str(meta.get("run_hint", "no-runner")))
   def language = _modeline_text(to_str(meta.get("language", "text")))
   def mode = b.get("readonly", false) ? "RO" : (b.get("dirty", false) ? "MOD" : "OK")
   def line = int(_editor_get("st").get("cursor_line", 0)) + 1
   def col = int(_editor_get("st").get("cursor_col", 0)) + 1
   def int total = int(max(1, ed.current_lines(_editor_get("st")).len))
   ;; Position through the file: the first line is 0% and the last is 100%.
   def pos_pct = total > 1 ? to_str(((line - 1) * 100) / (total - 1)) + "%" : "All"
   def loc = "Ln " + to_str(line) + ":" + to_str(col) + "  " + pos_pct
   def page = "buf " + to_str(int(_editor_get("st").get("active", 0)) + 1) + "/" + to_str(_editor_get("st").get("buffers", []).len)
   def git = branch.len > 0 ? branch + (dirty > 0 ? " +" + to_str(dirty) : "") : "no git"
   def diff = "+" + to_str(add_n) + " ~" + to_str(mod_n) + " -" + to_str(del_n) + (untracked_n > 0 ? " ?" + to_str(untracked_n) : "")
   def sel = _modeline_selection()
   def cursors = _extra_cursor_count() > 0 ? "cur " + to_str(_extra_cursor_count() + 1) : ""
   def term = termpane.is_open(_editor_get("term_state")) ? "term:" + termpane.mode(_editor_get("term_state")) : "term:off"
   def lsp_label = _editor_get("lsp_state").get("active", false) ? "lsp:on" : "lsp:off"
   def int ms10 = int(max(0, int(float(_editor_get("st").get("frame_ms", 0.0)) * 10.0)))
   def fps = "FPS " + to_str(int(_editor_get("st").get("fps", 0))) + " " + to_str(ms10 / 10) + "." + to_str(ms10 % 10) + "ms"
   def seen = _modeline_text(interact.show_key(_editor_get("ui_state")))
   def key_hint = chord.pending(_editor_get("chord_state")) ? ("prefix " + chord.describe(_editor_get("chord_state"))) : (seen.len > 0 ? ("key " + seen) : "C-SPC leader")
   def state_icon = mode == "MOD" ? "MOD" : (mode == "RO" ? "RO" : "OK")
   def left = state_icon + "  " + page + "  " + to_str(_editor_get("st").get("focus", "editor")) + "  " + _preview_modeline(display_path, sw * 0.18)
   def mid = key_hint + "  " + git + "  " + diff + "  " + language + "  " + run_hint
   def right_state = fps + "  " + loc + (sel.len > 0 ? "  " + sel : "") + (cursors.len > 0 ? "  " + cursors : "") + "  " + lsp_label + "  " + term
   def live_status = _modeline_text(interact.status(_editor_get("ui_state")))
   def right = live_status != "ready" ? fps + "  " + live_status + "  " + loc + (sel.len > 0 ? "  " + sel : "") + (cursors.len > 0 ? "  " + cursors : "") + "  " + lsp_label + "  " + term : right_state
   def f64 content_w = float(max(1.0, sw - pad * 2.0 - 16.0))
   ;; Each zone takes the width its text needs, capped so the other two keep
   ;; a share. Fixed fractions truncated short text while the bar sat empty.
   def f64 clip_adv = float(max(1.0, float(_editor_get("FONT_MODELINE_CLIP_ADV"))))
   def f64 right_need = float(str.utf8_len(right)) * clip_adv + 12.0
   def f64 left_need = float(str.utf8_len(left)) * clip_adv + 32.0
   def f64 right_w = float(min(right_need, content_w * 0.48))
   def f64 left_w = float(min(left_need, content_w * 0.38))
   def f64 left_x = pad + 8.0
   def f64 right_x = sw - pad - 8.0
   def f64 right_start = right_x - right_w
   def left_icon = mode == "MOD" ? "alert" : (mode == "RO" ? "lock" : "check")
   def f64 left_text_x = left_x + 20.0
   def f64 mid_icon_x = left_x + left_w + 10.0
   def f64 mid_x = mid_icon_x + 20.0
   def f64 mid_w = float(max(0.0, right_start - mid_x - 12.0))
   def text_y = _text_center_y_for(int(_editor_get("font_modeline")), y, h)
   _fill_rect(0.0, y, 4.0, h, mode == "MOD" ? C_WARN : (mode == "RO" ? C_CYAN : C_ACCENT))
   mut runs = []
   runs = _append_text_run(runs, _fit_chrome_text(int(_editor_get("font_modeline")), left, left_w - 20.0), left_text_x, text_y, mode == "MOD" ? U_WARN : (mode == "RO" ? U_CYAN : U_ACCENT))
   runs = _append_text_run(runs, _fit_chrome_text(int(_editor_get("font_modeline")), mid, mid_w), mid_x, text_y, chord.pending(_editor_get("chord_state")) ? U_ACCENT : (dirty > 0 ? U_ACCENT_2 : U_DIM))
   runs = _append_right_run(runs, int(_editor_get("font_modeline")), _fit_chrome_text(int(_editor_get("font_modeline")), right, right_w), right_x, text_y, live_status != "ready" ? U_ACCENT : U_MUTED)
   _flush_rects()
   def icon_y = _center_y(y, h, 14.0)
   _draw_icon(left_icon, left_x, icon_y, 14.0, mode == "MOD" ? C_WARN : (mode == "RO" ? C_CYAN : C_ACCENT))
   if mid_w > 0.0 { _draw_icon("git_branch", mid_icon_x, icon_y, 14.0, dirty > 0 ? C_ACCENT_2 : C_DIM) }
   if runs.len > 0 { gfx.draw_text_runs_flat_colors(int(_editor_get("font_modeline")), runs) }
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
   if probe_flags.get("GIT_INLINE", false) {
      def p = ospath.join(getcwd(), "etc/labs/editor.ny")
      match file_read(p) {
         ok(raw) -> { return [ed.buffer("editor.ny", p, to_str(raw))] }
         err(_) -> { return [ed.buffer("editor.ny", p, "")] }
      }
   }
   if probe_flags.get("INLAY_LIVE", false) { return [ed.buffer("inlay-live.ny", "/tmp/ny-inlay-live.ny", "def answer = 43\nfn demo() {\n   answer\n}\n")] }
   if probe_flags.get("TERMINAL_DUMP", false) { return [ed.buffer("terminal-visual.ny", "", "use core\nprint(\"terminal visual probe\")\n")] }
   if probe_flags.get("TEXTGRID", false) { return [ed.buffer("textgrid.ny", "", "fn main() int {\n\tprint(\"grid\")\n   return 0\n}\n")] }
   if probe_flags.get("BENCH", false) || probe_flags.get("SCROLLBAR", false) { return [ed.buffer(probe_flags.get("BENCH", false) ? "bench.ny" : "scrollbar.ny", "", _bench_text())] }
   if probe_flags.get("ZOOM", false) { return [ed.buffer("zoom-test.ny", "", "use core\n")] }
   if probe_flags.get("GOTO", false) { return [ed.buffer("goto-test.ny", "", "use core\nfn local_target() int { return 1 }\nfn main() int {\n   local_target()\n   project_target()\n}\n")] }
   if probe_flags.get("MULTICURSOR", false) { return [ed.buffer("multi-cursor.ny", "", "aaa\nbbb\nccc")] }
   if probe_flags.get("BUFFER_SEARCH", false) { return [ed.buffer("first.ny", "", "alpha"), ed.buffer("second.ny", "", "warmup\nα ALPHA alpha")] }
   if probe_flags.get("FIND", false) || probe_flags.get("PALETTE", false) || probe_flags.get("POPUPS", false) || probe_flags.get("WHICH_KEY", false) { return [ed.buffer("ui-probe.ny", "", PROBE_TEXT)] }
   if probe_flags.get("ESCAPE", false) { return [ed.buffer("escape-test.ny", "", "use core\n")] }
   if probe_flags.get("NAV", false) || probe_flags.get("TAB", false) { return [ed.buffer("nav-a.ny", "", "alpha"), ed.buffer("nav-b.ny", "", "beta")] }
   if probe_flags.get("SELECT", false) { return [ed.buffer("select-all.ny", "", "one\ntwo\nthree")] }
   if probe_flags.get("IMAGE", false) { return [session.read_buffer(_write_image_probe_fixture())] }
   if probe_flags.get("GIT", false) { return [ed.buffer("git-panel.ny", "", "use core\n")] }
   if probe_flags.get("CHORD", false) { return [ed.buffer("chord-test.ny", "", "")] }
   if probe_flags.get("UNDO", false) { return [ed.buffer("undo-test.ny", "", "a")] }
   if probe_flags.get("INPUT", false) { return [ed.buffer("input-test.ny", "", PROBE_TEXT)] }
   def project_root = common.env_trim("NY_EDITOR_PROJECT_ROOT")
   def fallback = project_root.len > 0 ? ospath.join(project_root, "README.md") : "README.md"
   session.seed_buffers(cli.args(), fallback)
}

def initial_buffers = _initial_buffers()
_editor_values["st"] = ed.state(initial_buffers)
_editor_get("st")["focus"] = "editor"
_editor_get("st")["show_hierarchy"] = true
_editor_get("st")["show_titlebar"] = true
_editor_get("st")["show_project"] = true
_editor_get("st")["show_outline"] = true
_editor_get("st")["show_status"] = true
_editor_get("st")["show_line_numbers"] = true
_editor_get("st")["show_indent_guides"] = false
_editor_get("st")["scroll_follow"] = true
_editor_get("st")["outline_scroll"] = 0
_editor_get("st")["rail_tab"] = "files"
_editor_get("st")["project_loaded"] = false
_editor_get("st")["git_loaded"] = false
_editor_get("st")["drag_terminal"] = false
_editor_get("st")["drag_scrollbar"] = false
_editor_get("st")["scrollbar_target"] = dict(0)
_editor_get("st")["git_scroll"] = 0
_editor_get("st")["timeline_scroll"] = 0
_editor_get("st")["rail_split"] = 0.0
_editor_get("st")["term_h"] = 220.0
_load_ui_cache()
if bool(_editor_get("st").get("show_project", true)) && bool(_editor_get("st").get("show_hierarchy", true)) { _ensure_project_tree() }
if probe_flags.get("BENCH", false) && common.env_truthy("NY_UI_HEADLESS") {
   _editor_get("st")["show_hierarchy"] = false
   _editor_get("st")["show_project"] = false
   _editor_get("st")["show_outline"] = false
   _editor_get("st")["rail_w"] = 0.0
}
_editor_values["next_cache_save_time"] = gfx.get_time() + float(CACHE_SAVE_SECONDS)
_nav_record_current()
_editor_values["fps_state"] = ui_runtime.fps_begin()

if probe_flags.get("NAV", false) {
   _nav_push_current()
   _editor_values["st"] = ed.select_buffer(_editor_get("st"), 1)
   _buffer_changed(false)
   _nav_record_current()
}

_lsp_sync_current(true)

if probe_flags.get("GOTO", false) {
   def root = common.env_trim("NY_EDITOR_PROBE_GOTO_ROOT")
   mut target_path = common.env_trim("NY_EDITOR_PROBE_GOTO_TARGET")
   if target_path.len <= 0 && root.len > 0 { target_path = ospath.join(root, "defs.ny") }
   _editor_values["project_state"] = project.new(root)
   _editor_values["project_state"] = project.refresh(_editor_get("project_state"), 1, 32)
   _editor_get("st")["project_loaded"] = true
   _invalidate_project_definitions()
   _editor_get("st")["cursor_line"] = 3
   _editor_get("st")["cursor_col"] = 6
   _goto_definition()
   assert(int(_editor_get("st").get("cursor_line", -1)) == 1 && int(_editor_get("st").get("cursor_col", -1)) == 3, "goto local definition")
   _editor_get("st")["cursor_line"] = 4
   _editor_get("st")["cursor_col"] = 7
   assert(root.len > 0 && file_exists(target_path), "goto project fixture")
   _goto_definition()
   assert(ospath.normalize(_current_path()) == ospath.normalize(target_path), "goto project definition path")
   assert(int(_editor_get("st").get("cursor_line", -1)) == 0, "goto project definition line")
   _editor_values["st"] = ed.state([ed.buffer("goto-empty.ny", "", "   \n")])
   _editor_get("st")["cursor_line"] = 0
   _editor_get("st")["cursor_col"] = 0
   _goto_definition()
   assert(int(_editor_get("st").get("cursor_line", -1)) == 0, "goto no symbol stays put")
   window.set_should_close(_editor_get("win"), true)
}

if probe_flags.get("ZOOM", false) {
   _set_editor_font_size(FONT_ZOOM_MIN + 1, false)
   def start_size = int(_editor_get("font_body_size"))
   mut line_limit_lay = dict(1)
   line_limit_lay["edit_w"] = 4000.0
   def int start_line_limit = _line_limit(line_limit_lay)
   def start_body_advance = float(_editor_get("FONT_BODY_ADV"))
   _run_command("zoom-in")
   def int zoomed_size = int(min(start_size + 1, FONT_ZOOM_MAX))
   def int terminal_font = int(int(_editor_get("font_body")))
   assert(int(_editor_get("font_body_size")) == zoomed_size, "editor zoom in")
   def int zoomed_line_limit = _line_limit(line_limit_lay)
   assert(zoomed_line_limit < start_line_limit, "editor line limit tracks zoomed body-font advance (" + to_str(start_body_advance) + " -> " + to_str(float(_editor_get("FONT_BODY_ADV"))) + ", " + to_str(start_line_limit) + " -> " + to_str(zoomed_line_limit) + ")")
   assert(int(_editor_get("term_state").get("font", 0)) == terminal_font, "terminal font follows editor zoom")
   _run_command("zoom-out")
   assert(int(_editor_get("font_body_size")) == start_size, "editor zoom out")
   assert(_line_limit(line_limit_lay) == start_line_limit, "editor line limit restores with body-font advance")
   _run_command("zoom-reset")
   assert(int(_editor_get("font_body_size")) == FONT_BODY_SIZE, "editor zoom reset")
   window.set_should_close(_editor_get("win"), true)
}

if probe_flags.get("FIND", false) {
   _run_command("find")
   _editor_values["find_state"] = find.set_query(_editor_get("find_state"), "tw")
   _find_refresh_now()
   assert(find.is_open(_editor_get("find_state")) && find.count(_editor_get("find_state")) >= 2, "ctrl-f find opens and matches")
   _find_step(1)
   assert(int(_editor_get("st").get("cursor_line", -1)) >= 10, "find next jumps")
   _editor_values["st"] = ed.state([ed.buffer("find-replace.ny", "", "one two\ntwo two")])
   _editor_values["hist"] = history.new()
   _editor_values["find_state"] = find.state()
   _find_replace_open()
   _editor_values["find_state"] = find.set_query(_editor_get("find_state"), "(tw)(o)")
   _editor_values["find_state"] = find.toggle_regex(_editor_get("find_state"))
   _editor_values["find_state"] = find.set_replacement(_editor_get("find_state"), "\\2\\1")
   _find_refresh_now()
   _find_replace_all()
   assert(_current_text() == "one otw\notw otw", "regex replace all")
   _undo()
   assert(_current_text() == "one two\ntwo two", "replace undo")
   window.set_should_close(_editor_get("win"), true)
}

if probe_flags.get("BUFFER_SEARCH", false) {
   _handle_key({"key": key.KEY_F, "mods": key.MOD_CONTROL | key.MOD_SHIFT})
   assert(pal.items_mode(_editor_get("palette_state")) == "find-buffers", "multi-buffer search opens from its keymap")
   def query_text = "alpha"
   mut query_byte = 0
   while query_byte < query_text.len {
      _handle_palette_char({"char": load8(query_text, query_byte), "mods": 0})
      query_byte += 1
   }
   assert(_palette_matches().len == 3, "results include repeated matches and case variants in other buffers")
   _handle_palette_char({"char": 122, "mods": 0})
   assert(_palette_matches().len == 0, "literal query ignores fuzzy matches")
   _handle_palette_key({"key": key.KEY_BACKSPACE, "mods": 0})
   assert(_palette_matches().len == 3, "deleting the extra byte restores the result list")
   _handle_palette_key({"key": key.KEY_DOWN, "mods": 0})
   assert(pal.index(_editor_get("palette_state")) == 1, "Down selects the result in the other buffer")
   _handle_palette_key({"key": key.KEY_ENTER, "mods": 0})
   assert(int(_editor_get("st").get("active", -1)) == 1, "Enter switches to the selected result's buffer")
   assert(int(_editor_get("st").get("cursor_line", -1)) == 1 && _selected_text_all() == "ALPHA", "jump selects the exact byte span in the other buffer")
   print("EDITOR-MULTI-BUFFER-SEARCH-OK")
   window.set_should_close(_editor_get("win"), true)
}

if probe_flags.get("CONTEXT", false) {
   _open_context(9999.0, 9999.0, _editor_context_entry(), "editor")
   assert(prompt.context_is_open(_editor_get("context_state")), "editor context opens")
   assert(to_str(prompt.context_entry(_editor_get("context_state")).get("scope", "")) == "editor", "editor context scope")
   def fit_ctx = _fit_rect(prompt.context_x(_editor_get("context_state")), prompt.context_y(_editor_get("context_state")), CONTEXT_W, CONTEXT_ROW_H * 4.0, START_W, START_H, 8.0)
   assert(float(fit_ctx.get(0, 0.0)) + CONTEXT_W <= START_W - 8.0 && float(fit_ctx.get(1, 0.0)) + CONTEXT_ROW_H * 4.0 <= START_H - 8.0, "context menu fits viewport")
   window.set_should_close(_editor_get("win"), true)
}

if probe_flags.get("PALETTE", false) {
   _handle_key({"key": key.KEY_P, "mods": key.MOD_CONTROL | key.MOD_SHIFT})
   assert(pal.is_open(_editor_get("palette_state")), "ctrl-shift-p opens palette")
   _editor_values["palette_state"] = pal.handle_char(_editor_get("palette_state"), {"char": 102, "mods": 0})
   _editor_values["palette_state"] = pal.handle_char(_editor_get("palette_state"), {"char": 105, "mods": 0})
   assert(pal.is_open(_editor_get("palette_state")) && pal.state_matches(_editor_get("palette_state")).len > 0, "palette filters commands")
   window.set_should_close(_editor_get("win"), true)
}

if probe_flags.get("WHICH_KEY", false) {
   def top_keys = pal.which_key("C-SPC")
   assert(top_keys.len > 0 && cmd.id_for_chord("C-f") == "find", "which-key and ctrl-f command registry")
   assert(cmd.id_for_chord("C-a") == "mark-whole", "ctrl-a command registry")
   assert(_which_key_tag("C-SPC", "SPC") == "ui", "which-key complete command metadata")
   assert(str.str_contains(_which_key_detail("C-SPC", "f"), "continue"), "which-key prefix detail")
   window.set_should_close(_editor_get("win"), true)
}

if probe_flags.get("SELECT", false) {
   _editor_get("st")["cursor_line"] = 1
   _editor_get("st")["cursor_col"] = 1
   _direct_key_fallback({"key": key.KEY_A, "mods": key.MOD_CONTROL})
   assert(_selection_any_valid() && _selected_text_all() == "one\ntwo\nthree", "ctrl-a selects whole buffer")
   _handle_escape_key({"key": key.KEY_ESCAPE, "mods": 0})
   assert(!_selection_any_valid() && !bool(_editor_get("st").get("quit_prompt", false)), "escape clears select all")
   _editor_get("st")["quit_prompt"] = true
   _editor_values["last_escape_ns"] = 1
   _handle_key({"key": key.KEY_DOWN, "mods": 0})
   assert(!bool(_editor_get("st").get("quit_prompt", false)) && int(_editor_get("last_escape_ns")) == 0, "movement clears quit prompt")
   mut dirty_a = ed.buffer("dirty-a.ny", "", "a")
   mut dirty_b = ed.buffer("dirty-b.ny", "", "b")
   dirty_a["dirty"] = true
   dirty_b["dirty"] = true
   _editor_values["st"] = ed.state([dirty_a, dirty_b])
   _kill_buffer()
   assert(bool(_editor_get("st").get("kill_confirm", false)) && int(_editor_get("st").get("kill_confirm_buf", -1)) == 0, "dirty kill arms active buffer")
   _editor_values["st"] = ed.select_buffer(_editor_get("st"), 1)
   _kill_buffer()
   assert(_editor_get("st").get("buffers", []).len == 2 && bool(_editor_get("st").get("kill_confirm", false)) && int(_editor_get("st").get("kill_confirm_buf", -1)) == 1, "dirty kill confirm does not transfer")
   window.set_should_close(_editor_get("win"), true)
}

if probe_flags.get("MULTICURSOR", false) {
   _editor_get("st")["cursor_line"] = 0
   _editor_get("st")["cursor_col"] = 2
   _direct_key_fallback({"key": key.KEY_DOWN, "mods": key.MOD_ALT | key.MOD_SHIFT})
   _direct_key_fallback({"key": key.KEY_DOWN, "mods": key.MOD_ALT | key.MOD_SHIFT})
   assert(_extra_cursor_count() == 2, "alt-shift-down grows cursor stack")
   _handle_escape_key({"key": key.KEY_ESCAPE, "mods": 0})
   assert(_extra_cursor_count() == 0 && !bool(_editor_get("st").get("quit_prompt", false)), "escape clears extra cursors")
   _direct_key_fallback({"key": key.KEY_DOWN, "mods": key.MOD_ALT | key.MOD_SHIFT})
   assert(_extra_cursor_count() == 1, "alt-shift-down adds cursor")
   _move_cursor_ext("right", false, false)
   assert(int(_editor_get("st").get("cursor_col", 0)) == 3 && _cursor_col(_extra_cursors().get(0, [])) == 3, "real cursor moves extra cursors")
   _move_cursor_ext("left", false, false)
   _edit_insert("!")
   assert(_current_text() == "aa!a\nbb!b\nccc", "multi cursor insert")
   _edit_newline()
   assert(_current_text() == "aa!\na\nbb!\nb\nccc", "multi cursor newline")
   _edit_backspace()
   assert(_current_text() == "aa!a\nbb!b\nccc", "multi cursor backspace merge")
   _undo()
   assert(_current_text() == "aa!\na\nbb!\nb\nccc" && _extra_cursor_count() == 1, "multi cursor undo restores state")
   _editor_values["st"] = ed.state([ed.buffer("multi-line-cursor.ny", "", "abc\ndef\nghi\njkl")])
   _editor_values["hist"] = history.new()
   _editor_get("st")["cursor_line"] = 0
   _editor_get("st")["cursor_col"] = 1
   _add_extra_cursor(1)
   _add_extra_cursor(1)
   assert(_extra_cursor_count() == 2, "repeated add-cursor below walks past existing cursors")
   _edit_insert("X\nY")
   assert(_current_text() == "aX\nYbc\ndX\nYef\ngX\nYhi\njkl", "multi cursor multiline insert")
   _handle_escape_key({"key": key.KEY_ESCAPE, "mods": 0})
   assert(_extra_cursor_count() == 0, "escape returns to one cursor after multiline edit")
   _editor_values["st"] = ed.state([ed.buffer("multi-select.ny", "", "abc\ndef\nghi")])
   _editor_values["hist"] = history.new()
   _editor_get("st")["cursor_line"] = 0
   _editor_get("st")["cursor_col"] = 1
   _add_extra_cursor(1)
   _move_cursor_ext("right", false, true)
   assert(_selection_ranges_active().len == 2 && _selected_text_all() == "b\ne", "shift-selection extends from all cursors")
   _edit_insert("Z")
   assert(_current_text() == "aZc\ndZf\nghi" && _extra_cursor_count() == 1, "typing replaces all cursor selections")
   _editor_values["st"] = ed.state([ed.buffer("multi-select-line.ny", "", "abc\ndef\nghi")])
   _editor_values["hist"] = history.new()
   _editor_get("st")["cursor_line"] = 0
   _editor_get("st")["cursor_col"] = 0
   _add_extra_cursor(1)
   _select_line()
   assert(_selection_ranges_active().len == 2 && _selected_text_all() == "abc\ndef", "select-line selects all cursor lines")
   window.set_should_close(_editor_get("win"), true)
}

if probe_flags.get("TERMINAL_DUMP", false) {
   _editor_get("term_state")["open"] = true
   _editor_get("term_state")["focus"] = true
   _editor_get("term_state")["mode"] = "terminal"
   _editor_get("term_state")["tabs"] = [{"mode": "terminal", "vt": _editor_get("term_state").get("vt")}]
   _editor_get("term_state")["active"] = 0
   _editor_values["term_state"] = termpane.write_text(_editor_get("term_state"), "terminal visual probe\n")
   _editor_values["term_state"] = termpane.write_text(_editor_get("term_state"), "fg should be light, bg should be dark\n")
   _editor_values["term_state"] = termpane.write_text(_editor_get("term_state"), "0123456789 abcdefghijklmnopqrstuvwxyz\n")
   assert(termpane.visible_cell_count(_editor_get("term_state")) > 0, "terminal visual text written")
   _set_focus("terminal")
} elif probe_flags.get("TERMINAL", false) {
   if common.env_truthy("NY_UI_HEADLESS") {
      _editor_get("term_state")["open"] = true
      _editor_get("term_state")["focus"] = true
      _editor_get("term_state")["mode"] = "terminal"
      _editor_get("term_state")["tabs"] = [{"mode": "terminal", "vt": _editor_get("term_state").get("vt")}]
      _editor_get("term_state")["active"] = 0
      _editor_values["term_state"] = termpane.write_text(_editor_get("term_state"), "EDITOR TERMINAL OK\n")
   } else {
      _editor_values["term_state"] = termpane.open_shell(_editor_get("term_state"))
      _editor_values["term_state"] = termpane.write_text(_editor_get("term_state"), "EDITOR TERMINAL OK\n")
      _editor_values["term_state"] = termpane.send_text(_editor_get("term_state"), "printf 'cwd: '; pwd\n")
   }
   _set_focus("terminal")
}

if probe_flags.get("SYNTAX", false) {
   def probe = "fn main() int { return 1 + value_name }"
   def runs = _syntax_runs("probe.ny", probe, 0)
   assert(_syntax_runs_text(runs) == probe, "syntax highlight preserves full line")
   assert(float(runs.get(0, []).get(2, -1.0)) == 0.0, "first syntax run starts at column zero")
   def old_buffers = _editor_get("st").get("buffers", [])
   def old_active = int(_editor_get("st").get("active", 0))
   _editor_get("st")["buffers"] = [ed.buffer("readme.md", "", "```ny\nfn main() int { return 1 }\n```")]
   _editor_get("st")["active"] = 0
   def md_runs = _syntax_runs("readme.md", "fn main() int { return 1 }", 1)
   assert(_syntax_runs_text(md_runs) == "fn main() int { return 1 }", "markdown ny fence preserves line")
   assert(md_runs.len > 0 && int(md_runs.get(0, []).get(1, syntax.TOK_TEXT)) == int(syntax.TOK_KEYWORD), "markdown ny fence uses nytrix syntax")
   _editor_get("st")["buffers"] = [ed.buffer("semantic.ny", "/tmp/semantic.ny", "thing")]
   _editor_get("st")["active"] = 0
   _editor_get("lsp_state")["encoding"] = "utf-8"
   _lsp_event({"kind": "semantic_tokens", "path": "/tmp/semantic.ny", "items": [{"line": 0, "col": 0, "length": 5, "type": "function"}]})
   def semantic_runs = _syntax_runs("semantic.ny", "thing", 0)
   assert(semantic_runs.len == 1 && int(semantic_runs.get(0).get(1)) == int(syntax.TOK_FUNCTION), "semantic response changes visible syntax run")
   _lsp_event({"kind": "inlay_hints", "path": "/tmp/semantic.ny", "items": [{"line": 0, "col": 2, "label": ": int", "padding_left": true, "padding_right": true}]})
   def f64 before_hint = _measure_prefix("thing", 2)
   def f64 after_hint = _visual_prefix("thing", 0, 2)
   assert(after_hint > before_hint && _visual_col_at("thing", 0, (before_hint + after_hint) * 0.5) == 2, "inlay reserves space and maps pointer to source column")
   assert(_visual_col_at("thing", 0, _visual_prefix("thing", 0, 4)) == 4, "pointer after hint maps to source column")
   def virtual_runs = _append_highlight_runs([], "semantic.ny", "thing", 0, 0.0, 0.0)
   assert(virtual_runs.len >= 12 && to_str(virtual_runs.get(0)) == "th" && to_str(virtual_runs.get(4)) == " : int " && int(virtual_runs.get(7)) == 10006, "inlay renders between source characters")
   _editor_get("lsp_state")["encoding"] = "utf-16"
   _editor_values["inlay_lines"] = {"0": [{"col": 3, "label": ": T"}, {"col": 4, "label": "!"}]}
   def f64 unicode_before = _measure_prefix("a😀b", 2)
   def f64 unicode_after = _visual_prefix("a😀b", 0, 2)
   assert(unicode_after > unicode_before && _visual_col_at("a😀b", 0, (unicode_before + unicode_after) * 0.5) == 2, "UTF-16 hint position maps after emoji")
   def f64 end_before = _visual_prefix("a😀b", 0, 2) + _measure_prefix("a😀b", 3) - _measure_prefix("a😀b", 2)
   assert(_visual_prefix("a😀b", 0, 3) > end_before && _visual_col_at("a😀b", 0, end_before + 1.0) == 3, "end-of-line hint maps to final source column")
   _touch_content()
   assert(to_str(_editor_values.get("semantic_path", "")) == "", "editing clears stale semantic tokens")
   assert(_inlays_for_line(0).len == 0, "editing clears stale inlays")
   _editor_get("st")["buffers"] = old_buffers
   _editor_get("st")["active"] = old_active
   mut big = []
   mut bi = 0
   while bi < BIG_FILE_LINES + 12 {
      big = big.append("fn big_" + to_str(bi) + "() int { return " + to_str(bi) + " }")
      bi += 1
   }
   _prepare_visible_runs(big, "huge.ny", BIG_FILE_LINES + 4, 4, 10.0, 10.0, 0.0, 180, false, true)
   assert(_editor_get("visible_text_runs_cache").len > 3 && int(_editor_get("visible_text_runs_cache").get(3, -1)) == int(syntax.TOK_KEYWORD), "big file visible syntax stays highlighted")
   assert(to_str(colorpicker.scan_hex_literal("bg: #b6a0ff;", 6).get("hex", "")) == "#b6a0ff", "hex color literal scanner")
   assert(to_str(colorpicker.scan_hex_literal("fg: #acf;", 6).get("hex", "")) == "#aaccff", "short hex color literal scanner")
   print("EDITOR-SYNTAX-PROBE-OK")
   window.set_should_close(_editor_get("win"), true)
}

if probe_flags.get("MODELINE", false) {
   def safe = _modeline_safe("ok \tbox " + chr(0x03BB) + chr(0x2192) + chr(0x2713))
   assert(_modeline_utf8_clean(safe) && str.utf8_valid(safe), "modeline preserves valid utf8 and strips controls")
   assert(str.utf8_valid(_preview_modeline(safe, 36.0)), "modeline clips on utf8 boundaries")
   window.set_should_close(_editor_get("win"), true)
}

if probe_flags.get("TEXTGRID", false) {
   def row0 = _editor_row_y(32.0, 0)
   def row1 = _editor_row_y(32.0, 1)
   def baseline = _editor_row_baseline_y(row0)
   def body_y = _row_text_y_from_baseline(int(_editor_get("font_body")), baseline, ed.LINE_H)
   def num_y = _row_text_y_from_baseline(int(_editor_get("font_small")), baseline, ed.LINE_H)
   assert(row1 - row0 == float(ed.LINE_H), "textgrid row origin stable")
   assert(_snap_px(row0) == row0 && _snap_px(body_y) == body_y && _snap_px(num_y) == num_y, "textgrid rows snap to pixels")
   assert(_snap_px(body_y + _font_ascent_px(int(_editor_get("font_body")), ed.LINE_H)) == baseline, "textgrid code baseline")
   assert(_snap_px(num_y + _font_ascent_px(int(_editor_get("font_small")), ed.LINE_H)) == baseline, "textgrid line-number baseline")
   assert(float(_editor_get("FONT_BODY_ADV")) >= 2.0 && float(_editor_get("FONT_BODY_ADV")) <= float(int(_editor_get("font_body_size"))) * 2.0, "textgrid body mono cell")
   def int cell = int(float(_editor_get("FONT_BODY_ADV")))
   assert(_prefix_x("1234", 4) == cell * 4, "textgrid mono cell width")
   assert(_prefix_x("x\t", 2) == cell * 4, "textgrid tab stop cell width")
   window.set_should_close(_editor_get("win"), true)
}

if probe_flags.get("IMAGE", false) {
   def ib = ed.current_buffer(_editor_get("st"))
   assert(to_str(ib.get("kind", "")) == "image", "image probe opens a preview buffer")
   assert(_is_svg_path(_buffer_source_path(ib)), "image probe uses svg source path")
}

if probe_flags.get("TERMINAL", false) && common.env_truthy("NY_UI_HEADLESS") {
   assert(termpane.is_open(_editor_get("term_state")), "terminal pane open")
   window.set_should_close(_editor_get("win"), true)
}

while !gfx.window_should_close(_editor_get("win")) {
   def t_frame0 = FRAME_TRACE ? ticks() : 0
   def dt = gfx.get_delta_time()
   _editor_values["chord_state"] = chord.tick(_editor_get("chord_state"), dt)
   _editor_values["ui_state"] = interact.tick(_editor_get("ui_state"), dt)
   if termpane.is_open(_editor_get("term_state")) { _editor_values["term_state"] = termpane.update(_editor_get("term_state")) }
   _lsp_pump()
   _dap_pump()
   _editor_values["fps_state"] = ui_runtime.fps_tick(_editor_get("fps_state"), dt)
   def fps = ui_runtime.fps_current(_editor_get("fps_state"), dt)
   if int(_editor_get("st").get("fps", -1)) != fps { _editor_get("st")["fps"] = fps }
   _editor_get("st")["frame_ms"] = dt * 1000.0
   if float(_editor_get("st").get("status_timer", 0.0)) > 0.0 { _editor_get("st")["status_timer"] = max(0.0, float(_editor_get("st").get("status_timer", 0.0)) - dt) }
   def fb = gfx.framebuffer_size_f64(START_W, START_H)
   def f64 sw = float(max(1.0, float(fb.get(0, START_W))))
   def f64 sh = float(max(1.0, float(fb.get(1, START_H))))
   def rail_w = bool(_editor_get("st").get("show_hierarchy", true)) ? float(_editor_get("st").get("rail_w", 250.0)) : 0.0
   def top_h = bool(_editor_get("st").get("show_titlebar", true)) ? TOP_H : 0.0
   mut lay = ed.editor_layout(sw, sh, rail_w, top_h)
   lay = _layout_with_terminal(lay, sh)
   if common.env_truthy("NYTRIX_UI_REPLAY") {
      def replay_lines = ed.current_lines(_editor_get("st"))
      def replay_project_rows = project.tree(_editor_get("project_state"))
      print("NYTRIX_UI_APP_STATE demo=editor frame=" + to_str(int(_editor_get("frame_count")) + 1) +
         " fps=" + to_str(fps) + " frame_ms=" + to_str(float(_editor_get("st").get("frame_ms", 0.0))) +
         " focus=" + to_str(_editor_get("st").get("focus", "")) +
         " active=" + to_str(int(_editor_get("st").get("active", 0))) +
         " buffers=" + to_str(_editor_get("st").get("buffers", []).len) +
         " lines=" + to_str(replay_lines.len) +
         " cursor=" + to_str(int(_editor_get("st").get("cursor_line", 0))) + ":" +
            to_str(int(_editor_get("st").get("cursor_col", 0))) +
         " scroll=" + to_str(int(_editor_get("st").get("scroll", 0))) +
         " project_rows=" + to_str(replay_project_rows.len) +
         " hierarchy=" + to_str(bool(_editor_get("st").get("show_hierarchy", true))) +
         " project=" + to_str(bool(_editor_get("st").get("show_project", true))) +
         " outline=" + to_str(bool(_editor_get("st").get("show_outline", true))) +
         " terminal=" + to_str(termpane.is_open(_editor_get("term_state"))))
   }
   def t_update = FRAME_TRACE ? ticks() : 0
   if probe_flags.get("TERMINAL_DUMP", false) && int(_editor_get("frame_count")) == 0 {
      assert(termpane.visible_cell_count(_editor_get("term_state")) > 0, "terminal visual text survived layout")
      print("[terminal:dump]\n" + termpane.debug_text(_editor_get("term_state"), 3, 100))
   }
   if probe_flags.get("INPUT", false) && !probe_flags.get("TERMINAL", false) && int(_editor_get("frame_count")) == 0 {
      window.push_event(_editor_get("win"), window.EVENT_KEY_PRESSED, {"key": key.KEY_RIGHT, "mods": 0})
      window.push_event(_editor_get("win"), window.EVENT_KEY_PRESSED, {"key": key.KEY_DOWN, "mods": 0})
      window.push_event(_editor_get("win"), window.EVENT_MOUSE_SCROLL, {"dy": -2.0, "x": float(lay.get("edit_x", 0.0)) + 40.0, "y": float(lay.get("edit_y", 0.0)) + 40.0})
   }
   if probe_flags.get("CONTEXT", false) && int(_editor_get("frame_count")) == 0 {
      def f64 click_x = float(lay.get("edit_x", 0.0)) + float(max(8.0, float(lay.get("edit_w", 0.0)) * 0.5))
      def f64 click_y = float(lay.get("edit_y", 0.0)) + float(max(8.0, min(42.0, float(lay.get("edit_h", 0.0)) * 0.5)))
      window.push_event(_editor_get("win"), window.EVENT_MOUSE_BUTTON_PRESSED, {"button": 1, "x": click_x, "y": click_y})
   }
   if probe_flags.get("NAV", false) && int(_editor_get("frame_count")) == 0 && int(_editor_get("nav_probe_phase")) == 0 {
      window.push_event(_editor_get("win"), window.EVENT_MOUSE_BUTTON_PRESSED, {"button": MOUSE_BACK, "x": float(lay.get("edit_x", 0.0)) + 30.0, "y": float(lay.get("edit_y", 0.0)) + 30.0})
   }
   if probe_flags.get("TAB", false) && int(_editor_get("frame_count")) == 0 {
      def tab_w_probe = _top_tab_w(sw, _editor_get("st").get("buffers", []).len)
      window.push_event(_editor_get("win"), window.EVENT_MOUSE_BUTTON_PRESSED, {"button": 0, "x": 6.0 + tab_w_probe + 18.0, "y": 10.0})
   }
   if probe_flags.get("TAB", false) && int(_editor_get("frame_count")) == 1 {
      window.push_event(_editor_get("win"), window.EVENT_MOUSE_BUTTON_PRESSED, {"button": 0, "x": 18.0, "y": 10.0})
   }
   if probe_flags.get("GIT", false) && int(_editor_get("frame_count")) == 0 {
      def tab_w_git = rail_w / 3.0
      def rail_x_probe = float(lay.get("rail_x", 0.0))
      def rail_y_probe = float(lay.get("rail_y", 0.0))
      window.push_event(_editor_get("win"), window.EVENT_MOUSE_BUTTON_PRESSED, {"button": 0, "x": rail_x_probe + tab_w_git * 2.0 + 12.0, "y": rail_y_probe + 10.0})
   }
   if probe_flags.get("UNDO", false) && int(_editor_get("frame_count")) == 0 {
      window.push_event(_editor_get("win"), window.EVENT_KEY_CHAR, {"char": 98, "mods": 0})
      window.push_event(_editor_get("win"), window.EVENT_KEY_PRESSED, {"key": key.KEY_Z, "mods": key.MOD_CONTROL})
   }
   if probe_flags.get("ESCAPE", false) && int(_editor_get("frame_count")) == 0 {
      window.push_event(_editor_get("win"), window.EVENT_KEY_PRESSED, {"key": key.KEY_ESCAPE, "mods": 0})
      window.push_event(_editor_get("win"), window.EVENT_KEY_CHAR, {"char": 27, "mods": 0})
   }
   if probe_flags.get("ESCAPE", false) && int(_editor_get("frame_count")) == 1 {
      window.push_event(_editor_get("win"), window.EVENT_KEY_PRESSED, {"key": key.KEY_ESCAPE, "mods": 0})
      window.push_event(_editor_get("win"), window.EVENT_KEY_CHAR, {"char": 27, "mods": 0})
   }
   if probe_flags.get("CHORD", false) && int(_editor_get("frame_count")) == 0 {
      window.push_event(_editor_get("win"), window.EVENT_KEY_PRESSED, {"key": key.KEY_SPACE, "mods": key.MOD_CONTROL})
      window.push_event(_editor_get("win"), window.EVENT_KEY_CHAR, {"char": 32, "mods": key.MOD_CONTROL})
      window.push_event(_editor_get("win"), window.EVENT_KEY_PRESSED, {"key": key.KEY_R, "mods": 0})
      window.push_event(_editor_get("win"), window.EVENT_KEY_CHAR, {"char": 114, "mods": 0})
      window.push_event(_editor_get("win"), window.EVENT_KEY_PRESSED, {"key": key.KEY_R, "mods": 0})
      window.push_event(_editor_get("win"), window.EVENT_KEY_CHAR, {"char": 114, "mods": 0})
      window.push_event(_editor_get("win"), window.EVENT_KEY_PRESSED, {"key": key.KEY_C, "mods": key.MOD_CONTROL})
      window.push_event(_editor_get("win"), window.EVENT_KEY_PRESSED, {"key": key.KEY_R, "mods": 0})
      window.push_event(_editor_get("win"), window.EVENT_KEY_CHAR, {"char": 114, "mods": 0})
      window.push_event(_editor_get("win"), window.EVENT_KEY_PRESSED, {"key": key.KEY_R, "mods": 0})
      window.push_event(_editor_get("win"), window.EVENT_KEY_CHAR, {"char": 114, "mods": 0})
   }
   if probe_flags.get("SCROLLBAR", false) && int(_editor_get("frame_count")) == 0 {
      def sx = float(lay.get("edit_x", 0.0)) + float(lay.get("edit_w", 0.0)) - 8.0
      def sy1 = float(lay.get("edit_y", 0.0)) + float(lay.get("edit_h", 0.0)) - 18.0
      window.push_event(_editor_get("win"), window.EVENT_MOUSE_BUTTON_PRESSED, {"button": 0, "x": sx, "y": sy1})
      window.push_event(_editor_get("win"), window.EVENT_MOUSE_POS_CHANGED, {"x": sx, "y": sy1})
      window.push_event(_editor_get("win"), window.EVENT_MOUSE_BUTTON_RELEASED, {"button": 0, "x": sx, "y": sy1})
   }
   _process_events(lay)
   _sync_mark_region()
   def t_events = FRAME_TRACE ? ticks() : 0
   _editor_omni_dump(lay, sw, sh)
   if probe_flags.get("INPUT", false) && !probe_flags.get("TERMINAL", false) && int(_editor_get("frame_count")) == 0 {
      if INPUT_TRACE {
         print("[editor:probe] line=" + to_str(int(_editor_get("st").get("cursor_line", -1))) + " col=" + to_str(int(_editor_get("st").get("cursor_col", -1))) + " scroll=" + to_str(int(_editor_get("st").get("scroll", -1))) + " follow=" + to_str(bool(_editor_get("st").get("scroll_follow", true))))
      }
      assert(int(_editor_get("st").get("cursor_line", -1)) == 1 && int(_editor_get("st").get("cursor_col", -1)) == 1 && int(_editor_get("st").get("scroll", 0)) > 0 && !bool(_editor_get("st").get("scroll_follow", true)), "editor input movement")
      window.set_should_close(_editor_get("win"), true)
   }
   if probe_flags.get("CONTEXT", false) && int(_editor_get("frame_count")) == 0 {
      assert(prompt.context_is_open(_editor_get("context_state")), "editor right-click context")
      assert(to_str(prompt.context_entry(_editor_get("context_state")).get("scope", "")) == "editor", "editor context scope")
      window.set_should_close(_editor_get("win"), true)
   }
   if probe_flags.get("NAV", false) {
      if int(_editor_get("nav_probe_phase")) == 0 {
         assert(int(_editor_get("st").get("active", -1)) == 0, "mouse back navigation")
         _editor_values["nav_probe_phase"] = 1
         window.push_event(_editor_get("win"), window.EVENT_MOUSE_BUTTON_PRESSED, {"button": MOUSE_FORWARD, "x": float(lay.get("edit_x", 0.0)) + 30.0, "y": float(lay.get("edit_y", 0.0)) + 30.0})
      } elif int(_editor_get("nav_probe_phase")) == 1 {
         assert(int(_editor_get("st").get("active", -1)) == 1, "mouse forward navigation")
         window.set_should_close(_editor_get("win"), true)
      }
   }
   if probe_flags.get("TAB", false) && int(_editor_get("frame_count")) == 0 {
      assert(int(_editor_get("st").get("active", -1)) == 1, "top tab selects second buffer")
      assert(int(_editor_get("force_full_redraw")) > 0, "top tab switch requests redraw")
   }
   if probe_flags.get("TAB", false) && int(_editor_get("frame_count")) == 1 {
      assert(int(_editor_get("st").get("active", -1)) == 0, "top tab selects first buffer")
      window.set_should_close(_editor_get("win"), true)
   }
   if probe_flags.get("GIT", false) && int(_editor_get("frame_count")) == 0 {
      assert(_rail_tab() == "git", "git rail tab selected")
      assert(bool(_editor_get("st").get("git_loaded", false)), "git tab status loaded")
      assert(int(_editor_get("force_full_redraw")) > 0, "git tab requests redraw")
      window.set_should_close(_editor_get("win"), true)
   }
   if probe_flags.get("UNDO", false) && int(_editor_get("frame_count")) == 0 {
      assert(_current_text() == "a", "ctrl-z undo")
      window.set_should_close(_editor_get("win"), true)
   }
   if probe_flags.get("ESCAPE", false) && int(_editor_get("frame_count")) == 0 {
      assert(bool(_editor_get("st").get("quit_prompt", false)) && !window.should_close(_editor_get("win")), "first esc prompts")
   }
   if probe_flags.get("ESCAPE", false) && int(_editor_get("frame_count")) == 1 {
      assert(window.should_close(_editor_get("win")), "esc esc closes editor")
   }
   if probe_flags.get("POPUPS", false) { _probe_popups_step(int(_editor_get("frame_count"))) }
   if probe_flags.get("CHORD", false) && int(_editor_get("frame_count")) == 0 {
      assert(_current_text() == "", "command chord chars do not insert")
      window.set_should_close(_editor_get("win"), true)
   }
   if probe_flags.get("SCROLLBAR", false) && int(_editor_get("frame_count")) == 0 {
      assert(int(_editor_get("st").get("scroll", 0)) > 0 && !bool(_editor_get("st").get("scroll_follow", true)), "editor scrollbar drag scrolls")
      window.set_should_close(_editor_get("win"), true)
   }
   if int(_editor_get("frame_count")) == 30 && common.env_truthy("NY_EDITOR_PROBE_READY") { print("EDITOR_READY") }
   if probe_flags.get("LSP", false) { _lsp_probe_step() }
   if probe_flags.get("DAP", false) { _dap_probe_step() }
   if probe_flags.get("TERMINAL", false) && int(_editor_get("frame_count")) >= 18 { window.set_should_close(_editor_get("win"), true) }
   if probe_flags.get("TERMINAL_DUMP", false) && int(_editor_get("frame_count")) >= int(max(12, auto_dump_delay + 3)) { window.set_should_close(_editor_get("win"), true) }
   def mp = window.mouse_pos(_editor_get("win"))
   def mpx = float(mp.get(0, 0.0))
   def mpy = float(mp.get(1, 0.0))
   if _dock_resize_hit(lay, mpx, mpy) || _editor_get("st").get("drag_terminal", false) {
      _set_cursor_kind("resize-ns")
   } elif ed.divider_hit(lay, mpx, mpy) || _editor_get("st").get("drag_divider", false) {
      _set_cursor_kind("resize-ew")
   } else {
      _set_cursor_kind("text")
   }
   _editor_values["frame_events_seen"] = bool(_editor_get("frame_events_seen")) || _editor_get("st").get("drag_terminal", false) || _editor_get("st").get("drag_divider", false) || _editor_get("st").get("drag_select", false) || _editor_get("st").get("drag_scrollbar", false)
   _editor_values["reuse_state"] = ui_reuse.note_events_seen(_editor_get("reuse_state"), bool(_editor_get("frame_events_seen")))
   def reuse_opts = _editor_reuse_opts(sw, sh)
   if ui_reuse.try_present(_editor_get("reuse_state"), reuse_opts) {
      _editor_values["reuse_present_count"] = int(_editor_get("reuse_present_count")) + (1)
      if probe_flags.get("BENCH", false) {
         _editor_values["frame_count"] = int(_editor_get("frame_count")) + (1)
         if int(_editor_get("frame_count")) >= BENCH_FRAME_LIMIT { window.set_should_close(_editor_get("win"), true) }
      } else {
         _editor_values["frame_count"] = int(_editor_get("frame_count")) + (1)
      }
      ui_runtime.close_on_timeout(_editor_get("win"), int(_editor_get("fps_state").get("start", 0)), timeout_limit)
      continue
   }
   if !gfx.begin_frame_clear(C_BG) {
      _editor_values["begin_fail_count"] = int(_editor_get("begin_fail_count")) + (1)
      if probe_flags.get("BENCH", false) {
         _editor_values["frame_count"] = int(_editor_get("frame_count")) + (1)
         if int(_editor_get("frame_count")) >= BENCH_FRAME_LIMIT { window.set_should_close(_editor_get("win"), true) }
      } else {
         _status("renderer begin failed on " + gfx.get_active_backend_name() + "; retrying")
         if int(_editor_get("begin_fail_count")) >= BEGIN_FAIL_LIMIT {
            _status("renderer begin failed repeatedly; closing safely")
            window.set_should_close(_editor_get("win"), true)
         }
      }
      ui_runtime.close_on_timeout(_editor_get("win"), int(_editor_get("fps_state").get("start", 0)), timeout_limit)
      continue
   }
   def t_begin = FRAME_TRACE ? ticks() : 0
   _editor_values["begin_fail_count"] = 0
   gfx.set_ortho_2d(0.0, sw, 0.0, sh)
   def caret = int(gfx.get_time() / CARET_BLINK_SEC) % 2 == 0
   if bool(_editor_get("st").get("show_titlebar", true)) { _draw_top(lay, fps) }
   def t_top = FRAME_TRACE ? ticks() : 0
   if bool(_editor_get("st").get("show_hierarchy", true)) { _draw_rail(lay) }
   def t_rail = FRAME_TRACE ? ticks() : 0
   _draw_editor(lay, caret)
   _draw_debug_gutter(lay)
   _draw_diagnostics(lay)
   def t_editor = FRAME_TRACE ? ticks() : 0
   _draw_terminal(lay)
   def t_terminal = FRAME_TRACE ? ticks() : 0
   if bool(_editor_get("st").get("show_status", true)) { _draw_status(lay) }
   def t_status = FRAME_TRACE ? ticks() : 0
   if _editor_get("st").get("quit_prompt", false) {
      def f64 pw = 240.0
      def f64 ph = 72.0
      def f64 px = float(sw) * 0.5 - float(pw) * 0.5
      def f64 py = float(sh) * 0.5 - float(ph) * 0.5
      _fill_rect(px, py, pw, ph, gfx.color_alpha(C_RAIL, 0.96))
      _stroke_rect(px, py, pw, ph, C_ACCENT, 2.0)
      _text_center(int(_editor_get("font_body")), "Quit? Esc again", px + pw * 0.5, py + 26.0, C_TEXT)
   }
   _draw_rename_prompt(sw, sh)
   _draw_completion(lay, sw, sh)
   _draw_lsp_popup(lay, sw, sh)
   _draw_debug_panel(lay, sw, sh)
   _draw_which_key(sw, sh)
   _draw_gui_overlays(lay, sw, sh)
   def t_draw = FRAME_TRACE ? ticks() : 0
   _trace_draw(int(_editor_get("frame_count")), t_begin, t_top, t_rail, t_editor, t_terminal, t_status, t_draw)
   _flush_rects()
   def t_flush = FRAME_TRACE ? ticks() : 0
   ui_dump.auto_dump_pre_frame(bool(_editor_get("auto_dump_done")), int(_editor_get("frame_count")), auto_dump_delay)
   if FRAME_HASH && int(_editor_get("frame_count")) + 1 >= FRAME_HASH_FRAME { gfx.request_frame_capture() }
   if probe_flags.get("INLAY_LIVE", false) { gfx.request_frame_capture() }
   gfx.end_frame()
   if probe_flags.get("INLAY_LIVE", false) { _inlay_live_probe(lay) }
   if probe_flags.get("GIT_INLINE", false) { _git_inline_probe(lay) }
   def t_end = FRAME_TRACE ? ticks() : 0
   if FRAME_HASH && !bool(_editor_get("frame_hash_done")) && int(_editor_get("frame_count")) >= FRAME_HASH_FRAME {
      def hash_line = ui_dump.framebuffer_hash_line()
      if hash_line.len > 0 {
         _editor_values["frame_hash_done"] = true
         print("[editor:framehash] frame=" + to_str(int(_editor_get("frame_count"))) + " " + hash_line)
         if FRAME_HASH_EXPECT.len > 0 && !str.str_contains(hash_line, FRAME_HASH_EXPECT) {
            print("[editor:framehash:mismatch] expected=" + FRAME_HASH_EXPECT + " got=" + hash_line)
            window.set_should_close(_editor_get("win"), true)
         }
         if !common.env_truthy("NY_EDITOR_FRAME_HASH_EVERY") { window.set_should_close(_editor_get("win"), true) }
      } elif int(_editor_get("frame_count")) >= FRAME_HASH_FRAME + 4 {
         _editor_values["frame_hash_done"] = true
         print("[editor:framehash] frame=" + to_str(int(_editor_get("frame_count"))) + " unavailable backend=" + gfx.get_active_backend_name())
         window.set_should_close(_editor_get("win"), true)
      }
   }
   _editor_values["reuse_state"] = ui_reuse.note_full_draw(_editor_get("reuse_state"), reuse_opts)
   if int(_editor_get("force_full_redraw")) > 0 { _editor_values["force_full_redraw"] = int(_editor_get("force_full_redraw")) - (1) }
   _trace_frame(int(_editor_get("frame_count")), t_frame0, t_update, t_events, t_begin, t_draw, t_flush, t_end)
   _editor_values["frame_count"] = int(_editor_get("frame_count")) + (1)
   if probe_flags.get("BENCH", false) && BENCH_WARMUP_FRAMES > 0 && int(_editor_get("frame_count")) == BENCH_WARMUP_FRAMES {
      _editor_values["fps_state"] = ui_runtime.fps_begin()
   }
   if probe_flags.get("IMAGE", false) && int(_editor_get("frame_count")) >= 2 { window.set_should_close(_editor_get("win"), true) }
   if probe_flags.get("BENCH", false) && int(_editor_get("frame_count")) >= BENCH_FRAME_LIMIT { window.set_should_close(_editor_get("win"), true) }
   if common.env_truthy("NYTRIX_UI_REPLAY") && int(_editor_get("frame_count")) >= common.env_int_clamped("NYTRIX_FIXTURE_FRAMES", 3, 1, 100000) {
      window.set_should_close(_editor_get("win"), true)
   }
   if !probe_flags.get("BENCH", false) && gfx.get_time() >= float(_editor_get("next_cache_save_time")) {
      _save_ui_cache()
      _editor_values["next_cache_save_time"] = gfx.get_time() + float(CACHE_SAVE_SECONDS)
   }
   _editor_values["auto_dump_done"] = ui_dump.auto_dump_post_frame(_editor_get("win"), bool(_editor_get("auto_dump_done")), int(_editor_get("frame_count")), auto_dump_delay, "build/cache/fb/ui/editor.png")
   ui_runtime.close_on_timeout(_editor_get("win"), int(_editor_get("fps_state").get("start", 0)), timeout_limit)
}

_bench_finish()
ui_runtime.fps_finish("editor", _editor_get("fps_state"))
_save_ui_cache()
_editor_values["term_state"] = termpane.dismiss(_editor_get("term_state"))
_release_image_preview_cache()
gfx.close_window()
exit(0)
