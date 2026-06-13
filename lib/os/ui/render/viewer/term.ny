;; Keywords: ui terminal ansi tty console text os render viewer
;; Terminal text parsing and ANSI rendering helpers for embedded consoles.
;; References:
;; - std.os.ui.render.viewer.gui
module std.os.ui.render.viewer.term(init, toggle, draw, handle_event, log, exec, clear, is_open, set_font, set_colors, get_history, get_input, font_cell_size, framebuffer_size, sync_framebuffer_size, resize_term)
use std.core
use std.os (ticks)
use std.math
use std.core.common as common
use std.core.term as cli_term
use std.core.str as str
use std.os.ui.window.consts
use std.os.ui.render
use std.os.ui.render.viewer.vterm as vterm
use std.os.ui.window as window
use std.os.ui.window.native as nativewin
use std.os.ui.window.input as uin

mut _is_open      = false
mut _input_render_list = [""]
def PADDING_X     = 14.0
def PADDING_Y     = 10.0
def _CURSOR_CAPTURED = 0x00034004
mut _scroll_off   = 0
mut _scroll_acc   = 0.0
mut _input        = ""
mut _history      = []
mut _history_wrapped = []
mut _exec_history = []
mut _hist_idx     = -1
mut _saved_input  = ""
mut _cursor       = 0
mut _font         = 0
mut _bg_color     = 0
mut _text_color   = 0xFFFFFFFF
mut _cyan_color   = 0
mut _white_pack   = 0xFFFFFFFF
mut _sep_color    = 0
mut _row_h   = 24.0
mut _glyph_h = 18.0
mut _glyph_y = 3.0
mut _font_ascent = 13.0
mut _font_descent = 3.0
mut _last_char_t = 0
mut _last_char_c = 0
mut _last_enter_submit_t = 0
mut _char_cb_active = false
mut _saved_cursor_mode = 0
mut _saved_raw_mouse_motion = 0
mut _suppress_toggle_key = 0
mut _suppress_toggle_char = false
mut _font_metrics_dirty = true
mut _input_line_dirty = true
mut _cursor_px_dirty = true
mut _prompt_px_dirty = true
mut _input_line_cache = "> "
mut _cursor_px_cache = 0.0
mut _prompt_px_cache = 0.0
mut _history_view_dirty = true
mut _history_view_lines = []
mut _history_view_rows = 0
mut _history_view_spacing = 0.0
mut _history_view_y = 0.0
mut _term_wrap_width = 0.0

fn init(any font, int bg_color=0, int text_color=0) any {
   "Initializes the terminal emulator with the specified font and colors."
   _font       = font
   _bg_color   = (bg_color != 0) ? bg_color : 0x00000000
   _white_pack = 0xffffffff
   _cyan_color = _white_pack
   _sep_color  = 0x22ffffff
   _text_color = (text_color != 0) ? text_color : _white_pack
   _font_metrics_dirty = true
   _input_line_dirty = true
   _cursor_px_dirty = true
   _prompt_px_dirty = true
   _ensure_font_metrics()
}

fn font_cell_size(any font, any font_size) list {
   "Returns terminal cell [width, height] derived from font metrics."
   def fs = float(font_size)
   if(!font){ return [fs * 0.6, max(fs, 20.0)] }
   def probe = measure_text_fast(font, "M")
   mut cw, ch = float(probe.get(0, 0.0)), float(probe.get(1, 0.0))
   if(cw <= 1.0){
      def aw = measure_text_fast(font, "A")
      cw = float(aw.get(0, 0.0))
   }
   if(cw <= 1.0){
      def iw = measure_text_fast(font, "i")
      cw = max(cw, float(iw.get(0, 0.0)))
   }
   if(cw <= 1.0){ cw = fs * 0.6 }
   if(ch <= 1.0){ ch = fs }
   cw, ch = max(1.0, float(int(cw + 0.5))), max(1.0, float(int(ch + 0.5)))
   [cw, ch]
}

fn framebuffer_size(any win) list {
   "Returns [width, height] of the window framebuffer, falling back to logical size."
   mut fw, fh = 0.0, 0.0
   #windows {
      def active_fb = get_framebuffer_size()
      fw, fh = float(active_fb.get(0, 0)), float(active_fb.get(1, 0))
   }
   if(win){
      if(fw <= 0.0 || fh <= 0.0){
         def fb = nativewin.get_framebuffer_size(window.id(win))
         fw, fh = float(fb.get(0, 0)), float(fb.get(1, 0))
      }
   }
   if(fw <= 0.0 || fh <= 0.0){
      def sz = window.size(win)
      fw, fh = float(sz.get(0, 1280)), float(sz.get(1, 720))
   }
   [fw, fh]
}

fn sync_framebuffer_size(any win, any win_w, any win_h) list {
   "Returns [changed, new_w, new_h] after comparing the actual framebuffer to the cached size."
   def fb = framebuffer_size(win)
   def nw = float(fb.get(0, win_w))
   def nh = float(fb.get(1, win_h))
   if(nw <= 0.0 || nh <= 0.0){ return [false, win_w, win_h] }
   if(int(nw) == int(win_w) && int(nh) == int(win_h)){ return [false, win_w, win_h] }
   [true, nw, nh]
}

fn resize_term(any vt, any win_w, any win_h, any font, any font_size) any {
   "Resizes vterm using stable monospace cell metrics(no per-cell stretching)."
   def fw, fh = float(win_w), float(win_h)
   def cell = font_cell_size(font, font_size)
   def cw = float(cell.get(0, float(font_size) * 0.6))
   def ch = float(cell.get(1, max(float(font_size), 20.0)))
   def cols = int(fw / cw)
   def rows = int(fh / ch)
   set_win_size(int(fw), int(fh))
   if(cols <= 0 || rows <= 0){ return vt }
   mut nvt = vterm.resize(vt, cols, rows)
   nvt = nvt.set("char_w", cw)
   nvt = nvt.set("char_h", ch)
   nvt = nvt.set("px_w", int(float(cols) * cw))
   nvt = nvt.set("px_h", int(float(rows) * ch))
   nvt
}

fn _ensure_font_metrics() any {
   if(!_font_metrics_dirty){ return nil }
   _font_metrics_dirty = false
   mut f_sz = 16.0
   mut ascent = 13.0
   mut descent = -3.0
   if(_font){
      def f = _font_get(_font)
      if(f){
         f_sz    = float(f.get("size", 16.0))
         ascent  = float(f.get("ascent", f_sz * 0.8))
         descent = float(f.get("descent", -f_sz * 0.2))
      }
   }
   _font_ascent  = max(1.0, ascent)
   _font_descent = max(0.0, abs(descent))
   def span = min(48.0, _font_ascent + _font_descent)
   _glyph_h = max(8.0, ceil(span))
   _row_h   = max(12.0, _glyph_h + 3.0)
   _glyph_y = max(1.0, floor((_row_h - _glyph_h) * 0.5 + 0.5))
   _input_line_dirty = true
   _cursor_px_dirty = true
   _prompt_px_dirty = true
   _history_view_dirty = true
}

fn _mark_history_dirty() any { _history_view_dirty = true }

fn _ensure_history_buffers() any {
   if(!is_list(_history)){ _history = borrow([]) }
   if(!is_list(_history_wrapped)){ _history_wrapped = borrow([]) }
   if(!is_list(_history_view_lines)){ _history_view_lines = borrow([]) }
   if(!is_list(_exec_history)){ _exec_history = borrow([]) }
}

fn _history_text(any line) str {
   if(is_str(line)){ return line }
   to_str(line)
}

fn _append_history_line(any line) any {
   _ensure_history_buffers()
   _history = borrow(_history.append(_history_text(line)))
   if(_history.len > 200){
      _history = borrow(slice(_history, _history.len - 200, _history.len, 1))
      if(_history_wrapped.len >= 200){ _history_wrapped = borrow(slice(_history_wrapped, 1, 200, 1)) }
   }
   _mark_history_dirty()
}

fn _sync_print_history() any {
   def pending = print_history_drain()
   if(!is_list(pending) || pending.len <= 0){ return nil }
   def n = pending.len
   def start = n > 48 ? (n - 48) : 0
   mut i = start
   while(i < n){
      _append_history_line(pending.get(i, ""))
      i += 1
   }
}

fn _mark_cursor_dirty() any { _cursor_px_dirty = true }

fn _mark_prompt_dirty() any { _prompt_px_dirty = true }

fn _cursor_px() f64 {
   if(_cursor_px_dirty){
      _cursor_px_cache = measure_text_fast(_font, str.str_slice(_input, 0, _cursor)).get(0)
      _cursor_px_dirty = false
   }
   _cursor_px_cache
}

fn _prompt_px() f64 {
   if(_prompt_px_dirty){
      _prompt_px_cache = measure_text_fast(_font, "> ").get(0)
      _prompt_px_dirty = false
   }
   _prompt_px_cache
}

fn toggle(any win, int src_key=0) any {
   "Toggles the terminal visibility and updates cursor lock state for the given window."
   def real_win = window.get_win(win)
   def handle = real_win.get("handle", 0)
   def prev_mode = real_win.get("cursor_mode", window.CURSOR_NORMAL)
   def prev_raw = real_win.get("raw_mouse_motion", false) ? 1 : 0
   _is_open = !_is_open
   window.set_input_exclusive(win, !_is_open)
   _suppress_toggle_key  = (src_key != 0) ? src_key : 0
   _suppress_toggle_char = (_is_open && src_key == uin.KEY_GRAVE)
   _char_cb_active = false
   if(_is_open){
      _saved_cursor_mode = prev_mode
      _saved_raw_mouse_motion = prev_raw
      if(handle){ nativewin.set_input_mode(handle, nativewin.RAW_MOUSE_MOTION, 0) }
      window.set_input_exclusive(win, false)
      window.set_cursor_mode(win, window.CURSOR_NORMAL)
   } else {
      def restore_mode = (_saved_cursor_mode != 0) ? _saved_cursor_mode : window.CURSOR_NORMAL
      window.set_input_exclusive(win, true)
      if(handle){ nativewin.set_input_mode(handle, nativewin.RAW_MOUSE_MOTION, _saved_raw_mouse_motion) }
      window.set_cursor_mode(win, restore_mode)
      if(restore_mode == window.CURSOR_DISABLED || restore_mode == _CURSOR_CAPTURED){
         def sz = window.size(win)
         window.set_cursor_pos(win, float(sz.get(0)) * 0.5, float(sz.get(1)) * 0.5)
      }
      _saved_cursor_mode = 0
      _saved_raw_mouse_motion = 0
   }
}

fn is_open() bool {
   "Returns true if the terminal is currently open."
   _is_open
}

fn log(any msg) any {
   "Logs a message to stdout and explicitly adds it to the HUD's history buffer."
   def line = to_str(msg)
   cli_term.write_str(cli_term.log_text(line) + "\n")
   _append_history_line(line)
}

fn clear() any {
   "Clears the terminal history buffer."
   _ensure_history_buffers()
   _history = borrow([])
   _history_wrapped = borrow([])
   _history_view_lines = borrow([])
   _history_view_rows = 0
   print_history_clear()
   _mark_history_dirty()
}

fn _wrap_history_line(any line, f64 max_w) list {
   def text = _history_text(line)
   if(max_w <= 0.0 || !_font){ return [text] }
   measure_text(_font, text)
   def f = _font_get(_font)
   if(!f){ return [text] }
   def glyphs_ptr = f.get("fast_glyphs", 0)
   if(!glyphs_ptr){ return [text] }
   mut out = []
   mut rest = text
   while(rest.len > 0){
      def total = rest.len
      mut last_fit = 0
      mut last_space = -1
      mut pen_x = 0.0
      mut i = 0
      while(i < total){
         def char_len = str._utf8_seq_len(rest, i, total)
         if(char_len <= 0){
            i += 1
            continue
         }
         def cp = str._utf8_decode_at(rest, i, char_len)
         mut adv = 8.0
         def page = (cp < 256) ? load64(glyphs_ptr, 0) : load64(glyphs_ptr, ((cp >> 8) & 65535) * 8)
         if(page){
            def off = page + (cp & 255) * 48
            if(load32(off, 40) != 0){ adv = load32_f32(off, 0) }
         }
         if(pen_x + adv > max_w){ break }
         if(cp == 32){ last_space = i + char_len }
         last_fit = i + char_len
         pen_x += adv
         i = i + char_len
      }
      mut final_cut = last_fit
      if(last_space > 0 && last_space <= last_fit && i < total){ final_cut = last_space }
      if(final_cut == 0 && total > 0){
         def char_len = str._utf8_seq_len(rest, 0, total)
         final_cut = (char_len > 0) ? char_len : 1
      }
      out = out.append(str.str_slice(rest, 0, final_cut))
      rest = str.str_slice(rest, final_cut, total)
      while(rest.len > 0 && load8(rest, 0) == 32){ rest = str.str_slice(rest, 1, rest.len) }
   }
   out
}

mut _last_wrap_w = 0.0

fn _ensure_history_view(int h_len, int max_rows, f64 sep_y, f64 gy, f64 rh, f64 wrap_w) any {
   _ensure_history_buffers()
   if(_scroll_off < 0){ _scroll_off = 0 }
   if(max_rows <= 0 || h_len <= 0){
      _history_view_dirty = false
      _history_view_rows = 0
      _history_view_lines = borrow([])
      _history_view_y = PADDING_Y + _glyph_y
      _history_view_spacing = rh
      _term_wrap_width = wrap_w
      return nil
   }
   if(!_history_view_dirty && _history_view_spacing == rh && _term_wrap_width == wrap_w){ return nil }
   if(abs(_last_wrap_w - wrap_w) > 1.0){ _last_wrap_w = wrap_w }
   mut rev_rows = []
   mut skip = _scroll_off
   mut i = h_len - 1
   while(i >= 0 && rev_rows.len < max_rows){
      def wrapped = _wrap_history_line(_history_text(_history.get(i, "")), wrap_w)
      mut wi = wrapped.len - 1
      while(wi >= 0 && rev_rows.len < max_rows){
         if(skip > 0){ skip -= 1 }
         else { rev_rows = rev_rows.append(wrapped.get(wi, "")) }
         wi -= 1
      }
      i -= 1
   }
   mut visible_rows = rev_rows.len
   mut lines = []
   mut ri = visible_rows - 1
   while(ri >= 0){
      lines = lines.append(rev_rows.get(ri, ""))
      ri -= 1
   }
   _history_view_dirty = false
   _term_wrap_width = wrap_w
   _history_view_rows = visible_rows
   _history_view_spacing = rh
   mut hist_y = sep_y - float(visible_rows) * rh + _glyph_y
   if(hist_y < PADDING_Y + _glyph_y){ hist_y = PADDING_Y + _glyph_y }
   _history_view_y = hist_y
   _history_view_lines = borrow(lines)
}

mut _term_max_rows_cache = -1

fn _term_max_rows(int max_rows) int {
   if(_term_max_rows_cache < 0){ _term_max_rows_cache = common.env_int_clamped("NY_TERM_MAX_ROWS", 8, 1, 100000) }
   min(max_rows, _term_max_rows_cache)
}

fn exec(fnptr callback) any {
   "Submits the current input buffer to the provided callback and resets input state."
   _ensure_history_buffers()
   def line = str.strip(_input)
   _input  = ""
   _cursor = 0
   _mark_cursor_dirty()
   if(line.len == 0){ return nil }
   log("> " + line)
   if(_exec_history.len == 0 || _exec_history.get(_exec_history.len-1) != line){
      _exec_history = borrow(_exec_history.append(line))
      if(_exec_history.len > 50){ _exec_history = borrow(slice(_exec_history, 1, 51, 1)) }
   }
   _hist_idx = -1
   callback(line)
}

fn _shift_char(int k) int {
   if(k == 49){ return 33  } if(k == 50){ return 64  } if(k == 51){ return 35  }
   if(k == 52){ return 36  } if(k == 53){ return 37  } if(k == 54){ return 94  }
   if(k == 55){ return 38  } if(k == 56){ return 42  } if(k == 57){ return 40  }
   if(k == 48){ return 41  } if(k == 45){ return 95  } if(k == 61){ return 43  }
   if(k == 91){ return 123 } if(k == 93){ return 125 } if(k == 92){ return 124 }
   if(k == 59){ return 58  } if(k == 39){ return 34  } if(k == 44){ return 60  }
   if(k == 46){ return 62  } if(k == 47){ return 63  }
   k
}

mut _char_repeat_ms_cache = -1.0

fn _char_repeat_ms() f64 {
   if(_char_repeat_ms_cache < 0.0){
      _char_repeat_ms_cache = 5.0
      def env_rep = common.env_trim("NY_TERM_REPEAT_MS")
      if(env_rep.len > 0){
         def sv = str.atof(env_rep)
         if(sv >= 1.0 && sv <= 500.0){ _char_repeat_ms_cache = sv }
      }
   }
   _char_repeat_ms_cache
}

fn _inject(int c) bool {
   if(c < 32 || c == 127){ return false }
   def char_str = str.chr(c)
   def left  = str.str_slice(_input, 0, _cursor)
   def right = str.str_slice(_input, _cursor, _input.len)
   _input    = left + char_str + right
   _cursor  += char_str.len
   _mark_cursor_dirty()
   _last_char_t, _last_char_c = ticks(), c
   true
}

@jit
fn draw(f64 ww, f64 wh, f64 phase=0.0) any {
   "Draws the terminal overlay including background, history, and input line."
   _sync_print_history()
   if(!_is_open){ return nil }
   _ensure_font_metrics()
   def rh  = _row_h
   def gy  = _glyph_y
   def pad_x   = PADDING_X
   def pad_top = PADDING_Y
   def th      = wh * 0.38
   def bg_col  = (_bg_color != 0) ? _bg_color : 0xE6080A12
   def sep_col = (_sep_color != 0) ? _sep_color : 0xCC55FFCC
   def prompt_col = (_cyan_color != 0) ? _cyan_color : 0xFF99EEFF
   def text_col = (_text_color != 0) ? _text_color : 0xFFF0F0F0
   draw_rect_fast(0.0, 0.0, ww, th, bg_col)
   def bar_h, bar_y = rh + 6.0, th - bar_h
   def sep_y  = bar_y - 2.0
   draw_rect_fast(0.0, sep_y, ww, 2.0, sep_col)
   def top_y = floor(bar_y + (bar_h - _glyph_h) * 0.5 + 0.5)
   def input_x = pad_x + _prompt_px()
   draw_text(_font, "> ", pad_x, top_y, prompt_col)
   draw_text(_font, _input, input_x, top_y, text_col)
   if(fmod(float(phase) / 0.6, 2.0) < 1.0){
      def cx = input_x + _cursor_px()
      draw_rect_fast(cx, top_y, 2.0, _glyph_h, 0xFFFFFFFF)
   }
   def h_len = _history.len
   def avail_h  = sep_y - pad_top
   def max_rows = _term_max_rows(max(1, int(avail_h / rh)))
   def wrap_w = ww - pad_x * 2.0 - 4.0
   _ensure_history_view(h_len, max_rows, sep_y, gy, rh, wrap_w)
   if(_history_view_rows > 0){ draw_text_batch(_font, _history_view_lines, pad_x, _history_view_y, _history_view_spacing, text_col) }
}

fn _enter_submit_once() any {
   def now_t = ticks()
   if(now_t - _last_enter_submit_t < 5000000){ return true }
   _last_enter_submit_t = now_t
   2
}

@jit
fn handle_event(int typ, any data) any {
   "Processes input events for the terminal(keyboard and mouse scroll)."
   if(!_is_open){ return false }
   if(typ == EVENT_KEY_CHAR){
      _ensure_history_buffers()
      if(_suppress_toggle_char){
         _suppress_toggle_char = false
         return true
      }
      _char_cb_active = true
      def c = data.get("char", 0)
      if(c <= 0){ return false }
      if(c == 10 || c == 13){ return _enter_submit_once() }
      _scroll_off = 0
      return _inject(c)
   } elif(typ == EVENT_KEY_PRESSED){
      _ensure_history_buffers()
      def k    = data.get("key")
      def mods = data.get("mod", 0)
      def action = data.get("action", 1)
      def scancode = data.get("scancode", data.get("raw_key", 0))
      if(_suppress_toggle_key != 0 && k == _suppress_toggle_key){
         _suppress_toggle_key = 0
         return true
      }
      def is_printable_ascii = (k >= 32 && k <= 126)
      if(is_printable_ascii){ return true }
      if((mods & MOD_CONTROL) != 0){
         if(k == uin.KEY_A){ _cursor = 0 _mark_cursor_dirty() return true }
         elif(k == uin.KEY_E){ _cursor = _input.len _mark_cursor_dirty() return true }
         elif(k == uin.KEY_U){
            _input = str.str_slice(_input, _cursor, _input.len)
            _cursor = 0 _mark_cursor_dirty() return true
         }
         elif(k == uin.KEY_K){ _input = str.str_slice(_input, 0, _cursor) _mark_cursor_dirty() return true }
         elif(k == uin.KEY_W){
            if(_cursor > 0){
               mut i = _cursor - 1
               while(i > 0 && load8(_input, i) == 32){ i -= 1 }
               while(i > 0 && load8(_input, i) != 32){ i -= 1 }
               if(i > 0){ i += 1 }
               _input  = str.str_slice(_input, 0, i) + str.str_slice(_input, _cursor, _input.len)
               _cursor = i
               _mark_cursor_dirty()
            }
            return true
         }
         elif(k == uin.KEY_L){ clear() return true }
         elif(k == uin.KEY_C){ _input = "" _cursor = 0 _mark_cursor_dirty() return true }
         elif(k == uin.KEY_R){
            if(_exec_history.len > 0){
               _hist_idx = (_hist_idx + 1) % _exec_history.len
               _input    = _exec_history.get(_exec_history.len - 1 - _hist_idx)
               _cursor   = _input.len
               _mark_cursor_dirty()
            }
            return true
         }
      }
      if(k == uin.KEY_ENTER || k == 257 || k == 335 || scancode == 36 || scancode == 104){ return _enter_submit_once() }
      elif(k == uin.KEY_BACKSPACE || k == 259){
         if(_cursor > 0){
            mut pl = 1
            if(_cursor > 1){
               mut b = load8(_input, _cursor - 1) & 255
               if((b & 0xC0) == 0x80){ while(_cursor - pl > 0 && (load8(_input, _cursor - pl) & 255 & 0xC0) == 0x80){ pl += 1 } }
            }
            _input  = str.str_slice(_input, 0, _cursor - pl) + str.str_slice(_input, _cursor, _input.len)
            _cursor -= pl
            _mark_cursor_dirty()
         }
         return true
      }
      elif(k == uin.KEY_DELETE || k == 261){
         if(_cursor < _input.len){
            mut nl, b0 = 1, load8(_input, _cursor) & 255
            if((b0 & 0x80) != 0){
               if((b0 & 0xE0) == 0xC0){ nl = 2 }
               elif((b0 & 0xF0) == 0xE0){ nl = 3 }
               elif((b0 & 0xF8) == 0xF0){ nl = 4 }
            }
            _input = str.str_slice(_input, 0, _cursor) + str.str_slice(_input, _cursor + nl, _input.len)
            _mark_cursor_dirty()
         }
         return true
      }
      elif(k == uin.KEY_LEFT || k == 263){
         if(_cursor > 0){
            mut s = 1
            while(_cursor - s > 0 && (load8(_input, _cursor - s) & 0xC0) == 0x80){ s += 1 }
            _cursor -= s
            _mark_cursor_dirty()
         }
         return true
      }
      elif(k == uin.KEY_RIGHT || k == 262){
         if(_cursor < _input.len){
            mut s, b0 = 1, load8(_input, _cursor) & 255
            if((b0 & 0x80) != 0){
               if((b0 & 0xE0) == 0xC0){ s = 2 }
               elif((b0 & 0xF0) == 0xE0){ s = 3 }
               elif((b0 & 0xF8) == 0xF0){ s = 4 }
            }
            _cursor += s
            _mark_cursor_dirty()
         }
         return true
      }
      elif(k == uin.KEY_UP || k == 265){
         if(_exec_history.len > 0){
            if(_hist_idx == -1){ _saved_input = _input }
            if(_hist_idx < _exec_history.len - 1){ _hist_idx += 1 }
            _input  = _exec_history.get(_exec_history.len - 1 - _hist_idx)
            _cursor = _input.len
            _mark_cursor_dirty()
         }
         return true
      }
      elif(k == uin.KEY_DOWN || k == 264){
         if(_hist_idx > 0){
            _hist_idx -= 1
            _input  = _exec_history.get(_exec_history.len - 1 - _hist_idx)
            _cursor = _input.len
            _mark_cursor_dirty()
         } elif(_hist_idx == 0){
            _hist_idx = -1
            _input    = _saved_input
            _cursor   = _input.len
            _mark_cursor_dirty()
         }
         return true
      }
      if((mods & MOD_CONTROL) == 0 && (mods & MOD_ALT) == 0 &&
         (mods & MOD_SUPER) == 0 && (mods & MOD_META) == 0 &&
         k >= 32 && k <= 255){
         if(_char_cb_active){ return true }
         mut kout = k
         if((mods & MOD_SHIFT) != 0){
            if(k >= 65 && k <= 90){ kout = k }
            else { kout = _shift_char(k) }
         } else {
            if(k >= 65 && k <= 90){ kout = k + 32 }
         }
         def now_t = ticks()
         def repeat_ok = (now_t - _last_char_t) > int(_char_repeat_ms() * 1000000.0)
         if(action == 2 && !repeat_ok && _last_char_c == kout){ return true }
         if(action == 2 || repeat_ok || _last_char_c != kout){
            _scroll_off = 0
            return _inject(kout)
         }
      }
   } elif(typ == EVENT_KEY_RELEASED){
      def k = data.get("key", 0)
      if(_suppress_toggle_key != 0 && k == _suppress_toggle_key){
         _suppress_toggle_key = 0
         return true
      }
   } elif(typ == EVENT_MOUSE_SCROLL){
      _ensure_history_buffers()
      mut dy = float(data.get("dy", 0.0))
      _scroll_acc += dy
      mut dcells = int(_scroll_acc)
      _scroll_acc -= float(dcells)
      if(dcells != 0){
         _scroll_off += dcells
         def h_len = _history.len
         if(_scroll_off > h_len){ _scroll_off = h_len }
         if(_scroll_off < 0){ _scroll_off = 0 }
         _mark_history_dirty()
      }
      return true
   }
   false
}

fn set_font(any font) any {
   "Sets the font used for rendering terminal text."
   _font = font
   _font_metrics_dirty = true
   _mark_prompt_dirty()
   _mark_history_dirty()
   _ensure_font_metrics()
}

fn set_colors(int bg, int text, int cyan) any {
   "Updates the terminal's theme colors."
   _bg_color, _text_color, _cyan_color = bg, text, cyan
}

fn get_history() list {
   "Returns the current terminal history list."
   _ensure_history_buffers()
   return _history
}

fn get_input() str {
   "Returns the current input buffer string."
   _input
}

#main {
   init(0)
   def cell = font_cell_size(0, 16)
   assert(!is_open() && is_list(cell) && cell.len == 2 && cell.get(0, 0.0) > 0.0 && cell.get(1, 0.0) > 0.0, "render term init/cell")
   clear()
   assert(get_history().len == 0 && get_input() == "", "render term empty")
   _append_history_line("render-term")
   assert(get_history().len == 1 && get_history().get(0, "") == "render-term", "render term history")
   assert(handle_event(0, dict()) == false, "render term closed event")
   set_colors(0, 0xffffffff, 0xff00ffff)
   set_font(0)
   assert(!is_open(), "render term remains closed")
   print("✓ std.os.ui.render.viewer.term self-test passed")
}
