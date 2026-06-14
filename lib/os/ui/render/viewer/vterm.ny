;; Keywords: ui terminal virtual-console ansi text os render viewer console
;; Virtual terminal state helpers for console-like UI surfaces.
;; References:
;; - std.os.ui.render.viewer.term
module std.os.ui.render.viewer.vterm(new, open, close, update, draw, handle_event, write, send_input, resize, set_fonts, set_viewport, is_running, get_title, cursor_blink_phase, needs_visual_refresh, idle_sleep_ms, default_shell_path, default_shell_args, env_bg_color, abgr_to_color, selection_dragging, selection_any, clear_selection)
use std.core
use std.core.mem (__copy_mem)
use std.os (environ, file_read, file_exists, msleep, ticks)
use std.os.sys as sys
use std.os.process as osproc
use std.os.thread
use std.os.clipboard as clipboard
use std.core.str as str
use std.math.crypto.encoding.base as str_base
use std.math
use std.parse.img.png as png_img
use std.os.ui.window.consts
use std.os.ui.render (
   BACKEND_MOCK, _font_get, color_pack, draw_rect, draw_rect_fast,
   draw_rect_tex_uv, draw_text, draw_text_runs_flat_colors, font_prepare,
   get_active_backend, measure_text, reset_overlay_state, set_unlit,
   texture_bind, texture_bind_default, texture_create_rgba, texture_destroy,
   terminal_fast_text_supported, draw_terminal_line_fast_ptr, draw_text_glyph_fast,
   font_fast_glyph_present, font_fast_glyph_texture
)

use std.os.ui.window as uiw
use std.os.ui.window.input
use std.core.common as common

mut _px = 0.0
mut _py = 0.0

fn memmove(any dst, any src, int n) any { __copy_mem(dst, src, n) }

fn set_viewport(any x, any y) any {
   "Sets the terminal drawing/input origin inside a larger UI."
   _px = float(x)
   _py = float(y)
   0
}

def IMAGE_PLACEHOLDER_CHAR = 0x10EEEE
def IMAGE_PLACEHOLDER_CHAR_OLD = 0xEEEE

fn _vterm_cell_metrics(int font_id) list {
   def f_obj = _font_get(font_id)
   def f_size = (f_obj != 0) ? float(f_obj.get("size", 16.0)) : 16.0
   def ascent  = (f_obj != 0) ? float(f_obj.get("ascent", f_size * 0.80)) : (f_size * 0.80)
   def descent = (f_obj != 0) ? float(f_obj.get("descent", 0.0 - f_size * 0.20)) : (0.0 - f_size * 0.20)
   def span = ascent - descent
   def cs = measure_text(font_id, "M")
   mut cw = float(cs.get(0, 0.0))
   if cw <= 0.1 { cw = max(7.0, floor(f_size * 0.55)) }
   mut ch = span * 1.18
   def ch_min = f_size * 1.05
   def ch_max = f_size * 1.45
   if ch < ch_min { ch = ch_min }
   if ch > ch_max { ch = ch_max }
   [float(cw), float(ch)]
}

fn _vt_clip_set(dict vt, str text) dict {
   def w = uiw.last()
   if w { uiw.set_clipboard(w, text) }
   vt = vt.set("clipboard_cache", text)
   clipboard.set_text(text)
   vt
}

fn _vt_clip_get(dict vt) str {
   def w = uiw.last()
   if w {
      def t = uiw.get_clipboard(w)
      if t.len > 0 { return t }
   }
   def cached = vt.get("clipboard_cache", "")
   if cached && cached.len > 0 { return cached }
   clipboard.get_text()
}

fn _vt_selection_text(dict vt, list history, int cs_row, int cs_col, int ce_row, int ce_col, int hist_len, int co) str {
   if co <= 0 { return "" }
   def g = vt.get("grid")
   def ascii_cache = vt.get("ascii_cache")
   mut s_row, s_col = cs_row, cs_col
   mut e_row, e_col = ce_row, ce_col
   if s_row > e_row || (s_row == e_row && s_col > e_col) {
      mut tmp = s_row s_row = e_row e_row = tmp
      tmp = s_col s_col = e_col e_col = tmp
   }
   if s_row < 0 { s_row = 0 }
   if e_row < s_row { return "" }
   if s_col < 0 { s_col = 0 } elif s_col >= co { s_col = co - 1 }
   if e_col < 0 { e_col = 0 } elif e_col >= co { e_col = co - 1 }
   mut full_b = Builder(max(128, (e_row - s_row + 1) * max(8, co / 2)))
   mut cur_y = s_row
   while cur_y <= e_row {
      def row_x1, row_x2 = (cur_y == s_row) ? s_col : 0, (cur_y == e_row) ? e_col : (co - 1)
      if row_x2 >= row_x1 {
         def line_ptr = (cur_y < hist_len) ? history.get(cur_y) : ptr_add(g, (cur_y - hist_len) * co * 16)
         mut row_b = Builder(max(32, row_x2 - row_x1 + 8))
         mut cur_x = row_x1
         while cur_x <= row_x2 {
            def cp = load32(line_ptr, cur_x * 16)
            def m  = load32(line_ptr, cur_x * 16 + 12)
            if (m & ATTR_WDUMMY) == 0 {
               if (m & ATTR_IMAGE) != 0 { row_b = builder_append(row_b, _cp_to_str(IMAGE_PLACEHOLDER_CHAR, ascii_cache)) } elif cp > 32 { row_b = builder_append(row_b, _cp_to_str(cp, ascii_cache)) } elif cp == 32 { row_b = builder_append(row_b, " ") }
            }
            cur_x += 1
         }
         mut row_txt = builder_to_str(row_b)
         builder_free(row_b)
         mut ti = row_txt.len - 1
         while ti >= 0 && load8(row_txt, ti) == 32 { ti -= 1 }
         if ti < row_txt.len - 1 { row_txt = str.str_slice(row_txt, 0, ti + 1) }
         full_b = builder_append(full_b, row_txt)
      }
      if cur_y < e_row { full_b = builder_append(full_b, "\n") }
      cur_y += 1
   }
   def out = builder_to_str(full_b)
   builder_free(full_b)
   out
}

def ATTR_BOLD      = 1
def ATTR_FAINT     = 2
def ATTR_ITALIC    = 4
def ATTR_UNDERLINE = 8
def ATTR_REVERSE   = 16
def ATTR_WIDE      = 32
def ATTR_WDUMMY    = 64
def ATTR_BLINK     = 128
def ATTR_INVIS     = 256
def ATTR_STRIKE    = 512
def ATTR_IMAGE     = 1024
def CURSOR_BLINK_NS = 320000000
def CURSOR_RECENT_KEY_NS = 450000000
def MODE_WRAP        = 1
def MODE_ALTSCREEN   = 2
def MODE_UTF8        = 4
def MODE_MOUSE       = 8
def ESC_START      = 1
def ESC_CSI        = 2
def ESC_STR        = 4
def ESC_ALTCHARSET = 8
def ESC_STR_ESC    = 16

fn _esc_is_altcharset(int u) bool {
   if u == 40 { return true }
   if u == 41 { return true }
   if u == 42 { return true }
   if u == 43 { return true }
   false
}

fn _esc_is_string_start(int u) bool {
   if u == 80 { return true }
   if u == 93 { return true }
   if u == 94 { return true }
   if u == 95 { return true }
   false
}

def SYS_BASE = 0xff000000
def SYS_TEXT = 0xffe5e5e5
def DEFAULT_BG_ALPHA = 255

fn _abgr_luma(int c) int {
   def r, g = c & 255, (c >> 8) & 255
   def b = (c >> 16) & 255
   (r * 54 + g * 183 + b * 19) / 256
}

fn _rgb_to_abgr(int rgb) int {
   def r, g = (rgb >> 16) & 255, (rgb >> 8) & 255
   def b = rgb & 255
   (255 << 24) | (b << 16) | (g << 8) | r
}

fn _argb_to_abgr(int argb) int {
   def a, r = (argb >> 24) & 255, (argb >> 16) & 255
   def g, b = (argb >> 8) & 255, argb & 255
   (a << 24) | (b << 16) | (g << 8) | r
}

fn _with_alpha_abgr(int c, int a8) int { ((a8 & 255) << 24) | (c & 0x00ffffff) }

fn _opaque_abgr(int c) int { (c & 0x00ffffff) | 0xff000000 }

fn _blend_abgr_over(int top, int base) int {
   def a = (top >> 24) & 255
   if a <= 0 { return base | 0xff000000 }
   if a >= 255 { return top | 0xff000000 }
   def inv = 255 - a
   def r = ((top & 255) * a + (base & 255) * inv) / 255
   def g = (((top >> 8) & 255) * a + ((base >> 8) & 255) * inv) / 255
   def b = (((top >> 16) & 255) * a + ((base >> 16) & 255) * inv) / 255
   0xff000000 | (b << 16) | (g << 8) | r
}

fn _parse_abgr(any v, int def_val) int {
   if !v || !is_str(v) { return def_val }
   mut s = str.strip(v)
   if s.len == 0 { return def_val }
   mut order = ""
   if str.startswith(str.lower(s), "argb:") { order = "argb" s = str.str_slice(s, 5, s.len) }
   elif str.startswith(str.lower(s), "abgr:") { order = "abgr" s = str.str_slice(s, 5, s.len) }
   if str.startswith(s, "0x") { s = str.str_slice(s, 2, s.len) }
   elif str.startswith(s, "#") { s = str.str_slice(s, 1, s.len) }
   mut res = 0
   mut i = 0 while i < s.len {
      def c = load8(s, i)
      mut val = 0
      if c >= 48 && c <= 57 { val = c - 48 }
      elif c >= 65 && c <= 70 { val = c - 55 }
      elif c >= 97 && c <= 102 { val = c - 87 }
      else { break }
      res = (res << 4) | val
      i += 1
   }
   if i == 6 { return _rgb_to_abgr(res) }
   if i == 8 {
      if order == "argb" { return _argb_to_abgr(res) }
      res
   }
   def_val
}

fn _env_abgr(str name, int def_val) int {
   def v = common.env_trim(name)
   if v.len == 0 { return def_val }
   _parse_abgr(v, def_val)
}

fn _theme_wal_special_abgr(str name, int def_val) int {
   if common.env_present("NY_TERM_NO_THEME") { return def_val }
   def home = common.env_trim("HOME")
   if home.len == 0 { return def_val }
   def r = file_read(home + "/.cache/wal/colors.json")
   if !is_ok(r) { return def_val }
   def raw = unwrap(r)
   def key = "\"" + name + "\""
   def kp = str.find(raw, key)
   if kp < 0 { return def_val }
   mut rest = str.str_slice(raw, kp + key.len, raw.len)
   def colon = str.find(rest, ":")
   if colon < 0 { return def_val }
   rest = str.str_slice(rest, colon + 1, rest.len)
   def hash = str.find(rest, "#")
   if hash < 0 { return def_val }
   mut end = hash + 1
   while end < rest.len {
      def ch = load8(rest, end)
      if !((ch >= 48 && ch <= 57) || (ch >= 65 && ch <= 70) || (ch >= 97 && ch <= 102)) { break }
      end += 1
   }
   _parse_abgr(str.str_slice(rest, hash, end), def_val)
}

fn env_bg_color(int def_val=0xff000000) int {
   "Returns the terminal background color from `NY_TERM_BG`, falling back to `def_val`."
   _opaque_abgr(_env_abgr("NY_TERM_BG", def_val))
}

fn abgr_to_color(int c) list {
   "Converts ABGR `0xAABBGGRR` into `[r,g,b,a]` floats."
   def a, b = float((c >> 24) & 255) / 255.0, float((c >> 16) & 255) / 255.0
   def g, r = float((c >> 8) & 255) / 255.0, float(c & 255) / 255.0
   [r, g, b, a]
}

fn _palette_get(any p, int idx, int def_val=0) int {
   if !p || idx < 0 || idx > 255 { return def_val }
   load32(p, idx * 4)
}

fn _maybe_apply_palette_overrides(any p) bool {
   if common.env_present("NY_TERM_NO_THEME") { return false }
   mut i = 0
   mut has_any = false
   while i < 16 {
      def key = "NY_TERM_COLOR" + to_str(i)
      if common.env_present(key) {
         has_any = true
         def c = _env_abgr(key, _palette_get(p, i, 0))
         store32(p, c, i * 4)
      }
      i += 1
   }
   if has_any { return true }
   def home = common.env_trim("HOME")
   if home.len > 0 {
      def wal_r = file_read(home + "/.cache/wal/colors")
      if is_ok(wal_r) {
         def wal = unwrap(wal_r)
         def lines = str.split(wal, "\n")
         mut j = 0
         def lines_n = lines.len
         while j < 16 && j < lines_n {
            def line = str.strip(lines.get(j))
            if line.len > 0 {
               def c = _parse_abgr(line, 0)
               if c != 0 { store32(p, c, j * 4) has_any = true }
            }
            j += 1
         }
         if has_any { return true }
      }
   }
   mut cfg = 0
   def xdg = common.env_trim("XDG_CONFIG_HOME")
   if xdg.len > 0 {
      def r1 = file_read(xdg + "/alacritty/alacritty.toml")
      if is_ok(r1) { cfg = unwrap(r1) }
      if !cfg {
         def r2 = file_read(xdg + "/alacritty/alacritty.yml")
         if is_ok(r2) { cfg = unwrap(r2) }
      }
   } else {
      def home2 = common.env_trim("HOME")
      if home2.len > 0 {
         def r1 = file_read(home2 + "/.config/alacritty/alacritty.toml")
         if is_ok(r1) { cfg = unwrap(r1) }
         if !cfg {
            def r2 = file_read(home2 + "/.config/alacritty/alacritty.yml")
            if is_ok(r2) { cfg = unwrap(r2) }
         }
      }
   }
   if !cfg || !is_str(cfg) { return false }
   def names = ["black","red","green","yellow","blue","magenta","cyan","white"]
   mut section = ""
   mut start = 0
   mut n = cfg.len
   while start < n {
      mut rel = str.find(str.str_slice(cfg, start, n), "\n")
      mut end = (rel < 0) ? n : (start + rel)
      mut line = str.strip(str.str_slice(cfg, start, end))
      start = end + 1
      if line.len == 0 { continue }
      if load8(line, 0) == 35 { continue }
      def l = str.lower(line)
      if str.find(l, "colors.normal") != -1 || str.startswith(l, "normal:") { section = "normal" }
      elif str.find(l, "colors.bright") != -1 || str.startswith(l, "bright:") { section = "bright" }
      if section != "normal" && section != "bright" { continue }
      def base = (section == "bright") ? 8 : 0
      mut ni = 0
      def names_n = names.len
      while ni < names_n {
         def nm = names.get(ni)
         if str.find(l, nm) != -1 {
            mut pos = str.find(l, "#")
            if pos == -1 { pos = str.find(l, "0x") }
            if pos != -1 {
               mut raw = str.str_slice(line, pos, line.len)
               mut q = str.find(raw, "\"") if q != -1 { raw = str.str_slice(raw, 0, q) }
               q = str.find(raw, "'") if q != -1 { raw = str.str_slice(raw, 0, q) }
               def c = _parse_abgr(raw, 0)
               if c != 0 { store32(p, c, (base + ni) * 4) has_any = true }
            }
            break
         }
         ni += 1
      }
   }
   has_any
}

def OFF_CX             = 0
def OFF_CY             = 4
def OFF_CUR_FG         = 8
def OFF_CUR_BG         = 12
def OFF_CUR_MODE       = 16
def OFF_TOP            = 20
def OFF_BOT            = 24
def OFF_ESC_STATE      = 28
def OFF_MODE           = 32
def OFF_CURSOR_VISIBLE = 36
def OFF_APPKEYS        = 37
def OFF_SAVED_CX       = 38
def OFF_SAVED_CY       = 42
def OFF_SAVED_FG       = 46
def OFF_SAVED_BG       = 50
def OFF_SAVED_MODE     = 54
def OFF_LAST_CHAR_T    = 58
def OFF_LAST_CHAR_C    = 66
def OFF_CSI_PRIV       = 70
def OFF_SEL_ACTIVE     = 74
def OFF_SEL_SX         = 78
def OFF_SEL_SY         = 82
def OFF_SEL_EX         = 86
def OFF_SEL_EY         = 90
def OFF_SCROLL         = 94
def OFF_SEL_DRAGGING   = 98
def OFF_UTF8_LEN       = 102
def OFF_LAST_CLICK_T   = 106
def OFF_CLICK_COUNT    = 114
def OFF_CLICK_X        = 118
def OFF_CLICK_Y        = 122
def OFF_UTF8_BUF       = 128
def OFF_SEL_MOVED      = 144
def OFF_SB_DRAGGING    = 145
def OFF_SCROLL_ACC     = 148
def OFF_SEL_SCROLL_ACC = 152
def OFF_LAST_MX        = 156
def OFF_LAST_MY        = 160
def OFF_SCROLL_F       = 164
def OFF_BRACKET_PASTE  = 168
def OFF_FOCUS_REPORT   = 169
def OFF_MOUSE_FLAGS    = 170
def OFF_MOUSE_BTNDOWN  = 171
def OFF_MOUSE_BTN      = 172
def OFF_KBD_PROTO      = 173
def OFF_ESC_STR_KIND   = 174
def OFF_LAST_KEY_T     = 176
def OFF_LAST_KEY_CODE  = 184
def OFF_CHAR_CB_ACTIVE = 220
def OFF_INSERT         = 221
def OFF_CURSOR_STYLE_S = 222
def OFF_PENDING_PRINT  = 223
def OFF_PENDING_CHAR   = 224
def OFF_PENDING_MOD    = 228
def OFF_PENDING_ACTION = 232
def OFF_PENDING_T      = 236
def OFF_LAST_KEY_MOD   = 188
def OFF_KBD_FLAGS      = 192
def OFF_BACKGROUND_ERASE = 196
def KBD_DISAMBIGUATE   = 1
def KBD_REPORT_EVENTS  = 2
def KBD_REPORT_ALL     = 8
def MOUSE_1000_BTN     = 1
def MOUSE_1002_DRAG    = 2
def MOUSE_1003_MOTION  = 4
def MOUSE_1006_SGR     = 8

fn _vterm_clamp_cursor(dict vt, any st, int co, int ro) any {
   if !st || co <= 0 || ro <= 0 { return 0 }
   mut cx, cy = load32(st, OFF_CX), load32(st, OFF_CY)
   def old_cx, old_cy = cx, cy
   if cx < 0 { cx = 0 } elif cx >= co { cx = co - 1 }
   if cy < 0 { cy = 0 } elif cy >= ro { cy = ro - 1 }
   if cx != old_cx || cy != old_cy {
      store32(st, cx, OFF_CX)
      store32(st, cy, OFF_CY)
   }
   0
}

fn _xterm_mod_param(int md) int {
   mut p = 1
   if (md & MOD_SHIFT) != 0 { p += 1 }
   if (md & MOD_ALT) != 0 { p += 2 }
   if (md & MOD_CONTROL) != 0 { p += 4 }
   p
}

fn _kitty_kbd_mod_param(int md) int {
   mut bits = 0
   if (md & MOD_SHIFT) != 0 { bits = bits | 1 }
   if (md & MOD_ALT) != 0 { bits = bits | 2 }
   if (md & MOD_CONTROL) != 0 { bits = bits | 4 }
   if (md & MOD_SUPER) != 0 { bits = bits | 8 }
   if (md & MOD_META) != 0 { bits = bits | 32 }
   1 + bits
}

fn _kitty_kbd_send(dict vt, int key_code, int md) any {
   def m = _kitty_kbd_mod_param(md)
   if m == 1 { send_input(vt, "\033[" + to_str(key_code) + "u") }
   else { send_input(vt, "\033[" + to_str(key_code) + ";" + to_str(m) + "u") }
   0
}

fn _kitty_kbd_send_event(dict vt, int key_code, int md, int ev_type) any {
   def m = _kitty_kbd_mod_param(md)
   if ev_type <= 1 {
      if m == 1 { send_input(vt, "\033[" + to_str(key_code) + "u") }
      else { send_input(vt, "\033[" + to_str(key_code) + ";" + to_str(m) + "u") }
   } else {
      send_input(vt, "\033[" + to_str(key_code) + ";" + to_str(m) + ":" + to_str(ev_type) + "u")
   }
   0
}

fn _mouse_cell_clamped(dict da, f64 cw, f64 ch, int co, int ro) list {
   def mx, my = float(da.get("x", 0.0)), float(da.get("y", 0.0))
   mut col = int((mx - _px) / cw) + 1
   mut row = int((my - _py) / ch) + 1
   if col < 1 { col = 1 } elif col > co { col = co }
   if row < 1 { row = 1 } elif row > ro { row = ro }
   [col, row]
}

fn _mouse_xy_clamped(dict da, f64 cw, f64 ch, int co, int ro) list {
   mut x = int((float(da.get("x", 0.0)) - _px) / cw)
   mut y = int((float(da.get("y", 0.0)) - _py) / ch)
   if x < 0 { x = 0 } elif x >= co { x = co - 1 }
   if y < 0 { y = 0 } elif y >= ro { y = ro - 1 }
   [x, y]
}

fn _mouse_xterm_button(int native_btn) int {
   if native_btn == 0 { return 0 }
   if native_btn == 2 { return 1 }
   if native_btn == 1 { return 2 }
   native_btn
}

fn _mouse_is_left(int native_btn) bool { native_btn == 0 }

fn _mouse_is_right(int native_btn) bool { native_btn == 1 }

fn _mouse_is_middle(int native_btn) bool { native_btn == 2 }

fn _mouse_send(dict vt, int btn_code, int col, int row, bool is_release) any { send_input(vt, "\x1b[<" + to_str(btn_code) + ";" + to_str(col) + ";" + to_str(row) + (is_release ? "m" : "M")) 0 }

fn _free_ptr(int p) any { if p { free(p) } 0 }

fn _vterm_new_fail(int cache=0, int palette=0, int grid=0, int st=0, int sh=0, int sh_buf=0, int sh_lk=0) int {
   _free_ptr(sh_buf)
   if sh_lk { mutex_free(sh_lk) }
   _free_ptr(sh)
   _free_ptr(st)
   _free_ptr(grid)
   _free_ptr(palette)
   _free_ptr(cache)
   0
}

fn new(int cols, int rows, dict fonts, int bg_color=0, int text_color=0) any {
   "Creates a new virtual terminal instance."
   mut vt = {"cols": cols, "rows": rows, "fonts": fonts}
   def theme_bg = _theme_wal_special_abgr("background", 0)
   def theme_fg = _theme_wal_special_abgr("foreground", 0)
   mut dbg = _env_abgr("NY_TERM_BG", theme_bg ? theme_bg : SYS_BASE)
   mut dfg = _env_abgr("NY_TERM_FG", theme_fg ? theme_fg : SYS_TEXT)
   if bg_color != 0 { dbg = bg_color }
   if text_color != 0 { dfg = text_color }
   mut cache = malloc(1024)
   if !cache { return 0 }
   mut ci = 0 while ci < 128 { store64(cache, str.chr(ci), ci * 8) ci += 1 }
   vt = vt.set("ascii_cache", cache)
   def palette = malloc(256 * 4)
   if !palette { return _vterm_new_fail(cache=cache) }
   _init_palette(palette)
   _maybe_apply_palette_overrides(palette)
   vt = vt.set("palette", palette)
   if bg_color == 0 && text_color == 0 && !common.env_present("NY_TERM_BG") && !common.env_present("NY_TERM_FG") {
      dbg, dfg = theme_bg ? theme_bg : _with_alpha_abgr(_palette_get(palette, 0, dbg), DEFAULT_BG_ALPHA), theme_fg ? theme_fg : _palette_get(palette, 7, dfg)
      def cfb = common.env_trim("COLORFGBG")
      if cfb.len > 0 {
         def semi = str.find(cfb, ";")
         if semi != -1 {
            def sfg, sbg = str.str_slice(cfb, 0, semi), str.str_slice(cfb, semi + 1, cfb.len)
            def ifg, ibg = str.atoi(str.strip(sfg)), str.atoi(str.strip(sbg))
            if ifg >= 0 && ifg < 16 { dfg = _palette_get(palette, ifg, dfg) }
            if theme_bg == 0 && ibg >= 0 && ibg < 16 { dbg = _with_alpha_abgr(_palette_get(palette, ibg, dbg), DEFAULT_BG_ALPHA) }
         }
      }
   }
   dbg = _opaque_abgr(dbg)
   dfg = _opaque_abgr(dfg)
   vt = vt.set("def_bg", dbg).set("def_fg", dfg)
   def bg_l = _abgr_luma(dbg)
   def sel_def = _with_alpha_abgr(_rgb_to_abgr((bg_l < 128) ? 0x6f648f : 0xd9d3eb), (bg_l < 128) ? 112 : 128)
   def cur_def = (bg_l < 128) ? 0x88ffffff : 0x88000000
   def cur_fg_def = (bg_l < 128) ? 0xff000000 : 0xffffffff
   vt = vt
   .set("sel_bg", _env_abgr("NY_TERM_SEL_BG", sel_def))
   .set("cursor_bg", _env_abgr("NY_TERM_CURSOR_BG", cur_def))
   .set("cursor_fg", _env_abgr("NY_TERM_CURSOR_FG", cur_fg_def))
   .set("cursor_blink_enabled", _env_bool_cached("NY_TERM_CURSOR_BLINK", true))
   .set("scroll_follow_enabled", _env_bool_cached("NY_TERM_SCROLL_FOLLOW", true))
   .set("sticky_selection", _env_bool_cached("NY_TERM_STICKY_SELECTION", false))
   .set("synthesize_printable_keys", _env_bool_cached("NY_TERM_SYNTH_PRINTABLE_KEYS", false))
   .set("parse_bytes", common.env_int_clamped("NY_TERM_PARSE_BYTES", 262144, 4096, 4194304))
   .set("repeat_ms", float(common.env_int_clamped("NY_TERM_REPEAT_MS", 0, 0, 500)))
   def cursor_style_raw = common.env_lower("NY_TERM_CURSOR_STYLE")
   mut cursor_style = 2
   if cursor_style_raw == "block" || cursor_style_raw == "box" { cursor_style = 1 }
   elif cursor_style_raw == "underline" || cursor_style_raw == "line" { cursor_style = 3 }
   vt = vt.set("cursor_style", cursor_style)
   def st_tmp = 0
   vt = vt.set("cursor_style_default", cursor_style)
   def bytes = cols * rows * 16
   def grid = malloc(bytes)
   if !grid { return _vterm_new_fail(cache=cache, palette=palette) }
   _clear_grid(grid, cols, rows, dfg, dbg)
   vt = vt.set("grid", grid)
   vt = vt.set("alt_grid", 0)
   def st = malloc(256)
   if !st { return _vterm_new_fail(cache=cache, palette=palette, grid=grid) }
   memset(st, 0, 256)
   store32(st, 0, OFF_CX) store32(st, 0, OFF_CY)
   store32(st, dfg, OFF_CUR_FG) store32(st, dbg, OFF_CUR_BG)
   store32(st, 0, OFF_CUR_MODE) store32(st, 0, OFF_TOP) store32(st, rows - 1, OFF_BOT)
   store32(st, 0, OFF_ESC_STATE) store32(st, MODE_WRAP | MODE_UTF8, OFF_MODE)
   store32_f32(st, 0.0, OFF_SCROLL_F)
   store8(st, 1, OFF_CURSOR_VISIBLE) store8(st, 0, OFF_APPKEYS)
   store8(st, 0, OFF_KBD_PROTO)
   store32(st, 0, OFF_KBD_FLAGS)
   store8(st, _env_bool_cached("NY_TERM_BCE", false) ? 1 : 0, OFF_BACKGROUND_ERASE)
   store8(st, 0, OFF_INSERT)
   store8(st, int(vt.get("cursor_style_default", 2)), OFF_CURSOR_STYLE_S)
   vt = vt
   .set("state", st)
   .set("history", [])
   .set("max_history", 5000)
   .set("last_child_poll_t", 0)
   .set("esc_buf", "")
   .set("title", "Terminal")
   .set("kbd_stack", [])
   .set("kbd_stack_alt", [])
   def kg = {"images": dict(8), "inflight": dict(8), "placements": dict(8), "next_id": 1}
   vt = vt.set("kitty_graphics", kg)
   def fr = fonts.get("regular")
   def cell = _vterm_cell_metrics(fr)
   def cw = cell.get(0, 9.0)
   def ch = cell.get(1, 18.0)
   vt = vt.set("char_w", float(cw)).set("char_h", float(ch))
   vt = vt.set("px_w", int(float(cols) * float(cw))).set("px_h", int(float(rows) * float(ch)))
   def sh = malloc(128)
   if !sh { return _vterm_new_fail(cache=cache, palette=palette, grid=grid, st=st) }
   def sh_lk = mutex_new()
   def sh_buf = malloc(262144)
   if !sh_lk || !sh_buf { return _vterm_new_fail(cache=cache, palette=palette, grid=grid, st=st, sh=sh, sh_buf=sh_buf, sh_lk=sh_lk) }
   memset(sh, 0, 128)
   store64(sh, sh_lk, 0)
   store64(sh, sh_buf, 8)
   store64(sh, 0, 16)
   store64(sh, 262144, 24)
   store64(sh, 0, 32)
   store64(sh, -1, 40)
   store64(sh, 0, 48)
   vt = vt.set("shared", sh)
   vt
}

fn _init_palette(any p) any {
   def ansi16 = [
      0xff000000, 0xff0000cd, 0xff00cd00, 0xff00cdcd,
      0xffcd0000, 0xffcd00cd, 0xffcdcd00, 0xffe5e5e5,
      0xff7f7f7f, 0xff0000ff, 0xff00ff00, 0xff00ffff,
      0xff5c5cff, 0xffff00ff, 0xffffff00, 0xffffffff
   ]
   mut ai = 0
   while ai < ansi16.len {
      store32(p, ansi16.get(ai), ai * 4)
      ai += 1
   }
   mut pi = 16 while pi < 232 {
      def r = (pi - 16) / 36 def g = ((pi - 16) % 36) / 6 def b = (pi - 16) % 6
      store32(p, color_pack(float(r)/5.0, float(g)/5.0, float(b)/5.0, 1.0), pi * 4) pi += 1
   }
   pi = 232 while pi < 256 {
      def v = float(pi - 232) / 24.0
      store32(p, color_pack(v, v, v, 1.0), pi * 4) pi += 1
   }
   0
}

fn _clear_grid(any p, int cols, int rows, int fg, int bg) any {
   mut i = 0 def n = cols * rows
   while i < n {
      def off = i * 16 store32(p, 32, off) store32(p, fg, off + 4)
      store32(p, bg, off + 8) store32(p, 0, off + 12) i += 1
   }
   0
}

fn _ceil_div_f(f64 num, f64 denom) int {
   if denom <= 0.0 { return 0 }
   def v, i = num / denom, int(floor(v))
   (float(i) < v) ? i + 1 : i
}

fn _env_bool_cached(str name, bool fallback) bool {
   def raw = common.env_lower(name)
   if raw.len == 0 { return fallback }
   if raw == "0" || raw == "false" || raw == "off" || raw == "no" { return false }
   if raw == "1" || raw == "true" || raw == "on" || raw == "yes" { return true }
   str.atof(raw) != 0
}

fn _env_float_between(str name, f64 fallback, f64 lo, f64 hi) f64 {
   def raw = common.env_trim(name)
   if raw.len == 0 { return fallback }
   def v = str.atof(raw)
   (v >= lo && v <= hi) ? v : fallback
}

fn _kg_reserve_enabled() bool { _env_bool_cached("NY_KG_RESERVE", true) }

fn _kg_reserve_move_enabled() bool { _env_bool_cached("NY_KG_RESERVE_MOVE", true) }

fn default_shell_args(bool login=false) list {
   "Returns default interactive shell args, disabling login mode in timeout/CI test runs."
   #windows { return ["/Q"] }
   #endif
   if login && !(common.env_present("NY_UI_TIMEOUT") || common.env_present("CI") || common.env_truthy("NYTRIX_TEST_MODE")) { return ["@login", "-i"] }
   ["-i"]
}

fn default_shell_path() str {
   "Returns the preferred shell executable path."
   #windows {
      def comspec = common.env_trim("COMSPEC")
      if comspec.len > 0 && file_exists(comspec) { return comspec }
      def comspec_l = common.env_trim("ComSpec")
      if comspec_l.len > 0 && file_exists(comspec_l) { return comspec_l }
      if file_exists("C:\\Windows\\System32\\cmd.exe") { return "C:\\Windows\\System32\\cmd.exe" }
      return "cmd.exe"
   }
   #endif
   def shell = common.env_trim("SHELL")
   if shell.len > 0 && file_exists(shell) { return shell }
   if file_exists("/bin/sh") { return "/bin/sh" }
   "/bin/bash"
}

fn _vterm_reader_thread(any sh) any {
   def lk = load64(sh, 0) def fd = load64(sh, 40)
   mut t = malloc(32768)
   if !t { store64(sh, 0, 32) return 0 }
   while load64(sh, 32) != 0 {
      def r = __read_off(fd, t, 32768, 0)
      if r == 0 { store64(sh, 0, 32) break }
      elif r < 0 { if r == -11 || r == -4 { msleep(1) continue } store64(sh, 0, 32) break }
      mutex_lock(lk)
      mut end = load64(sh, 16)
      mut cap = load64(sh, 24)
      mut buf = load64(sh, 8)
      mut off = load64(sh, 48)
      if end + r > cap && off > 0 {
         def live = end - off
         if live > 0 { memmove(buf, ptr_add(buf, off), live) }
         end = live
         off = 0
         store64(sh, end, 16)
         store64(sh, off, 48)
      }
      if end + r > cap {
         def new_cap = max(cap * 2, end + r + 65536)
         def new_buf = realloc(buf, new_cap)
         if !new_buf { mutex_unlock(lk) store64(sh, 0, 32) break }
         store64(sh, new_buf, 8)
         store64(sh, new_cap, 24)
         buf, cap = new_buf, new_cap
      }
      memcpy(ptr_add(buf, end), t, r)
      store64(sh, end + r, 16)
      mutex_unlock(lk)
   }
   free(t)
   0
}

fn _vterm_close_child_fds() any {
   "Closes inherited non-stdio file descriptors in the PTY child before exec."
   mut max_fd = common.env_int_clamped("NY_TERM_CHILD_CLOSE_FD_MAX", 4096, 64, 65536)
   mut fd = 3
   while fd < max_fd {
      __close(fd)
      fd += 1
   }
   0
}

fn _env_name(str entry) str {
   def eq = str.find(entry, "=")
   eq < 0 ? entry : str.str_slice(entry, 0, eq)
}

fn _child_env_overrides(list child_env, str inherited) bool {
   def inherited_name = _env_name(inherited)
   if inherited_name.len <= 0 { return false }
   mut i = 0
   def n = child_env.len
   while i < n {
      def name = _env_name(to_str(child_env.get(i)))
      if name == inherited_name { return true }
      i += 1
   }
   false
}

fn _open_pipe(dict vt, str cmd, list args) Result<dict, str> {
   def p = osproc.popen(cmd, args)
   if !p || !is_list(p) || p.len < 3 { return err("pipe spawn failed") }
   def pid = p.get(0, -1)
   def in_fd = p.get(1, -1)
   def out_fd = p.get(2, -1)
   if pid <= 0 || in_fd < 0 || out_fd < 0 { return err("pipe spawn failed") }
   mut nvt = vt.set("master_fd", in_fd)
   nvt = nvt.set("read_fd", out_fd)
   nvt = nvt.set("pid", pid)
   nvt = nvt.set("shell_path", cmd)
   nvt = nvt.set("shell_args", args)
   nvt = nvt.set("pty_mode", "pipe")
   def sh = nvt.get("shared")
   store64(sh, 1, 32)
   store64(sh, out_fd, 40)
   thread_spawn(_vterm_reader_thread, sh)
   ok(nvt)
}

fn open(dict vt, str cmd="/bin/sh", list args=[]) Result<dict, str> {
   "Opens a new PTY and spawns a shell process, connecting it to the virtual terminal."
   mut fds = malloc(8)
   if !fds { return err("openpty fds alloc failed") }
   def res = sys.sys_openpty(fds)
   if is_err(res) {
      free(fds)
      #windows { return _open_pipe(vt, cmd, args) }
      #endif
      return err(to_str(unwrap_or(res, -1)))
   }
   def m = load32(fds, 0) def s = load32(fds, 4) free(fds)
   def _pxw, _pxh = vt.get("px_w", 0), vt.get("px_h", 0)
   _resize_pty(m, vt.get("cols"), vt.get("rows"), _pxw, _pxh)
   __tty_sane_fd(s)
   def pid = __fork()
   if pid == 0 {
      __close(m) __setsid() __dup2(s, 0) __dup2(s, 1) __dup2(s, 2)
      mut _ = __ioctl(s, 0x540E, 0)
      __tty_sane_fd(s)
      if s > 2 { __close(s) }
      _vterm_close_child_fds()
      mut shell_name = cmd
      def last_slash = str.find_last(cmd, "/")
      if last_slash != -1 { shell_name = str.str_slice(cmd, last_slash + 1, cmd.len) }
      mut argv0 = shell_name
      if args.len > 0 && args.get(0) == "@login" { argv0 = "-" + shell_name }
      mut av = [argv0]
      mut j = 0
      def args_n = args.len
      while j < args_n {
         def arg = args.get(j)
         if arg != "@login" { av = av.append(arg) }
         j += 1
      }
      def el = environ()
      def term_override = common.env_trim("NY_TERM_TERM")
      def term_val = (term_override.len > 0) ? term_override : "xterm-256color"
      def window_id = vt.get("window_id", 0)
      def child_env = vt.get("child_env", [])
      def child_env_list = is_list(child_env)
      mut ne, ft = [], false
      mut i = 0
      def el_n = el.len
      while i < el_n {
         def e = el.get(i)
         if child_env_list && _child_env_overrides(child_env, e) { i += 1 continue }
         if str.startswith(e, "TERM=") { ne = ne.append("TERM=" + term_val) ft = true }
         elif str.startswith(e, "COLORTERM=") { ne = ne.append(e) }
         elif str.startswith(e, "NO_COLOR=") { }
         elif str.startswith(e, "CLICOLOR=") { }
         elif str.startswith(e, "CLICOLOR_FORCE=") { }
         elif str.startswith(e, "FORCE_COLOR=") { }
         else { ne = ne.append(e) }
         i += 1
      }
      if !ft && !(child_env_list && _child_env_overrides(child_env, "TERM=")) { ne = ne.append("TERM=" + term_val) }
      if !(child_env_list && _child_env_overrides(child_env, "COLORTERM=")) { ne = ne.append("COLORTERM=truecolor") }
      ne = ne.append("TERM_PROGRAM=nytrix")
      if !(child_env_list && _child_env_overrides(child_env, "CLICOLOR=")) { ne = ne.append("CLICOLOR=1") }
      if !(child_env_list && _child_env_overrides(child_env, "CLICOLOR_FORCE=")) { ne = ne.append("CLICOLOR_FORCE=1") }
      if !(child_env_list && _child_env_overrides(child_env, "FORCE_COLOR=")) { ne = ne.append("FORCE_COLOR=1") }
      if child_env_list {
         mut ei = 0
         def child_env_n = child_env.len
         while ei < child_env_n {
            def entry = to_str(child_env.get(ei))
            if entry.len > 0 { ne = ne.append(entry) }
            ei += 1
         }
      }
      if window_id != 0 {
         ne = ne.append("WINDOWID=" + to_str(window_id))
         ne = ne.append("KITTY_WINDOW_ID=" + to_str(window_id))
      }
      ne = ne.append("KITTY_PID=" + to_str(__getpid()))
      __execve(cmd, av, ne) __exit(1)
   }
   if pid < 0 {
      __close(m)
      __close(s)
      return err(to_str(pid))
   }
   __close(s)
   mut nvt = vt.set("master_fd", m) nvt = nvt.set("pid", pid)
   nvt = nvt.set("shell_path", cmd)
   nvt = nvt.set("shell_args", args)
   def sh = nvt.get("shared") store64(sh, 1, 32) store64(sh, m, 40)
   thread_spawn(_vterm_reader_thread, sh)
   ok(nvt)
}

fn close(any vt) any {
   "Closes the virtual terminal, terminating the associated PTY and freeing resources."
   if !vt || !is_dict(vt) { return 0 }
   def sh = vt.get("shared") store64(sh, 0, 32)
   def kg = vt.get("kitty_graphics", 0)
   if kg && is_dict(kg) {
      def images = kg.get("images", 0)
      if images && is_dict(images) {
         def keys = dict_keys(images)
         mut i = 0
         def keys_n = keys.len
         while i < keys_n {
            def k = keys.get(i)
            def info = images.get(k, 0)
            if info && is_dict(info) { texture_destroy(info.get("tex", 0)) }
            i += 1
         }
      }
   }
   def m = vt.get("master_fd") if m >= 0 { __close(m) }
   def rfd = vt.get("read_fd", -1)
   if rfd >= 0 && rfd != m { __close(rfd) }
   def drain = vt.get("drain_buf", 0)
   if drain { free(drain) }
   0
}

fn _poll_child_exit(dict vt) bool {
   def pid = int(vt.get("pid", 0))
   if pid <= 0 { return false }
   def status_ptr = malloc(8)
   if !status_ptr { return false }
   def wr = __wait4(pid, status_ptr, 1)
   free(status_ptr)
   if wr == pid {
      def sh = vt.get("shared", 0)
      if sh { store64(sh, 0, 32) }
      return true
   }
   false
}

fn is_running(dict vt) bool {
   "Checks if the terminal's underlying process is still running."
   if !vt || !is_dict(vt) { return false }
   def sh = vt.get("shared", 0)
   if !sh { return false }
   if load64(sh, 32) == 0 { return false }
   def now_t = ticks()
   def last_poll_t = vt.get("last_child_poll_t", 0)
   if last_poll_t == 0 || (now_t - last_poll_t) >= 250000000 {
      vt.set("last_child_poll_t", now_t)
      _poll_child_exit(vt)
   }
   load64(sh, 32) != 0
}

fn get_title(any vt) any { "Returns the current title of the terminal window." vt.get("title") }

fn _cursor_blink_enabled(dict vt) bool {
   if !vt || !is_dict(vt) { return false }
   def st = vt.get("state", 0)
   if !st { return false }
   if load8(st, OFF_CURSOR_VISIBLE) == 0 { return false }
   if load32(st, OFF_SCROLL) != 0 { return false }
   def enabled = vt.get("cursor_blink_enabled", true)
   enabled
}

fn cursor_blink_phase(dict vt, int now_ticks=-1) int {
   "Returns the current cursor blink phase(0/1), or -1 when blinking is inactive."
   if now_ticks < 0 { now_ticks = ticks() }
   if !_cursor_blink_enabled(vt) { return -1 }
   int((now_ticks / CURSOR_BLINK_NS) % 2)
}

fn needs_visual_refresh(dict vt, int now_ticks=-1, int last_blink_phase=-1) bool {
   "Returns true when draw() should run again even without new PTY/input data."
   if now_ticks < 0 { now_ticks = ticks() }
   if !vt || !is_dict(vt) { return false }
   def st = vt.get("state", 0)
   if st {
      def target = load32_f32(st, OFF_SCROLL_F)
      def vis = vt.get("scroll_vis", target)
      if abs(target - vis) > 0.01 { return true }
   }
   if last_blink_phase >= 0 {
      def phase = cursor_blink_phase(vt, now_ticks)
      if phase >= 0 && phase != last_blink_phase { return true }
   }
   false
}

fn idle_sleep_ms(dict vt, int now_ticks=-1) int {
   "Returns an idle poll sleep duration that preserves responsiveness while
   avoiding a hot 1-2ms spin loop."
   if now_ticks < 0 { now_ticks = ticks() }
   if !vt || !is_dict(vt) { return 8 }
   def st = vt.get("state", 0)
   if st {
      def target = load32_f32(st, OFF_SCROLL_F)
      def vis = vt.get("scroll_vis", target)
      if abs(target - vis) > 0.01 { return 4 }
   }
   if _cursor_blink_enabled(vt) {
      def phase_ns = now_ticks % CURSOR_BLINK_NS
      def remain_ns = CURSOR_BLINK_NS - phase_ns
      def remain_ms = int(remain_ns / 1000000)
      if remain_ms <= 2 { return 1 }
      if remain_ms <= 8 { return 2 }
      if remain_ms <= 16 { return 4 }
      if remain_ms <= 33 { return 8 }
      return 12
   }
   16
}

fn update(dict vt) dict {
   "Reads new data from the PTY and updates the terminal grid and state."
   mut nvt = vt
   if int(nvt.get("last_update_bytes", 0)) != 0 { nvt = nvt.set("last_update_bytes", 0) }
   def pending_st = nvt.get("state")
   if pending_st { nvt = _vterm_flush_pending_printable(nvt, pending_st) }
   def sh = nvt.get("shared") def lk = load64(sh, 0)
   mutex_lock(lk)
   def end = load64(sh, 16)
   def read_off = load64(sh, 48)
   def avail = end - read_off
   if avail <= 0 {
      if end != 0 || read_off != 0 {
         store64(sh, 0, 16)
         store64(sh, 0, 48)
      }
      mutex_unlock(lk)
      return nvt
   }
   def bp = load64(sh, 8)
   def st = nvt.get("state")
   def max_drain = int(nvt.get("parse_bytes", 262144))
   def sz = (avail > max_drain) ? max_drain : avail
   nvt = nvt.set("last_update_bytes", sz)
   mut t_buf = nvt.get("drain_buf", 0)
   mut t_cap = nvt.get("drain_cap", 0)
   if t_cap < sz {
      def new_buf = t_buf ? realloc(t_buf, sz) : malloc(sz)
      if !new_buf { mutex_unlock(lk) return nvt }
      t_buf = new_buf
      t_cap = sz
      nvt = nvt.set("drain_buf", t_buf)
      nvt = nvt.set("drain_cap", t_cap)
   }
   memcpy(t_buf, ptr_add(bp, read_off), sz)
   def next_off = read_off + sz
   if next_off >= end {
      store64(sh, 0, 16)
      store64(sh, 0, 48)
   } else {
      store64(sh, next_off, 48)
   }
   mutex_unlock(lk)
   if sz > 0 { _vterm_clear_selection_for_output(st) }
   mut u_len = load32(st, OFF_UTF8_LEN)
   mut p = 0
   mut co, ro = nvt.get("cols"), nvt.get("rows")
   mut g, pal = nvt.get("grid"), nvt.get("palette")
   mut dfg, dbg = nvt.get("def_fg"), nvt.get("def_bg")
   _vterm_clamp_cursor(nvt, st, co, ro)
   while p < sz {
      def b = load8(t_buf, p) & 255
      if u_len > 0 {
         if (b & 0xC0) == 0x80 {
            store8(st + OFF_UTF8_BUF, b, u_len)
            u_len += 1
            def b0 = load8(st + OFF_UTF8_BUF, 0) & 255
            mut exp = 0
            if (b0 & 0xE0) == 0xC0 { exp = 2 }
            elif (b0 & 0xF0) == 0xE0 { exp = 3 }
            elif (b0 & 0xF8) == 0xF0 { exp = 4 }
            if exp == u_len {
               def old_vt = nvt
               nvt = _tputc_fast(nvt, st, co, ro, g, pal, dfg, dbg, str._utf8_decode_at(st + OFF_UTF8_BUF, 0, u_len))
               u_len = 0
               if (nvt != old_vt) && nvt.get("grid") != g {
                  co, ro = nvt.get("cols"), nvt.get("rows")
                  g, pal = nvt.get("grid"), nvt.get("palette")
                  dfg, dbg = nvt.get("def_fg"), nvt.get("def_bg")
               }
            } elif u_len >= 4 || exp == 0 {
               def old_vt = nvt
               nvt = _tputc_fast(nvt, st, co, ro, g, pal, dfg, dbg, 63)
               u_len = 0
               if (nvt != old_vt) && nvt.get("grid") != g {
                  co, ro = nvt.get("cols"), nvt.get("rows")
                  g, pal = nvt.get("grid"), nvt.get("palette")
                  dfg, dbg = nvt.get("def_fg"), nvt.get("def_bg")
               }
            }
         } else {
            def old_vt = nvt
            nvt = _tputc_fast(nvt, st, co, ro, g, pal, dfg, dbg, 63)
            u_len = 0 p -= 1
            if (nvt != old_vt) && nvt.get("grid") != g {
               co, ro = nvt.get("cols"), nvt.get("rows")
               g, pal = nvt.get("grid"), nvt.get("palette")
               dfg, dbg = nvt.get("def_fg"), nvt.get("def_bg")
            }
         }
      } else {
         if load32(st, OFF_ESC_STATE) == 0 && load8(st, OFF_INSERT) == 0 && b >= 32 && b < 127 {
            mut q = p + 1
            while q < sz {
               def nb = load8(t_buf, q) & 255
               if nb < 32 || nb >= 127 { break }
               q += 1
            }
            nvt = _tput_ascii_run_fast(nvt, st, co, ro, g, dfg, dbg, t_buf, p, q - p)
            p = q
            continue
         }
         if (b & 0x80) == 0 {
            def old_vt = nvt
            nvt = _tputc_fast(nvt, st, co, ro, g, pal, dfg, dbg, b)
            if (nvt != old_vt) && nvt.get("grid") != g {
               co, ro = nvt.get("cols"), nvt.get("rows")
               g, pal = nvt.get("grid"), nvt.get("palette")
               dfg, dbg = nvt.get("def_fg"), nvt.get("def_bg")
            }
         } elif (b & 0xE0) == 0xC0 || (b & 0xF0) == 0xE0 || (b & 0xF8) == 0xF0 {
            store8(st + OFF_UTF8_BUF, b, 0)
            u_len = 1
         } else {
            def old_vt = nvt
            nvt = _tputc_fast(nvt, st, co, ro, g, pal, dfg, dbg, 63)
            if (nvt != old_vt) && nvt.get("grid") != g {
               co, ro = nvt.get("cols"), nvt.get("rows")
               g, pal = nvt.get("grid"), nvt.get("palette")
               dfg, dbg = nvt.get("def_fg"), nvt.get("def_bg")
            }
         }
      }
      p += 1
   }
   store32(st, u_len, OFF_UTF8_LEN)
   nvt
}

fn _kg_draw_stripe(dict vt,
   dict images,
   int id24,
   int id4plus1,
   int img_row,
   int img_col_start,
   int img_col_end,
   int screen_x_cell,
   f64 screen_y_pix,
   f64 cw,
   f64 ch) any{
   "Draws a horizontal stripe of a Kitty graphics image."
   if img_row <= 0 || img_col_start <= 0 || img_col_end < img_col_start { return 0 }
   def msb = (id4plus1 > 0) ? (id4plus1 - 1) : 0
   def image_id = id24 | (msb << 24)
   def info = images.get(image_id, 0)
   if !info || !is_dict(info) { return 0 }
   def tex = info.get("tex", 0)
   if !tex { return 0 }
   mut total_cols = info.get("cols", 0)
   mut total_rows = info.get("rows", 0)
   if total_cols <= 0 || total_rows <= 0 { return 0 }
   def fit_x, fit_y = info.get("fit_x", -1.0), info.get("fit_y", -1.0)
   def fit_w, fit_h = info.get("fit_w", 0.0), info.get("fit_h", 0.0)
   if fit_w > 0.0 && fit_h > 0.0 && fit_x >= 0.0 && fit_y >= 0.0 {
      def box_x, box_y = float(screen_x_cell - (img_col_start - 1)) * cw + _px, screen_y_pix - float(img_row - 1) * ch
      def sx1, sx2 = float(img_col_start - 1) * cw, float(img_col_end) * cw
      def sy1, sy2 = float(img_row - 1) * ch, float(img_row) * ch
      def ix1, ix2 = max(sx1, fit_x), min(sx2, fit_x + fit_w)
      def iy1, iy2 = max(sy1, fit_y), min(sy2, fit_y + fit_h)
      if ix2 <= ix1 || iy2 <= iy1 { return 0 }
      def u1, u2 = (ix1 - fit_x) / fit_w, (ix2 - fit_x) / fit_w
      def v1, v2 = (iy1 - fit_y) / fit_h, (iy2 - fit_y) / fit_h
      def dx, dy = box_x + ix1, box_y + iy1
      def dw, dh = ix2 - ix1, iy2 - iy1
      draw_rect_tex_uv(dx, dy, dw, dh, tex, u1, v1, u2, v2, 1.0, 1.0, 1.0, 1.0)
      return 0
   }
   def u1, u2 = float(img_col_start - 1) / float(total_cols), float(img_col_end) / float(total_cols)
   def v1, v2 = float(img_row - 1) / float(total_rows), float(img_row) / float(total_rows)
   def dx, dy = float(screen_x_cell) * cw + _px, screen_y_pix
   def dw, dh = float(img_col_end - img_col_start + 1) * cw, ch
   draw_rect_tex_uv(dx, dy, dw, dh, tex, u1, v1, u2, v2, 1.0, 1.0, 1.0, 1.0)
   0
}

fn _kg_draw_image_run(dict vt,
   dict images,
   any line_ptr,
   f64 y_pix,
   int x1,
   int x2,
   int id24,
   int co,
   f64 cw,
   f64 ch) any{
   "Processes a run of image placeholder cells and draws the corresponding image stripes."
   mut last_row = 0
   mut last_col = 0
   mut last_id4 = 0
   mut stripe_start_x = x1
   mut stripe_start_col = 0
   if x1 > 0 {
      def offp = (x1 - 1) * 16
      def mp = load32(line_ptr, offp + 12)
      if (mp & ATTR_IMAGE) != 0 {
         def fgp = load32(line_ptr, offp + 4)
         if _img_id24_from_abgr(fgp) == id24 {
            def cpp = load32(line_ptr, offp)
            last_row = _img_row(cpp)
            last_col = _img_col(cpp)
            last_id4 = _img_id4plus1(cpp)
            if last_col > 0 { stripe_start_col = last_col + 1 }
         }
      }
   }
   mut x = x1
   while x < x2 {
      def off = x * 16
      mut cp = load32(line_ptr, off)
      mut cur_row = _img_row(cp)
      mut cur_col = _img_col(cp)
      mut cur_id4 = _img_id4plus1(cp)
      def diacc = _img_diacritic_count(cp)
      if last_row > 0 && (diacc == 0 || cur_row == 0) { cur_row = last_row }
      if last_col > 0 && (diacc <= 1 || cur_col == 0) && cur_row == last_row { cur_col = last_col + 1 }
      if last_id4 > 0 && (diacc <= 2 || cur_id4 == 0) && cur_row == last_row && cur_col == last_col + 1 { cur_id4 = last_id4 }
      if cur_row == 0 { cur_row = 1 }
      if cur_col == 0 { cur_col = 1 }
      if stripe_start_col == 0 {
         stripe_start_col = cur_col
         stripe_start_x = x
      }
      if last_row > 0 && (cur_row != last_row || cur_col != last_col + 1 || cur_id4 != last_id4) {
         _kg_draw_stripe(vt,
            images,
            id24,
            last_id4,
            last_row,
            stripe_start_col,
            last_col,
            stripe_start_x,
            y_pix,
            cw,
         ch)
         stripe_start_col = cur_col
         stripe_start_x = x
      }
      last_row = cur_row
      last_col = cur_col
      last_id4 = cur_id4
      if _img_row(cp) == 0 { cp = _img_set_row(cp, cur_row) }
      if _img_col(cp) == 0 && (cur_col & ~0x1FF) == 0 { cp = _img_set_col(cp, cur_col) }
      if _img_id4plus1(cp) == 0 { cp = _img_set_id4plus1(cp, cur_id4) }
      store32(line_ptr, cp, off)
      x += 1
   }
   if last_row > 0 { _kg_draw_stripe(vt, images, id24, last_id4, last_row, stripe_start_col, last_col, stripe_start_x, y_pix, cw, ch) }
   0
}

fn _kg_draw_line_images(dict vt, dict images, any line_ptr, f64 y_pix, int co, f64 cw, f64 ch) any {
   mut x = 0
   while x < co {
      def off = x * 16
      def m = load32(line_ptr, off + 12)
      if (m & ATTR_IMAGE) == 0 { x += 1 continue }
      def fg = load32(line_ptr, off + 4)
      def id24 = _img_id24_from_abgr(fg)
      mut run_end = x + 1
      while run_end < co {
         def o2, m2 = run_end * 16, load32(line_ptr, o2 + 12)
         if (m2 & ATTR_IMAGE) == 0 { break }
         def fg2 = load32(line_ptr, o2 + 4)
         if _img_id24_from_abgr(fg2) != id24 { break }
         run_end += 1
      }
      _kg_draw_image_run(vt, images, line_ptr, y_pix, x, run_end, id24, co, cw, ch)
      x = run_end
   }
   0
}

fn _kg_draw_visible_images(dict vt,
   int co,
   int ro,
   any grid,
   list history,
   int hist_len,
   int scroll_off,
   f64 scroll_frac,
   f64 cw,
   f64 ch,
   f64 wh) any{
   "Draws all Kitty graphics images that are currently visible on the screen."
   def kg = vt.get("kitty_graphics", 0)
   if !kg || !is_dict(kg) { return 0 }
   def images = kg.get("images", 0)
   if !images || !is_dict(images) || images.len == 0 { return 0 }
   def placements = kg.get("placements", 0)
   if !_kg_reserve_enabled() && placements && is_dict(placements) {
      def pkeys = dict_keys(placements)
      mut pi = 0
      def pkeys_n = pkeys.len
      while pi < pkeys_n {
         def k = pkeys.get(pi)
         def pl = placements.get(k, 0)
         if pl && is_dict(pl) {
            def image_id = pl.get("image_id", 0)
            def info = images.get(image_id, 0)
            if info && is_dict(info) {
               def tex = info.get("tex", 0)
               if tex {
                  def cx, cy = pl.get("x", 0), pl.get("y", 0)
                  def cols = pl.get("cols", 0)
                  def rows = pl.get("rows", 0)
                  def x_off = pl.get("x_off", 0)
                  def y_off = pl.get("y_off", 0)
                  if cols > 0 && rows > 0 {
                     def dx, dy = float(cx) * cw + _px + float(x_off), float(cy) * ch + _py + float(y_off)
                     def dw, dh = float(cols) * cw, float(rows) * ch
                     def iw, ih = info.get("w", 0), info.get("h", 0)
                     if iw > 0 && ih > 0 {
                        def sx, sy = dw / float(iw), dh / float(ih)
                        def sc = (sx < sy) ? sx : sy
                        def fit_w = float(iw) * sc
                        def fit_h = float(ih) * sc
                        def fit_x, fit_y = 0.0, 0.0
                        draw_rect_tex_uv(dx + fit_x,
                           dy + fit_y,
                           fit_w,
                           fit_h,
                           tex,
                           0.0,
                           0.0,
                           1.0,
                           1.0,
                           1.0,
                           1.0,
                           1.0,
                        1.0)
                     } else {
                        draw_rect_tex_uv(dx, dy, dw, dh, tex, 0.0, 0.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0)
                     }
                  }
               }
            }
         }
         pi += 1
      }
   }
   mut r = -1
   while r <= ro {
      def abs_r = (hist_len - scroll_off) + r
      if abs_r < 0 || abs_r >= hist_len + ro { r += 1 continue }
      mut line_ptr = 0
      if abs_r < hist_len { line_ptr = history.get(abs_r) }
      else { line_ptr = grid + ((abs_r - hist_len) * co * 16) }
      def ry = (float(r) + scroll_frac) * ch + _py
      if ry + ch < _py || ry > _py + wh { r += 1 continue }
      _kg_draw_line_images(vt, images, line_ptr, ry, co, cw, ch)
      r += 1
   }
   0
}

fn _wcwidth(int u) int {
   if u < 32 || (u >= 0x7f && u < 0xa0) { return -1 }
   if (u >= 0x0300 && u <= 0x036F) || (u >= 0x0483 && u <= 0x0489) ||
   (u >= 0x0591 && u <= 0x05BD) || u == 0x05BF || (u >= 0x05C1 && u <= 0x05C2) ||
   u == 0x05C4 || (u >= 0x064B && u <= 0x0655) || u == 0x0670 ||
   (u >= 0x06D6 && u <= 0x06DC) || (u >= 0x06DD && u <= 0x06DF) ||
   (u >= 0x06E0 && u <= 0x06E4) || (u >= 0x06E7 && u <= 0x06E8) ||
   (u >= 0x06EA && u <= 0x06ED) || u == 0x0711 || (u >= 0x0730 && u <= 0x074A) ||
   (u >= 0x07A6 && u <= 0x07B0) || (u >= 0x07EB && u <= 0x07F3) ||
   (u >= 0x0901 && u <= 0x0902) || u == 0x093C || (u >= 0x0941 && u <= 0x0948) ||
   u == 0x094D || (u >= 0x0951 && u <= 0x0954) || (u >= 0x0962 && u <= 0x0963) ||
   u == 0x0981 || u == 0x09BC || (u >= 0x09C1 && u <= 0x09C4){ return 0 }
   if u == 0x09CD || (u >= 0x09E2 && u <= 0x09E3) || u == 0x0A02 || u == 0x0A3C ||
   (u >= 0x0A41 && u <= 0x0A42) || (u >= 0x0A47 && u <= 0x0A48) ||
   (u >= 0x0A4B && u <= 0x0A4D) || (u >= 0x0A70 && u <= 0x0A71) ||
   (u >= 0x0A81 && u <= 0x0A82) || u == 0x0ABC || (u >= 0x0AC1 && u <= 0x0AC5) ||
   (u >= 0x0AC7 && u <= 0x0AC8) || u == 0x0ACD || (u >= 0x0AE2 && u <= 0x0AE3) ||
   u == 0x0B01 || u == 0x0B3C || u == 0x0B3F || (u >= 0x0B41 && u <= 0x0B43) ||
   u == 0x0B4D || u == 0x0B56 || u == 0x0B82 || u == 0x0BC0 || u == 0x0BCD ||
   (u >= 0x0C3E && u <= 0x0C40) || (u >= 0x0C46 && u <= 0x0C48) ||
   (u >= 0x0C4A && u <= 0x0C4D) || (u >= 0x0C55 && u <= 0x0C56){ return 0 }
   if u == 0x0CBF || u == 0x0CC6 || (u >= 0x0CCA && u <= 0x0CCB) ||
   (u >= 0x0CD5 && u <= 0x0CD6) || (u >= 0x0D41 && u <= 0x0D43) ||
   u == 0x0D4D || u == 0x0DCA || (u >= 0x0DD2 && u <= 0x0DD4) || u == 0x0DD6 ||
   u == 0x0E31 || (u >= 0x0E34 && u <= 0x0E3A) || (u >= 0x0E47 && u <= 0x0E4E) ||
   u == 0x0EB1 || (u >= 0x0EB4 && u <= 0x0EB9) || (u >= 0x0EBB && u <= 0x0EBC) ||
   (u >= 0x0EC8 && u <= 0x0ECD) || (u >= 0x0F18 && u <= 0x0F19) ||
   u == 0x0F35 || u == 0x0F37 || u == 0x0F39 || (u >= 0x0F71 && u <= 0x0F7E) ||
   (u >= 0x0F80 && u <= 0x0F84) || (u >= 0x0F86 && u <= 0x0F87) ||
   (u >= 0x0F90 && u <= 0x0F97) || (u >= 0x0F99 && u <= 0x0FBC){ return 0 }
   if u == 0x0FC6 || (u >= 0x102D && u <= 0x1030) || u == 0x1032 ||
   (u >= 0x1036 && u <= 0x1037) || u == 0x1039 || (u >= 0x1058 && u <= 0x1059) ||
   (u >= 0x1160 && u <= 0x11FF) || (u >= 0x1712 && u <= 0x1714) ||
   (u >= 0x1732 && u <= 0x1734) || (u >= 0x1752 && u <= 0x1753) ||
   (u >= 0x1772 && u <= 0x1773) || (u >= 0x17B4 && u <= 0x17B5) ||
   (u >= 0x17B7 && u <= 0x17BD) || u == 0x17C6 || (u >= 0x17C9 && u <= 0x17D3) ||
   u == 0x17DD || (u >= 0x180B && u <= 0x180D) || u == 0x18A9 ||
   (u >= 0x1920 && u <= 0x1922) || (u >= 0x1927 && u <= 0x1928){ return 0 }
   if u == 0x1932 || (u >= 0x1939 && u <= 0x193B) || (u >= 0x1A17 && u <= 0x1A18) ||
   (u >= 0x1B00 && u <= 0x1B03) || u == 0x1B34 || (u >= 0x1B36 && u <= 0x1B3A) ||
   u == 0x1B3C || u == 0x1B42 || (u >= 0x1B6B && u <= 0x1B73) ||
   (u >= 0x1DC0 && u <= 0x1DCA) || (u >= 0x1DFE && u <= 0x1DFF) ||
   (u >= 0x200B && u <= 0x200F) || (u >= 0x202A && u <= 0x202E) ||
   (u >= 0x2060 && u <= 0x2063) || (u >= 0x206A && u <= 0x206F) ||
   (u >= 0x20D0 && u <= 0x20EF) || (u >= 0x302A && u <= 0x302F) ||
   (u >= 0x3099 && u <= 0x309A) || u == 0xFB1E || (u >= 0xFE00 && u <= 0xFE0F) ||
   (u >= 0xFE20 && u <= 0xFE23) || u == 0xFEFF || (u >= 0xFFF9 && u <= 0xFFFB){ return 0 }
   if (u >= 0x1D167 && u <= 0x1D169) || (u >= 0x1D173 && u <= 0x1D17A) ||
   (u >= 0x1D185 && u <= 0x1D18B) || (u >= 0x1D1AA && u <= 0x1D1AD) ||
   (u >= 0x1D242 && u <= 0x1D244) || u == 0xE0001 ||
   (u >= 0xE0020 && u <= 0xE007F) || (u >= 0xE0100 && u <= 0xE01EF){
      return 0
   }
   if u >= 0x1F1E6 && u <= 0x1F1FF { return 2 }
   if u >= 0x1100 && u <= 0x115F { return 2 }
   if u == 0x2329 || u == 0x232A { return 2 }
   if u >= 0x2E80 && u <= 0x303E { return 2 }
   if u >= 0x3041 && u <= 0x33FF { return 2 }
   if u >= 0x3400 && u <= 0x4DBF { return 2 }
   if u >= 0x4E00 && u <= 0x9FFF { return 2 }
   if u >= 0xA000 && u <= 0xA4CF { return 2 }
   if u >= 0xAC00 && u <= 0xD7A3 { return 2 }
   if u >= 0xF900 && u <= 0xFAFF { return 2 }
   if u >= 0xFE10 && u <= 0xFE19 { return 2 }
   if u >= 0xFE30 && u <= 0xFE6F { return 2 }
   if u >= 0xFF00 && u <= 0xFF60 { return 2 }
   if u >= 0xFFE0 && u <= 0xFFE6 { return 2 }
   if u >= 0x20000 && u <= 0x2FFFD { return 2 }
   if u >= 0x30000 && u <= 0x3FFFD { return 2 }
   if u >= 0xE000 && u <= 0xF8FF { return 1 }
   if u >= 0x2500 && u <= 0x28FF { return 1 }
   if u >= 0x1F300 && u <= 0x1FAFF { return 2 }
   return 1
}

fn _is_space_like(int u) bool {
   if u == 0x00A0 { return true }
   if u == 0x1680 { return true }
   if u >= 0x2000 && u <= 0x200A { return true }
   if u == 0x202F { return true }
   if u == 0x205F { return true }
   if u == 0x3000 { return true }
   if u == 0x200B { return true }
   false
}

fn _snap_scroll_frac(f64 frac, f64 ch) f64 {
   if ch <= 0.0 { return frac }
   mut px = frac * ch
   mut ipx = 0
   if px >= 0.0 { ipx = int(floor(px + 0.5)) }
   else { ipx = -int(floor(-px + 0.5)) }
   mut out = float(ipx) / ch
   if abs(out) < 0.001 { out = 0.0 }
   if out > 0.99 { out = 0.99 } elif out < -0.99 { out = -0.99 }
   out
}

fn _scroll_process_scaled(f64 dy_raw, f64 default_scale, str scale_env_key, f64 default_max, any max_env_key=0) f64 {
   def scale = scale_env_key ? _env_float_between(scale_env_key, default_scale, 0.01, 100.0) : default_scale
   def inv = _env_bool_cached("NY_TERM_SCROLL_INVERT", false)
   mut dy = dy_raw * scale
   if inv { dy = -dy }
   def max_step = _env_float_between(max_env_key ? max_env_key : "NY_TERM_SCROLL_MAX", default_max, 1.0, 200.0)
   if dy > max_step { dy = max_step }
   if dy < -max_step { dy = -max_step }
   dy
}

fn _scroll_process(f64 dy_raw) f64 { _scroll_process_scaled(dy_raw, 3.0, "NY_TERM_SCROLL_SCALE", 30.0, "NY_TERM_SCROLL_MAX") }

fn _scroll_process_app(f64 dy_raw) f64 {
   "App/TUI scrolling: keep wheel semantics near 1 input step per detent.
   This avoids oversending mouse-wheel or arrow events under Wayland/Xwayland."
   _scroll_process_scaled(dy_raw, 1.0, "NY_TERM_APP_SCROLL_SCALE", 8.0, "NY_TERM_APP_SCROLL_MAX")
}

fn _vterm_handle_mouse_scroll(dict vt, any st, int ro, int hist_len, f64 cw, f64 ch, any da) dict {
   if is_dict(da) {
      if da.contains("x") { store32_f32(st, float(da.get("x", 0.0)), OFF_LAST_MX) }
      if da.contains("y") { store32_f32(st, float(da.get("y", 0.0)), OFF_LAST_MY) }
   }
   if _vterm_has_selection_state(st) {
      _vterm_clear_selection(st)
      _vterm_reset_click_sequence(st)
   }
   def dy_raw = float(da.get("dy", 0.0))
   def mode = load32(st, OFF_MODE)
   if (mode & MODE_MOUSE) != 0 && (da.get("mod", 0) & MOD_SHIFT) == 0 {
      def dy = _scroll_process_app(dy_raw)
      def lmx, lmy = load32_f32(st, OFF_LAST_MX), load32_f32(st, OFF_LAST_MY)
      def mcell = _mouse_cell_clamped({"x": lmx, "y": lmy}, cw, ch, int(vt.get("cols")), ro)
      def mcol = mcell.get(0)
      def mrow = mcell.get(1)
      def acc = load32_f32(st, OFF_SCROLL_ACC)
      mut nacc = acc + dy
      mut dcells = int(nacc)
      nacc -= float(dcells)
      store32_f32(st, nacc, OFF_SCROLL_ACC)
      def btn = (dcells > 0) ? 64 : 65
      _send_input_repeat(vt, "\x1b[<" + to_str(btn) + ";" + to_str(mcol) + ";" + to_str(mrow) + "M", abs(dcells))
      return vt
   }
   if (mode & MODE_ALTSCREEN) != 0 {
      def dy = _scroll_process_app(dy_raw)
      def acc = load32_f32(st, OFF_SCROLL_ACC)
      mut nacc = acc + dy
      mut dcells = int(nacc)
      nacc -= float(dcells)
      store32_f32(st, nacc, OFF_SCROLL_ACC)
      def appk = load8(st, OFF_APPKEYS) != 0
      def up_seq = appk ? "\033OA" : "\033[A"
      def down_seq = appk ? "\033OB" : "\033[B"
      _send_input_repeat(vt, (dcells > 0) ? up_seq : down_seq, abs(dcells))
      return vt
   }
   def dy = _scroll_process(dy_raw)
   mut off_f = load32_f32(st, OFF_SCROLL_F)
   off_f += dy
   if off_f < 0.0 { off_f = 0.0 }
   if off_f > float(hist_len) { off_f = float(hist_len) }
   store32_f32(st, off_f, OFF_SCROLL_F)
   store32(st, int(floor(off_f)), OFF_SCROLL)
   store32_f32(st, 0.0, OFF_SCROLL_ACC)
   vt
}

fn _tputc_fast(dict vt, any st, int co, int ro, any g, any pal, int dfg, int dbg, int u) any {
   if u >= 0xA0 && _is_space_like(u) { u = 32 }
   mut s_state = load32(st, OFF_ESC_STATE)
   if s_state == 0 {
      if u < 32 || u == 127 { return _tcontrolcode_fast(vt, st, co, ro, g, pal, dfg, dbg, u) }
      else { return _tputc_raw_fast(vt, st, co, ro, g, u) }
   } elif s_state == ESC_START {
      if u == 91 { store32(st, ESC_CSI, OFF_ESC_STATE) store8(st, 0, OFF_CSI_PRIV) return vt.set("esc_buf", "") }
      if _esc_is_altcharset(u) { store32(st, ESC_ALTCHARSET, OFF_ESC_STATE) return vt }
      if _esc_is_string_start(u) {
         store8(st, u, OFF_ESC_STR_KIND)
         store32(st, ESC_STR, OFF_ESC_STATE)
         return vt.set("esc_buf", "")
      }
      if u == 99 {
         _tresetattr_fast(st, dfg, dbg) _tclearregion_fast(st, co, ro, g, dfg, dbg, 0, 0, co - 1, ro - 1)
         store32(st, 0, OFF_CX) store32(st, 0, OFF_CY)
         store32(st, 0, OFF_TOP) store32(st, ro - 1, OFF_BOT)
         store32(st, MODE_WRAP | MODE_UTF8, OFF_MODE) store32(st, 0, OFF_ESC_STATE) return vt
      }
      if u == 68 { mut nvt = _tnewline_fast(vt, st, co, ro, g, dfg, dbg, false)
         store32(st, 0, OFF_ESC_STATE)
      return nvt }
      if u == 69 {
         mut nvt = _tnewline_fast(vt, st, co, ro, g, dfg, dbg, true)
         store32(st, 0, OFF_ESC_STATE)
         return nvt
      }
      if u == 77 {
         mut cy = load32(st, OFF_CY)
         mut nvt = vt
         if cy == load32(st, OFF_TOP) { nvt = _tscrolldown_fast(vt, st, co, ro, g, dfg, dbg, load32(st, OFF_TOP), 1) }
         else { store32(st, cy - 1, OFF_CY) }
         store32(st, 0, OFF_ESC_STATE) return nvt
      }
      if u == 55 {
         store32(st, load32(st, OFF_CX), OFF_SAVED_CX) store32(st, load32(st, OFF_CY), OFF_SAVED_CY)
         store32(st, load32(st, OFF_CUR_FG), OFF_SAVED_FG) store32(st, load32(st, OFF_CUR_BG), OFF_SAVED_BG)
         store32(st, load32(st, OFF_CUR_MODE), OFF_SAVED_MODE) store32(st, 0, OFF_ESC_STATE) return vt
      }
      if u == 56 {
         store32(st, load32(st, OFF_SAVED_CX), OFF_CX) store32(st, load32(st, OFF_SAVED_CY), OFF_CY)
         store32(st, load32(st, OFF_SAVED_FG), OFF_CUR_FG) store32(st, load32(st, OFF_SAVED_BG), OFF_CUR_BG)
         store32(st, load32(st, OFF_SAVED_MODE), OFF_CUR_MODE) store32(st, 0, OFF_ESC_STATE) return vt
      }
      store32(st, 0, OFF_ESC_STATE) return vt
   } elif s_state == ESC_CSI {
      if u >= 64 && u <= 126 {
         mut nvt = _tcsihandle_fast(vt, st, co, ro, g, pal, dfg, dbg, u, vt.get("esc_buf"))
         store32(st, 0, OFF_ESC_STATE)
         store8(st, 0, OFF_CSI_PRIV)
         return nvt
      }
      else {
         if u >= 60 && u <= 63 {
            store8(st, u, OFF_CSI_PRIV)
            return vt.set("esc_buf", str.chr(u))
         }
         if (u >= 48 && u <= 57) || u == 59 || u == 58 { return vt.set("esc_buf", vt.get("esc_buf") + str.chr(u)) }
         return vt
      }
   } elif s_state == ESC_STR {
      def kind = load8(st, OFF_ESC_STR_KIND)
      if u == 0x9c || (kind == 93 && u == 7) {
         mut nvt = _tstrhandle(vt, kind, vt.get("esc_buf"))
         store32(st, 0, OFF_ESC_STATE)
         store8(st, 0, OFF_ESC_STR_KIND)
         return nvt
      } elif u == 27 {
         store32(st, ESC_STR_ESC, OFF_ESC_STATE)
         return vt
      } elif u == 0x18 || u == 0x1a {
         store32(st, 0, OFF_ESC_STATE)
         store8(st, 0, OFF_ESC_STR_KIND)
         return vt
      } else {
         return vt.set("esc_buf", vt.get("esc_buf") + str.chr(u))
      }
   } elif s_state == ESC_STR_ESC {
      def kind = load8(st, OFF_ESC_STR_KIND)
      if u == 92 {
         mut nvt = _tstrhandle(vt, kind, vt.get("esc_buf"))
         store32(st, 0, OFF_ESC_STATE)
         store8(st, 0, OFF_ESC_STR_KIND)
         return nvt
      }
      mut nvt = _tstrhandle(vt, kind, vt.get("esc_buf"))
      store32(st, ESC_START, OFF_ESC_STATE)
      store8(st, 0, OFF_ESC_STR_KIND)
      nvt = nvt.set("esc_buf", "")
      return _tputc_fast(nvt, st, co, ro, g, pal, dfg, dbg, u)
   }
   store32(st, 0, OFF_ESC_STATE) vt
}

fn _tcontrolcode_fast(dict vt, any st, int co, int ro, any g, any pal, int dfg, int dbg, int u) any {
   mut nvt = vt
   if u == 7 { return vt }
   elif u == 8 {
      mut cx = load32(st, OFF_CX) if cx > 0 { store32(st, cx - 1, OFF_CX) }
   } elif u == 9 {
      mut cx = load32(st, OFF_CX) cx = (cx + 8) & ~7 if cx >= co { cx = co - 1 } store32(st, cx, OFF_CX)
   } elif u == 10 || u == 11 || u == 12 {
      nvt = _tnewline_fast(vt, st, co, ro, g, dfg, dbg, false)
   } elif u == 13 {
      store32(st, 0, OFF_CX)
   } elif u == 14 || u == 15 {
      return vt
   } elif u == 24 || u == 26 {
      store32(st, 0, OFF_ESC_STATE)
   } elif u == 27 {
      store32(st, ESC_START, OFF_ESC_STATE) return vt
   }
   nvt
}

fn _tnewline_fast(dict vt, any st, int co, int ro, any g, int dfg, int dbg, bool first_col) any {
   mut cy = load32(st, OFF_CY)
   mut nvt = vt
   if cy == load32(st, OFF_BOT) { nvt = _tscrollup_fast(vt, st, co, ro, g, dfg, dbg, load32(st, OFF_TOP), 1) cy = load32(st, OFF_CY) }
   else { store32(st, cy + 1, OFF_CY) cy += 1 }
   if first_col { store32(st, 0, OFF_CX) }
   nvt
}

fn _tmoveto_fast(any st, int co, int ro, int x, int y) any {
   mut nx, ny = x, y if nx < 0 { nx = 0 } elif nx >= co { nx = co - 1 }
   if ny < 0 { ny = 0 } elif ny >= ro { ny = ro - 1 }
   store32(st, nx, OFF_CX) store32(st, ny, OFF_CY)
   0
}

def _IMG_SHIFT_ROW = 0
def _IMG_SHIFT_COL = 9
def _IMG_SHIFT_ID4 = 18
def _IMG_SHIFT_DIACRITIC = 27
def _IMG_MASK_9 = 0x1FF
def _IMG_MASK_2 = 0x3

fn _img_row(int cp) int { (cp >> _IMG_SHIFT_ROW) & _IMG_MASK_9 }

fn _img_col(int cp) int { (cp >> _IMG_SHIFT_COL) & _IMG_MASK_9 }

fn _img_id4plus1(int cp) int { (cp >> _IMG_SHIFT_ID4) & _IMG_MASK_9 }

fn _img_diacritic_count(int cp) int { (cp >> _IMG_SHIFT_DIACRITIC) & _IMG_MASK_2 }

fn _img_set_row(int cp, int value) int { (cp & ~(_IMG_MASK_9 << _IMG_SHIFT_ROW)) | ((value & _IMG_MASK_9) << _IMG_SHIFT_ROW) }

fn _img_set_col(int cp, int value) int { (cp & ~(_IMG_MASK_9 << _IMG_SHIFT_COL)) | ((value & _IMG_MASK_9) << _IMG_SHIFT_COL) }

fn _img_set_id4plus1(int cp, int value) int { (cp & ~(_IMG_MASK_9 << _IMG_SHIFT_ID4)) | ((value & _IMG_MASK_9) << _IMG_SHIFT_ID4) }

fn _img_set_diacritic_count(int cp, int value) int { (cp & ~(_IMG_MASK_2 << _IMG_SHIFT_DIACRITIC)) | ((value & _IMG_MASK_2) << _IMG_SHIFT_DIACRITIC) }

fn _img_id24_from_abgr(int fg_abgr) int {
   def r, g = fg_abgr & 255, (fg_abgr >> 8) & 255
   def b = (fg_abgr >> 16) & 255
   b | (g << 8) | (r << 16)
}

fn _img_abgr_from_id(int id) int {
   def b, g = id & 255, (id >> 8) & 255
   def r = (id >> 16) & 255
   (255 << 24) | (b << 16) | (g << 8) | r
}

fn _diacritic_to_num(int code) int {
   case code {
      0x305 -> 1
      0x30d..0x30e -> code - 0x30d + 2
      0x310 -> 4
      0x312 -> 5
      0x33d..0x33f -> code - 0x33d + 6
      0x346 -> 9
      0x34a..0x34c -> code - 0x34a + 10
      0x350..0x352 -> code - 0x350 + 13
      0x357 -> 16
      0x35b -> 17
      0x363..0x36f -> code - 0x363 + 18
      0x483..0x487 -> code - 0x483 + 31
      0x592..0x595 -> code - 0x592 + 36
      0x597..0x599 -> code - 0x597 + 40
      0x59c..0x5a1 -> code - 0x59c + 43
      0x5a8..0x5a9 -> code - 0x5a8 + 49
      0x5ab..0x5ac -> code - 0x5ab + 51
      0x5af -> 53
      0x5c4 -> 54
      0x610..0x617 -> code - 0x610 + 55
      0x657..0x65b -> code - 0x657 + 63
      0x65d..0x65e -> code - 0x65d + 68
      0x6d6..0x6dc -> code - 0x6d6 + 70
      0x6df..0x6e2 -> code - 0x6df + 77
      0x6e4 -> 81
      0x6e7..0x6e8 -> code - 0x6e7 + 82
      0x6eb..0x6ec -> code - 0x6eb + 84
      0x730 -> 86
      0x732..0x733 -> code - 0x732 + 87
      0x735..0x736 -> code - 0x735 + 89
      0x73a -> 91
      0x73d -> 92
      0x73f..0x741 -> code - 0x73f + 93
      0x743 -> 96
      0x745 -> 97
      0x747 -> 98
      0x749..0x74a -> code - 0x749 + 99
      0x7eb..0x7f1 -> code - 0x7eb + 101
      0x7f3 -> 108
      0x816..0x819 -> code - 0x816 + 109
      0x81b..0x823 -> code - 0x81b + 113
      0x825..0x827 -> code - 0x825 + 122
      0x829..0x82d -> code - 0x829 + 125
      0x951 -> 130
      0x953..0x954 -> code - 0x953 + 131
      0xf82..0xf83 -> code - 0xf82 + 133
      0xf86..0xf87 -> code - 0xf86 + 135
      0x135d..0x135f -> code - 0x135d + 137
      0x17dd -> 140
      0x193a -> 141
      0x1a17 -> 142
      0x1a75..0x1a7c -> code - 0x1a75 + 143
      0x1b6b -> 151
      0x1b6d..0x1b73 -> code - 0x1b6d + 152
      0x1cd0..0x1cd2 -> code - 0x1cd0 + 159
      0x1cda..0x1cdb -> code - 0x1cda + 162
      0x1ce0 -> 164
      0x1dc0..0x1dc1 -> code - 0x1dc0 + 165
      0x1dc3..0x1dc9 -> code - 0x1dc3 + 167
      0x1dcb..0x1dcc -> code - 0x1dcb + 174
      0x1dd1..0x1de6 -> code - 0x1dd1 + 176
      0x1dfe -> 198
      0x20d0..0x20d1 -> code - 0x20d0 + 199
      0x20d4..0x20d7 -> code - 0x20d4 + 201
      0x20db..0x20dc -> code - 0x20db + 205
      0x20e1 -> 207
      0x20e7 -> 208
      0x20e9 -> 209
      0x20f0 -> 210
      0x2cef..0x2cf1 -> code - 0x2cef + 211
      0x2de0..0x2dff -> code - 0x2de0 + 214
      0xa66f -> 246
      0xa67c..0xa67d -> code - 0xa67c + 247
      0xa6f0..0xa6f1 -> code - 0xa6f0 + 249
      0xa8e0..0xa8f1 -> code - 0xa8e0 + 251
      0xaab0 -> 269
      0xaab2..0xaab3 -> code - 0xaab2 + 270
      0xaab7..0xaab8 -> code - 0xaab7 + 272
      0xaabe..0xaabf -> code - 0xaabe + 274
      0xaac1 -> 276
      0xfe20..0xfe26 -> code - 0xfe20 + 277
      0x10a0f -> 284
      0x10a38 -> 285
      0x1d185..0x1d189 -> code - 0x1d185 + 286
      0x1d1aa..0x1d1ad -> code - 0x1d1aa + 291
      0x1d242..0x1d244 -> code - 0x1d242 + 295
      _ -> 0
   }
}

fn _tputc_raw_fast(dict vt, any st, int co, int ro, any g, int u) any {
   _vterm_clamp_cursor(vt, st, co, ro)
   mut cx, cy = load32(st, OFF_CX), load32(st, OFF_CY)
   def w = _wcwidth(u)
   if w == 0 {
      def num = _diacritic_to_num(u)
      if num == 0 { return vt }
      mut px, py = cx - 1, cy
      if cx <= 0 {
         px, py = co - 1, cy - 1
      } elif cx >= co {
         px = co - 1
      }
      if py < 0 || py >= ro { return vt }
      if px < 0 || px >= co { return vt }
      def offp = (py * co + px) * 16
      def m = load32(g, offp + 12)
      if (m & ATTR_IMAGE) == 0 { return vt }
      mut cp = load32(g, offp)
      mut dc = _img_diacritic_count(cp)
      if dc == 0 { cp = _img_set_row(cp, num) } elif dc == 1 { cp = _img_set_col(cp, num) } elif dc == 2 { cp = _img_set_id4plus1(cp, num) }
      if dc < 3 { dc += 1 }
      cp = _img_set_diacritic_count(cp, dc)
      store32(g, cp, offp)
      return vt
   }
   mut nvt = vt
   if cx + w > co {
      if (load32(st, OFF_MODE) & MODE_WRAP) != 0 {
         store32(st, 0, OFF_CX)
         if cy == load32(st, OFF_BOT) { nvt = _tscrollup_fast(vt, st, co, ro, g, load32(st, OFF_CUR_FG), load32(st, OFF_CUR_BG), load32(st, OFF_TOP), 1) }
         else { store32(st, cy + 1, OFF_CY) }
         cy, cx = load32(st, OFF_CY), 0
      } else { cx = co - w store32(st, cx, OFF_CX) }
   }
   mut md = load32(st, OFF_CUR_MODE)
   if w == 2 { md = md | ATTR_WIDE }
   if load8(st, OFF_INSERT) != 0 {
      def bot = load32(st, OFF_BOT)
      def src = ptr_add(g, (cy * co + cx) * 16)
      def mv = co - cx - w
      if mv > 0 { memmove(ptr_add(src, w * 16), src, mv * 16) }
   }
   def off = (cy * co + cx) * 16
   mut cp = u
   if u == IMAGE_PLACEHOLDER_CHAR || u == IMAGE_PLACEHOLDER_CHAR_OLD {
      md = md | ATTR_IMAGE
      cp = 0
   }
   store32(g, cp, off)
   store32(g, load32(st, OFF_CUR_FG), off + 4)
   store32(g, load32(st, OFF_CUR_BG), off + 8)
   store32(g, md, off + 12)
   store32(st, u, OFF_LAST_CHAR_C)
   if w == 2 && cx + 1 < co {
      def off2 = off + 16 store32(g, 0, off2) store32(g, load32(st, OFF_CUR_FG), off2 + 4)
      store32(g, load32(st, OFF_CUR_BG), off2 + 8) store32(g, md | ATTR_WDUMMY, off2 + 12)
   }
   store32(st, cx + w, OFF_CX)
   nvt
}

fn _tput_ascii_run_fast(dict vt, any st, int co, int ro, any g, int dfg, int dbg, any src, int start, int count) any {
   if count <= 0 { return vt }
   mut nvt = vt
   mut cx, cy = load32(st, OFF_CX), load32(st, OFF_CY)
   if cx < 0 { cx = 0 } elif cx > co { cx = co }
   if cy < 0 { cy = 0 } elif cy >= ro { cy = ro - 1 }
   def wrap = (load32(st, OFF_MODE) & MODE_WRAP) != 0
   def fg = load32(st, OFF_CUR_FG)
   def bg = load32(st, OFF_CUR_BG)
   def md = load32(st, OFF_CUR_MODE) & ~ATTR_WIDE & ~ATTR_WDUMMY & ~ATTR_IMAGE
   mut i = 0
   while i < count {
      if cx >= co {
         if wrap {
            cx = 0
            if cy == load32(st, OFF_BOT) {
               nvt = _tscrollup_fast(nvt, st, co, ro, g, dfg, dbg, load32(st, OFF_TOP), 1)
               cy = load32(st, OFF_CY)
            } else {
               cy += 1
               store32(st, cy, OFF_CY)
            }
         } else {
            cx = co - 1
         }
      }
      mut span = min(count - i, co - cx)
      if span <= 0 { span = 1 }
      mut j = 0
      while j < span {
         def cp = load8(src, start + i + j) & 255
         def off = (cy * co + cx + j) * 16
         store32(g, cp, off)
         store32(g, fg, off + 4)
         store32(g, bg, off + 8)
         store32(g, md, off + 12)
         j += 1
      }
      store32(st, load8(src, start + i + span - 1) & 255, OFF_LAST_CHAR_C)
      cx += span
      i += span
      if !wrap && cx >= co && i < count {
         i = count
      }
   }
   store32(st, cx, OFF_CX)
   store32(st, cy, OFF_CY)
   nvt
}

fn _tscrollup_fast(dict vt, any st, int co, int ro, any g, int dfg, int dbg, int orig, int n) any {
   def bo = load32(st, OFF_BOT) mut nn = n if nn > bo - orig + 1 { nn = bo - orig + 1 }
   if nn <= 0 { return vt }
   if orig == load32(st, OFF_TOP) && bo == ro - 1 && (load32(st, OFF_MODE) & MODE_ALTSCREEN) == 0 {
      mut h = vt.get("history", [])
      def max_h = vt.get("max_history", 5000)
      mut i = 0 while i < nn {
         def line_ptr = malloc(co * 16)
         if !line_ptr { i += 1 continue }
         memcpy(line_ptr, ptr_add(g, (orig + i) * co * 16), co * 16)
         h = h.append(line_ptr)
         i += 1
      }
      if h.len > max_h {
         def discarded = slice(h, 0, h.len - max_h, 1)
         mut di = 0
         def discarded_n = discarded.len
         while di < discarded_n { free(discarded.get(di)) di += 1 }
         h = slice(h, h.len - max_h, h.len, 1)
      }
      vt = vt.set("history", h)
      if load32(st, OFF_SCROLL) > 0 { _scroll_set(st, load32(st, OFF_SCROLL) + nn, h.len) }
   }
   def ds = ptr_add(g, orig * co * 16) def sc = ptr_add(g, (orig + nn) * co * 16)
   def by = (bo - orig - nn + 1) * co * 16 if by > 0 { memcpy(ds, sc, by) }
   mut y = bo - nn + 1 while y <= bo { _tclearline_fast(st, co, g, dfg, dbg, y, 0, co - 1) y += 1 }
   vt
}

fn _tscrolldown_fast(dict vt, any st, int co, int ro, any g, int dfg, int dbg, int orig, int n) any {
   def bo = load32(st, OFF_BOT) mut nn = n if nn > bo - orig + 1 { nn = bo - orig + 1 }
   if nn > 0 {
      def ds = ptr_add(g, (orig + nn) * co * 16) def sc = ptr_add(g, orig * co * 16)
      def by = (bo - orig - nn + 1) * co * 16
      if by > 0 { memmove(ds, sc, by) }
      mut y = orig while y < orig + nn { _tclearline_fast(st, co, g, dfg, dbg, y, 0, co - 1) y += 1 }
   }
   vt
}

fn _tclearline_fast(any st, int co, any g, int dfg, int dbg, int y, int x1, int x2) any {
   def bg = (load8(st, OFF_BACKGROUND_ERASE) != 0) ? load32(st, OFF_CUR_BG) : dbg
   mut x = x1 while x <= x2 { def off = (y * co + x) * 16 store32(g, 32, off)
      store32(g, dfg, off+4)
      store32(g, bg, off+8)
      store32(g, 0, off+12)
   x += 1 }
   0
}

fn _tclearregion_fast(any st, int co, int ro, any g, int dfg, int dbg, int x1, int y1, int x2, int y2) any {
   mut y = y1
   while y <= y2 {
      def mx1, mx2 = (y == y1) ? x1 : 0, (y == y2) ? x2 : co - 1
      _tclearline_fast(st, co, g, dfg, dbg, y, mx1, mx2)
      y += 1
   }
   0
}

fn _kbd_flags_set(any st, int flags) any {
   store32(st, flags, OFF_KBD_FLAGS)
   store8(st, (flags != 0) ? 1 : 0, OFF_KBD_PROTO)
   0
}

fn _kbd_stack_get(dict vt, any st) list {
   if (load32(st, OFF_MODE) & MODE_ALTSCREEN) != 0 { return vt.get("kbd_stack_alt", []) }
   vt.get("kbd_stack", [])
}

fn _kbd_stack_set(dict vt, any st, list stack) dict {
   if (load32(st, OFF_MODE) & MODE_ALTSCREEN) != 0 { return vt.set("kbd_stack_alt", stack) }
   vt.set("kbd_stack", stack)
}

fn _tcsi_args(str buf) list {
   mut norm_buf = buf
   mut bi2 = 0
   while bi2 < norm_buf.len {
      if load8(norm_buf, bi2) == 58 { norm_buf = str.str_slice(norm_buf, 0, bi2) + ";" + str.str_slice(norm_buf, bi2 + 1, norm_buf.len) }
      bi2 += 1
   }
   def pt = str.split(norm_buf, ";")
   mut ag = []
   mut i = 0
   while i < pt.len {
      def p = str.strip(pt.get(i))
      ag = ag.append((p.len > 0) ? str.atoi(p) : 0)
      i += 1
   }
   [ag, (ag.len > 0) ? ag.get(0) : 0, (ag.len > 1) ? ag.get(1) : 0]
}

fn _tcsi_kitty_keyboard(dict vt, any st, int u, int priv, list ag, int a0, int a1) any {
   if u != 117 { return nil }
   if priv == 61 {
      def flags = a0
      def mode = (ag.len > 1) ? a1 : 1
      mut cur = load32(st, OFF_KBD_FLAGS)
      if mode == 1 { cur = flags }
      elif mode == 2 { cur = cur | flags }
      elif mode == 3 { cur = cur & ~flags }
      _kbd_flags_set(st, cur)
      return vt
   }
   if priv == 63 {
      send_input(vt, "\033[?" + to_str(load32(st, OFF_KBD_FLAGS)) + "u")
      return vt
   }
   if priv == 62 {
      mut stack = _kbd_stack_get(vt, st)
      stack = stack.append(load32(st, OFF_KBD_FLAGS))
      if stack.len > 32 { stack = slice(stack, stack.len - 32, stack.len, 1) }
      _kbd_flags_set(st, (ag.len > 0) ? a0 : 0)
      return _kbd_stack_set(vt, st, stack)
   }
   if priv == 60 {
      def n = (ag.len > 0) ? a0 : 1
      mut stack = _kbd_stack_get(vt, st)
      mut new_flags = 0
      if stack.len > 0 {
         mut idx = stack.len - n
         if idx < 0 { idx = 0 }
         new_flags = stack.get(idx)
         if idx == 0 { stack = [] } else { stack = slice(stack, 0, idx, 1) }
      }
      _kbd_flags_set(st, new_flags)
      return _kbd_stack_set(vt, st, stack)
   }
   nil
}

fn _tcsi_clear_ops(dict vt, any st, int co, int ro, any g, int dfg, int dbg, int u, int a0) any {
   if u == 74 {
      if a0 == 0 {
         def cy = load32(st, OFF_CY)
         _tclearregion_fast(st, co, ro, g, dfg, dbg, load32(st, OFF_CX), cy, co - 1, cy)
         if cy < ro - 1 { _tclearregion_fast(st, co, ro, g, dfg, dbg, 0, cy + 1, co - 1, ro - 1) }
      } elif a0 == 1 {
         if load32(st, OFF_CY) > 0 { _tclearregion_fast(st, co, ro, g, dfg, dbg, 0, 0, co - 1, load32(st, OFF_CY) - 1) }
         _tclearregion_fast(st, co, ro, g, dfg, dbg, 0, load32(st, OFF_CY), load32(st, OFF_CX), load32(st, OFF_CY))
      } elif a0 == 2 {
         _tclearregion_fast(st, co, ro, g, dfg, dbg, 0, 0, co - 1, ro - 1)
      } elif a0 == 3 {
         mut h = vt.get("history", [])
         mut hi = 0
         while hi < h.len { free(h.get(hi)) hi += 1 }
         vt = vt.set("history", [])
         _scroll_set(st, 0, 0)
      }
      return vt
   }
   if u == 75 {
      if a0 == 0 { _tclearline_fast(st, co, g, dfg, dbg, load32(st, OFF_CY), load32(st, OFF_CX), co - 1) }
      elif a0 == 1 { _tclearline_fast(st, co, g, dfg, dbg, load32(st, OFF_CY), 0, load32(st, OFF_CX)) }
      elif a0 == 2 { _tclearline_fast(st, co, g, dfg, dbg, load32(st, OFF_CY), 0, co - 1) }
      return vt
   }
   nil
}

fn _tcsi_window_report(dict vt, int co, int ro, int u, int a0, bool pr) any {
   if u != 116 || pr { return nil }
   if a0 == 14 {
      def px_w = vt.get("px_w", int(float(co) * vt.get("char_w", 9.0)))
      def px_h = vt.get("px_h", int(float(ro) * vt.get("char_h", 18.0)))
      send_input(vt, "\033[4;" + to_str(px_h) + ";" + to_str(px_w) + "t")
   } elif a0 == 16 {
      def cw, ch = vt.get("char_w", 9.0), vt.get("char_h", 18.0)
      send_input(vt, "\033[6;" + to_str(int(ch)) + ";" + to_str(int(cw)) + "t")
   } elif a0 == 18 {
      send_input(vt, "\033[8;" + to_str(ro) + ";" + to_str(co) + "t")
   }
   vt
}

fn _tcsi_edit_ops(dict vt, any st, int co, int ro, any g, int dfg, int dbg, int u, int a0) any {
   if u == 64 {
      def cy = load32(st, OFF_CY) def cx = load32(st, OFF_CX) def n = (a0 == 0) ? 1 : a0
      def room = co - cx mut mv = room - n if mv < 0 { mv = 0 }
      if mv > 0 { memmove(ptr_add(g, (cy * co + cx + n) * 16), ptr_add(g, (cy * co + cx) * 16), mv * 16) }
      _tclearline_fast(st, co, g, dfg, dbg, cy, cx, min(cx + n - 1, co - 1))
      return vt
   }
   if u == 80 {
      def cy = load32(st, OFF_CY) def cx = load32(st, OFF_CX) def n = (a0 == 0) ? 1 : a0
      def mv = co - cx - n if mv > 0 { memmove(ptr_add(g, (cy * co + cx) * 16), ptr_add(g, (cy * co + cx + n) * 16), mv * 16) }
      _tclearline_fast(st, co, g, dfg, dbg, cy, co - n, co - 1)
      return vt
   }
   if u == 76 {
      vt = _tscrolldown_fast(vt, st, co, ro, g, dfg, dbg, load32(st, OFF_CY), (a0 == 0) ? 1 : a0)
      return vt
   }
   if u == 77 {
      vt = _tscrollup_fast(vt, st, co, ro, g, dfg, dbg, load32(st, OFF_CY), (a0 == 0) ? 1 : a0)
      return vt
   }
   if u == 88 {
      def cy = load32(st, OFF_CY) def cx = load32(st, OFF_CX) def n = (a0 == 0) ? 1 : a0
      _tclearline_fast(st, co, g, dfg, dbg, cy, cx, min(cx + n - 1, co - 1))
      return vt
   }
   if u == 98 {
      def lc = load32(st, OFF_LAST_CHAR_C) def n = (a0 == 0) ? 1 : a0
      if lc >= 32 { mut ri = 0 while ri < n { vt = _tputc_raw_fast(vt, st, co, ro, g, lc) ri += 1 } }
      return vt
   }
   nil
}

fn _tcsi_cursor_style(dict vt, any st, int u, int a0) any {
   if u != 113 { return nil }
   store8(st, 1, OFF_CURSOR_VISIBLE)
   def cs = case a0 {
      0 -> vt.get("cursor_style_default", 2)
      1, 2 -> 1
      3, 4 -> 3
      5, 6 -> 2
      _ -> vt.get("cursor_style_default", 2)
   }
   store8(st, cs, OFF_CURSOR_STYLE_S)
   vt
}

fn _tcsihandle_fast(dict vt, any st, int co, int ro, any g, any pal, int dfg, int dbg, int u, str buf) any {
   mut csi_buf = buf
   mut priv = load8(st, OFF_CSI_PRIV)
   if csi_buf.len > 0 {
      def first = load8(csi_buf, 0) & 255
      if first == 63 || first == 62 || first == 60 || first == 61 {
         priv = first
         csi_buf = str.str_slice(csi_buf, 1, csi_buf.len)
      }
   }
   def parsed = _tcsi_args(csi_buf)
   def ag = parsed.get(0, [])
   def pr = priv == 63
   def a0 = int(parsed.get(1, 0)) def a1 = int(parsed.get(2, 0))
   def kitty = _tcsi_kitty_keyboard(vt, st, u, priv, ag, a0, a1)
   if kitty != nil { return kitty }
   if u == 99 {
      send_input(vt, "\033[?62c")
      return vt
   }
   if u == 113 && priv == 62 {
      send_input(vt, "\033P>|kitty(0.0.0)\033\\")
      return vt
   }
   def cleared = _tcsi_clear_ops(vt, st, co, ro, g, dfg, dbg, u, a0)
   if cleared != nil { return cleared }
   if u == 109 {
      _tsetattr_fast(st, pal, dfg, dbg, ag)
      return vt
   }
   def window_report = _tcsi_window_report(vt, co, ro, u, a0, pr)
   if window_report != nil { return window_report }
   def edit = _tcsi_edit_ops(vt, st, co, ro, g, dfg, dbg, u, a0)
   if edit != nil { return edit }
   def cursor_style = _tcsi_cursor_style(vt, st, u, a0)
   if cursor_style != nil { return cursor_style }
   if u == 65 { _tmoveto_fast(st, co, ro, load32(st, OFF_CX), load32(st, OFF_CY) - ((a0 == 0) ? 1 : a0)) }
   elif u == 66 || u == 101 { _tmoveto_fast(st, co, ro, load32(st, OFF_CX), load32(st, OFF_CY) + ((a0 == 0) ? 1 : a0)) }
   elif u == 67 || u == 97 { _tmoveto_fast(st, co, ro, load32(st, OFF_CX) + ((a0 == 0) ? 1 : a0), load32(st, OFF_CY)) }
   elif u == 68 { _tmoveto_fast(st, co, ro, load32(st, OFF_CX) - ((a0 == 0) ? 1 : a0), load32(st, OFF_CY)) }
   elif u == 71 { _tmoveto_fast(st, co, ro, (a0 == 0) ? 0 : a0 - 1, load32(st, OFF_CY)) }
   elif u == 100 { _tmoveto_fast(st, co, ro, load32(st, OFF_CX), (a0 == 0) ? 0 : a0 - 1) }
   elif u == 115 {
      store32(st, load32(st, OFF_CX), OFF_SAVED_CX)
      store32(st, load32(st, OFF_CY), OFF_SAVED_CY)
   }
   elif u == 117 { _tmoveto_fast(st, co, ro, load32(st, OFF_SAVED_CX), load32(st, OFF_SAVED_CY)) }
   elif u == 83 { vt = _tscrollup_fast(vt, st, co, ro, g, dfg, dbg, load32(st, OFF_TOP), (a0 == 0) ? 1 : a0) }
   elif u == 84 { vt = _tscrolldown_fast(vt, st, co, ro, g, dfg, dbg, load32(st, OFF_TOP), (a0 == 0) ? 1 : a0) }
   elif u == 72 || u == 102 { _tmoveto_fast(st, co, ro, (a1 == 0) ? 0 : a1 - 1, (a0 == 0) ? 0 : a0 - 1) }
   elif u == 114 { if !pr { mut t=(a0==0)?1:a0 mut b=(a1==0)?ro:a1 store32(st, t-1, OFF_TOP)
         store32(st, b-1, OFF_BOT)
   _tmoveto_fast(st, co, ro, 0, 0) } }
   elif u == 104 { vt = _tsetmode_fast(vt, st, co, ro, g, dfg, dbg, pr, true, ag) }
   elif u == 108 { vt = _tsetmode_fast(vt, st, co, ro, g, dfg, dbg, pr, false, ag) }
   elif u == 69 { _tmoveto_fast(st, co, ro, 0, load32(st, OFF_CY) + ((a0==0)?1:a0)) }
   elif u == 70 { _tmoveto_fast(st, co, ro, 0, load32(st, OFF_CY) - ((a0==0)?1:a0)) }
   vt
}

fn _tsetattr_fast(any st, any pal, int dfg, int dbg, list ag) any {
   mut i = 0 def n = ag.len if n == 0 { _tresetattr_fast(st, dfg, dbg) return 0 }
   while i < n {
      def a = ag.get(i)
      mut md = load32(st, OFF_CUR_MODE)
      if a == 0 { _tresetattr_fast(st, dfg, dbg) }
      elif a == 1 { store32(st, md | ATTR_BOLD, OFF_CUR_MODE) }
      elif a == 2 { store32(st, md | ATTR_FAINT, OFF_CUR_MODE) }
      elif a == 3 { store32(st, md | ATTR_ITALIC, OFF_CUR_MODE) }
      elif a == 4 || a == 21 { store32(st, md | ATTR_UNDERLINE, OFF_CUR_MODE) }
      elif a == 5 || a == 6 { store32(st, md | ATTR_BLINK, OFF_CUR_MODE) }
      elif a == 7 { store32(st, md | ATTR_REVERSE, OFF_CUR_MODE) }
      elif a == 8 { store32(st, md | ATTR_INVIS, OFF_CUR_MODE) }
      elif a == 9 { store32(st, md | ATTR_STRIKE, OFF_CUR_MODE) }
      elif a == 22 { store32(st, md & ~(ATTR_BOLD|ATTR_FAINT), OFF_CUR_MODE) }
      elif a == 23 { store32(st, md & ~ATTR_ITALIC, OFF_CUR_MODE) }
      elif a == 24 { store32(st, md & ~ATTR_UNDERLINE, OFF_CUR_MODE) }
      elif a == 25 { store32(st, md & ~ATTR_BLINK, OFF_CUR_MODE) }
      elif a == 27 { store32(st, md & ~ATTR_REVERSE, OFF_CUR_MODE) }
      elif a == 28 { store32(st, md & ~ATTR_INVIS, OFF_CUR_MODE) }
      elif a == 29 { store32(st, md & ~ATTR_STRIKE, OFF_CUR_MODE) }
      elif a == 53 { store32(st, md | ATTR_STRIKE, OFF_CUR_MODE) }
      elif a >= 30 && a <= 37 {
         mut idx = a - 30
         if (md & ATTR_BOLD) != 0 { idx += 8 }
         store32(st, load32(pal, idx * 4), OFF_CUR_FG)
      }
      elif a == 39 { store32(st, dfg, OFF_CUR_FG) }
      elif a >= 40 && a <= 47 {
         mut idx = a - 40
         if (md & ATTR_BOLD) != 0 { idx += 8 }
         store32(st, load32(pal, idx * 4), OFF_CUR_BG)
      }
      elif a == 49 { store32(st, dbg, OFF_CUR_BG) }
      elif a >= 90 && a <= 97 { store32(st, load32(pal, (a-90+8)*4), OFF_CUR_FG) }
      elif a >= 100 && a <= 107 { store32(st, load32(pal, (a-100+8)*4), OFF_CUR_BG) }
      elif a == 38 || a == 48 {
         if i + 2 < n {
            def t = ag.get(i + 1)
            if t == 5 { store32(st, load32(pal, ag.get(i+2)*4), (a==38)?OFF_CUR_FG:OFF_CUR_BG) i += 2 }
            elif t == 2 && i+4 < n {
               def c = color_pack(float(ag.get(i+2))/255.0, float(ag.get(i+3))/255.0, float(ag.get(i+4))/255.0, 1.0)
               store32(st, c, (a==38)?OFF_CUR_FG:OFF_CUR_BG) i += 4
            }
         }
      } i += 1
   }
   0
}

fn _tresetattr_fast(any st, int dfg, int dbg) any {
   store32(st, dfg, OFF_CUR_FG)
   store32(st, dbg, OFF_CUR_BG)
   store32(st, 0, OFF_CUR_MODE)
   0
}

fn _tsetmode_fast(dict vt, any st, int co, int ro, any g, int dfg, int dbg, bool pr, bool set, list ag) any {
   mut nvt = vt
   mut i = 0
   def ag_n = ag.len
   while i < ag_n {
      def a = ag.get(i)
      if pr {
         if a == 1049 || a == 47 || a == 1047 { nvt = _tswapscreen_fast(nvt, st, co, ro, g, dfg, dbg, set) }
         elif a == 25 { store8(st, set ? 1 : 0, OFF_CURSOR_VISIBLE) }
         elif a == 7 { mut m = load32(st, OFF_MODE) store32(st, set ? (m | MODE_WRAP) : (m & ~MODE_WRAP), OFF_MODE) }
         elif a == 1 { store8(st, set ? 1 : 0, OFF_APPKEYS) }
         elif a == 1000 || a == 1002 || a == 1003 || a == 1006 {
            mut m = load32(st, OFF_MODE)
            store32(st, set ? (m | MODE_MOUSE) : (m & ~MODE_MOUSE), OFF_MODE)
            mut mf = load8(st, OFF_MOUSE_FLAGS)
            if a == 1000 { mf = set ? (mf | MOUSE_1000_BTN) : (mf & ~MOUSE_1000_BTN) }
            elif a == 1002 { mf = set ? (mf | MOUSE_1002_DRAG) : (mf & ~MOUSE_1002_DRAG) }
            elif a == 1003 { mf = set ? (mf | MOUSE_1003_MOTION) : (mf & ~MOUSE_1003_MOTION) }
            elif a == 1006 { mf = set ? (mf | MOUSE_1006_SGR) : (mf & ~MOUSE_1006_SGR) }
            store8(st, mf, OFF_MOUSE_FLAGS)
         }
         elif a == 1004 { store8(st, set ? 1 : 0, OFF_FOCUS_REPORT) }
         elif a == 2004 { store8(st, set ? 1 : 0, OFF_BRACKET_PASTE) }
      } else {
         if a == 4 { store8(st, set ? 1 : 0, OFF_INSERT) }
         elif a == 20 {
            mut m = load32(st, OFF_MODE)
            def LNM_BIT = 256
            store32(st, set ? (m | LNM_BIT) : (m & ~LNM_BIT), OFF_MODE)
         }
      }
      i += 1
   }
   nvt
}

fn _tswapscreen_fast(dict vt, any st, int co, int ro, any g, int dfg, int dbg, bool set) any {
   mut nvt = vt
   mut alt = nvt.get("alt_grid", 0)
   mut m = load32(st, OFF_MODE)
   if set && !alt {
      alt = malloc(co * ro * 16)
      if !alt { return nvt }
      _clear_grid(alt, co, ro, dfg, dbg)
      nvt = nvt.set("alt_grid", alt)
   }
   if set {
      store32(st, load32(st, OFF_CX), OFF_SAVED_CX)
      store32(st, load32(st, OFF_CY), OFF_SAVED_CY)
      store32(st, load32(st, OFF_CUR_FG), OFF_SAVED_FG)
      store32(st, load32(st, OFF_CUR_BG), OFF_SAVED_BG)
      store32(st, load32(st, OFF_CUR_MODE), OFF_SAVED_MODE)
      store32(st, m | MODE_ALTSCREEN, OFF_MODE)
      store32(st, dfg, OFF_CUR_FG)
      store32(st, dbg, OFF_CUR_BG)
      store32(st, 0, OFF_CUR_MODE)
      nvt = nvt.set("grid", alt)
      nvt = nvt.set("primary_grid", g)
   } else {
      store32(st, m & ~MODE_ALTSCREEN, OFF_MODE)
      store32(st, load32(st, OFF_SAVED_CX), OFF_CX)
      store32(st, load32(st, OFF_SAVED_CY), OFF_CY)
      store32(st, load32(st, OFF_SAVED_FG), OFF_CUR_FG)
      store32(st, load32(st, OFF_SAVED_BG), OFF_CUR_BG)
      store32(st, load32(st, OFF_SAVED_MODE), OFF_CUR_MODE)
      def prim = nvt.get("primary_grid", 0)
      if prim { nvt = nvt.set("grid", prim) }
   }
   nvt
}

fn _kg_reply(dict vt, int image_id, str msg) any { send_input(vt, "\033_Gi=" + to_str(image_id) + ";" + msg + "\033\\") 0 }

fn _kg_parse_apc(str buf) dict {
   mut s = buf
   if s.len > 0 && load8(s, 0) == 71 { s = str.str_slice(s, 1, s.len) }
   def semi = str.find(s, ";")
   mut head = s
   mut payload = ""
   if semi >= 0 {
      head = str.str_slice(s, 0, semi)
      payload = str.str_slice(s, semi + 1, s.len)
   }
   mut params = dict(8)
   def parts = str.split(head, ",")
   mut i = 0
   def parts_n = parts.len
   while i < parts_n {
      def part = parts.get(i)
      if !part || part.len == 0 { i += 1 continue }
      def eqp = str.find(part, "=")
      if eqp < 0 { i += 1 continue }
      def k, v = str.str_slice(part, 0, eqp), str.str_slice(part, eqp + 1, part.len)
      if k.len != 1 { i += 1 continue }
      def kc = load8(k, 0)
      if kc == 97 || kc == 116 || kc == 111 || kc == 100 { if v.len > 0 { params = params.set(k, load8(v, 0)) } } else { params = params.set(k, str.atoi(v)) }
      i += 1
   }
   return {"params": params, "payload": payload}
}

fn _kg_fit_info(dict vt, dict info) dict {
   def w, h = info.get("w", 0), info.get("h", 0)
   def ccols = info.get("cols", 0)
   def crows = info.get("rows", 0)
   if ccols <= 0 || crows <= 0 || w <= 0 || h <= 0 { return info }
   def cw, ch = vt.get("char_w", 9.0), vt.get("char_h", 18.0)
   def box_w, box_h = float(ccols) * cw, float(crows) * ch
   if box_w <= 0.0 || box_h <= 0.0 { return info }
   def sx, sy = box_w / float(w), box_h / float(h)
   def sc = (sx < sy) ? sx : sy
   def fit_w = float(w) * sc
   def fit_h = float(h) * sc
   def fit_x = (box_w - fit_w) * 0.5
   def fit_y = (box_h - fit_h) * 0.5
   mut out = info.set("fit_x", fit_x)
   out = out.set("fit_y", fit_y)
   out = out.set("fit_w", fit_w)
   out = out.set("fit_h", fit_h)
   def fit_x2, fit_y2 = fit_x + fit_w, fit_y + fit_h
   def fit_off_x, fit_off_y = int(floor(fit_x / cw + 0.000001)), int(floor(fit_y / ch + 0.000001))
   out = out.set("fit_off_x", fit_off_x)
   out = out.set("fit_off_y", fit_off_y)
   out = out.set("fit_cols", max(1, int(ceil(fit_x2 / cw - 0.000001)) - fit_off_x))
   out = out.set("fit_rows", max(1, int(ceil(fit_y2 / ch - 0.000001)) - fit_off_y))
   out
}

fn _kg_store_image_bytes(dict vt,
   int image_id,
   any bytes,
   int cols,
   int rows,
   int fmt,
   int data_w,
   int data_h,
   str ext) dict{
   "Decodes raw/PNG data and uploads to GPU. fmt: 24/32/100. data_w/h for raw."
   mut tex = 0
   mut w = 0
   mut h = 0
   if !bytes || !is_str(bytes) { return vt }
   if fmt == 24 || fmt == 32 {
      if data_w <= 0 || data_h <= 0 { return vt }
      w, h = data_w, data_h
      if fmt == 32 {
         if bytes.len < w * h * 4 { return vt }
         tex = texture_create_rgba(w, h, bytes, 37)
      } else {
         if bytes.len < w * h * 3 { return vt }
         def px = malloc(w * h * 4)
         if !px { return vt }
         mut i, o = 0, 0
         def n = w * h
         while i < n {
            def src = i * 3
            store8(px, load8(bytes, src + 0), o + 0)
            store8(px, load8(bytes, src + 1), o + 1)
            store8(px, load8(bytes, src + 2), o + 2)
            store8(px, 255, o + 3)
            i += 1
            o += 4
         }
         tex = texture_create_rgba(w, h, px, 37)
         free(px)
      }
   } else {
      if bytes.len < 4 { return vt }
      def is_png = load8(bytes, 0) == 137 && load8(bytes, 1) == 80 && load8(bytes, 2) == 78 && load8(bytes, 3) == 71
      if !is_png && ext != ".png" { return vt }
      def img = png_img.decode(bytes)
      if !img || !is_dict(img) { return vt }
      w, h = img.get("width", 0), img.get("height", 0)
      def pixels = img.get("data", 0)
      if w <= 0 || h <= 0 || !pixels { return vt }
      tex = texture_create_rgba(w, h, pixels, 37)
   }
   if !tex || tex <= 0 { return vt }
   mut ccols = cols
   mut crows = rows
   if (ccols <= 0 || crows <= 0) && w > 0 && h > 0 {
      def cw, ch = vt.get("char_w", 9.0), vt.get("char_h", 18.0)
      if ccols <= 0 { ccols = _ceil_div_f(float(w), cw) }
      if crows <= 0 { crows = _ceil_div_f(float(h), ch) }
   }
   mut info = {"tex": tex, "w": w, "h": h}
   if ccols > 0 { info = info.set("cols", ccols) }
   if crows > 0 { info = info.set("rows", crows) }
   info = _kg_fit_info(vt, info)
   mut kg = vt.get("kitty_graphics", dict(8))
   mut images = kg.get("images", dict(8))
   def old = images.get(image_id, 0)
   if old && is_dict(old) { texture_destroy(old.get("tex", 0)) }
   images = images.set(image_id, info)
   kg = kg.set("images", images)
   vt = vt.set("kitty_graphics", kg)
   vt
}

fn _kg_update_dims(dict vt, int image_id, int cols, int rows) dict {
   mut kg = vt.get("kitty_graphics", dict(8))
   mut images = kg.get("images", dict(8))
   def old = images.get(image_id, 0)
   if old && is_dict(old) {
      mut info = old
      if cols > 0 { info = info.set("cols", cols) }
      if rows > 0 { info = info.set("rows", rows) }
      info = _kg_fit_info(vt, info)
      images = images.set(image_id, info)
      kg = kg.set("images", images)
      return vt.set("kitty_graphics", kg)
   }
   mut info = dict(8)
   if cols > 0 { info = info.set("cols", cols) }
   if rows > 0 { info = info.set("rows", rows) }
   images = images.set(image_id, info)
   kg = kg.set("images", images)
   vt.set("kitty_graphics", kg)
}

fn _kg_image_id_from_cell(int cp, int fg_abgr) int {
   def id24 = _img_id24_from_abgr(fg_abgr)
   def id4p1 = _img_id4plus1(cp)
   def msb = (id4p1 > 0) ? (id4p1 - 1) : 0
   id24 | (msb << 24)
}

fn _kg_clear_placeholders_grid(any p, int count, int dfg, int dbg, int image_id, bool del_all) any {
   mut i = 0
   while i < count {
      def off = i * 16
      def m = load32(p, off + 12)
      if (m & ATTR_IMAGE) != 0 {
         def cp = load32(p, off)
         def fg = load32(p, off + 4)
         def img_id = _kg_image_id_from_cell(cp, fg)
         if del_all || img_id == image_id {
            store32(p, 32, off)
            store32(p, dfg, off + 4)
            store32(p, dbg, off + 8)
            store32(p, 0, off + 12)
         }
      }
      i += 1
   }
   0
}

fn _kg_clear_placeholders(dict vt, int image_id, bool del_all) dict {
   def dfg, dbg = vt.get("def_fg"), vt.get("def_bg")
   def co, ro = vt.get("cols"), vt.get("rows")
   def g = vt.get("grid")
   _kg_clear_placeholders_grid(g, co * ro, dfg, dbg, image_id, del_all)
   def ag = vt.get("alt_grid", 0)
   if ag { _kg_clear_placeholders_grid(ag, co * ro, dfg, dbg, image_id, del_all) }
   def h = vt.get("history", [])
   mut i = 0
   def h_n = h.len
   while i < h_n {
      def line_ptr = h.get(i)
      _kg_clear_placeholders_grid(line_ptr, co, dfg, dbg, image_id, del_all)
      i += 1
   }
   vt
}

fn _kg_clear_placeholders_current(dict vt, int image_id, bool del_all) dict {
   def dfg, dbg = vt.get("def_fg"), vt.get("def_bg")
   def co, ro = vt.get("cols"), vt.get("rows")
   def g = vt.get("grid")
   _kg_clear_placeholders_grid(g, co * ro, dfg, dbg, image_id, del_all)
   def ag = vt.get("alt_grid", 0)
   if ag { _kg_clear_placeholders_grid(ag, co * ro, dfg, dbg, image_id, del_all) }
   vt
}

fn _kg_add_placement(dict vt,
   int image_id,
   int placement_id,
   int cx,
   int cy,
   int cols,
   int rows,
   int x_off,
   int y_off,
   int z,
   bool insert_lines,
   int reserve_override) dict{
   "Stores/updates a placement(image displayed at a given cell rectangle)."
   if image_id == 0 || cols <= 0 || rows <= 0 { return vt }
   def reserve = (reserve_override != 0) ? (reserve_override > 0) : _kg_reserve_enabled()
   if reserve {
      vt = _kg_clear_placeholders_current(vt, image_id, false)
      def st = vt.get("state")
      def g = vt.get("grid")
      def co = vt.get("cols")
      def ro = vt.get("rows")
      if insert_lines && st && g && co > 0 && ro > 0 {
         def dfg, dbg = vt.get("def_fg"), vt.get("def_bg")
         vt = _tscrolldown_fast(vt, st, co, ro, g, dfg, dbg, cy, rows)
      }
      if st && g && co > 0 && ro > 0 {
         def fg, bg = _img_abgr_from_id(image_id), load32(st, OFF_CUR_BG)
         def id4 = ((image_id >> 24) & 255) + 1
         mut ry = 0
         while ry < rows {
            def y = cy + ry
            if y < 0 || y >= ro { ry += 1 continue }
            mut rx = 0
            while rx < cols {
               def x = cx + rx
               if x >= 0 && x < co {
                  def off = (y * co + x) * 16
                  mut cp = 0
                  cp = _img_set_row(cp, ry + 1)
                  cp = _img_set_col(cp, rx + 1)
                  cp = _img_set_id4plus1(cp, id4)
                  cp = _img_set_diacritic_count(cp, 3)
                  store32(g, cp, off)
                  store32(g, fg, off + 4)
                  store32(g, bg, off + 8)
                  store32(g, ATTR_IMAGE, off + 12)
               }
               rx += 1
            }
            ry += 1
         }
      }
      if _kg_reserve_move_enabled() {
         def st2 = vt.get("state")
         if st2 {
            store32(st2, 0, OFF_CX)
            store32(st2, min(ro - 1, cy + rows), OFF_CY)
         }
      }
      return vt
   }
   mut kg = vt.get("kitty_graphics", dict(8))
   mut placements = kg.get("placements", dict(8))
   def pid = (placement_id == 0) ? 0 : placement_id
   def key = to_str(image_id) + ":" + to_str(pid)
   def pl = {
      "image_id": image_id,
      "p": pid,
      "x": cx,
      "y": cy,
      "cols": cols,
      "rows": rows,
      "x_off": x_off,
      "y_off": y_off,
      "z": z
   }
   placements = placements.set(key, pl)
   kg = kg.set("placements", placements)
   vt.set("kitty_graphics", kg)
}

fn _kg_place_at_cursor(dict vt,
   int image_id,
   int placement_id,
   int cols,
   int rows,
   int x_off,
   int y_off,
   int z_index,
   bool explicit_pos,
   int cell_x,
   int cell_y,
   bool do_not_move) dict{
   def st = vt.get("state")
   def co = vt.get("cols")
   def ro = vt.get("rows")
   mut cx, cy = load32(st, OFF_CX), load32(st, OFF_CY)
   if explicit_pos {
      if cell_x >= 0 { cx = (cell_x > 0) ? (cell_x - 1) : 0 }
      if cell_y >= 0 { cy = (cell_y > 0) ? (cell_y - 1) : 0 }
      if cx < 0 { cx = 0 } elif cx >= co { cx = co - 1 }
      if cy < 0 { cy = 0 } elif cy >= ro { cy = ro - 1 }
   }
   def insert_lines = (!explicit_pos && !do_not_move)
   def reserve_override = explicit_pos ? -1 : 0
   vt = _kg_add_placement(vt,
      image_id,
      placement_id,
      cx,
      cy,
      cols,
      rows,
      x_off,
      y_off,
      z_index,
      insert_lines,
   reserve_override)
   if !do_not_move { store32(st, cx + cols, OFF_CX) }
   vt
}

fn _kg_delete(dict vt, int spec, int image_id) dict {
   mut kg = vt.get("kitty_graphics", dict(8))
   mut images = kg.get("images", dict(8))
   mut inflight = kg.get("inflight", dict(8))
   mut placements = kg.get("placements", dict(8))
   def s = (spec == 0) ? 97 : spec
   if s == 97 || s == 65 {
      def keys = dict_keys(images)
      mut i = 0
      def keys_n = keys.len
      while i < keys_n {
         def k = keys.get(i)
         def info = images.get(k, 0)
         if info && is_dict(info) { texture_destroy(info.get("tex", 0)) }
         i += 1
      }
      images = dict(8)
      inflight = dict(8)
      placements = dict(8)
      vt = _kg_clear_placeholders(vt, 0, true)
   } elif s == 105 || s == 73 {
      def info = images.get(image_id, 0)
      if info && is_dict(info) { texture_destroy(info.get("tex", 0)) }
      images = images.delete(image_id)
      inflight = inflight.delete(image_id)
      def pkeys = dict_keys(placements)
      mut pi = 0
      def pkeys_n = pkeys.len
      while pi < pkeys_n {
         def k = pkeys.get(pi)
         def pl = placements.get(k, 0)
         if pl && is_dict(pl) && pl.get("image_id", 0) == image_id { placements = placements.delete(k) }
         pi += 1
      }
      vt = _kg_clear_placeholders(vt, image_id, false)
   }
   kg = kg.set("images", images)
   kg = kg.set("inflight", inflight)
   kg = kg.set("placements", placements)
   vt.set("kitty_graphics", kg)
}

fn _kg_pixel_cell_dims(dict vt, int cols, int rows, int px_w, int px_h) list {
   mut out_cols, out_rows = cols, rows
   if (out_cols <= 0 || out_rows <= 0) && (px_w > 0 || px_h > 0) {
      def cw, ch = vt.get("char_w", 9.0), vt.get("char_h", 18.0)
      if out_cols <= 0 && px_w > 0 { out_cols = _ceil_div_f(float(px_w), cw) }
      if out_rows <= 0 && px_h > 0 { out_rows = _ceil_div_f(float(px_h), ch) }
   }
   [out_cols, out_rows]
}

fn _kg_query_image(dict vt, int image_id, int quiet) dict {
   if image_id == 0 {
      if quiet == 0 { _kg_reply(vt, 0, "ENOENT") }
      return vt
   }
   def kgq = vt.get("kitty_graphics", dict(8))
   def imgs = kgq.get("images", dict(8))
   def info = imgs.get(image_id, 0)
   if info && is_dict(info) { if quiet == 0 { _kg_reply(vt, image_id, "OK") } }
   else { if quiet == 0 { _kg_reply(vt, image_id, "ENOENT") } }
   vt
}

fn _kg_assign_image_id(dict vt) list {
   mut kg_id = vt.get("kitty_graphics", dict(8))
   mut next_id = kg_id.get("next_id", 1)
   def image_id = next_id
   next_id += 1
   kg_id = kg_id.set("next_id", next_id)
   [vt.set("kitty_graphics", kg_id), image_id]
}

fn _kg_image_cell_dims(dict vt, int image_id, int cols, int rows) list {
   mut out_cols, out_rows = cols, rows
   if out_cols <= 0 || out_rows <= 0 {
      def kg2 = vt.get("kitty_graphics", dict(8))
      def images2 = kg2.get("images", dict(8))
      def info2 = images2.get(image_id, 0)
      if info2 && is_dict(info2) {
         def iw, ih = info2.get("w", 0), info2.get("h", 0)
         def cw, ch = vt.get("char_w", 9.0), vt.get("char_h", 18.0)
         if out_cols <= 0 && iw > 0 { out_cols = _ceil_div_f(float(iw), cw) }
         if out_rows <= 0 && ih > 0 { out_rows = _ceil_div_f(float(ih), ch) }
         if out_cols > 0 || out_rows > 0 { vt = _kg_update_dims(vt, image_id, out_cols, out_rows) }
      }
   }
   [vt, out_cols, out_rows]
}

fn _kg_place_existing(dict vt, int image_id, int placement_id, int cols, int rows, int x_off, int y_off, int z_index, bool explicit_pos, int cell_x, int cell_y, bool do_not_move) dict {
   if image_id == 0 { return vt }
   def d = _kg_image_cell_dims(vt, image_id, cols, rows)
   vt = d.get(0)
   cols = d.get(1)
   rows = d.get(2)
   _kg_place_at_cursor(vt,
      image_id,
      placement_id,
      cols,
      rows,
      x_off,
      y_off,
      z_index,
      explicit_pos,
      cell_x,
      cell_y,
   do_not_move)
}

fn _kg_transmit_direct(dict vt, int image_id, str payload, int cols, int rows, int fmt, int px_w, int px_h, int quiet, int more) dict {
   mut kg = vt.get("kitty_graphics", dict(8))
   mut inflight = kg.get("inflight", dict(8))
   def prev = inflight.get(image_id, "")
   inflight = inflight.set(image_id, prev + payload)
   kg = kg.set("inflight", inflight)
   vt = vt.set("kitty_graphics", kg)
   if more != 0 { return vt }
   def b64 = inflight.get(image_id, "")
   inflight = inflight.delete(image_id)
   kg = kg.set("inflight", inflight)
   vt = vt.set("kitty_graphics", kg)
   def bytes = str_base.decode64(b64)
   vt = _kg_store_image_bytes(vt, image_id, bytes, cols, rows, fmt, px_w, px_h, "")
   if quiet == 0 { _kg_reply(vt, image_id, "OK") }
   vt
}

fn _kg_transmit_file(dict vt, int image_id, str payload, int cols, int rows, int fmt, int px_w, int px_h, int quiet) dict {
   def path = str_base.decode64(payload)
   match file_read(path) {
      ok(bytes) -> {
         vt = _kg_store_image_bytes(vt, image_id, bytes, cols, rows, fmt, px_w, px_h, path)
         if quiet == 0 { _kg_reply(vt, image_id, "OK") }
      }
      err(ignorederr) -> { ignorederr
         if quiet == 0 { _kg_reply(vt, image_id, "ENOENT") }
      }
   }
   vt
}

fn _kg_transmit_payload(dict vt, int image_id, int medium, str payload, int cols, int rows, int fmt, int px_w, int px_h, int quiet, int more) dict {
   if medium == 100 { return _kg_transmit_direct(vt, image_id, payload, cols, rows, fmt, px_w, px_h, quiet, more) }
   if medium == 102 || medium == 116 { return _kg_transmit_file(vt, image_id, payload, cols, rows, fmt, px_w, px_h, quiet) }
   if quiet == 0 { _kg_reply(vt, image_id, "EINVAL") }
   vt
}

fn _kg_place_transmitted(dict vt, int action, int virt, int image_id, int placement_id, int cols, int rows, int x_off, int y_off, int z_index, bool explicit_pos, int cell_x, int cell_y, bool do_not_move) dict {
   if action != 84 || virt != 0 { return vt }
   def d = _kg_image_cell_dims(vt, image_id, cols, rows)
   vt = d.get(0)
   cols = d.get(1)
   rows = d.get(2)
   if cols <= 0 || rows <= 0 { return vt }
   _kg_place_at_cursor(vt,
      image_id,
      placement_id,
      cols,
      rows,
      x_off,
      y_off,
      z_index,
      explicit_pos,
      cell_x,
      cell_y,
   do_not_move)
}

fn _kg_handle_apc(dict vt, str buf) dict {
   def parsed = _kg_parse_apc(buf)
   def p = parsed.get("params", dict(8))
   def payload = parsed.get("payload", "")
   mut action = p.get("a", 116) ;; 't' default
   mut medium = p.get("t", 100) ;; 'd' default
   mut image_id = p.get("i", 0)
   def quiet = p.get("q", 0)
   def fmt = p.get("f", 100)
   def more = p.get("m", 0)
   def placement_id = p.get("p", 0)
   def x_off = p.get("X", 0)
   def y_off = p.get("Y", 0)
   def cell_x = p.get("x", -1)
   def cell_y = p.get("y", -1)
   def z_index = p.get("z", 0)
   mut cols = p.get("c", 0)
   mut rows = p.get("r", 0)
   def px_w, px_h = p.get("s", 0), p.get("v", 0)
   def virt = p.get("U", 0)
   mut do_not_move = p.get("C", 0) != 0
   def delspec = p.get("d", 0)
   def explicit_pos = (cell_x >= 0 || cell_y >= 0)
   if explicit_pos && !_kg_reserve_enabled() { do_not_move = true }
   def pd = _kg_pixel_cell_dims(vt, cols, rows, px_w, px_h)
   cols = pd.get(0)
   rows = pd.get(1)
   if action == 100 { return _kg_delete(vt, delspec, image_id) }
   if action == 113 { return _kg_query_image(vt, image_id, quiet) }
   if image_id == 0 && action != 112 {
      def idr = _kg_assign_image_id(vt)
      vt = idr.get(0)
      image_id = idr.get(1)
   }
   if image_id != 0 && (cols > 0 || rows > 0) { vt = _kg_update_dims(vt, image_id, cols, rows) }
   if action == 112 {
      if virt == 0 {
         return _kg_place_existing(vt, image_id, placement_id, cols, rows, x_off, y_off, z_index, explicit_pos, cell_x, cell_y, do_not_move)
      }
      return vt
   }
   if image_id == 0 { return vt }
   vt = _kg_transmit_payload(vt, image_id, medium, payload, cols, rows, fmt, px_w, px_h, quiet, more)
   if medium == 100 && more != 0 { return vt }
   _kg_place_transmitted(vt, action, virt, image_id, placement_id, cols, rows, x_off, y_off, z_index, explicit_pos, cell_x, cell_y, do_not_move)
}

fn _tstrhandle(dict vt, int kind, str buf) dict {
   if kind == 93 {
      if str.startswith(buf, "0;") || str.startswith(buf, "2;") {
         def semi = str.find(buf, ";")
         if semi >= 0 { return vt.set("title", str.str_slice(buf, semi + 1, buf.len)) }
      } elif str.startswith(buf, "4;") {
         def p1 = str.find(buf, ";")
         mut p2 = -1
         if p1 >= 0 { p2 = str.find(str.str_slice(buf, p1 + 1, buf.len), ";") }
         if p1 >= 0 && p2 >= 0 {
            def idx_s = str.str_slice(buf, p1 + 1, p1 + 1 + p2)
            def val_s = str.str_slice(buf, p1 + 1 + p2 + 1, buf.len)
            def idx = str.atoi(str.strip(idx_s))
            def pal = vt.get("palette", 0)
            if pal && idx >= 0 && idx < 256 {
               def c = _parse_abgr(val_s, 0)
               if c != 0 { store32(pal, c, idx * 4) }
            }
         }
      } elif str.startswith(buf, "10;") {
         def semi = str.find(buf, ";")
         if semi >= 0 {
            def val = str.str_slice(buf, semi + 1, buf.len)
            def c = _parse_abgr(val, 0)
            if c != 0 { vt = vt.set("def_fg", _opaque_abgr(c)) }
         }
      } elif str.startswith(buf, "11;") {
         def semi = str.find(buf, ";")
         if semi >= 0 {
            def val = str.str_slice(buf, semi + 1, buf.len)
            def c = _parse_abgr(val, 0)
            if c != 0 { vt = vt.set("def_bg", _opaque_abgr(c)) }
         }
      } elif str.startswith(buf, "12;") {
         def semi = str.find(buf, ";")
         if semi >= 0 {
            def val = str.str_slice(buf, semi + 1, buf.len)
            def c = _parse_abgr(val, 0)
            if c != 0 { vt = vt.set("cursor_bg", c) }
         }
      } elif str.startswith(buf, "52;") {
         def p1 = str.find(buf, ";")
         mut p2 = -1
         if p1 >= 0 { p2 = str.find(str.str_slice(buf, p1 + 1, buf.len), ";") }
         if p1 >= 0 && p2 >= 0 {
            def sel = str.str_slice(buf, p1 + 1, p1 + 1 + p2)
            def data = str.str_slice(buf, p1 + 1 + p2 + 1, buf.len)
            if str.find(sel, "c") != -1 {
               if data.len <= 1024 * 1024 {
                  if data.len == 0 { vt = _vt_clip_set(vt, "") } else {
                     def decoded = str_base.decode64(data)
                     vt = _vt_clip_set(vt, decoded)
                  }
               }
            }
         }
      }
      return vt
   }
   if kind == 95 && buf.len > 0 && load8(buf, 0) == 71 { return _kg_handle_apc(vt, buf) }
   vt
}

fn send_input(dict vt, str s) any {
   "Sends terminal input, retrying short writes so paste/repeat bursts are not truncated."
   def m = vt.get("master_fd")
   if m < 0 || s.len <= 0 { return 0 }
   mut off = 0
   while off < s.len {
      match sys.sys_write(m, to_int(s) + off, s.len - off) {
         ok(w) -> {
            if w <= 0 { return 0 }
            off += w
         }
         err(_e) -> { return 0 }
      }
   }
   0
}

fn _send_input_repeat(dict vt, str seq, int count) any {
   if count <= 0 || seq.len <= 0 { return 0 }
   if count == 1 { return send_input(vt, seq) }
   mut b = Builder(seq.len * count)
   mut i = 0
   while i < count {
      b = builder_append(b, seq)
      i += 1
   }
   def out = builder_to_str(b)
   builder_free(b)
   send_input(vt, out)
}

mut _scratch_buf = 0
mut _scratch_cap = 0

fn _get_scratch(int len) any {
   if _scratch_cap < len + 128 {
      if _scratch_buf { free(_scratch_buf) }
      def p = malloc(len + 128)
      if !p {
         _scratch_buf, _scratch_cap = 0, 0
         return 0
      }
      _scratch_buf, _scratch_cap = p, len + 128
   }
   _scratch_buf + 64
}

fn _cp_to_str(int cp, any acache) any {
   if cp < 128 { return str.chr(cp) }
   def scratch = _get_scratch(8)
   if !scratch { return "" }
   def n = str._utf8_encode_at(scratch, 0, cp)
   init_str(scratch, n)
}

fn _scroll_set(any st, int off, int hist_len) any {
   mut o = int(off)
   if o < 0 { o = 0 }
   if o > hist_len { o = hist_len }
   store32(st, o, OFF_SCROLL)
   store32_f32(st, float(o), OFF_SCROLL_F)
   store32_f32(st, 0.0, OFF_SCROLL_ACC)
   0
}

fn _vterm_clear_selection(any st) any {
   if !st { return 0 }
   store8(st, 0, OFF_SEL_ACTIVE)
   store8(st, 0, OFF_SEL_DRAGGING)
   store8(st, 0, OFF_SEL_MOVED)
   store32(st, 0, OFF_SEL_SX)
   store32(st, 0, OFF_SEL_SY)
   store32(st, 0, OFF_SEL_EX)
   store32(st, 0, OFF_SEL_EY)
   store32_f32(st, 0.0, OFF_SEL_SCROLL_ACC)
   0
}

fn _vterm_clear_selection_for_output(any st) any {
   if !st { return 0 }
   if load8(st, OFF_SEL_ACTIVE) != 0 || load8(st, OFF_SEL_DRAGGING) != 0 || load8(st, OFF_SEL_MOVED) != 0 {
      _vterm_clear_selection(st)
      _vterm_reset_click_sequence(st)
   }
   0
}

fn _vterm_has_selection_state(any st) bool {
   st && (load8(st, OFF_SEL_ACTIVE) != 0 || load8(st, OFF_SEL_DRAGGING) != 0 || load8(st, OFF_SEL_MOVED) != 0)
}

fn _vterm_state(any vt) any {
   is_dict(vt) ? vt.get("state", 0) : 0
}

fn selection_dragging(any vt) bool {
   "Runs the selection dragging operation."
   def st = _vterm_state(vt)
   st && load8(st, OFF_SEL_DRAGGING) != 0
}

fn selection_any(any vt) bool {
   "Runs the selection any operation."
   _vterm_has_selection_state(_vterm_state(vt))
}

fn clear_selection(any vt) bool {
   "Clears clear selection."
   if !is_dict(vt) || vt.get("sticky_selection", false) { return false }
   def st = _vterm_state(vt)
   if !_vterm_has_selection_state(st) { return false }
   _vterm_clear_selection(st)
   true
}

fn _vterm_selection_visible(dict vt, any st) bool {
   if !st || load8(st, OFF_SEL_ACTIVE) == 0 { return false }
   if load8(st, OFF_SEL_DRAGGING) != 0 { return true }
   vt.get("sticky_selection", false)
}

fn _vterm_reset_click_sequence(any st) any {
   if !st { return 0 }
   store64(st, 0, OFF_LAST_CLICK_T)
   store32(st, 0, OFF_CLICK_COUNT)
   0
}

fn _vterm_stop_selection_drag(any st, bool keep_active) any {
   if !st { return 0 }
   if !keep_active { return _vterm_clear_selection(st) }
   store8(st, 0, OFF_SEL_DRAGGING)
   store8(st, 0, OFF_SEL_MOVED)
   store32_f32(st, 0.0, OFF_SEL_SCROLL_ACC)
   0
}

fn _term_follow_enabled(dict vt) bool {
   def enabled = vt.get("scroll_follow_enabled", true)
   enabled
}

fn _term_repeat_ms(dict vt) f64 {
   def rep = float(vt.get("repeat_ms", 0.0))
   rep
}

fn _vterm_draw_scrollbar(any st, int ro, int hist_len, int scroll_off, any vis, any ww, any wh, any px, any py) any {
   if hist_len <= 0 || (load32(st, OFF_MODE) & MODE_ALTSCREEN) != 0 { return 0 }
   def sb_w, sb_x = 6.0, px + float(ww) - sb_w
   def track_h = float(wh)
   def total_lines = float(hist_len + ro)
   def thumb_h = max(32.0, track_h * float(ro) / total_lines)
   def max_off = max(1.0, float(hist_len))
   def scroll_thumb = 1.0 - (vis / max_off)
   def thumb_y = py + (track_h - thumb_h) * scroll_thumb
   draw_rect(sb_x, py, sb_w, float(wh), 0x22000000)
   draw_rect(sb_x, thumb_y, sb_w, thumb_h, (scroll_off > 0) ? 0xDD999999 : 0x88999999)
}

fn _vterm_draw_cpu(dict vt, any st, int co, int ro, any g, list history, int hist_len, int scroll_off, any scroll_frac_r, any cw, any ch, any wh, any px, any py, int f_reg, any gy_reg, any db) any {
   def acache = vt.get("ascii_cache")
   def sel_bg = _vterm_overlay_color(vt, "sel_bg", 0x70aa5544)
   def sel_active = _vterm_selection_visible(vt, st)
   mut text_runs = []
   mut s_col, s_row = load32(st, OFF_SEL_SX), load32(st, OFF_SEL_SY)
   mut e_col, e_row = load32(st, OFF_SEL_EX), load32(st, OFF_SEL_EY)
   if sel_active && (s_row > e_row || (s_row == e_row && s_col > e_col)) {
      mut tmp = s_row s_row = e_row e_row = tmp
      tmp = s_col s_col = e_col e_col = tmp
   }
   mut r = -1
   while r <= ro {
      def abs_r = (hist_len - scroll_off) + r
      if abs_r < 0 || abs_r >= hist_len + ro { r += 1 continue }
      def line_ptr = _vterm_draw_line_ptr(g, history, hist_len, co, abs_r)
      def ry = floor((float(r) + scroll_frac_r) * ch + py + 0.5)
      if ry + ch < py || ry > py + wh { r += 1 continue }
      mut sel_in_row = false
      mut sel_s, sel_e = 0, -1
      if sel_active {
         if abs_r > s_row && abs_r < e_row { sel_in_row = true sel_s = 0 sel_e = co - 1 }
         elif abs_r == s_row && abs_r == e_row { sel_in_row = true sel_s = s_col sel_e = e_col }
         elif abs_r == s_row { sel_in_row = true sel_s = s_col sel_e = co - 1 }
         elif abs_r == e_row { sel_in_row = true sel_s = 0 sel_e = e_col }
      }
      mut c = 0
      mut run_b = []
      mut run_x = 0.0
      mut run_fg = 0
      while c < co {
         def off = c * 16
         def cp = load32(line_ptr, off)
         def md = load32(line_ptr, off + 12)
         def bg_val = _opaque_abgr(load32(line_ptr, off + 8))
         def text_visible = (md & (ATTR_WDUMMY | ATTR_INVIS | ATTR_IMAGE)) == 0 && cp > 32
         mut rfg = 0
         mut rbg = bg_val
         if (md & ATTR_REVERSE) != 0 {
            rbg = load32(line_ptr, off + 4) | 0xFF000000
            if text_visible { rfg = bg_val | 0xFF000000 }
         } elif text_visible {
            rfg = load32(line_ptr, off + 4) | 0xFF000000
         }
         if sel_in_row && c >= sel_s && c <= sel_e { rbg = _blend_abgr_over(sel_bg, (rbg == 0) ? db : rbg) }
         def rx = floor(px + float(c) * cw + 0.5)
         if rbg != db && rbg != 0 && (rbg & 0xFF000000) != 0 { draw_rect(rx, ry, cw, ch, rbg) }
         if text_visible {
            if run_b.len <= 0 || run_fg != rfg {
               if run_b.len > 0 {
                  def txt = builder_to_str(run_b)
                  builder_free(run_b)
                  if txt.len > 0 {
                     text_runs = text_runs.append(txt)
                     text_runs = text_runs.append(run_x)
                     text_runs = text_runs.append(ry + gy_reg)
                     text_runs = text_runs.append(run_fg)
                  }
               }
               run_b = Builder(32)
               run_x = rx
               run_fg = rfg
            }
            if cp < 128 { run_b = builder_append_byte(run_b, cp) }
            else { run_b = builder_append(run_b, _cp_to_str(cp, acache)) }
         } elif run_b.len > 0 {
            def txt = builder_to_str(run_b)
            builder_free(run_b)
            if txt.len > 0 {
               text_runs = text_runs.append(txt)
               text_runs = text_runs.append(run_x)
               text_runs = text_runs.append(ry + gy_reg)
               text_runs = text_runs.append(run_fg)
            }
            run_b = []
         }
         c += 1
      }
      if run_b.len > 0 {
         def txt = builder_to_str(run_b)
         builder_free(run_b)
         if txt.len > 0 {
            text_runs = text_runs.append(txt)
            text_runs = text_runs.append(run_x)
            text_runs = text_runs.append(ry + gy_reg)
            text_runs = text_runs.append(run_fg)
         }
      }
      r += 1
   }
   if text_runs.len > 0 { draw_text_runs_flat_colors(f_reg, text_runs) }
   if load8(st, OFF_CURSOR_VISIBLE) != 0 && scroll_off == 0 && !sel_active {
      _vterm_clamp_cursor(vt, st, co, ro)
      mut ccx, ccy = load32(st, OFF_CX), load32(st, OFF_CY)
      if ccx >= co { ccx = co - 1 }
      if ccy >= ro { ccy = ro - 1 } elif ccy < 0 { ccy = 0 }
      def cs_raw = load8(st, OFF_CURSOR_STYLE_S)
      def cursor_style = (cs_raw > 0) ? cs_raw : vt.get("cursor_style", 2)
      def do_blink = vt.get("cursor_blink_enabled", true)
      def now_t = ticks()
      def recent_key = (now_t - max(load64(st, OFF_LAST_KEY_T), load64(st, OFF_LAST_CHAR_T))) < CURSOR_RECENT_KEY_NS
      def visible = recent_key || !do_blink || (int(now_t / CURSOR_BLINK_NS) % 2 == 0)
      if visible {
         def rx, ry = floor(float(ccx) * cw + px + 0.5), floor((float(ccy) + scroll_frac_r) * ch + py + 0.5)
         def cur_bg = _vterm_cursor_color(vt, "cursor_bg", 0xffffffff)
         if cursor_style == 3 {
            def uh = max(2.0, floor(ch * 0.12 + 0.5))
            draw_rect(rx, ry + ch - uh, cw, uh, cur_bg)
         } elif cursor_style == 2 {
            def bw = max(2.0, floor(cw * 0.12 + 0.5))
            draw_rect(rx, ry, bw, ch, cur_bg)
         } else {
            draw_rect(rx, ry, cw, ch, cur_bg)
         }
      }
   }
   vt
}

fn _vterm_cursor_color(dict vt, str key, int fallback) int {
   def c = int(vt.get(key, fallback))
   (c & 0x00ffffff) | 0xff000000
}

fn _vterm_overlay_color(dict vt, str key, int fallback) int {
   int(vt.get(key, fallback))
}

fn _vterm_draw_cursor(dict vt, any st, int co, int ro, any g, any gptr, any cw, any ch, any px, any py, any scroll_frac_r, any gy_reg, any ascent, int glyph_tex) any {
   if load8(st, OFF_CURSOR_VISIBLE) == 0 { return 0 }
   if _vterm_selection_visible(vt, st) { return 0 }
   _vterm_clamp_cursor(vt, st, co, ro)
   mut ccx, ccy = load32(st, OFF_CX), load32(st, OFF_CY)
   if ccx >= co { ccx = co - 1 }
   if ccy >= ro { ccy = ro - 1 } elif ccy < 0 { ccy = 0 }
   def cs_raw = load8(st, OFF_CURSOR_STYLE_S)
   def cursor_style = (cs_raw > 0) ? cs_raw : vt.get("cursor_style", 2)
   def do_blink = vt.get("cursor_blink_enabled", true)
   def now_t = ticks()
   def recent_key = (now_t - max(load64(st, OFF_LAST_KEY_T), load64(st, OFF_LAST_CHAR_T))) < CURSOR_RECENT_KEY_NS
   def visible = recent_key || !do_blink || (int(now_t / CURSOR_BLINK_NS) % 2 == 0)
   if !visible { return 0 }
   def rx, ry = floor(float(ccx) * cw + px + 0.5), floor((float(ccy) + scroll_frac_r) * ch + py + 0.5)
   def cur_bg, cur_fg = _vterm_cursor_color(vt, "cursor_bg", 0xffffffff), _vterm_cursor_color(vt, "cursor_fg", 0xff000000)
   def c_off = (ccy * co + ccx) * 16
   def c_md = load32(g, c_off + 12)
   def cur_cw = ((c_md & ATTR_WIDE) != 0) ? (cw * 2.0) : cw
   texture_bind_default()
   if cursor_style == 3 {
      def uh = max(2.0, floor(ch * 0.12 + 0.5))
      draw_rect_fast(rx, ry + ch - uh, cur_cw, uh, cur_bg)
   } elif cursor_style == 2 {
      def bw = max(2.0, floor(cw * 0.12 + 0.5))
      draw_rect_fast(rx, ry, bw, ch, cur_bg)
   } else {
      draw_rect_fast(rx, ry, cur_cw, ch, cur_bg)
   }
   def c_cp = load32(g, c_off)
   if c_cp > 32 && cursor_style != 2 {
      if glyph_tex >= 0 { texture_bind(glyph_tex) }
      def by = floor(ry + gy_reg + ascent + 0.5)
      draw_text_glyph_fast(gptr, rx, by, c_cp, cur_fg)
   }
}

fn _vterm_draw_line_ptr(any g, list history, int hist_len, int co, int abs_r) any {
   if abs_r < hist_len { return history.get(abs_r) }
   g + ((abs_r - hist_len) * co * 16)
}

@jit
fn _vterm_draw_background_line_ptr(any line_ptr, int co, f64 px, f64 ry, f64 cw, f64 ch, int db, int reverse_mask) any {
   mut c = 0
   while c < co {
      def off = c * 16
      def md = load32(line_ptr, off + 12)
      mut rbg = (md & reverse_mask) ? (load32(line_ptr, off + 4) | 0xFF000000) : ((load32(line_ptr, off + 8) & 0x00ffffff) | 0xff000000)
      if rbg != db && rbg != 0 && (rbg & 0xFF000000) != 0 {
         mut run_len = 1
         while c + run_len < co {
            def off2 = (c + run_len) * 16
            def md2 = load32(line_ptr, off2 + 12)
            def rbg2 = (md2 & reverse_mask) ? (load32(line_ptr, off2 + 4) | 0xFF000000) : ((load32(line_ptr, off2 + 8) & 0x00ffffff) | 0xff000000)
            if rbg2 == db || rbg2 != rbg { break }
            run_len += 1
         }
         draw_rect_fast(px + float(c) * cw, ry, cw * float(run_len), ch, rbg)
         c += run_len
      } else {
         c += 1
      }
   }
   0
}

fn _vterm_draw_backgrounds(dict vt, any st, int co, int ro, any g, list history, int hist_len, int scroll_off, any scroll_frac_r, any cw, any ch, any wh, any px, any db) any {
   mut r = -1
   while r <= ro {
      def abs_r = (hist_len - scroll_off) + r
      if abs_r < 0 || abs_r >= hist_len + ro { r += 1 continue }
      def line_ptr = _vterm_draw_line_ptr(g, history, hist_len, co, abs_r)
      def ry = (float(r) + scroll_frac_r) * ch + _py
      if ry + ch < _py || ry > _py + wh { r += 1 continue }
      _vterm_draw_background_line_ptr(line_ptr, co, px, ry, cw, ch, db, ATTR_REVERSE)
      r += 1
   }
}

fn _vterm_draw_selection_overlay(dict vt, any st, int co, int ro, any g, list history, int hist_len, int scroll_off, any scroll_frac_r, any cw, any ch, any wh, any px, any db) any {
   if !_vterm_selection_visible(vt, st) { return 0 }
   def ssx = load32(st, OFF_SEL_SX) def ssy = load32(st, OFF_SEL_SY)
   def sex = load32(st, OFF_SEL_EX) def sey = load32(st, OFF_SEL_EY)
   mut s_row, s_col, e_row, e_col = ssy, ssx, sey, sex
   if s_row > e_row || (s_row == e_row && s_col > e_col) { s_row = sey s_col = sex e_row = ssy e_col = ssx }
   def sel_bg = _vterm_overlay_color(vt, "sel_bg", 0x70aa5544)
   texture_bind_default()
   mut r = -1
   while r <= ro {
      def abs_r = (hist_len - scroll_off) + r
      if abs_r < 0 || abs_r >= hist_len + ro { r += 1 continue }
      def line_ptr = _vterm_draw_line_ptr(g, history, hist_len, co, abs_r)
      def ry = (float(r) + scroll_frac_r) * ch + _py
      if ry + ch < _py || ry > _py + wh { r += 1 continue }
      mut x1, x2 = 0, -1
      if abs_r > s_row && abs_r < e_row { x1 = 0 x2 = co - 1 }
      elif abs_r == s_row && abs_r == e_row { x1 = s_col x2 = e_col }
      elif abs_r == s_row { x1 = s_col x2 = co - 1 }
      elif abs_r == e_row { x1 = 0 x2 = e_col }
      if x2 >= x1 {
         if x1 < 0 { x1 = 0 } elif x1 >= co { x1 = co - 1 }
         if x2 < 0 { x2 = 0 } elif x2 >= co { x2 = co - 1 }
         mut c = x1
         while c <= x2 {
            def off = c * 16
            def md = load32(line_ptr, off + 12)
            def bg_val = _opaque_abgr(load32(line_ptr, off + 8))
            def fg_val = load32(line_ptr, off + 4)
            def base = (md & ATTR_REVERSE) ? (fg_val | 0xff000000) : bg_val
            def mixed = _blend_abgr_over(sel_bg, (base == 0) ? db : base)
            mut run_len = 1
            while c + run_len <= x2 {
               def off2 = (c + run_len) * 16
               def md2 = load32(line_ptr, off2 + 12)
               def bg2 = _opaque_abgr(load32(line_ptr, off2 + 8))
               def fg2 = load32(line_ptr, off2 + 4)
               def base2 = (md2 & ATTR_REVERSE) ? (fg2 | 0xff000000) : bg2
               def mixed2 = _blend_abgr_over(sel_bg, (base2 == 0) ? db : base2)
               if mixed2 != mixed { break }
               run_len += 1
            }
            draw_rect_fast(px + float(c) * cw, ry, float(run_len) * cw, ch, mixed)
            c += run_len
         }
      }
      r += 1
   }
   0
}

fn _vterm_draw_foregrounds(dict vt, int co, int ro, any g, list history, int hist_len, int scroll_off, any scroll_frac_r, any cw, any ch, any wh, any px, any py, any gptr, int f_reg, any gy_reg, any ascent) any {
   def skip_mask = ATTR_WDUMMY | ATTR_INVIS | ATTR_IMAGE
   def acache = vt.get("ascii_cache")
   mut r = -1
   while r <= ro {
      def abs_r = (hist_len - scroll_off) + r
      if abs_r < 0 || abs_r >= hist_len + ro { r += 1 continue }
      def line_ptr = _vterm_draw_line_ptr(g, history, hist_len, co, abs_r)
      def ry = floor((float(r) + scroll_frac_r) * ch + py + 0.5)
      if ry + ch < py || ry > py + wh { r += 1 continue }
      def by = floor(ry + gy_reg + ascent + 0.5)
      draw_terminal_line_fast_ptr(line_ptr, co, px, by, cw, gptr, skip_mask, ATTR_REVERSE)
      mut c = 0
      while c < co {
         def off = c * 16
         def cp = load32(line_ptr, off)
         if cp > 32 {
            def md = load32(line_ptr, off + 12)
            if (md & skip_mask) == 0 && !font_fast_glyph_present(gptr, cp) {
               def fg = ((md & ATTR_REVERSE) != 0) ? (load32(line_ptr, off + 8) | 0xFF000000) : (load32(line_ptr, off + 4) | 0xFF000000)
               draw_text(f_reg, _cp_to_str(cp, acache), floor(px + float(c) * cw + 0.5), ry + gy_reg, fg)
            }
         }
         c += 1
      }
      r += 1
   }
}

fn draw(dict vt, any ww, any wh) any {
   "Draws draw."
   def st = vt.get("state")
   def raw_db = vt.get("def_bg")
   def db = _opaque_abgr(raw_db)
   if db != raw_db {
      vt = vt.set("def_bg", db)
      def cur_bg = load32(st, OFF_CUR_BG)
      if (cur_bg & 0x00ffffff) == (db & 0x00ffffff) { store32(st, db, OFF_CUR_BG) }
   }
   def co = vt.get("cols") def ro = vt.get("rows") def g = vt.get("grid")
   def fonts = vt.get("fonts")
   mut f_reg = fonts.get("regular") if !f_reg { f_reg = 0 }
   f_reg = font_prepare(f_reg, " ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789$#/>-_=+[]{}().,:;")
   def f_obj = _font_get(f_reg)
   def f_size = (f_obj != 0) ? float(f_obj.get("size", 16.0)) : 16.0
   def ascent  = (f_obj != 0) ? float(f_obj.get("ascent", 12.0)) : (f_size * 0.80)
   def descent = (f_obj != 0) ? float(f_obj.get("descent", -4.0)) : (0.0 - f_size * 0.20)
   mut ch, cw = vt.get("char_h", floor(f_size * 1.25)), vt.get("char_w", 9.0)
   def gy_reg = max(0.0, floor((ch - (ascent - descent)) * 0.5 + 0.5))
   def px = floor(_px + 0.5)
   def py = floor(_py + 0.5)
   def history = vt.get("history")
   def hist_len = history.len
   mut scroll_f_val = load32_f32(st, OFF_SCROLL_F)
   if scroll_f_val < 0.0 {
      scroll_f_val = 0.0
      _scroll_set(st, 0, hist_len)
   }
   if scroll_f_val > float(hist_len) {
      scroll_f_val = float(hist_len)
      _scroll_set(st, int(scroll_f_val), hist_len)
   }
   def vis_prev = vt.get("scroll_vis", scroll_f_val)
   def vis = vis_prev + (scroll_f_val - vis_prev) * 0.25
   if vis != vis_prev { vt = vt.set("scroll_vis", vis) }
   def scroll_i = int(floor(vis))
   def scroll_frac = vis - float(scroll_i)
   def scroll_frac_r = _snap_scroll_frac(scroll_frac, ch)
   def scroll_off = scroll_i
   set_unlit(true)
   def backend = get_active_backend()
   if backend != BACKEND_MOCK { texture_bind_default() }
   draw_rect(px, py, ww, wh, db)
   if !terminal_fast_text_supported() {
      return _vterm_draw_cpu(vt, st, co, ro, g, history, hist_len, scroll_off, scroll_frac_r, cw, ch, wh, px, py, f_reg, gy_reg, db)
   }
   def gptr = (f_obj != 0) ? f_obj.get("fast_glyphs", 0) : 0
   if !gptr { return _vterm_draw_cpu(vt, st, co, ro, g, history, hist_len, scroll_off, scroll_frac_r, cw, ch, wh, px, py, f_reg, gy_reg, db) }
   def page0 = load64(gptr, 0)
   if !page0 { return _vterm_draw_cpu(vt, st, co, ro, g, history, hist_len, scroll_off, scroll_frac_r, cw, ch, wh, px, py, f_reg, gy_reg, db) }
   reset_overlay_state()
   set_unlit(true)
   def tid_def = font_fast_glyph_texture(gptr, 63)
   if tid_def < 0 { return _vterm_draw_cpu(vt, st, co, ro, g, history, hist_len, scroll_off, scroll_frac_r, cw, ch, wh, px, py, f_reg, gy_reg, db) }
   texture_bind_default()
   _vterm_draw_backgrounds(vt, st, co, ro, g, history, hist_len, scroll_off, scroll_frac_r, cw, ch, wh, px, db)
   _kg_draw_visible_images(vt, co, ro, g, history, hist_len, scroll_off, scroll_frac_r, cw, ch, wh)
   _vterm_draw_selection_overlay(vt, st, co, ro, g, history, hist_len, scroll_off, scroll_frac_r, cw, ch, wh, px, db)
   ;; The Vulkan text backend binds/syncs atlas pages per vertex; avoid forcing
   ;; one concrete texture before foregrounds, which can add a redundant flush.
   _vterm_draw_foregrounds(vt, co, ro, g, history, hist_len, scroll_off, scroll_frac_r, cw, ch, wh, px, py, gptr, f_reg, gy_reg, ascent)
   texture_bind_default()
   _vterm_draw_scrollbar(st, ro, hist_len, scroll_off, vis, ww, wh, px, py)
   if scroll_i == 0 { _vterm_draw_cursor(vt, st, co, ro, g, gptr, cw, ch, px, py, scroll_frac_r, gy_reg, ascent, tid_def) }
   vt
}

fn _vterm_handle_mouse_press(dict vt, any st, int co, int ro, list history, int hist_len, f64 cw, f64 ch, any da) dict {
   store32_f32(st, float(da.get("x", 0.0)), OFF_LAST_MX)
   store32_f32(st, float(da.get("y", 0.0)), OFF_LAST_MY)
   if _vterm_has_selection_state(st) && (da.get("mod", 0) & MOD_SHIFT) == 0 {
      _vterm_clear_selection(st)
      _vterm_reset_click_sequence(st)
      return vt
   }
   if (load32(st, OFF_MODE) & MODE_MOUSE) != 0 && (da.get("mod", 0) & MOD_SHIFT) == 0 &&
   load8(st, OFF_SB_DRAGGING) == 0 && load8(st, OFF_SEL_DRAGGING) == 0{
      def mf = load8(st, OFF_MOUSE_FLAGS)
      if mf != 0 {
         def btn = _mouse_xterm_button(int(da.get("button", 0)))
         def cell = _mouse_cell_clamped(da, cw, ch, co, ro)
         _mouse_send(vt, btn, cell.get(0), cell.get(1), false)
         store8(st, 1, OFF_MOUSE_BTNDOWN)
         store8(st, btn, OFF_MOUSE_BTN)
         return vt
      }
   }
   def native_btn = int(da.get("button", 0))
   if !_mouse_is_left(native_btn) {
      if _mouse_is_right(native_btn) {
         _vterm_clear_selection(st)
         _vterm_reset_click_sequence(st)
      }
      return vt
   }
   def g = vt.get("grid")
   def now = ticks()
   def mx_raw = float(da.get("x")) def my_raw = float(da.get("y"))
   def xy = _mouse_xy_clamped(da, cw, ch, co, ro)
   def sx, sy = xy.get(0), xy.get(1)
   def ww, wh = float(da.get("ww", float(co) * cw + _px * 2.0)), float(da.get("wh", float(ro) * ch + _py * 2.0))
   def sb_w, sb_x = 6.0, ww - sb_w - _px
   if hist_len > 0 && mx_raw >= sb_x {
      _vterm_clear_selection(st)
      _vterm_reset_click_sequence(st)
      store8(st, 1, OFF_SB_DRAGGING)
      def track_h = wh - _py * 2.0
      def fy = (my_raw - _py) / track_h
      mut n_off = int((1.0 - fy) * float(hist_len))
      if n_off < 0 { n_off = 0 } elif n_off > hist_len { n_off = hist_len }
      _scroll_set(st, n_off, hist_len)
      return vt
   }
   def last_t = load64(st, OFF_LAST_CLICK_T)
   def lx = load32(st, OFF_CLICK_X) def ly = load32(st, OFF_CLICK_Y)
   def abs_sy = (hist_len - load32(st, OFF_SCROLL)) + sy
   mut count = 1
   if now - last_t < 400000000 && sx == lx && sy == ly { count = load32(st, OFF_CLICK_COUNT) + 1 }
   if count > 3 { count = 1 }
   store32(st, count, OFF_CLICK_COUNT) store64(st, now, OFF_LAST_CLICK_T)
   store32(st, sx, OFF_CLICK_X) store32(st, sy, OFF_CLICK_Y)
   store8(st, 1, OFF_SEL_DRAGGING) store8(st, 0, OFF_SEL_MOVED)
   if count == 1 {
      store8(st, 0, OFF_SEL_ACTIVE)
      store32(st, sx, OFF_SEL_SX) store32(st, abs_sy, OFF_SEL_SY)
      store32(st, sx, OFF_SEL_EX) store32(st, abs_sy, OFF_SEL_EY)
   } elif count == 2 {
      store8(st, 1, OFF_SEL_ACTIVE)
      def line_ptr = (abs_sy < hist_len) ? history.get(abs_sy) : ptr_add(g, (abs_sy - hist_len) * co * 16)
      mut x1 = sx while x1 > 0 {
         def cp = load32(line_ptr, (x1 - 1) * 16)
         if cp <= 32 || cp == 34 || cp == 39 || cp == 40 || cp == 41 || cp == 44 ||
         cp == 46 || cp == 58 || cp == 59 || cp == 61 || cp == 91 || cp == 93 ||
         cp == 96 || cp == 123 || cp == 125{ break }
         x1 -= 1
      }
      mut x2 = sx while x2 < co - 1 {
         def cp = load32(line_ptr, (x2 + 1) * 16)
         if cp <= 32 || cp == 34 || cp == 39 || cp == 40 || cp == 41 || cp == 44 ||
         cp == 46 || cp == 58 || cp == 59 || cp == 61 || cp == 91 || cp == 93 ||
         cp == 96 || cp == 123 || cp == 125{ break }
         x2 += 1
      }
      store32(st, x1, OFF_SEL_SX)
      store32(st, abs_sy, OFF_SEL_SY)
      store32(st, x2, OFF_SEL_EX)
      store32(st, abs_sy, OFF_SEL_EY)
   } elif count == 3 {
      store8(st, 1, OFF_SEL_ACTIVE)
      store32(st, 0, OFF_SEL_SX)
      store32(st, abs_sy, OFF_SEL_SY)
      store32(st, co - 1, OFF_SEL_EX)
      store32(st, abs_sy, OFF_SEL_EY)
   }
   vt
}

fn _vterm_handle_mouse_motion(dict vt, any st, int co, int ro, int hist_len, f64 cw, f64 ch, any da) dict {
   def mx = float(da.get("x")) def my = float(da.get("y"))
   store32_f32(st, mx, OFF_LAST_MX)
   store32_f32(st, my, OFF_LAST_MY)
   if (load32(st, OFF_MODE) & MODE_MOUSE) != 0 && (da.get("mod", 0) & MOD_SHIFT) == 0 {
      def mf = load8(st, OFF_MOUSE_FLAGS)
      if (mf & MOUSE_1003_MOTION) != 0 || ((mf & MOUSE_1002_DRAG) != 0 && load8(st, OFF_MOUSE_BTNDOWN) != 0) {
         def cell = _mouse_cell_clamped(da, cw, ch, co, ro)
         def col = cell.get(0) def row = cell.get(1)
         def btn = load8(st, OFF_MOUSE_BTN)
         _mouse_send(vt, btn + 32, col, row, false)
         return vt
      }
   }
   if load8(st, OFF_SB_DRAGGING) != 0 {
      def wh = float(da.get("wh", float(ro) * ch + _py * 2.0))
      def track_h = wh - _py * 2.0
      def fy = (my - _py) / track_h
      mut n_off = int((1.0 - fy) * float(hist_len))
      if n_off < 0 { n_off = 0 } elif n_off > hist_len { n_off = hist_len }
      _scroll_set(st, n_off, hist_len)
      return vt
   }
   if load8(st, OFF_SEL_DRAGGING) == 0 { return vt }
   def xy = _mouse_xy_clamped(da, cw, ch, co, ro)
   def ex, ey = xy.get(0), xy.get(1)
   mut cur_off = load32(st, OFF_SCROLL)
   mut acc = load32_f32(st, OFF_SEL_SCROLL_ACC)
   if my < _py { acc += _scroll_process((_py - my) / 8.0) } elif my > (float(ro) * ch + _py) { acc += _scroll_process(-((my - (float(ro) * ch + _py)) / 8.0)) } else { acc = 0.0 }
   mut delta = int(acc)
   acc -= float(delta)
   store32_f32(st, acc, OFF_SEL_SCROLL_ACC)
   mut n_scroll = cur_off + delta
   if n_scroll < 0 { n_scroll = 0 } elif n_scroll > hist_len { n_scroll = hist_len }
   if n_scroll != cur_off { _scroll_set(st, n_scroll, hist_len) }
   mut abs_ey = (hist_len - n_scroll) + ey
   if abs_ey < 0 { abs_ey = 0 } elif abs_ey >= hist_len + int(vt.get("rows")) { abs_ey = hist_len + int(vt.get("rows")) - 1 }
   store32(st, ex, OFF_SEL_EX)
   store32(st, abs_ey, OFF_SEL_EY)
   if ex != load32(st, OFF_SEL_SX) || abs_ey != load32(st, OFF_SEL_SY) {
      store8(st, 1, OFF_SEL_MOVED)
      store8(st, 1, OFF_SEL_ACTIVE)
   }
   vt
}

fn _vterm_handle_mouse_release(dict vt, any st, int co, list history, int hist_len, f64 cw, f64 ch, any da) dict {
   store8(st, 0, OFF_SEL_DRAGGING)
   store8(st, 0, OFF_SB_DRAGGING)
   if (load32(st, OFF_MODE) & MODE_MOUSE) != 0 && (da.get("mod", 0) & MOD_SHIFT) == 0 {
      def mf = load8(st, OFF_MOUSE_FLAGS)
      if mf != 0 {
         def btn = _mouse_xterm_button(int(da.get("button", 0)))
         def cell = _mouse_cell_clamped(da, cw, ch, int(vt.get("cols")), int(vt.get("rows")))
         _mouse_send(vt, btn, cell.get(0), cell.get(1), true)
         store8(st, 0, OFF_MOUSE_BTNDOWN)
         return vt
      }
   }
   def native_btn = int(da.get("button", 0))
   if _mouse_is_left(native_btn) {
      if load8(st, OFF_SEL_ACTIVE) != 0 {
         def moved = load8(st, OFF_SEL_MOVED) != 0
         def cnt = load32(st, OFF_CLICK_COUNT)
         if moved || cnt >= 2 {
            def full_txt = _vt_selection_text(vt, history, load32(st, OFF_SEL_SY), load32(st, OFF_SEL_SX), load32(st, OFF_SEL_EY), load32(st,
               OFF_SEL_EX),
               hist_len,
            co)
            def keep_sticky = vt.get("sticky_selection", false)
            if keep_sticky {
               _vterm_stop_selection_drag(st, true)
               if moved { _vterm_reset_click_sequence(st) }
            } else {
               _vterm_clear_selection(st)
               _vterm_reset_click_sequence(st)
            }
            if full_txt.len > 0 {
               vt = _vt_clip_set(vt, full_txt)
            }
         } else {
            _vterm_clear_selection(st)
         }
      } else {
         _vterm_clear_selection(st)
      }
   } elif _mouse_is_middle(native_btn) {
      def txt = _vt_clip_get(vt)
      if txt.len > 0 {
         _scroll_set(st, 0, hist_len)
         if load8(st, OFF_BRACKET_PASTE) != 0 { send_input(vt, "\033[200~" + txt + "\033[201~") } else { send_input(vt, txt) }
      }
   } elif _mouse_is_right(native_btn) {
      _vterm_clear_selection(st)
      _vterm_reset_click_sequence(st)
   }
   vt
}

fn _vterm_follow_cursor(dict vt, any st, int scroll_off, int hist_len) any {
   if load8(st, OFF_SEL_ACTIVE) != 0 || load8(st, OFF_SEL_DRAGGING) != 0 || load8(st, OFF_SEL_MOVED) != 0 { _vterm_clear_selection(st) }
   if _term_follow_enabled(vt) && scroll_off != 0 { _scroll_set(st, 0, hist_len) }
}

fn _vterm_key_hit(dict vt) list { [true, vt] }

fn _vterm_key_miss(dict vt) list { [false, vt] }

fn _vterm_clear_pending_printable(any st) any {
   store8(st, 0, OFF_PENDING_PRINT)
   store32(st, 0, OFF_PENDING_CHAR)
   store32(st, 0, OFF_PENDING_MOD)
   store32(st, 0, OFF_PENDING_ACTION)
   store64(st, 0, OFF_PENDING_T)
   0
}

fn _vterm_printable_code(int k, int md) int {
   if (md & (MOD_CONTROL | MOD_ALT | MOD_SUPER | MOD_META)) != 0 { return -1 }
   if k >= 32 && k <= 126 {
      mut out = k
      if (md & MOD_SHIFT) != 0 {
         if k >= 65 && k <= 90 { out = k }
         else { out = _shift_char(k) }
      } else {
         if k >= 65 && k <= 90 { out = k + 32 }
      }
      return out
   }
   if k >= KEY_KP_0 && k <= KEY_KP_9 { return 48 + (k - KEY_KP_0) }
   case k {
      KEY_KP_DECIMAL -> 46
      KEY_KP_DIVIDE -> 47
      KEY_KP_MULTIPLY -> 42
      KEY_KP_SUBTRACT -> 45
      KEY_KP_ADD -> 43
      KEY_KP_EQUAL -> 61
      _ -> (k > 126 && k < 256) ? k : -1
   }
}

fn _vterm_send_code(dict vt, any st, int hist_len, int scroll_off, int code, int md) dict {
   if code < 32 || code == 127 { return vt }
   _vterm_follow_cursor(vt, st, scroll_off, hist_len)
   store64(st, ticks(), OFF_LAST_CHAR_T)
   store32(st, code, OFF_LAST_CHAR_C)
   if (md & MOD_ALT) != 0 { send_input(vt, "\033") }
   def text = _cp_to_str(code, vt.get("ascii_cache"))
   send_input(vt, text)
   if vt.get("master_fd", -1) < 0 { return write(vt, text) }
   vt
}

fn _vterm_queue_printable(dict vt, any st, int k, int md, int action) list {
   def code = _vterm_printable_code(k, md)
   if code < 0 { return _vterm_key_miss(vt) }
   if action != 1 && action != 2 { return _vterm_key_hit(vt) }
   store8(st, 1, OFF_PENDING_PRINT)
   store32(st, code, OFF_PENDING_CHAR)
   store32(st, md, OFF_PENDING_MOD)
   store32(st, action, OFF_PENDING_ACTION)
   store64(st, ticks(), OFF_PENDING_T)
   _vterm_key_hit(vt)
}

fn _vterm_flush_pending_printable(dict vt, any st) dict {
   if load8(st, OFF_PENDING_PRINT) == 0 { return vt }
   def code = load32(st, OFF_PENDING_CHAR)
   def md = load32(st, OFF_PENDING_MOD)
   def action = load32(st, OFF_PENDING_ACTION)
   _vterm_clear_pending_printable(st)
   if action != 1 && action != 2 { return vt }
   _vterm_send_code(vt, st, vt.get("history").len, load32(st, OFF_SCROLL), code, md)
}

fn _vterm_handle_key_release(dict vt, any st, any da) dict {
   def k = da.get("key")
   def md = da.get("mod", 0) & 0xFF
   def flags = load32(st, OFF_KBD_FLAGS)
   if (flags & KBD_REPORT_EVENTS) == 0 { return vt }
   if k >= 32 && k <= 126 {
      def cp = (k >= 65 && k <= 90) ? (k + 32) : k
      _kitty_kbd_send_event(vt, cp, md, 3)
   } elif (flags & KBD_REPORT_ALL) != 0 {
      if k == 27 || k == KEY_ESCAPE { _kitty_kbd_send_event(vt, 27, md, 3) }
      elif k == 13 || k == KEY_ENTER || k == KEY_KP_ENTER { _kitty_kbd_send_event(vt, 13, md, 3) }
      elif k == 9 || k == KEY_TAB { _kitty_kbd_send_event(vt, 9, md, 3) }
      elif k == 8 || k == KEY_BACKSPACE { _kitty_kbd_send_event(vt, 127, md, 3) }
   }
   vt
}

fn _vterm_handle_key_char(dict vt, any st, int hist_len, int scroll_off, any da) dict {
   def c = da.get("char")
   if c >= 32 && c != 127 {
      if load8(st, OFF_PENDING_PRINT) != 0 {
         _vterm_clear_pending_printable(st)
      }
      def rep_ms = _term_repeat_ms(vt)
      def now_t = ticks()
      if rep_ms > 0.0
      && load32(st, OFF_LAST_CHAR_C) == int(c)
      && (now_t - load64(st, OFF_LAST_CHAR_T)) < int(rep_ms * 1000000.0){
         return vt
      }
      store8(st, 1, OFF_CHAR_CB_ACTIVE)
      def md = da.get("mod", 0)
      def out = _vterm_send_code(vt, st, hist_len, scroll_off, c, md)
      return out
   }
   vt
}

fn _vterm_handle_ctrl_shift_key(dict vt, any st, int co, list history, int hist_len, int scroll_off, int k) list {
   if k == KEY_V || k == KEY_INSERT {
      def txt = _vt_clip_get(vt)
      if txt.len > 0 {
         _vterm_follow_cursor(vt, st, scroll_off, hist_len)
         if load8(st, OFF_BRACKET_PASTE) != 0 { send_input(vt, "\033[200~" + txt + "\033[201~") } else { send_input(vt, txt) }
      }
      return _vterm_key_hit(vt)
   }
   if k == KEY_C {
      if load8(st, OFF_SEL_ACTIVE) == 0 { return _vterm_key_hit(vt) }
      def full_txt = _vt_selection_text(vt, history, load32(st, OFF_SEL_SY), load32(st, OFF_SEL_SX), load32(st, OFF_SEL_EY), load32(st,
         OFF_SEL_EX),
         hist_len,
      co)
      if full_txt.len > 0 {
         vt = _vt_clip_set(vt, full_txt)
      }
      return _vterm_key_hit(vt)
   }
   _vterm_key_miss(vt)
}

fn _vterm_handle_shift_insert(dict vt, any st, int hist_len, int scroll_off, int k) list {
   if k != KEY_INSERT { return _vterm_key_miss(vt) }
   def txt = _vt_clip_get(vt)
   if txt.len > 0 {
      _vterm_follow_cursor(vt, st, scroll_off, hist_len)
      if load8(st, OFF_BRACKET_PASTE) != 0 { send_input(vt, "\033[200~" + txt + "\033[201~") } else { send_input(vt, txt) }
   }
   _vterm_key_hit(vt)
}

fn _vterm_handle_ctrl_insert(dict vt, any st, int co, list history, int hist_len, int k) list {
   if k != KEY_INSERT { return _vterm_key_miss(vt) }
   if load8(st, OFF_SEL_ACTIVE) != 0 {
      def full_txt = _vt_selection_text(vt, history, load32(st, OFF_SEL_SY), load32(st, OFF_SEL_SX), load32(st, OFF_SEL_EY), load32(st,
         OFF_SEL_EX),
         hist_len,
      co)
      if full_txt.len > 0 {
         vt = _vt_clip_set(vt, full_txt)
      }
   }
   _vterm_key_hit(vt)
}

fn _vterm_handle_modified_printable(dict vt, int k, int md, int flags, int action) list {
   if (md & (MOD_ALT | MOD_CONTROL | MOD_SUPER | MOD_META)) == 0 { return _vterm_key_miss(vt) }
   if k < 32 || k > 126 { return _vterm_key_miss(vt) }
   def cp = (k >= 65 && k <= 90) ? (k + 32) : k
   def ev = (action == 2) ? 2 : 1
   if (flags & KBD_REPORT_EVENTS) != 0 { _kitty_kbd_send_event(vt, cp, md, ev) }
   else { _kitty_kbd_send(vt, cp, md) }
   _vterm_key_hit(vt)
}

fn _vterm_handle_control_key(dict vt, any st, int hist_len, int scroll_off, int k) list {
   mut seq = ""
   if k == 32 { seq = "\000" }
   elif k >= 65 && k <= 90 { seq = str.chr(k - 64) }
   elif k >= 97 && k <= 122 { seq = str.chr(k - 96) }
   elif k == 91 { seq = "\033" }
   elif k == 92 { seq = "\034" }
   elif k == 93 { seq = "\035" }
   elif k == 94 { seq = "\036" }
   elif k == 95 || k == 47 { seq = "\037" }
   elif k == 8 || k == 259 { seq = "\010" }
   if seq.len == 0 { return _vterm_key_miss(vt) }
   _vterm_follow_cursor(vt, st, scroll_off, hist_len)
   send_input(vt, seq)
   _vterm_key_hit(vt)
}

fn _vterm_handle_shift_scroll(dict vt, any st, int hist_len, int scroll_off, int k) list {
   if k == KEY_PAGE_UP {
      if _vterm_has_selection_state(st) {
         _vterm_clear_selection(st)
         _vterm_reset_click_sequence(st)
      }
      mut n_off = scroll_off + max(1, int(vt.get("rows")) - 2)
      if n_off > hist_len { n_off = hist_len }
      _scroll_set(st, n_off, hist_len)
      return _vterm_key_hit(vt)
   }
   if k == KEY_PAGE_DOWN {
      if _vterm_has_selection_state(st) {
         _vterm_clear_selection(st)
         _vterm_reset_click_sequence(st)
      }
      mut n_off = scroll_off - max(1, int(vt.get("rows")) - 2)
      if n_off < 0 { n_off = 0 }
      _scroll_set(st, n_off, hist_len)
      return _vterm_key_hit(vt)
   }
   _vterm_key_miss(vt)
}

fn _vterm_handle_kitty_special(dict vt, int k, int md, int flags, int action) list {
   if (flags & (KBD_DISAMBIGUATE | KBD_REPORT_ALL)) == 0 { return _vterm_key_miss(vt) }
   def ev = (action == 2) ? 2 : 1
   if k == 13 || k == KEY_ENTER || k == KEY_KP_ENTER { _kitty_kbd_send_event(vt, 13, md, ev) return _vterm_key_hit(vt) }
   elif k == 8 || k == KEY_BACKSPACE { _kitty_kbd_send_event(vt, 127, md, ev) return _vterm_key_hit(vt) }
   elif k == 9 || k == KEY_TAB { _kitty_kbd_send_event(vt, 9, md, ev) return _vterm_key_hit(vt) }
   elif k == 27 || k == KEY_ESCAPE { _kitty_kbd_send_event(vt, 27, md, ev) return _vterm_key_hit(vt) }
   _vterm_key_miss(vt)
}

fn _vterm_arrow_suffix(int k) str {
   if k == KEY_UP { return "A" }
   if k == KEY_DOWN { return "B" }
   if k == KEY_RIGHT { return "C" }
   "D"
}

fn _vterm_handle_special_key(dict vt, any st, int hist_len, int scroll_off, int k, int md, int action, bool appk) list {
   if k == 13 || k == KEY_ENTER || k == KEY_KP_ENTER {
      ;; Enter is a command submit, not text. Native backends deliver held Enter
      ;; as KEY_PRESSED action=2 repeat events; forwarding those floods the PTY
      ;; with blank commands and lets frames expose half-drained prompt/newline
      ;; output. Keep the first press, drop autorepeat.
      if action == 2 { return _vterm_key_hit(vt) }
      _vterm_follow_cursor(vt, st, scroll_off, hist_len)
      send_input(vt, "\r")
      return _vterm_key_hit(vt)
   } elif k == 8 || k == KEY_BACKSPACE {
      _vterm_follow_cursor(vt, st, scroll_off, hist_len)
      send_input(vt, str.chr(127))
      return _vterm_key_hit(vt)
   } elif k == 9 || k == KEY_TAB {
      if (md & MOD_SHIFT) != 0 { send_input(vt, "\033[Z") }
      else { send_input(vt, "\t") }
      return _vterm_key_hit(vt)
   } elif k == 27 || k == KEY_ESCAPE {
      send_input(vt, "\033")
      return _vterm_key_hit(vt)
   } elif k == 127 || k == KEY_DELETE {
      send_input(vt, "\033[3~")
      return _vterm_key_hit(vt)
   } elif k == KEY_UP || k == KEY_DOWN || k == KEY_RIGHT || k == KEY_LEFT {
      def p = _xterm_mod_param(md)
      if p != 1 { send_input(vt, "\033[1;" + to_str(p) + _vterm_arrow_suffix(k)) }
      elif k == KEY_UP { send_input(vt, appk ? "\033OA" : "\033[A") }
      elif k == KEY_DOWN { send_input(vt, appk ? "\033OB" : "\033[B") }
      elif k == KEY_RIGHT { send_input(vt, appk ? "\033OC" : "\033[C") }
      else { send_input(vt, appk ? "\033OD" : "\033[D") }
      return _vterm_key_hit(vt)
   } elif k == KEY_PAGE_UP || k == KEY_PAGE_DOWN {
      def p = _xterm_mod_param(md)
      def base = ((k == KEY_PAGE_UP) ? "5" : "6")
      if p != 1 { send_input(vt, "\033[" + base + ";" + to_str(p) + "~") }
      else { send_input(vt, "\033[" + base + "~") }
      return _vterm_key_hit(vt)
   } elif k == KEY_HOME || k == KEY_END {
      def p = _xterm_mod_param(md)
      if p != 1 { send_input(vt, "\033[1;" + to_str(p) + ((k == KEY_HOME) ? "H" : "F")) }
      elif k == KEY_HOME { send_input(vt, "\033[1~") }
      else { send_input(vt, "\033[4~") }
      return _vterm_key_hit(vt)
   } elif k >= KEY_F1 && k <= KEY_F12 {
      def p = _xterm_mod_param(md)
      def codes = ["\033OP","\033OQ","\033OR","\033OS","\033[15~","\033[17~","\033[18~","\033[19~","\033[20~","\033[21~","\033[23~","\033[24~"]
      def f_idx = k - KEY_F1
      mut seq = codes.get(f_idx)
      if p != 1 {
         if f_idx >= 0 && f_idx <= 3 {
            def suf = (f_idx == 0) ? "P" : ((f_idx == 1) ? "Q" : ((f_idx == 2) ? "R" : "S"))
            seq = "\033[1;" + to_str(p) + suf
         }
         else {
            def til = str.find(seq, "~")
            if til != -1 { seq = str.str_slice(seq, 0, til) + ";" + to_str(p) + "~" }
         }
      }
      send_input(vt, seq)
      return _vterm_key_hit(vt)
   }
   _vterm_key_miss(vt)
}

fn _vterm_handle_printable_key(dict vt, any st, int hist_len, int scroll_off, int k, int md, int action) list {
   if (md & (MOD_CONTROL | MOD_ALT | MOD_SUPER | MOD_META)) != 0 { return _vterm_key_miss(vt) }
   if !vt.get("synthesize_printable_keys", false) { return _vterm_queue_printable(vt, st, k, md, action) }
   if load8(st, OFF_CHAR_CB_ACTIVE) != 0 { return _vterm_key_hit(vt) }
   def rep_ms = _term_repeat_ms(vt)
   def now_t = ticks()
   if k >= 32 && k <= 126 {
      mut kout = k
      if (md & MOD_SHIFT) != 0 { if k >= 65 && k <= 90 { kout = k } else { kout = _shift_char(k) } } else { if k >= 65 && k <= 90 { kout = k + 32 } }
      def repeat_ok = rep_ms <= 0.0 || (now_t - load64(st, OFF_LAST_CHAR_T)) > int(rep_ms * 1000000.0)
      if rep_ms > 0.0 && action == 2 && !repeat_ok && load32(st, OFF_LAST_CHAR_C) == kout { return _vterm_key_hit(vt) }
      if action != 2 && !repeat_ok && load32(st, OFF_LAST_CHAR_C) == kout { return _vterm_key_miss(vt) }
      _vterm_follow_cursor(vt, st, scroll_off, hist_len)
      store64(st, now_t, OFF_LAST_CHAR_T)
      store32(st, int(kout), OFF_LAST_CHAR_C)
      send_input(vt, str.chr(kout))
      return _vterm_key_hit(vt)
   }
   if k > 126 && k < 256 {
      if rep_ms > 0.0
      && (now_t - load64(st, OFF_LAST_CHAR_T)) <= int(rep_ms * 1000000.0)
      && load32(st, OFF_LAST_CHAR_C) == k{
         return _vterm_key_miss(vt)
      }
      _vterm_follow_cursor(vt, st, scroll_off, hist_len)
      store64(st, now_t, OFF_LAST_CHAR_T)
      store32(st, int(k), OFF_LAST_CHAR_C)
      send_input(vt, str.chr(k))
      return _vterm_key_hit(vt)
   }
   _vterm_key_miss(vt)
}

fn _vterm_handle_key_press(dict vt, any st, int co, list history, int hist_len, int scroll_off, any da) dict {
   def k = da.get("key") def raw_md = da.get("mod", 0) def appk = load8(st, OFF_APPKEYS) != 0
   def md = raw_md & 0xFF
   def flags = load32(st, OFF_KBD_FLAGS)
   def kbd = flags != 0
   def action = da.get("action", 1)
   if action != 2 {
      def now = ticks()
      def last_t = load64(st, OFF_LAST_KEY_T)
      def last_k = load32(st, OFF_LAST_KEY_CODE)
      def last_m = load32(st, OFF_LAST_KEY_MOD)
      if last_k == k && last_m == md && (now - last_t) < 5000000 { return vt }
      store64(st, now, OFF_LAST_KEY_T)
      store32(st, k, OFF_LAST_KEY_CODE)
      store32(st, md, OFF_LAST_KEY_MOD)
   }
   if (md & MOD_CONTROL) != 0 && (md & MOD_SHIFT) != 0 {
      def cs = _vterm_handle_ctrl_shift_key(vt, st, co, history, hist_len, scroll_off, k)
      if cs.get(0) { return cs.get(1) }
   }
   if (md & MOD_CONTROL) != 0 {
      def ci = _vterm_handle_ctrl_insert(vt, st, co, history, hist_len, k)
      if ci.get(0) { return ci.get(1) }
   }
   if (md & MOD_SHIFT) != 0 {
      def si = _vterm_handle_shift_insert(vt, st, hist_len, scroll_off, k)
      if si.get(0) { return si.get(1) }
   }
   if kbd {
      def printable = _vterm_handle_modified_printable(vt, k, md, flags, action)
      if printable.get(0) { return printable.get(1) }
   }
   if (md & MOD_CONTROL) != 0 {
      def ctrl = _vterm_handle_control_key(vt, st, hist_len, scroll_off, k)
      if ctrl.get(0) { return ctrl.get(1) }
   }
   if (md & MOD_SHIFT) != 0 {
      def shifted = _vterm_handle_shift_scroll(vt, st, hist_len, scroll_off, k)
      if shifted.get(0) { return shifted.get(1) }
   }
   if kbd {
      def kitty = _vterm_handle_kitty_special(vt, k, md, flags, action)
      if kitty.get(0) { return kitty.get(1) }
   }
   def special = _vterm_handle_special_key(vt, st, hist_len, scroll_off, k, md, action, appk)
   if special.get(0) { return special.get(1) }
   def typed = _vterm_handle_printable_key(vt, st, hist_len, scroll_off, k, md, action)
   if typed.get(0) { return typed.get(1) }
   vt
}

fn handle_event(dict vt, int ty, any da) dict {
   "Runs the handle event operation."
   def st = vt.get("state")
   def co = vt.get("cols") def ro = vt.get("rows")
   def history = vt.get("history")
   def hist_len = history.len
   def scroll_off = load32(st, OFF_SCROLL)
   def cw = vt.get("char_w", 9.0)
   def ch = vt.get("char_h", 18.0)
   if ty == EVENT_MOUSE_LEAVE || ty == EVENT_FOCUS_OUT {
      if ty == EVENT_MOUSE_LEAVE {
         _vterm_clear_selection(st)
         _vterm_reset_click_sequence(st)
         store8(st, 0, OFF_SB_DRAGGING)
         return vt
      }
      _vterm_clear_selection(st)
      _vterm_reset_click_sequence(st)
      store8(st, 0, OFF_SB_DRAGGING)
      if load8(st, OFF_FOCUS_REPORT) != 0 { send_input(vt, "\033[O") }
      return vt
   } elif ty == EVENT_FOCUS_IN {
      if load8(st, OFF_FOCUS_REPORT) != 0 { send_input(vt, "\033[I") }
      return vt
   } elif ty == EVENT_MOUSE_BUTTON_PRESSED {
      return _vterm_handle_mouse_press(vt, st, co, ro, history, hist_len, cw, ch, da)
   } elif ty == EVENT_MOUSE_POS_CHANGED {
      return _vterm_handle_mouse_motion(vt, st, co, ro, hist_len, cw, ch, da)
   } elif ty == EVENT_MOUSE_SCROLL {
      return _vterm_handle_mouse_scroll(vt, st, ro, hist_len, cw, ch, da)
   } elif ty == EVENT_MOUSE_BUTTON_RELEASED {
      return _vterm_handle_mouse_release(vt, st, co, history, hist_len, cw, ch, da)
   } elif ty == EVENT_KEY_RELEASED {
      return _vterm_handle_key_release(vt, st, da)
   } elif ty == EVENT_KEY_CHAR {
      return _vterm_handle_key_char(vt, st, hist_len, scroll_off, da)
   } elif ty == EVENT_KEY_PRESSED {
      return _vterm_handle_key_press(vt, st, co, history, hist_len, scroll_off, da)
   }
   vt
}

fn _shift_char(int k) int {
   if k == 49 { return 33 }
   if k == 50 { return 64 }
   if k == 51 { return 35 }
   if k == 52 { return 36 }
   if k == 53 { return 37 }
   if k == 54 { return 94 }
   if k == 55 { return 38 }
   if k == 56 { return 42 }
   if k == 57 { return 40 }
   if k == 48 { return 41 }
   if k == 45 { return 95 }
   if k == 61 { return 43 }
   if k == 91 { return 123 }
   if k == 93 { return 125 }
   if k == 92 { return 124 }
   if k == 59 { return 58 }
   if k == 39 { return 34 }
   if k == 44 { return 60 }
   if k == 46 { return 62 }
   if k == 47 { return 63 }
   k
}

fn _utf8_cont_byte(int b) bool {
   def c = b & 255
   c >= 128 && c <= 191
}

fn write(dict vt, str s) dict {
   "Writes write."
   def st = vt.get("state") def co = vt.get("cols") def ro = vt.get("rows") def g = vt.get("grid")
   def pal = vt.get("palette") def dfg = vt.get("def_fg") def dbg = vt.get("def_bg")
   mut i = 0
   def n = s.len
   mut nvt = vt
   if n > 0 { _vterm_clear_selection_for_output(st) }
   while i < n {
      def b0 = load8(s, i) & 255
      if load32(st, OFF_ESC_STATE) == 0 && load8(st, OFF_INSERT) == 0 && b0 >= 32 && b0 < 127 {
         mut q = i + 1
         while q < n {
            def nb = load8(s, q) & 255
            if nb < 32 || nb >= 127 { break }
            q += 1
         }
         nvt = _tput_ascii_run_fast(nvt, st, co, ro, g, dfg, dbg, s, i, q - i)
         i = q
         continue
      }
      mut cp = b0
      mut w = 1
      if (b0 & 0x80) != 0 {
         if b0 >= 194 && b0 <= 223 && i + 1 < n {
            def b1 = load8(s, i + 1) & 255
            if _utf8_cont_byte(b1) {
               cp = ((b0 & 31) << 6) | (b1 & 63)
               w = 2
            } else {
               cp = 63
            }
         } elif b0 >= 224 && b0 <= 239 && i + 2 < n {
            def b1 = load8(s, i + 1) & 255
            def b2 = load8(s, i + 2) & 255
            if _utf8_cont_byte(b1) && _utf8_cont_byte(b2) && !(b0 == 224 && b1 < 160) && !(b0 == 237 && b1 >= 160) {
               cp = ((b0 & 15) << 12) | ((b1 & 63) << 6) | (b2 & 63)
               w = 3
            } else {
               cp = 63
            }
         } elif b0 >= 240 && b0 <= 244 && i + 3 < n {
            def b1 = load8(s, i + 1) & 255
            def b2 = load8(s, i + 2) & 255
            def b3 = load8(s, i + 3) & 255
            if _utf8_cont_byte(b1) && _utf8_cont_byte(b2) && _utf8_cont_byte(b3) && !(b0 == 240 && b1 < 144) && !(b0 == 244 && b1 > 143) {
               cp = ((b0 & 7) << 18) | ((b1 & 63) << 12) | ((b2 & 63) << 6) | (b3 & 63)
               w = 4
            } else {
               cp = 63
            }
         } else {
            cp = 63
         }
      }
      nvt = _tputc_fast(nvt, st, co, ro, g, pal, dfg, dbg, cp)
      i += w
   }
   nvt
}

fn resize(dict vt, int cols, int rows) dict {
   "Resizes resize."
   def oc = vt.get("cols") def or = vt.get("rows")
   if cols == oc && rows == or { return vt }
   def dfg = vt.get("def_fg") def dbg = vt.get("def_bg")
   def og, ng = vt.get("grid"), malloc(cols * rows * 16)
   if !ng { return vt }
   _clear_grid(ng, cols, rows, dfg, dbg)
   def copy_rows = min(rows, or)
   def copy_cols = min(cols, oc)
   mut r = 0 while r < copy_rows {
      memcpy(ptr_add(ng, r * cols * 16), ptr_add(og, r * oc * 16), copy_cols * 16)
      r += 1
   }
   mut nvt = vt.set("grid", ng)
   nvt = nvt.set("cols", cols)
   nvt = nvt.set("rows", rows)
   free(og)
   def al = nvt.get("alt_grid")
   if al { free(al) nvt = nvt.set("alt_grid", 0) }
   def cw, ch = nvt.get("char_w", 9.0), nvt.get("char_h", 18.0)
   def px_w, px_h = int(float(cols) * cw), int(float(rows) * ch)
   nvt = nvt.set("px_w", px_w)
   nvt = nvt.set("px_h", px_h)
   def m = nvt.get("master_fd") if m >= 0 { _resize_pty(m, cols, rows, px_w, px_h) }
   def st = nvt.get("state")
   store32(st, rows - 1, OFF_BOT)
   mut cx, cy = load32(st, OFF_CX), load32(st, OFF_CY)
   if cx >= cols { cx = cols - 1 } if cx < 0 { cx = 0 }
   if cy >= rows { cy = rows - 1 } if cy < 0 { cy = 0 }
   store32(st, cx, OFF_CX) store32(st, cy, OFF_CY)
   mut top = load32(st, OFF_TOP)
   if top >= rows { top = 0 } store32(st, top, OFF_TOP)
   def fr = nvt.get("fonts").get("regular")
   def cell = _vterm_cell_metrics(fr)
   if cell.get(0, 0.0) > 0.1 {
      nvt = nvt.set("char_w", float(cell.get(0)))
      nvt = nvt.set("char_h", float(cell.get(1)))
   }
   nvt
}

fn set_fonts(dict vt, dict fonts) dict {
   "Replaces terminal fonts and refreshes cell/PTY pixel metrics without changing grid contents."
   mut nvt = vt.set("fonts", fonts)
   def fr = fonts.get("regular")
   def cell = _vterm_cell_metrics(fr)
   mut cw = float(cell.get(0, nvt.get("char_w", 9.0)))
   mut ch = float(cell.get(1, nvt.get("char_h", 18.0)))
   if cw <= 0.1 { cw = 9.0 }
   if ch <= 0.1 { ch = 18.0 }
   def cols = max(1, int(nvt.get("cols", 1)))
   def rows = max(1, int(nvt.get("rows", 1)))
   def px_w, px_h = int(float(cols) * cw), int(float(rows) * ch)
   nvt = nvt.set("char_w", cw)
   nvt = nvt.set("char_h", ch)
   nvt = nvt.set("px_w", px_w)
   nvt = nvt.set("px_h", px_h)
   def m = nvt.get("master_fd", -1)
   if m >= 0 { _resize_pty(m, cols, rows, px_w, px_h) }
   nvt
}

fn _resize_pty(int m, int cols, int rows, int px_w=0, int px_h=0) any {
   mut w, h = px_w, px_h
   if w < 0 { w = 0 } if h < 0 { h = 0 }
   if w > 65535 { w = 65535 } if h > 65535 { h = 65535 }
   mut ws = malloc(8)
   if !ws { return 0 }
   store16(ws, rows, 0)
   store16(ws, cols, 2)
   store16(ws, w, 4)
   store16(ws, h, 6)
   __ioctl(m, 0x5414, ws) free(ws)
   0
}

#main {
   def fonts = {"regular": 0, "bold": 0, "italic": 0, "emoji": 0}
   mut vt = new(4, 3, fonts).set("master_fd", -1)
   assert(is_dict(vt) && vt.get("cols") == 4 && vt.get("rows") == 3, "vterm new size")
   assert(get_title(vt) == "Terminal" && !is_running(vt), "vterm initial state")
   assert(is_str(default_shell_path()) && default_shell_path().len > 0, "vterm default shell path")
   assert(is_list(default_shell_args(false)) && default_shell_args(false).len >= 1, "vterm default shell args")
   assert(is_list(abgr_to_color(0xff112233)) && abgr_to_color(0xff112233).len == 4, "vterm color conversion")
   assert(is_int(env_bg_color()), "vterm env bg color")
   vt = write(vt, "AB")
   def grid = vt.get("grid")
   assert(load32(grid, 0) == 65 && load32(grid, 16) == 66, "vterm ascii write")
   vt = write(vt, "\033]0;Probe\033\\")
   assert(get_title(vt) == "Probe", "vterm osc title")
   vt = handle_event(vt, EVENT_MOUSE_LEAVE, dict(8))
   vt = handle_event(vt, EVENT_MOUSE_SCROLL, {"dy": 1.0, "x": 12.0, "y": 9.0})
   def st = vt.get("state")
   assert(load32(st, 94) == 0 && load32_f32(st, 156) == 12.0 && load32_f32(st, 160) == 9.0, "vterm scroll state")
   assert(!selection_dragging(vt) && !selection_any(vt), "vterm selection helpers")
   assert(cursor_blink_phase(vt) >= -1 && is_bool(needs_visual_refresh(vt)) && idle_sleep_ms(vt) > 0, "vterm refresh state")
   vt = resize(vt, 5, 4)
   assert(vt.get("cols") == 5 && vt.get("rows") == 4, "vterm resize")
   def vt0 = new(80, 24, fonts, 0, 0).set("master_fd", -1)
   def st0 = vt0.get("state")
   assert(load32(st0, 0) == 0 && load32(st0, 4) == 0, "vterm cursor initial")
   def vt1 = handle_event(vt0, EVENT_KEY_PRESSED, {"key": 65, "action": 1, "mod": 0, "ww": 800.0, "wh": 480.0})
   def st1 = vt1.get("state")
   assert(load32(st1, 0) == 0 && load32(st1, 4) == 0, "vterm key press cursor")
   def vt2 = handle_event(vt1, EVENT_KEY_CHAR, {"char": 97, "mod": 0, "plain": true, "ww": 800.0, "wh": 480.0})
   def st2 = vt2.get("state")
   assert(load32(st2, 0) == 1 && load32(st2, 4) == 0, "vterm key char cursor")
   close(vt)
   close(vt2)
   print("✓ std.os.ui.render.viewer.vterm self-test passed")
}
