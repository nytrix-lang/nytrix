;; Keywords: render terminal tty tui ansi backend input
;; Terminal backend state for the CPU renderer: selection, raw input, and
;; true-color presentation. It does not import the renderer, so the renderer
;; can drive it from init_window/end_frame/window_should_close.
module core.os.ui.render.tty(
   requested_by_name, auto_selected, begin, end, active, size, frame_to_ansi,
   present_rgba, decode_keys, read_keys, KEY_QUIT
)

use core
use core.str as str
use core.term (write_str, enter_alt_screen, leave_alt_screen, cursor_hide,
   cursor_show, set_raw_mode, set_cooked_mode)
use core.os.prim (env)

;; Returned by decode_keys for Ctrl+C: the window should close.
def int KEY_QUIT = -1
mut bool _active = false

fn _env_lower(str name) str {
   def v = env(name)
   is_str(v) ? str.lower(str.strip(v)) : ""
}

fn requested_by_name(str name) bool {
   "True when a backend name selects the terminal: tty, terminal, ansi, or tui."
   def n = str.lower(str.strip(name))
   n == "tty" || n == "terminal" || n == "ansi" || n == "tui"
}

fn _interactive() bool { __tty_isatty(0) != 0 && __tty_isatty(1) != 0 }

fn auto_selected() bool {
   "Automatic choice: no display server, both stdin and stdout are terminals,
   and neither NY_UI_TTY=0 nor a test replay disables it."
   if requested_by_name(_env_lower("NY_UI_BACKEND")) { return true }
   if requested_by_name(_env_lower("NY_UI_RENDER_BACKEND")) { return true }
   def opt = _env_lower("NY_UI_TTY")
   if opt == "0" || opt == "false" || opt == "off" { return false }
   if _env_lower("NYTRIX_UI_REPLAY").len > 0 { return false }
   if _env_lower("WAYLAND_DISPLAY").len > 0 || _env_lower("DISPLAY").len > 0 { return false }
   if opt == "1" || opt == "true" || opt == "on" { return true }
   _interactive()
}

fn active() bool { "True between begin and end." _active }

fn begin() bool {
   "Enters the alternate screen with raw, unechoed input and a hidden cursor."
   if _active { return true }
   if _interactive() {
      enter_alt_screen()
      cursor_hide()
      set_raw_mode()
   }
   _active = true
   true
}

fn end() bool {
   "Restores the cooked terminal, the cursor, and the primary screen."
   if !_active { return false }
   if _interactive() {
      set_cooked_mode()
      write_str("\x1b[0m")
      cursor_show()
      leave_alt_screen()
   }
   _active = false
   true
}

fn size() list {
   "Returns [columns, rows] of the controlling terminal, else COLUMNS/LINES, else 80x24."
   def ptr dims = malloc(8)
   mut cols, rows = 0, 0
   if dims {
      if int(__tty_size(dims)) == 0 {
         cols = load32(dims, 0)
         rows = load32(dims, 4)
      }
      free(dims)
   }
   if cols <= 0 || rows <= 0 {
      def c = str.atoi(_env_lower("COLUMNS"))
      def r = str.atoi(_env_lower("LINES"))
      if c > 0 && r > 0 {
         cols = c
         rows = r
      }
   }
   if cols <= 0 || rows <= 0 { return [80, 24] }
   [cols, rows]
}

fn frame_to_ansi(dict frame, int columns=80, int rows=24) str {
   "Samples an RGBA8 frame into true-color terminal cells; does not own frame memory."
   def int width = int(frame.get("width", 0))
   def int height = int(frame.get("height", 0))
   def ptr data = __ffi_untag_ptr(frame.get("data", 0))
   if width <= 0 || height <= 0 || columns <= 0 || rows <= 0 || !data { return "" }
   if int(frame.get("channels", 4)) != 4 || int(frame.get("bpp", 4)) != 4 { return "" }
   mut pieces = []
   mut row = 0
   while row < rows {
      pieces = pieces.append("\x1b[" + to_str(row + 1) + ";1H")
      def int sy = int(row * height / rows)
      mut previous = -1
      mut column = 0
      while column < columns {
         def int sx = int(column * width / columns)
         def int offset = (sy * width + sx) * 4
         def int red = load8(data, offset) & 255
         def int green = load8(data, offset + 1) & 255
         def int blue = load8(data, offset + 2) & 255
         def int rgb = red | (green << 8) | (blue << 16)
         if rgb != previous {
            pieces = pieces.append("\x1b[48;2;" + to_str(red) + ";" + to_str(green) + ";" + to_str(blue) + "m")
            previous = rgb
         }
         pieces = pieces.append(" ")
         column += 1
      }
      row += 1
   }
   pieces = pieces.append("\x1b[0m\x1b[1;1H")
   str.join(pieces, "")
}

fn present_rgba(dict frame) bool {
   "Writes one frame sized to the current terminal; the caller owns the frame."
   def dims = size()
   def output = frame_to_ansi(frame, int(dims.get(0, 80)), int(dims.get(1, 24)))
   if output.len <= 0 { return false }
   write_str(output)
   true
}

fn _letter_key(int b) int {
   if b >= 97 && b <= 122 { return b - 32 }
   b
}

fn decode_keys(list bytes) list {
   "Maps raw terminal bytes to window key codes(consts KEY_*), KEY_QUIT for Ctrl+C."
   mut out = []
   mut i = 0
   def n = bytes.len
   while i < n {
      def b = int(bytes.get(i, 0))
      if b == 27 {
         def next = i + 1 < n ? int(bytes.get(i + 1, 0)) : -1
         if (next == 91 || next == 79) && i + 2 < n {
            def code = int(bytes.get(i + 2, 0))
            mut key = 0
            if code == 65 { key = 265 }
            elif code == 66 { key = 264 }
            elif code == 67 { key = 262 }
            elif code == 68 { key = 263 }
            elif code == 72 { key = 268 }
            elif code == 70 { key = 269 }
            if key != 0 {
               out = out.append(key)
               i += 3
               continue
            }
            ;; Unknown CSI: skip to its final byte.
            mut j = i + 2
            while j < n && (int(bytes.get(j, 0)) < 64 || int(bytes.get(j, 0)) > 126) { j += 1 }
            i = j + 1
            continue
         }
         out = out.append(256)
         i += 1
         continue
      }
      if b == 3 { out = out.append(KEY_QUIT) }
      elif b == 13 || b == 10 { out = out.append(257) }
      elif b == 9 { out = out.append(258) }
      elif b == 127 || b == 8 { out = out.append(259) }
      elif b >= 32 && b < 127 { out = out.append(_letter_key(b)) }
      i += 1
   }
   out
}

fn read_keys(int limit=64) list {
   "Reads every pending input byte without blocking and decodes it."
   if !_active || !_interactive() { return [] }
   mut bytes = []
   while bytes.len < limit {
      def k = int(__poll_key())
      if k == 0 { break }
      bytes = bytes.append(k)
   }
   decode_keys(bytes)
}
