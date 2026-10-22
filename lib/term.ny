;; Keywords: term terminal ansi tty core console
;; Terminal styling, tables, progress bars, canvas drawing, keyboard input, and TUI control.
;; References:
;; - core
module core.term(bold, italic, dim, underline, color, style, panel, table, tree, bar, bar_update, bar_finish, bar_range, bar_write, get_terminal_size, clear_screen, cursor_hide, cursor_show, cursor_move, cursor_up, cursor_down, cursor_left, cursor_right, enable_wrap, disable_wrap, screen_reset, enter_alt_screen, leave_alt_screen, tui_begin, tui_end, tui_canvas_loop, is_quit_key, color_names, get_color_name, get_color, shapes, log_color_enabled, log_tag_color, log_tag, log_text, log_line, elog_line, canvas, canvas_clear, canvas_free, canvas_zclear, shade, canvas_set, canvas_print, canvas_box, canvas_refresh, get_key, poll_key, set_raw_mode, set_cooked_mode, write_str)
use core.str
use core.reflect
use core
use core as core
use core.os.time
use core.math.float
use core.os.sys
use core.os.ffi (cptr)
use core.os
use core.os.path
use core.os.platform as os_platform
use core.common as common

mut _vt_enabled = 0
mut _alt_enabled = 0
mut _refresh_buf = 0
mut _refresh_size = 0

fn _term_bytes(int n) bytes {
   "Allocates zeroed byte storage for terminal buffers."
   if n <= 0 { n = 1 }
   ;; Managed bytes carry the runtime heap header that bytes_get/bytes_set
   ;; require; a raw malloc pointer makes every buffered write a silent no-op.
   def p = bytes(n + 1)
   if !p { panic("terminal buffer allocation failed") }
   p
}

fn bytes_get(any b, int i) int { load8(b, i) }

fn bytes_set(any b, int i, int v) any { store8(b, v, i) }

fn _ensure_vt() int {
   if _vt_enabled { return 0 }
   if os_platform.is_windows() { __enable_vt() }
   _vt_enabled = 1
   0
}

fn write_str(any s) int {
   "Safely write a Nytrix string to stdout(bypassing metadata)."
   if s { unwrap(sys_write(1, cptr(s), s.len)) }
   0
}

fn clear_screen() int {
   "Clears the entire screen and moves cursor to home."
   _ensure_vt()
   def clear = "\033[2J"
   def home = "\033[H"
   ;; Literal byte counts: a computed `.len` intermittently truncates the
   ;; write on the native route (tagged/raw count-argument instability).
   unwrap(sys_write(1, cptr(clear), 4))
   unwrap(sys_write(1, cptr(home), 3))
   0
}

fn cursor_hide() int {
   "Hides the terminal cursor."
   _ensure_vt()
   def seq = "\033[?25l"
   unwrap(sys_write(1, cptr(seq), 6))
   0
}

fn cursor_show() int {
   "Shows the terminal cursor."
   _ensure_vt()
   def seq = "\033[?25h"
   unwrap(sys_write(1, cptr(seq), 6))
   0
}

fn cursor_move(any x, any y) int {
   "Moves the cursor to position(x, y). 1-based indexing."
   _ensure_vt()
   def s = f"\033[{to_str(y)};{to_str(x)}H"
   unwrap(sys_write(1, cptr(s), s.len))
   0
}

fn cursor_up(any n=1) int {
   "Moves cursor up by `n` lines."
   _ensure_vt()
   if is_int(n) == 0 { n = 1 }
   def s = f"\033[{to_str(n)}A"
   unwrap(sys_write(1, cptr(s), s.len))
   0
}

fn cursor_down(any n=1) int {
   "Moves cursor down by `n` lines."
   _ensure_vt()
   if is_int(n) == 0 { n = 1 }
   def s = f"\033[{to_str(n)}B"
   unwrap(sys_write(1, cptr(s), s.len))
   0
}

fn cursor_right(any n=1) int {
   "Moves cursor right by `n` columns."
   _ensure_vt()
   if is_int(n) == 0 { n = 1 }
   def s = f"\033[{to_str(n)}C"
   unwrap(sys_write(1, cptr(s), s.len))
   0
}

fn cursor_left(any n=1) int {
   "Moves cursor left by `n` columns."
   _ensure_vt()
   if is_int(n) == 0 { n = 1 }
   def s = f"\033[{to_str(n)}D"
   unwrap(sys_write(1, cptr(s), s.len))
   0
}

fn disable_wrap() int {
   "Disables line wrapping."
   _ensure_vt()
   def seq = "\033[?7l"
   unwrap(sys_write(1, cptr(seq), 5))
   0
}

fn enable_wrap() int {
   "Enables line wrapping."
   _ensure_vt()
   def seq = "\033[?7h"
   unwrap(sys_write(1, cptr(seq), 5))
   0
}

fn screen_reset() int {
   "Resets the terminal state: visible cursor, enabled wrap, cleared screen, reset styles."
   _ensure_vt()
   set_cooked_mode()
   def seq = "\033[0m\033[?25h\033[?7h\033[2J\033[H"
   unwrap(sys_write(1, cptr(seq), 22))
   0
}

fn enter_alt_screen() int {
   "Switches to terminal alternate screen buffer."
   _ensure_vt()
   if _alt_enabled { return 0 }
   def seq = "\033[?1049h\033[H"
   unwrap(sys_write(1, cptr(seq), 11))
   _alt_enabled = 1
   0
}

fn leave_alt_screen() int {
   "Leaves alternate screen buffer and returns to previous screen."
   _ensure_vt()
   if !_alt_enabled { return 0 }
   def seq = "\033[?1049l"
   unwrap(sys_write(1, cptr(seq), 8))
   _alt_enabled = 0
   0
}

fn tui_begin() int {
   "Enters stable full-screen TUI mode using alternate screen buffer."
   if __tty_isatty(0) == 0 || __tty_isatty(1) == 0 { return 0 }
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
   ;; Cleanup belongs to the successful begin transition, not to a fresh
   ;; descriptor probe.  A redirected/closed descriptor must not strand the
   ;; terminal in the alternate screen or raw mode.
   if !_alt_enabled { return 0 }
   _ensure_vt()
   set_cooked_mode()
   def cleanup_seq = "\033[0m\033[?25h\033[?7h\033[?2004l\033[?1000l\033[?1002l\033[?1003l\033[?1006l"
   ;; Keep the byte count explicit: the native string metadata includes the
   ;; terminator/ABI padding for this escape-heavy literal.
   unwrap(sys_write(1, cptr(cleanup_seq), 55))
   ;; Restore the saved shell screen and cursor last. Do not append a line feed:
   ;; it moves the prompt down and leaves an empty line after the app exits.
   leave_alt_screen()
   0
}

fn tui_canvas_loop(fnptr draw, int delay=16, int budget=0) int {
   "Runs a resize-aware TUI canvas loop. Calls draw(canvas, zbuf, width, height) once per frame. budget caps headless frames(0 = one headless frame; interactive runs until a quit key). Fixture-driven examples should use core.os.fixture canvas_loop instead, which keeps admission, quit keys, pacing, and the wall deadline in the bounded-fixture protocol."
   def interactive = __tty_isatty(0) != 0 && __tty_isatty(1) != 0
   if interactive { tui_begin() }
   defer { if interactive { tui_end() } }
   mut s = get_terminal_size()
   mut w, h = s[0], s[1] - 1
   if w < 1 { w = 80 }
   if h < 1 { h = 23 }
   if w > 300 { w = 300 }
   if h > 120 { h = 120 }
   mut c, z = canvas(w, h), [0.0] * w * h
   defer { canvas_free(c) }
   mut running, frames, k = 1, 0, 0
   while running && !is_quit_key(k) {
      ;; Terminal-size probes are comparatively expensive and some pseudo-TTYs
      ;; report alternating row values.  Check periodically so a flaky size
      ;; query cannot turn every frame into a full-screen clear storm.
      if frames % 30 == 0 {
         s = get_terminal_size()
         if w != s[0] || h != s[1] - 1 {
            w, h = s[0], s[1] - 1
            if w < 1 { w = 80 }
            if h < 1 { h = 23 }
            if w > 300 { w = 300 }
            if h > 120 { h = 120 }
            canvas_free(c)
            c, z = canvas(w, h), [0.0] * w * h
            clear_screen()
         }
      }
      draw(c, z, w, h)
      if delay > 0 { msleep(delay) }
      k = poll_key()
      frames += 1
      if !interactive && frames >= 1 && budget <= 0 { running = 0 }
      if !interactive && budget > 0 && frames >= budget { running = 0 }
   }
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
   ;; __tty_raw controls stdin's mode; its argument is enable/disable, not a
   ;; file descriptor.  The runtime owns the actual input descriptor.
   def rt = int(__tty_raw(1))
   if os_platform.is_windows() { return rt }
   if rt == 0 { return 0 }
   if common.env_truthy("CI") || common.env_truthy("NYTRIX_CI") || common.env_truthy("NYTRIX_TEST_MODE") { return rt }
   use core.os.process
   def st = _stty_bin()
   mut rc = -1
   if os_platform.is_macos() {
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
   def rt = int(__tty_raw(0))
   if os_platform.is_windows() { return rt }
   if rt == 0 { return 0 }
   def sane = int(__tty_sane_fd(0))
   if sane == 0 { return 0 }
   if common.env_truthy("CI") || common.env_truthy("NYTRIX_CI") || common.env_truthy("NYTRIX_TEST_MODE") { return rt }
   use core.os.process
   def st = _stty_bin()
   mut rc = -1
   if os_platform.is_macos() {
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
   if __tty_isatty(0) == 0 || __tty_isatty(1) == 0 { return 0 }
   while true {
      ;; Polling keeps this boundary on the same raw key ABI as poll_key and
      ;; avoids routing one byte through the tagged syscall/result bridge.
      def k = __poll_key()
      if k != 0 { return k }
      msleep(1)
   }
   0
}

fn poll_key() int {
   "Polls for a key without blocking. Returns 0 if no key."
   ;; A pipe is not a key press.  Callers decide their non-interactive frame
   ;; policy; poll_key must never synthesize a quit key for them.
   if __tty_isatty(0) == 0 || __tty_isatty(1) == 0 { return 0 }
   def direct = __poll_key()
   if direct != 0 { return direct }
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
   unwrap(sys_write(1, cptr(out), out.len))
   if cur >= total { bar.set(9, 1) }
   return 0
}

fn bar_finish(list bar) int {
   "Completes the progress bar, ensuring it reaches 100% and cleanup/newline as needed."
   if bar.get(1) < bar.get(0) { bar_update(bar, bar.get(0)) }
   if bar.get(6) {
      print("")
   } else {
      def clear = "\r\033[K"
      unwrap(sys_write(1, cptr(clear), clear.len))
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
   def clear = "\r\033[K"
   unwrap(sys_write(1, cptr(clear), clear.len))
   print(msg)
   bar_update(bar_obj, bar_obj.get(1))
   0
}

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

fn get_terminal_size() list<int> {
   "Returns one canonical [width, height] list from the runtime terminal query."
   def raw = __terminal_size_raw()
   if is_list(raw) {
      def cols, rows = int(raw.get(0, 80)), int(raw.get(1, 24))
      if cols > 0 && rows > 0 { return [cols, rows] }
   }
   def env_c, env_l = env("COLUMNS"), env("LINES")
   if is_str(env_c) && is_str(env_l) {
      mut ic, il = 0, 0
      use core.reflect
      ic, il = int(env_c), int(env_l)
      if ic > 0 && il > 0 { return [ic, il] }
   }
   return [80, 24]
}

fn canvas(int w, int h) any {
   "Creates an opaque, backend-neutral terminal canvas handle."
   __canvas(w, h)
}

fn canvas_clear(any canv) int {
   "Clears all cells and attributes in the canvas handle."
   __canvas_clear(canv)
   0
}

fn canvas_free(any canv) int {
   "Releases a canvas handle after its last use."
   __canvas_free(canv)
   0
}

fn canvas_zclear(any canv, list z) int {
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
   def j = i < 0 ? 0 : i >= n ? n - 1 : i
   utf8_slice(pal, j, j + 1)
}

fn canvas_set(any canv, int x, int y, any ch, int color_idx=0, int is_bold=0) int {
   "Sets a UTF-8 cell and its attributes on the canvas handle."
   __canvas_set(canv, x, y, ch, color_idx, is_bold)
   0
}

fn canvas_print(any canv, int x, int y, str text, int color_idx=0, int is_bold=0) int {
   "Prints UTF-8 text horizontally on the canvas handle."
   __canvas_print(canv, x, y, text, color_idx, is_bold)
   0
}

fn canvas_box(any canv, int x, int y, int w, int h, any title="", int color_idx=0) int {
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
   "Renders the canvas handle in one terminal write."
   __canvas_refresh(canv)
   0
}
