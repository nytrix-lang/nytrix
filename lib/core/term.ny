;; Keywords: term terminal ansi tty core console
;; Terminal styling, tables, progress bars, canvas drawing, keyboard input, and TUI control.
;; References:
;; - std.core
module std.core.term(bold, italic, dim, underline, color, style, panel, table, tree, bar, bar_update, bar_finish, bar_range, bar_write, get_terminal_size, clear_screen, cursor_hide, cursor_show, cursor_move, cursor_up, cursor_down, cursor_left, cursor_right, enable_wrap, disable_wrap, screen_reset, enter_alt_screen, leave_alt_screen, tui_begin, tui_end, tui_canvas_loop, is_quit_key, color_names, get_color_name, get_color, shapes, log_color_enabled, log_tag_color, log_tag, log_text, log_line, elog_line, canvas, canvas_clear, canvas_zclear, shade, canvas_set, canvas_print, canvas_box, canvas_refresh, get_key, poll_key, set_raw_mode, set_cooked_mode, write_str)
use std.core.str
use std.core.reflect
use std.core
use std.core as core
use std.os.time
use std.math.float
use std.os.sys
use std.os
use std.os.path
use std.os.platform as platform
use std.core.common as common

mut _vt_enabled = 0
mut _alt_enabled = 0
mut _refresh_buf = 0
mut _refresh_size = 0

fn _term_bytes(int n) ptr {
   "Allocates zeroed byte storage for terminal buffers."
   if n <= 0 { n = 1 }
   def p = malloc(n + 1)
   if p { memset(p, 0, n + 1) }
   p
}

fn bytes_get(any b, int i) int { load8(b, i) }

fn bytes_set(any b, int i, int v) any { store8(b, v, i) }

fn _ensure_vt() int {
   if _vt_enabled { return 0 }
   if platform.is_windows() { __enable_vt() }
   _vt_enabled = 1
   0
}

fn write_str(any s) int {
   "Safely write a Nytrix string to stdout(bypassing metadata)."
   if s { unwrap(sys_write(1, s, s.len)) }
   0
}

fn clear_screen() int {
   "Clears the entire screen and moves cursor to home."
   _ensure_vt()
   unwrap(sys_write(1, "\033[2J", 4))
   unwrap(sys_write(1, "\033[H", 3))
   0
}

fn cursor_hide() int {
   "Hides the terminal cursor."
   _ensure_vt()
   unwrap(sys_write(1, "\033[?25l", 6))
   0
}

fn cursor_show() int {
   "Shows the terminal cursor."
   _ensure_vt()
   unwrap(sys_write(1, "\033[?25h", 6))
   0
}

fn cursor_move(any x, any y) int {
   "Moves the cursor to position(x, y). 1-based indexing."
   _ensure_vt()
   def s = f"\033[{to_str(y)};{to_str(x)}H"
   unwrap(sys_write(1, s, s.len))
   0
}

fn cursor_up(any n=1) int {
   "Moves cursor up by `n` lines."
   _ensure_vt()
   if is_int(n) == 0 { n = 1 }
   def s = f"\033[{to_str(n)}A"
   unwrap(sys_write(1, s, s.len))
   0
}

fn cursor_down(any n=1) int {
   "Moves cursor down by `n` lines."
   _ensure_vt()
   if is_int(n) == 0 { n = 1 }
   def s = f"\033[{to_str(n)}B"
   unwrap(sys_write(1, s, s.len))
   0
}

fn cursor_right(any n=1) int {
   "Moves cursor right by `n` columns."
   _ensure_vt()
   if is_int(n) == 0 { n = 1 }
   def s = f"\033[{to_str(n)}C"
   unwrap(sys_write(1, s, s.len))
   0
}

fn cursor_left(any n=1) int {
   "Moves cursor left by `n` columns."
   _ensure_vt()
   if is_int(n) == 0 { n = 1 }
   def s = f"\033[{to_str(n)}D"
   unwrap(sys_write(1, s, s.len))
   0
}

fn disable_wrap() int {
   "Disables line wrapping."
   _ensure_vt()
   unwrap(sys_write(1, "\033[?7l", 5))
   0
}

fn enable_wrap() int {
   "Enables line wrapping."
   _ensure_vt()
   unwrap(sys_write(1, "\033[?7h", 5))
   0
}

fn screen_reset() int {
   "Resets the terminal state: visible cursor, enabled wrap, cleared screen, reset styles."
   _ensure_vt()
   set_cooked_mode()
   unwrap(sys_write(1, "\033[0m\033[?25h\033[?7h\033[2J\033[H", 22))
   0
}

fn enter_alt_screen() int {
   "Switches to terminal alternate screen buffer."
   _ensure_vt()
   if _alt_enabled { return 0 }
   unwrap(sys_write(1, "\033[?1049h\033[H", 11))
   _alt_enabled = 1
   0
}

fn leave_alt_screen() int {
   "Leaves alternate screen buffer and returns to previous screen."
   _ensure_vt()
   if !_alt_enabled { return 0 }
   unwrap(sys_write(1, "\033[?1049l", 8))
   _alt_enabled = 0
   0
}

fn tui_begin() int {
   "Enters stable full-screen TUI mode using alternate screen buffer."
   _ensure_vt()
   enter_alt_screen()
   set_raw_mode()
   disable_wrap()
   clear_screen()
   cursor_hide()
   0
}

fn tui_end() int {
   "Restores terminal after TUI mode, leaving alternate screen buffer."
   _ensure_vt()
   set_cooked_mode()
   def cleanup_seq = "\033[0m\033[?25h\033[?7h\033[?2004l\033[?1000l\033[?1002l\033[?1003l\033[?1006l"
   unwrap(sys_write(1, cleanup_seq, cleanup_seq.len))
   leave_alt_screen()
   unwrap(sys_write(1, cleanup_seq, cleanup_seq.len))
   0
}


fn tui_canvas_loop(any draw, int delay=16) int {
   "Runs a resize-aware TUI canvas loop. Calls draw(canvas, zbuf, width, height) once per frame."
   tui_begin()
   mut s = get_terminal_size()
   mut w, h = s[0], s[1] - 1
   mut c, z, k = canvas(w, h), [0.0] * w * h, 0
   while !is_quit_key(k) {
      s = get_terminal_size()
      if w != s[0] || h != s[1] - 1 {
         w, h = s[0], s[1] - 1
         c, z = canvas(w, h), [0.0] * w * h
         clear_screen()
      }
      draw(c, z, w, h)
      if delay > 0 { msleep(delay) }
      k = poll_key()
   }
   tui_end()
   0
}

fn is_quit_key(any key) bool {
   "Returns true for quit keys in TUI mode(Esc or Ctrl+C)."
   key == 27 || key == 3
}

fn _stty_bin() str {
   if file_exists("/usr/bin/stty") { return "/usr/bin/stty" }
   if file_exists("/bin/stty") { return "/bin/stty" }
   "stty"
}

fn set_raw_mode() int {
   "Enables raw mode for the terminal(no echo, no buffering)."
   def rt = __tty_raw(1)
   if platform.is_windows() { return rt }
   if rt == 0 { return 0 }
   if common.env_truthy("CI") || common.env_truthy("NYTRIX_CI") || common.env_truthy("NYTRIX_TEST_MODE") { return rt }
   use std.os.process
   def st = _stty_bin()
   mut rc = -1
   if platform.is_macos() {
      rc = run(st, ["-f", "/dev/tty", "raw", "-echo"])
   } else {
      rc = run(st, ["-F", "/dev/tty", "raw", "-echo"])
   }
   if rc != 0 { rc = run(st, ["raw", "-echo"]) }
   if rc == 0 { return 0 }
   rc
}

fn set_cooked_mode() int {
   "Restores terminal to normal mode."
   def rt = __tty_raw(0)
   if platform.is_windows() { return rt }
   if rt == 0 { return 0 }
   def sane = __tty_sane_fd(0)
   if sane == 0 { return 0 }
   if common.env_truthy("CI") || common.env_truthy("NYTRIX_CI") || common.env_truthy("NYTRIX_TEST_MODE") { return rt }
   use std.os.process
   def st = _stty_bin()
   mut rc = -1
   if platform.is_macos() {
      rc = run(st, ["-f", "/dev/tty", "-raw", "echo"])
   } else {
      rc = run(st, ["-F", "/dev/tty", "-raw", "echo"])
   }
   if rc != 0 { rc = run(st, ["-raw", "echo"]) }
   if rc == 0 { return 0 }
   rc
}

fn get_key() int {
   "Reads a single key from stdin in raw mode. Blocks until key is pressed."
   def b = _term_bytes(1)
   def res = unwrap(sys_read(0, b, 1))
   if res <= 0 {
      free(b)
      return 0
   }
   def k = __load8_idx(b, 0)
   free(b)
   k
}

fn poll_key() int {
   "Polls for a key without blocking. Returns 0 if no key."
   if __tty_pending() <= 0 { return 0 }
   get_key()
}

fn bold(any s) str {
   "Wraps string `s` with ANSI bold escape codes."
   f"\033[1m{s}\033[0m"
}

fn italic(any s) str {
   "Wraps string `s` with ANSI italic escape codes."
   f"\033[3m{s}\033[0m"
}

fn dim(any s) str {
   "Wraps string `s` with ANSI dim/faint escape codes."
   f"\033[2m{s}\033[0m"
}

fn underline(any s) str {
   "Wraps string `s` with ANSI underline escape codes."
   f"\033[4m{s}\033[0m"
}

fn _ansi_color_code(any c) str {
   case c {
      "black"   -> "30"
      "red"     -> "31"
      "green"   -> "32"
      "yellow"  -> "33"
      "blue"    -> "34"
      "magenta" -> "35"
      "cyan"    -> "36"
      "white"   -> "37"
      "gray"    -> "90"
      _         -> "37"
   }
}

fn color(any s, any c) str {
   "Wraps string `s` with ANSI foreground color escape codes for color `c`."
   def code = _ansi_color_code(c)
   f"\033[{code}m{s}\033[0m"
}

fn style(any text, any color_name="", any is_bold=0) str {
   "Applies ANSI styling to text."
   if is_str(color_name) == 0 { color_name = "" }
   if is_int(is_bold) == 0 { is_bold = 0 }
   mut out = text
   if is_bold { out = f"\033[1m{out}" }
   if color_name.len > 0 {
      def code = _ansi_color_code(color_name)
      out = f"\033[{code}m{out}"
   }
   if is_bold || color_name.len > 0 { out = f"{out}\033[0m" }
   return out
}

fn color_names() list {
   "Returns a list of supported color names."
   def c = ["black", "red", "green", "yellow", "blue", "magenta", "cyan", "white", "gray"]
   c
}

fn get_color_name(int idx) str {
   "Returns the color name at index `idx` (cycling)."
   def c, n = color_names(), c.len
   c.get(idx % n)
}

fn get_color(any text, int idx) str {
   "Returns `text` wrapped in the color at index `idx`."
   color(text, get_color_name(idx))
}

fn log_color_enabled() bool {
   "Returns true when terminal log colors should be emitted."
   if common.env_present("NO_COLOR") { return false }
   if common.env_present("NY_UI_COLOR") { return common.env_enabled("NY_UI_COLOR") }
   if common.env_present("NYTRIX_COLOR") { return common.env_enabled("NYTRIX_COLOR") }
   if common.env_present("NYTRIX_TOOL_COLOR") {
      def mode = common.env_lower("NYTRIX_TOOL_COLOR")
      if mode == "never" || mode == "off" || mode == "0" { return false }
      if mode == "always" || mode == "on" || mode == "1" { return true }
   }
   true
}

fn _tag_has(str key, str a, str b="", str c="", str d="", str e="") bool {
   if a.len > 0 && str.find(key, a) >= 0 { return true }
   if b.len > 0 && str.find(key, b) >= 0 { return true }
   if c.len > 0 && str.find(key, c) >= 0 { return true }
   if d.len > 0 && str.find(key, d) >= 0 { return true }
   if e.len > 0 && str.find(key, e) >= 0 { return true }
   false
}

fn log_tag_color(any tag) str {
   "Returns a semantic color name for a bracketed log tag."
   def key = str.lower(to_str(tag))
   if _tag_has(key, "fail", "error", "err", "panic") { return "red" }
   if _tag_has(key, "warn", "retry", "fallback", "slow") { return "yellow" }
   if _tag_has(key, "ok", "done", "complete", "ready") { return "green" }
   if _tag_has(key, "batch", "dump", "render", "frame") { return "cyan" }
   if _tag_has(key, "gltf", "model", "scene", "vk", "gfx") { return "magenta" }
   "blue"
}

fn log_tag(any tag) str {
   "Formats `[tag]` with semantic color inside the brackets when colors are enabled."
   def raw = to_str(tag)
   log_color_enabled() ? ("[" + style(raw, log_tag_color(raw), 1) + "]") : ("[" + raw + "]")
}

fn _log_color_bracket(str line, int start, int stop) str {
   def tag = str.str_slice(line, start + 1, stop)
   log_tag(tag)
}

fn log_text(any line) str {
   "Colors bracketed log tags inside a line, e.g. `[gltf]`, `[vk:error]`."
   def s = to_str(line)
   if !log_color_enabled() { return s }
   def n = s.len
   mut out = ""
   mut i = 0
   while i < n {
      def ch = load8(s, i)
      if ch == 91 {
         mut j = i + 1
         mut ok = false
         while j < n && j - i <= 48 {
            def cj = load8(s, j)
            if cj == 93 { ok = true break }
            if cj < 32 || cj == 91 { break }
            j += 1
         }
         if ok {
            out = out + _log_color_bracket(s, i, j)
            i = j + 1
            continue
         }
      }
      out = out + str.str_slice(s, i, i + 1)
      i += 1
   }
   out
}

fn log_line(any tag, any msg="") int {
   "Prints a semantic colored `[tag] message` line."
   print(log_tag(tag) + (to_str(msg).len > 0 ? (" " + to_str(msg)) : ""))
   0
}

fn elog_line(any tag, any msg="") int {
   "Prints a semantic colored `[tag] message` line to stderr."
   eprint(log_tag(tag) + (to_str(msg).len > 0 ? (" " + to_str(msg)) : ""))
   0
}

fn shapes() dict {
   "Returns a dictionary of common TUI shapes/symbols."
   {
      "v_line": "\xe2\x94\x82",
      "h_line": "\xe2\x94\x80",
      "top_left": "\xe2\x95\xad",
      "top_right": "\xe2\x95\xae",
      "bot_left": "\xe2\x95\xb0",
      "bot_right": "\xe2\x95\xaf",
      "cross": "\xe2\x94\xbc",
      "t_down": "\xe2\x94\xac",
      "t_up": "\xe2\x94\xb4",
      "t_left": "\xe2\x94\xa4",
      "t_right": "\xe2\x94\x9c",
      "shade": "\xe2\x96\x92",
      "dot": "\xc2\xb7",
      "block": "\xe2\x96\x88"
   }
}

fn panel(any text, any title="", any border_color="white") int {
   "Prints a styled panel with optional title."
   def l = text.len
   mut w = l + 4
   if title.len > 0 {
      def tl = title.len
      if tl + 4 > w { w = tl + 4 }
   }
   mut top = "╭"
   mut i = 0
   while i < w - 2 {
      top = f"{top}─"
      i += 1
   }
   top = f"{top}╮"
   top = color(top, border_color)
   if title.len > 0 {
      top = color("╭─ ", border_color)
      def title_col = color(title, "cyan")
      def mid_col = color(" ─", border_color)
      def bar_col = color("─", border_color)
      def cap_col = color("╮", border_color)
      top = f"{top}{title_col}{mid_col}"
      def rem = w - 2 - 2 - title.len - 2
      i = 0
      while i < rem {
         top = f"{top}{bar_col}"
         i += 1
      }
      top = f"{top}{cap_col}"
   }
   print(top)
   def padding = w - 4 - text.len
   def cbar = color("│", border_color)
   mut line = f"{cbar} {text} "
   i = 0
   while i < padding {
      line = f"{line} "
      i += 1
   }
   line = f"{line}{cbar}"
   print(line)
   mut bot = "╰"
   i = 0
   while i < w - 2 {
      bot = f"{bot}─"
      i += 1
   }
   bot = f"{bot}╯"
   print(color(bot, border_color))
   0
}

fn table(list headers, list rows) int {
   "Prints a simple table."
   def cols = headers.len
   mut widths = list(8)
   mut i = 0
   while i < cols {
      widths = widths.append(len(headers.get(i)))
      i += 1
   }
   mut r = 0 def nr = rows.len
   while r < nr {
      def row = rows.get(r)
      mut c = 0
      while c < cols {
         def val = row.get(c)
         if val.len > widths.get(c) { widths.set(c, val.len) }
         c += 1
      }
      r += 1
   }
   mut line = ""
   i = 0
   while i < cols {
      def h, w = headers.get(i), widths.get(i)
      line = f"{line}{bold(h)}"
      def pad = w - h.len + 2
      mut p = 0
      while p < pad { line = f"{line} " p += 1 }
      line = f"{line} "
      i += 1
   }
   print(line)
   mut sep = ""
   def slen = line.len
   i = 0
   while i < (slen / 2) {
      sep = f"{sep}─"
      i += 1
   }
   print(color(sep, "gray"))
   r = 0
   while r < nr {
      def row = rows.get(r)
      mut line_row, c = "", 0
      while c < cols {
         def val = row.get(c)
         def w = widths.get(c)
         line_row = f"{line_row}{val}"
         def pad = w - val.len + 2
         mut p = 0
         while p < pad {
            line_row = f"{line_row} "
            p += 1
         }
         line_row = f"{line_row} "
         c += 1
      }
      print(line_row)
      r += 1
   }
   0
}

fn tree(any node, any pref="", any head_in="") int {
   "Prints a tree structure. Node is [label, [children...]] or just label string."
   def prefix = is_str(pref) ? to_str(pref) : ""
   def head = is_str(head_in) ? to_str(head_in) : ""
   if is_str(node) {
      print(f"{prefix}{head}{node}")
      return 0
   }
   def label = node.get(0)
   print(f"{prefix}{head}{bold(label)}")
   def children = node.get(1)
   def count = children.len
   mut i = 0
   while i < count {
      def last = (i == count - 1)
      def child = children.get(i)
      def next_prefix = last ? f"{prefix}    " : f"{prefix}│   "
      def next_head = last ? "╰── " : "├── "
      tree(child, next_prefix, next_head)
      i += 1
   }
   0
}

fn _bar_pad2(int n) str {
   if n < 0 { n = 0 }
   if n < 10 { return "0" + to_str(n) }
   to_str(n)
}

fn _bar_duration(any seconds) str {
   mut total = int(seconds)
   if total < 0 { total = 0 }
   def hours = total / 3600
   def mins = (total / 60) % 60
   def secs = total % 60
   if hours > 0 { return to_str(hours) + ":" + _bar_pad2(mins) + ":" + _bar_pad2(secs) }
   _bar_pad2(mins) + ":" + _bar_pad2(secs)
}

fn _bar_rate(f64 rate) str {
   if rate <= 0.0 { return "?it/s" }
   if rate >= 100.0 { return to_fixed(rate, 0) + "it/s" }
   if rate >= 10.0 { return to_fixed(rate, 1) + "it/s" }
   to_fixed(rate, 2) + "it/s"
}

fn bar(any tot=100, any d="Progress", any w=40, any bc="green", any se=1, any lv=1) list {
   "Create a progress bar. Returns a bar object(list)."
   mut total = 100
   if is_int(tot) { total = tot }
   if total < 0 { total = 0 }
   mut desc = "Progress"
   if is_str(d) { desc = d }
   mut width = 40
   if is_int(w) { width = w }
   if width < 4 { width = 4 }
   if width > 120 { width = 120 }
   mut bar_color = "green"
   if is_str(bc) { bar_color = bc }
   mut show_eta = 1
   if is_int(se) { show_eta = se }
   mut leave = 1
   if is_int(lv) { leave = lv }
   mut bar = list(12)
   bar = bar.append(total)
   bar = bar.append(0)
   bar = bar.append(desc)
   bar = bar.append(width)
   bar = bar.append(bar_color)
   bar = bar.append(show_eta)
   bar = bar.append(leave)
   def start_time = ticks()
   bar = bar.append(start_time)
   bar = bar.append(start_time)
   bar = bar.append(0)
   bar = bar.append(0)
   bar = bar.append(0.0)
   return bar
}

fn bar_update(list bar, any current) int {
   "Updates the progress bar to the `current` value and renders it to stdout."
   if bar.get(9) == 1 { return 0 }
   mut cur = 0
   if is_int(current) { cur = current }
   if cur < 0 { cur = 0 }
   def total = bar.get(0)
   if total > 0 && cur > total { cur = total }
   if cur <= bar.get(10) && cur != 0 && cur < bar.get(0) { return 0 }
   bar.set(1, cur)
   bar.set(10, cur)
   def desc = bar.get(2)
   def width = bar.get(3)
   def bar_color = bar.get(4)
   def start_time = bar.get(7)
   def last_time = bar.get(8)
   def now = ticks()
   def dt = now - last_time
   def elapsed = (now - start_time) / 1000000000.0
   mut avg_rate = bar.get(11)
   if dt > 150000000 || cur == total {
      if elapsed > 0.0 && cur > 0 {
         def inst = cur / elapsed
         if avg_rate == 0.0 {
            avg_rate = inst
         } else {
            avg_rate = (avg_rate * 0.8) + (inst * 0.2)
         }
         bar.set(11, avg_rate)
         bar.set(8, now)
      }
   }
   mut den = total
   if den <= 0 { den = 1 }
   def pct = (cur * 100) / den
   def filled = (cur * width) / den
   mut b_len = filled
   if b_len > width { b_len = width }
   mut e_len = width - b_len
   if e_len < 0 { e_len = 0 }
   def b_str, e_str = color(str.repeat("=", b_len), bar_color), color(str.repeat(".", e_len), "gray")
   mut rate = avg_rate
   if rate <= 0.0 && cur > 0 {
      if elapsed > 0.0 { rate = cur / elapsed }
      else { rate = cur * 1000.0 }
   }
   mut eta = "?"
   if rate > 0.0 && total > 0 && cur < total { eta = _bar_duration((total - cur) / rate) }
   elif total > 0 && cur >= total { eta = "00:00" }
   mut timing = "[" + _bar_duration(elapsed) + "<" + eta + ", " + _bar_rate(rate) + "]"
   if bar.get(5) == 0 { timing = "[" + _bar_duration(elapsed) + ", " + _bar_rate(rate) + "]" }
   def out = f"\r\033[K{desc}: {to_str(pct)}%|{b_str}{e_str}| {to_str(cur)}/{to_str(total)} {timing}"
   unwrap(sys_write(1, out, out.len))
   if cur >= total { bar.set(9, 1) }
   return 0
}

fn bar_finish(list bar) int {
   "Completes the progress bar, ensuring it reaches 100% and cleanup/newline as needed."
   if bar.get(1) < bar.get(0) { bar_update(bar, bar.get(0)) }
   if bar.get(6) {
      print("")
   } else {
      unwrap(sys_write(1, "\r\033[K", 4))
   }
   bar.set(9, 1)
   0
}

fn bar_range(any n, any desc="") list {
   "Compatibility wrapper for `bar(n, desc)`. Returns a new bar object."
   return bar(n, desc)
}

fn bar_write(list bar_obj, any msg) int {
   "Clears the current progress bar line, prints `msg`, and redraws the bar."
   unwrap(sys_write(1, "\r\033[K", 4))
   print(msg)
   bar_update(bar_obj, bar_obj.get(1))
   0
}

mut _term_buf = 0
mut _chr_cache = 0

fn _chr_cached(int code) str {
   if code < 0 || code > 255 { return "" }
   if !_chr_cache {
      _chr_cache = list(256)
      mut i = 0
      while i < 256 {
         _chr_cache = _chr_cache.append(str.chr(i))
         i += 1
      }
   }
   _chr_cache.get(code, "")
}

fn get_terminal_size() list {
   "Retrieves terminal [width, height] using ioctl, environment variables, or defaults."
   if !_term_buf { _term_buf = malloc(8) }
   def buf = _term_buf
   if __tty_size(buf) == 0 {
      def cols = load32(buf, 0)
      def rows = load32(buf, 4)
      if rows > 0 && cols > 0 { return [cols, rows] }
   }
   def env_c, env_l = env("COLUMNS"), env("LINES")
   if is_str(env_c) && is_str(env_l) {
      mut ic, il = 0, 0
      use std.core.reflect
      ic, il = int(env_c), int(env_l)
      if ic > 0 && il > 0 { return [ic, il] }
   }
   return [80, 24]
}

fn canvas(int w, int h) list {
   "Creates a new terminal canvas for buffered drawing."
   mut c = list(10)
   c = c.append(w)
   c = c.append(h)
   mut char_buf = list(w * h)
   mut i = 0
   while i < w * h {
      char_buf = char_buf.append(" ")
      i += 1
   }
   c = c.append(char_buf)
   c = c.append(_term_bytes(w * h))
   c = c.append(_term_bytes(w * h))
   c = c.append(_term_bytes(w * h))
   canvas_clear(c)
   return c
}

fn canvas_clear(list canv) int {
   "Clears all buffers(characters, attributes, and colors) in the canvas."
   def w, h = canv.get(0), canv.get(1)
   def buf, attr, col = canv.get(2), canv.get(3), canv.get(4)
   def blen = canv.get(5)
   mut i = 0
   def n = w * h
   while i < n {
      buf.set(i, " ")
      bytes_set(attr, i, 0)
      bytes_set(col, i, 0)
      bytes_set(blen, i, 1)
      i += 1
   }
   0
}


fn canvas_zclear(list canv, list z) int {
   "Clears a canvas and zeroes a matching depth/z buffer."
   mut i = 0
   while i < z.len { z[i] = 0.0 i += 1 }
   canvas_clear(canv)
}

fn shade(str pal, any v, any mul=1.0) str {
   "Picks a clamped shade character from a palette."
   def n = pal.len
   if n <= 0 { return "" }
   def i = int(v * mul)
   pal[i < 0 ? 0 : i >= n ? n - 1 : i]
}

fn _byte_len(any s) int {
   if !is_str(s) { return 0 }
   def n = s.len
   if n == 0 { return 0 }
   if load8(s, n) == 0 { return n }
   mut i = n
   while load8(s, i) != 0 { i += 1 }
   i
}

fn _substr_bytes(str s, int start, int len) str {
   if len <= 0 { return "" }
   mut out = malloc(len + 1)
   if !out { return "" }
   init_str(out, len)
   mut i = 0
   while i < len {
      store8(out, load8(s, start + i), i)
      i += 1
   }
   store8(out, 0, len)
   out
}

fn canvas_set(list canv, int x, int y, any ch, int color_idx=0, int is_bold=0) int {
   "Sets a character and its attributes at(x, y) on the canvas."
   def w, h = canv.get(0), canv.get(1)
   if x < 0 || x >= w || y < 0 || y >= h { return 0 }
   def idx = y * w + x
   def chars = canv.get(2)
   def attr = canv.get(3)
   def col = canv.get(4)
   def blen = canv.get(5)
   if !is_list(chars) || idx < 0 || idx >= chars.len { return 0 }
   mut char_str = ""
   if is_str(ch) {
      char_str = ch
   } else {
      if ch >= 0 && ch <= 255 {
         char_str = _chr_cached(ch)
      } else {
         char_str = str.chr(ch)
      }
   }
   if char_str.len == 0 { char_str = "?" }
   chars.set(idx, char_str)
   bytes_set(attr, idx, is_bold)
   bytes_set(col, idx, color_idx)
   bytes_set(blen, idx, _byte_len(char_str))
   0
}

fn canvas_print(list canv, int x, int y, str text, int color_idx=0, int is_bold=0) int {
   "Prints a string horizontally on the canvas starting at(x, y)."
   def w, h = canv.get(0), canv.get(1)
   if y < 0 || y >= h || text.len == 0 { return 0 }
   mut i = 0
   mut col = 0
   def l = text.len
   while i < l {
      if x + col >= w { break }
      def b0 = load8(text, i)
      mut clen = 1
      if b0 >= 240 { clen = 4 }
      elif b0 >= 224 { clen = 3 }
      elif b0 >= 192 { clen = 2 }
      if i + clen > l { clen = l - i }
      def ch = _substr_bytes(text, i, clen)
      canvas_set(canv, x + col, y, ch, color_idx, is_bold)
      i = i + clen
      col += 1
   }
   0
}

fn canvas_box(list canv, int x, int y, int w, int h, any title="", int color_idx=0) int {
   "Draws a styled box with an optional title on the canvas."
   def s = shapes()
   mut i = 0
   while i < w {
      canvas_set(canv, x + i, y, s.get("h_line"), color_idx)
      canvas_set(canv, x + i, y + h - 1, s.get("h_line"), color_idx)
      i += 1
   }
   i = 0
   while i < h {
      canvas_set(canv, x, y + i, s.get("v_line"), color_idx)
      canvas_set(canv, x + w - 1, y + i, s.get("v_line"), color_idx)
      i += 1
   }
   canvas_set(canv, x, y, s.get("top_left"), color_idx)
   canvas_set(canv, x + w - 1, y, s.get("top_right"), color_idx)
   canvas_set(canv, x, y + h - 1, s.get("bot_left"), color_idx)
   canvas_set(canv, x + w - 1, y + h - 1, s.get("bot_right"), color_idx)
   if title.len > 0 { canvas_print(canv, x + 2, y, f" {title} ", color_idx, 1) }
   0
}

fn canvas_refresh(any canv) int {
   "Renders the entire canvas buffer to the physical terminal using an optimized single-write approach."
   if !is_list(canv) { return 0 }
   def w, h = canv.get(0), canv.get(1)
   if w <= 0 || h <= 0 { return 0 }
   def buf = canv.get(2)
   def attr = canv.get(3)
   def col = canv.get(4)
   def blen = canv.get(5)
   def r_buf = _term_bytes(w * h * 64 + 1024)
   if !r_buf { return 0 }
   mut p = 0
   bytes_set(r_buf, p, 27)
   bytes_set(r_buf, p + 1, 91)
   bytes_set(r_buf, p + 2, 72)
   p += 3
   mut last_c, last_b = -1, -1
   mut y = 0
   while y < h {
      mut x = 0
      while x < w {
         def idx = y * w + x
         def cell = buf.get(idx, " ")
         def b = bytes_get(attr, idx)
         def c = bytes_get(col, idx)
         if c != last_c || b != last_b {
            bytes_set(r_buf, p, 27)
            bytes_set(r_buf, p + 1, 91)
            p += 2
            if c == 0 && b == 0 {
               bytes_set(r_buf, p, 48)
               p += 1
             } else {
                if b {
                   bytes_set(r_buf, p, 49)
                   bytes_set(r_buf, p + 1, 59)
                   p += 2
                }
                if c >= 9 && c <= 255 {
                   bytes_set(r_buf, p, 51)
                   bytes_set(r_buf, p + 1, 56)
                   bytes_set(r_buf, p + 2, 59)
                   bytes_set(r_buf, p + 3, 53)
                   bytes_set(r_buf, p + 4, 59)
                   p += 5
                   if c >= 100 {
                      bytes_set(r_buf, p, 48 + c / 100)
                      bytes_set(r_buf, p + 1, 48 + (c / 10) % 10)
                      bytes_set(r_buf, p + 2, 48 + c % 10)
                      p += 3
                   } elif c >= 10 {
                      bytes_set(r_buf, p, 48 + c / 10)
                      bytes_set(r_buf, p + 1, 48 + c % 10)
                      p += 2
                   } else {
                      bytes_set(r_buf, p, 48 + c)
                      p += 1
                   }
                } else {
                   def code = case c {
                      1 -> 49 2 -> 50 3 -> 51
                      4 -> 52 5 -> 53 6 -> 54
                      7 -> 55 8 -> 48 _ -> 55
                   }
                   bytes_set(r_buf, p, 51)
                   if c == 8 { bytes_set(r_buf, p, 57) }
                   bytes_set(r_buf, p + 1, code)
                   p += 2
                }
             }
            bytes_set(r_buf, p, 109)
            p += 1
            last_c, last_b = c, b
         }
         def clen = bytes_get(blen, idx)
         mut j = 0
         while j < clen {
            bytes_set(r_buf, p, load8(cell, j))
            p += 1
            j += 1
         }
         x += 1
      }
      if y < h - 1 {
         bytes_set(r_buf, p, 13)
         bytes_set(r_buf, p + 1, 10)
         p += 2
      }
      y += 1
   }
   bytes_set(r_buf, p, 27)
   bytes_set(r_buf, p + 1, 91)
   bytes_set(r_buf, p + 2, 48)
   bytes_set(r_buf, p + 3, 109)
   p += 4
   unwrap(sys_write(1, r_buf, p))
   free(r_buf)
   0
}
