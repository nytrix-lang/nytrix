;; Keywords: cli tui
;; Cli Tui module.

module std.str.term (
    bold, italic, dim, underline, color, style, panel, table, tree, bar, bar_update,
    bar_finish, bar_range, bar_write, get_terminal_size,
    clear_screen, cursor_hide, cursor_show, cursor_move, cursor_up, cursor_down,
    cursor_left, cursor_right, enable_wrap, disable_wrap, screen_reset,
    color_names, get_color_name, get_color, shapes,
    canvas, canvas_clear, canvas_set, canvas_print, canvas_box, canvas_refresh,
    get_key, poll_key, set_raw_mode, set_cooked_mode, write_str
)
use std.str *
use std.core.reflect *
use std.core *
use std.core as core
use std.os.time *
use std.math.float *
use std.core *
use std.core *
use std.os.sys *

fn write_str(s){
   "Safely write a Nytrix string to stdout (bypassing metadata)."
   if(s){ unwrap(sys_write(1, s, str_len(s))) }
}

;; TUI Control

fn clear_screen(){
   "Clears the entire screen and moves cursor to home."
   unwrap(sys_write(1, "\033[2J", 4))
   unwrap(sys_write(1, "\033[H", 3))
}

fn cursor_hide(){
   "Hides the terminal cursor."
   unwrap(sys_write(1, "\033[?25l", 6))
}

fn cursor_show(){
   "Shows the terminal cursor."
   unwrap(sys_write(1, "\033[?25h", 6))
}

fn cursor_move(x, y){
   "Moves the cursor to position (x, y). 1-based indexing."
   def s = f"\033[{to_str(y)};{to_str(x)}H"
   unwrap(sys_write(1, s, str_len(s)))
}

fn cursor_up(n=1){
   "Moves cursor up by `n` lines."
   if(is_int(n) == 0){ n = 1 }
   def s = f"\033[{to_str(n)}A"
   unwrap(sys_write(1, s, str_len(s)))
}

fn cursor_down(n=1){
   "Moves cursor down by `n` lines."
   if(is_int(n) == 0){ n = 1 }
   def s = f"\033[{to_str(n)}B"
   unwrap(sys_write(1, s, str_len(s)))
}

fn cursor_right(n=1){
   "Moves cursor right by `n` columns."
   if(is_int(n) == 0){ n = 1 }
   def s = f"\033[{to_str(n)}C"
   unwrap(sys_write(1, s, str_len(s)))
}

fn cursor_left(n=1){
   "Moves cursor left by `n` columns."
   if(is_int(n) == 0){ n = 1 }
   def s = f"\033[{to_str(n)}D"
   unwrap(sys_write(1, s, str_len(s)))
}

fn disable_wrap(){
   "Disables line wrapping."
   unwrap(sys_write(1, "\033[?7l", 5))
}

fn enable_wrap(){
   "Enables line wrapping."
   unwrap(sys_write(1, "\033[?7h", 5))
}

fn screen_reset(){
   "Resets the terminal state: visible cursor, enabled wrap, cleared screen, reset styles."
   set_cooked_mode()
   unwrap(sys_write(1, "\033[0m", 4))
   cursor_show()
   enable_wrap()
   clear_screen()
}

;; Raw Mode Input

fn set_raw_mode(){
   "Enables raw mode for the terminal (no echo, no buffering)."
   use std.os.process *
   run("/bin/stty", ["raw", "-echo"])
}

fn set_cooked_mode(){
   "Restores terminal to normal mode."
   use std.os.process *
   run("/bin/stty", ["-raw", "echo"])
}

fn get_key(){
   "Reads a single key from stdin in raw mode. Blocks until key is pressed."
   def b = bytes(1)
   def n = sys_read(0, b, 1)
   if(n <= 0){ return 0 }
   bytes_get(b, 0)
}

fn poll_key(){
   "Polls for a key without blocking. Returns 0 if no key."
   0
}

; ANSI Styling

fn bold(s){
   "Wraps string `s` with ANSI bold escape codes."
   f"\033[1m{s}\033[0m"
}

fn italic(s){
   "Wraps string `s` with ANSI italic escape codes."
   f"\033[3m{s}\033[0m"
}

fn dim(s){
   "Wraps string `s` with ANSI dim/faint escape codes."
   f"\033[2m{s}\033[0m"
}

fn underline(s){
   "Wraps string `s` with ANSI underline escape codes."
   f"\033[4m{s}\033[0m"
}

fn color(s, c){
   "Wraps string `s` with ANSI foreground color escape codes for color `c`."
   def code = case c {
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
   f"\033[{code}m{s}\033[0m"
}

fn style(text, color_name="", is_bold=0){
   "Applies ANSI styling to text."
   if(is_str(color_name) == 0){ color_name = "" }
   if(is_int(is_bold) == 0){ is_bold = 0 }
   mut out = text
   if(is_bold){ out = f"\033[1m{out}" }
   if(str_len(color_name) > 0){
      out = color(out, color_name)
   }
   if(is_bold || str_len(color_name) > 0){ out = f"{out}\033[0m" }
   return out
}

fn color_names(){
   "Returns a list of supported color names."
   def c = ["black", "red", "green", "yellow", "blue", "magenta", "cyan", "white", "gray"]
   c
}

fn get_color_name(idx){
   "Returns the color name at index `idx` (cycling)."
   def c = color_names()
   def n = core.len(c)
   get(c, idx % n)
}

fn get_color(text, idx){
   "Returns `text` wrapped in the color at index `idx`."
   color(text, get_color_name(idx))
}

fn shapes(){
   "Returns a dictionary of common TUI shapes/symbols."
   def s = dict(64)
   dict_set(s, "v_line", "\xe2\x94\x82")   ;; │
   dict_set(s, "h_line", "\xe2\x94\x80")   ;; ─
   dict_set(s, "top_left", "\xe2\x95\xad") ;; ╭
   dict_set(s, "top_right", "\xe2\x95\xae");; ╮
   dict_set(s, "bot_left", "\xe2\x95\xb0") ;; ╰
   dict_set(s, "bot_right", "\xe2\x95\xaf");; ╯
   dict_set(s, "cross", "\xe2\x94\xbc")    ;; ┼
   dict_set(s, "t_down", "\xe2\x94\xac")   ;; ┬
   dict_set(s, "t_up", "\xe2\x94\xb4")     ;; ┴
   dict_set(s, "t_left", "\xe2\x94\xa4")   ;; ┤
   dict_set(s, "t_right", "\xe2\x94\x9c")  ;; ├
   dict_set(s, "shade", "\xe2\x96\x92")    ;; ▒
   dict_set(s, "dot", "\xc2\xb7")          ;; ·
   dict_set(s, "block", "\xe2\x96\x88")    ;; █
   s
}

; Components

fn panel(text, title="", border_color="white"){
   "Prints a styled panel with optional title."
   def l = str_len(text)
   mut w = l + 4
   if(str_len(title) > 0){
      def tl = str_len(title)
      if(tl + 4 > w){ w = tl + 4 }
   }
   ; Top
   mut top = "╭"
   mut i = 0
   while(i < w - 2){
      top = f"{top}─"
      i = i + 1
   }
   top = f"{top}╮"
   top = color(top, border_color)
   if(str_len(title) > 0){
      top = color("╭─ ", border_color)
      def title_col = color(title, "cyan")
      def mid_col = color(" ─", border_color)
      def bar_col = color("─", border_color)
      def cap_col = color("╮", border_color)
      top = f"{top}{title_col}{mid_col}"
      def rem = w - 2 - 2 - str_len(title) - 2
      i = 0
      while(i < rem){
         top = f"{top}{bar_col}"
         i = i + 1
      }
      top = f"{top}{cap_col}"
   }
   print(top)
   ; Content
   def padding = w - 4 - str_len(text)
   def cbar = color("│", border_color)
   mut line = f"{cbar} {text} "
   i = 0
   while(i < padding){ line = f"{line} " i = i + 1 }
   line = f"{line}{cbar}"
   print(line)
   ; Bottom
   mut bot = "╰"
   i = 0
   while(i < w - 2){ bot = f"{bot}─" i = i + 1 }
   bot = f"{bot}╯"
   print(color(bot, border_color))
}

fn table(headers, rows){
   "Prints a simple table."
   def cols = core.len(headers)
   mut widths = list(8)
   mut i = 0
   while(i < cols){
      widths = append(widths, str_len(get(headers, i)))
      i = i + 1
   }
   mut r = 0 def nr = core.len(rows)
   while(r < nr){
      def row = get(rows, r)
      mut c = 0
      while(c < cols){
         def val = get(row, c)
         if(str_len(val) > get(widths, c)){ set_idx(widths, c, str_len(val)) }
         c = c + 1
      }
      r = r + 1
   }
   mut line = ""
   i = 0
   while(i < cols){
      def h = get(headers, i)
      def w = get(widths, i)
      line = f"{line}{bold(h)}"
      def pad = w - str_len(h) + 2
      mut p = 0
      while(p < pad){ line = f"{line} " p = p + 1 }
      line = f"{line} "
      i = i + 1
   }
   print(line)
   mut sep = "" def slen = str_len(line) ; Approximation
   i = 0 while(i < (slen / 2)){ sep = f"{sep}─" i = i + 1 }
   print(color(sep, "gray"))
   r = 0
   while(r < nr){
      def row = get(rows, r)
      mut line_row = "" mut c = 0
      while(c < cols){
         def val = get(row, c)
         def w = get(widths, c)
         line_row = f"{line_row}{val}"
         def pad = w - str_len(val) + 2
         mut p = 0 while(p < pad){ line_row = f"{line_row} " p = p + 1 }
         line_row = f"{line_row} "
         c = c + 1
      }
      print(line_row)
      r = r + 1
   }
}

fn tree(node, pref="", head_in=""){
   "Prints a tree structure. Node is [label, [children...]] or just label string."
   def prefix = case type(pref) { "str" -> pref _ -> "" }
   def head = case type(head_in) { "str" -> head_in _ -> "" }
   if(is_str(node)){ print(f"{prefix}{head}{node}") return 0 }
   def label = get(node, 0)
   print(f"{prefix}{head}{bold(label)}")
   def children = get(node, 1)
   def count = core.len(children)
   mut i = 0
   while(i < count){
      def last = (i == count - 1)
      def child = get(children, i)
      mut next_prefix = f"{prefix}│   "
      mut next_head = "├── "
      if(last){ next_prefix = f"{prefix}    " next_head = "╰── " }
      tree(child, next_prefix, next_head)
      i = i + 1
   }
}

fn bar(tot=100, d="Progress", w=40, bc="green", se=1, lv=1){
   "Create a progress bar. Returns a bar object (list)."
   def total = case is_int(tot) { 1 -> tot _ -> 100 }
   def desc = case is_str(d) { 1 -> d _ -> "Progress" }
   def width = case is_int(w) { 1 -> w _ -> 40 }
   def bar_color = case is_str(bc) { 1 -> bc _ -> "green" }
   def show_eta = case is_int(se) { 1 -> se _ -> 1 }
   def leave = case is_int(lv) { 1 -> lv _ -> 1 }
   mut bar = list(12)
   bar = append(bar, total)         ; 0
   bar = append(bar, 0)             ; 1
   bar = append(bar, desc)          ; 2
   bar = append(bar, width)         ; 3
   bar = append(bar, bar_color)     ; 4
   bar = append(bar, show_eta)      ; 5
   bar = append(bar, leave)         ; 6
   def start_time = ticks() / 1000000
   bar = append(bar, start_time)    ; 7
   bar = append(bar, start_time)    ; 8
   bar = append(bar, 0)             ; 9
   bar = append(bar, 0)             ; 10
   bar = append(bar, 0.0)           ; 11
   return bar
}

fn bar_update(bar, current){
   "Updates the progress bar to the `current` value and renders it to stdout."
   if(get(bar, 9) == 1){ return 0 }
   if(current <= get(bar, 10) && current != 0 && current < get(bar, 0)){ return 0 }
   set_idx(bar, 1, current) set_idx(bar, 10, current)
   def total = get(bar, 0) def desc = get(bar, 2) def width = get(bar, 3)
   def bar_color = get(bar, 4) def show_eta = get(bar, 5)
   def start_time = get(bar, 7) def last_time = get(bar, 8)
   def now = ticks() / 1000000 def dt = now - last_time
   mut avg_rate = get(bar, 11)
   if(dt > 150 || current == total){
      def elapsed = (now - start_time) / 1000.0
      if(elapsed > 0.05){
         def inst = current / elapsed
         avg_rate = case avg_rate == 0.0 { 1 -> inst _ -> (avg_rate * 0.8) + (inst * 0.2) }
         set_idx(bar, 11, avg_rate) set_idx(bar, 8, now)
      }
   }
   def den = case total == 0 { 1 -> 1 _ -> total }
   def pct = (current * 100) / den
   def filled = (current * width) / den
   def b_len = case filled > width { 1 -> width _ -> filled }
   def e_len = case width - filled < 0 { 1 -> 0 _ -> width - filled }
   def b_str = color(repeat("█", b_len), bar_color)
   def e_str = color(repeat("░", e_len), "gray")
   def out = f"\r\033[K{desc}: {to_str(pct)}%|{b_str}{e_str}| {to_str(current)}/{to_str(total)}"
   unwrap(sys_write(1, out, str_len(out)))
   if(current >= total){ set_idx(bar, 9, 1) }
   return 0
}

fn bar_finish(bar){
   "Completes the progress bar, ensuring it reaches 100% and cleanup/newline as needed."
   if(get(bar, 1) < get(bar, 0)){ bar_update(bar, get(bar, 0)) }
   if(get(bar, 6)){ print("") } else { unwrap(sys_write(1, "\r\033[K", 4)) }
   set_idx(bar, 9, 1)
}

fn bar_range(n, desc=""){
   "Compatibility wrapper for `bar(n, desc)`. Returns a new bar object."
   return bar(n, desc)
}
fn bar_write(bar_obj, msg){
   "Clears the current progress bar line, prints `msg`, and redraws the bar."
   unwrap(sys_write(1, "\r\033[K", 4)) print(msg) bar_update(bar_obj, get(bar_obj, 1))
}

mut _term_buf = 0
mut _chr_cache = 0

fn _chr_cached(code){
   "Internal: cached ASCII byte -> single-character string."
   if(code < 0 || code > 255){ return "" }
   if(!_chr_cache){
      _chr_cache = list(256)
      mut i = 0
      while(i < 256){
         _chr_cache = append(_chr_cache, chr(i))
         i = i + 1
      }
   }
   get(_chr_cache, code, "")
}

fn get_terminal_size(){
   "Retrieves terminal [width, height] using ioctl, environment variables, or defaults."
   if(!_term_buf){ _term_buf = malloc(8) }
   def buf = _term_buf
   ;; Try ioctl on stdout (1)
   mut r = syscall(16, 1, 0x5413, buf, 0, 0, 0)
   ;; If stdout fails, try /dev/tty
   if(r != 0){
      use std.str.io *
      def fd = sys_open("/dev/tty", 0, 0)
      if(fd > 0){
         r = syscall(16, fd, 0x5413, buf, 0, 0, 0)
         unwrap(sys_close(fd))
      }
   }
   if(r == 0){
      def rows = load8(buf, 0) | (load8(buf, 1) << 8)
      def cols = load8(buf, 2) | (load8(buf, 3) << 8)
      if(rows > 0 && cols > 0){
         return [cols, rows]
      }
   }
   ;; Fallback to Environment Variables
   use std.os *
   def env_c = env("COLUMNS")
   def env_l = env("LINES")
   if(is_str(env_c) && is_str(env_l)){
      mut ic = 0 mut il = 0
      use std.core.reflect *
      ic = int(env_c) il = int(env_l)
      if(ic > 0 && il > 0){ return [ic, il] }
   }
   return [80, 24] ; Final fallback
}

;; Canvas / Windowing System (ncurses-like)

fn canvas(w, h){
   "Creates a new terminal canvas for buffered drawing."
   mut c = list(10)
   c = append(c, w) ; 0: width
   c = append(c, h) ; 1: height
   ;; 2: char buffer (list of strings, one per cell, UTF-8 safe)
   mut char_buf = list(w * h)
   mut i = 0
   while(i < w * h){
      char_buf = append(char_buf, " ")
      i = i + 1
   }
   c = append(c, char_buf)
   c = append(c, bytes(w * h)) ; 3: attr buffer (0=norm, 1=bold)
   c = append(c, bytes(w * h)) ; 4: color buffer (color index 0-8)
   c = append(c, bytes(w * h)) ; 5: byte-length buffer
   canvas_clear(c)
   return c
}

fn canvas_clear(canv){
   "Clears all buffers (characters, attributes, and colors) in the canvas."
   def w = get(canv, 0) def h = get(canv, 1)
   def buf = get(canv, 2) def attr = get(canv, 3) def col = get(canv, 4)
   def blen = get(canv, 5)
   mut i = 0 def n = w * h
   while(i < n){
      set_idx(buf, i, " ")
      bytes_set(attr, i, 0)
      bytes_set(col, i, 0)
      bytes_set(blen, i, 1)
      i = i + 1
   }
}

fn _byte_len(s){
   "Internal: returns UTF-8 byte length (robust to char-count headers)."
   if(!is_str(s)){ return 0 }
   def n = str_len(s)
   if(n == 0){ return 0 }
   if(load8(s, n) == 0){ return n }
   mut i = n
   while(load8(s, i) != 0){ i = i + 1 }
   i
}

fn _substr_bytes(s, start, len){
   "Internal: substring helper with byte indices (UTF-8 safe if boundaries are valid)."
   if(len <= 0){ return "" }
   mut out = malloc(len + 1)
   if(!out){ return "" }
   init_str(out, len)
   mut i = 0
   while(i < len){
      store8(out, load8(s, start + i), i)
      i = i + 1
   }
   store8(out, 0, len)
   out
}

fn canvas_set(canv, x, y, char, color_idx=0, is_bold=0){
   "Sets a character and its attributes at (x, y) on the canvas."
   def w = get(canv, 0) def h = get(canv, 1)
   if(x < 0 || x >= w || y < 0 || y >= h){ return 0 }
   def idx = y * w + x
   mut char_str = ""
   if(is_str(char)){
      char_str = char
   } else {
      char_str = _chr_cached(char)
   }
   if(str_len(char_str) == 0){ char_str = "?" }
   set_idx(get(canv, 2), idx, char_str)
   bytes_set(get(canv, 3), idx, is_bold)
   bytes_set(get(canv, 4), idx, color_idx)
   bytes_set(get(canv, 5), idx, _byte_len(char_str))
}

fn canvas_print(canv, x, y, text, color_idx=0, is_bold=0){
   "Prints a string horizontally on the canvas starting at (x, y)."
   mut i = 0
   mut col = 0
   def l = str_len(text)
   while(i < l){
      def b0 = load8(text, i)
      mut clen = 1
      if(b0 >= 240){ clen = 4 }
      elif(b0 >= 224){ clen = 3 }
      elif(b0 >= 192){ clen = 2 }
      def ch = _substr_bytes(text, i, clen)
      canvas_set(canv, x + col, y, ch, color_idx, is_bold)
      i = i + clen
      col = col + 1
   }
}

fn canvas_box(canv, x, y, w, h, title="", color_idx=0){
   "Draws a styled box with an optional title on the canvas."
   def s = shapes()
   mut i = 0
   while(i < w){
      canvas_set(canv, x + i, y, dict_get(s, "h_line"), color_idx)
      canvas_set(canv, x + i, y + h - 1, dict_get(s, "h_line"), color_idx)
      i = i + 1
   }
   i = 0
   while(i < h){
      canvas_set(canv, x, y + i, dict_get(s, "v_line"), color_idx)
      canvas_set(canv, x + w - 1, y + i, dict_get(s, "v_line"), color_idx)
      i = i + 1
   }
   canvas_set(canv, x, y, dict_get(s, "top_left"), color_idx)
   canvas_set(canv, x + w - 1, y, dict_get(s, "top_right"), color_idx)
   canvas_set(canv, x, y + h - 1, dict_get(s, "bot_left"), color_idx)
   canvas_set(canv, x + w - 1, y + h - 1, dict_get(s, "bot_right"), color_idx)
   if(str_len(title) > 0){
      canvas_print(canv, x + 2, y, f" {title} ", color_idx, 1)
   }
}

fn canvas_refresh(canv){
   "Renders the entire canvas buffer to the physical terminal using an optimized single-write approach."
   def w = get(canv, 0)
   def h = get(canv, 1)
   def buf = get(canv, 2)
   def attr = get(canv, 3)
   def col = get(canv, 4)
   def blen = get(canv, 5)
   def r_buf = bytes(w * h * 64 + 1024)
   mut p = 0
   ;; Home command: \033[H (27 91 72)
   bytes_set(r_buf, p, 27) bytes_set(r_buf, p+1, 91) bytes_set(r_buf, p+2, 72)
   p = p + 3
   mut last_c = -1
   mut last_b = -1
   mut y = 0
   while y < h {
      mut x = 0
      while x < w {
         def idx = y * w + x
         def cell = get(buf, idx, " ")
         def b = bytes_get(attr, idx)
         def c = bytes_get(col, idx)
         if c != last_c || b != last_b {
            ;; \033[ (27 91)
            bytes_set(r_buf, p, 27) bytes_set(r_buf, p+1, 91) p = p + 2
            if c == 0 && b == 0 {
               bytes_set(r_buf, p, 48) p = p + 1 ;; '0'
            } else {
               if b {
                  bytes_set(r_buf, p, 49) bytes_set(r_buf, p+1, 59) p = p + 2 ;; '1;'
               }
               def code = case c {
                  1 -> 49 2 -> 50 3 -> 51
                  4 -> 52 5 -> 53 6 -> 54
                  7 -> 55 8 -> 48 _ -> 55
               }
               bytes_set(r_buf, p, 51) ;; '3'
               if c == 8 { bytes_set(r_buf, p, 57) } ;; '9' for gray
               bytes_set(r_buf, p+1, code)
               p = p + 2
            }
            bytes_set(r_buf, p, 109) p = p + 1 ;; 'm'
            last_c = c last_b = b
         }
         def clen = bytes_get(blen, idx)
         mut j = 0
         while(j < clen){
            bytes_set(r_buf, p, load8(cell, j))
            p = p + 1
            j = j + 1
         }
         x = x + 1
      }
      if y < h - 1 {
         bytes_set(r_buf, p, 10) ;; '\n'
         p = p + 1
      }
      y = y + 1
   }
   ;; Reset color at end: \033[0m
   bytes_set(r_buf, p, 27) bytes_set(r_buf, p+1, 91) bytes_set(r_buf, p+2, 48) bytes_set(r_buf, p+3, 109)
   p = p + 4
   unwrap(sys_write(1, r_buf, p))
   free(r_buf)
}
