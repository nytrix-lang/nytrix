;; Keywords: ui vterm
;; Virtual Terminal Emulator for Nytrix

module std.ui.vterm (
   new, open, close, update, draw, handle_event,
   write, send_input, resize,
   is_running, get_title
)

use std.core *
use std.os *
use std.os.sys as sys
use std.os.thread *
use std.os.clipboard as clipboard
use std.str as str
use std.math *
use std.ui.consts *
use std.ui.gfx *
use std.ui.window as uiw
use std.ui.input *

mut _active_vt_hack = dict()
def PADDING_X = 12.0
def PADDING_Y = 12.0

;; Attribute Modes
def ATTR_BOLD      = 1
def ATTR_FAINT     = 2
def ATTR_ITALIC    = 4
def ATTR_UNDERLINE = 8
def ATTR_REVERSE   = 16
def ATTR_WIDE      = 32
def ATTR_WDUMMY    = 64

;; Terminal Modes
def MODE_WRAP        = 1
def MODE_ALTSCREEN   = 2
def MODE_UTF8        = 4

;; Escape States
def ESC_START      = 1
def ESC_CSI        = 2
def ESC_STR        = 4
def ESC_ALTCHARSET = 8

;; Base16 Tomorrow Night
def B16_00 = 0xff211f1d
def B16_05 = 0xffc5c8c6

;; State block offsets
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
def OFF_UTF8_BUF       = 128 ;; 16 bytes for safe decoding
def OFF_SEL_MOVED      = 144 ;; 1 byte: set when drag actually moves from start pos
def OFF_SB_DRAGGING    = 145 ;; 1 byte: drag scrollbar thumb
def OFF_SCROLL_ACC     = 148 ;; 4 bytes (f32): accumulated scroll for trackpads

fn new(cols, rows, fonts, bg_color=0, text_color=0){
   mut vt = dict()
   vt = dict_set(vt, "cols", cols)
   vt = dict_set(vt, "rows", rows)
   vt = dict_set(vt, "fonts", fonts)
   
   def dbg = (bg_color != 0) ? bg_color : B16_00
   def dfg = (text_color != 0) ? text_color : B16_05
   vt = dict_set(vt, "def_bg", dbg)
   vt = dict_set(vt, "def_fg", dfg)
   
   mut cache = dict(128)
   mut ci = 0 while(ci < 128){ cache = dict_set(cache, ci, str.chr(ci)) ci += 1 }
   vt = dict_set(vt, "ascii_cache", cache)
   
   def bytes = cols * rows * 16
   def grid = malloc(bytes)
   _clear_grid(grid, cols, rows, dfg, dbg)
   vt = dict_set(vt, "grid", grid)
   vt = dict_set(vt, "alt_grid", 0)
   
   def palette = malloc(256 * 4)
   _init_palette(palette)
   vt = dict_set(vt, "palette", palette)
   
   def st = malloc(256)
   memset(st, 0, 256)
   store32(st, 0, OFF_CX) store32(st, 0, OFF_CY)
   store32(st, dfg, OFF_CUR_FG) store32(st, dbg, OFF_CUR_BG)
   store32(st, 0, OFF_CUR_MODE) store32(st, 0, OFF_TOP) store32(st, rows - 1, OFF_BOT)
   store32(st, 0, OFF_ESC_STATE) store32(st, MODE_WRAP | MODE_UTF8, OFF_MODE)
   store8(st, 1, OFF_CURSOR_VISIBLE) store8(st, 0, OFF_APPKEYS)
   vt = dict_set(vt, "state", st)
   vt = dict_set(vt, "history", [])
   vt = dict_set(vt, "max_history", 5000)
   vt = dict_set(vt, "esc_buf", "")
   vt = dict_set(vt, "title", "Terminal")
   
   ;; Cache character metrics based on first font load
   def fr = dict_get(fonts, "regular")
   def f_obj = _font_get(fr)
   def f_size = float(dict_get(f_obj, "size", 16))
   def ch = floor(f_size * 1.25)
   def cs = measure_text(fr, "A") def cw = get(cs, 0)
   vt = dict_set(vt, "char_w", float(cw))
   vt = dict_set(vt, "char_h", float(ch))

   def sh = malloc(128)
   memset(sh, 0, 128)
   store64(sh, mutex_new(), 0)
   store64(sh, malloc(262144), 8)
   store64(sh, 0, 16)
   store64(sh, 262144, 24)
   store64(sh, 0, 32)
   store64(sh, -1, 40)
   vt = dict_set(vt, "shared", sh)
   
   vt
}

fn _init_palette(p){
   store32(p, 0xff000000, 0 * 4) store32(p, 0xffcc6666, 1 * 4)
   store32(p, 0xffb5bd8d, 2 * 4) store32(p, 0xfff0c674, 3 * 4)
   store32(p, 0xff81a2be, 4 * 4) store32(p, 0xffb294bb, 5 * 4)
   store32(p, 0xff8abeb7, 6 * 4) store32(p, 0xffc5c8c6, 7 * 4)
   store32(p, 0xff969892, 8 * 4) store32(p, 0xffcc6666, 9 * 4)
   store32(p, 0xffb5bd8d, 10 * 4) store32(p, 0xfff0c674, 11 * 4)
   store32(p, 0xff81a2be, 12 * 4) store32(p, 0xffb294bb, 13 * 4)
   store32(p, 0xff8abeb7, 14 * 4) store32(p, 0xffffffff, 15 * 4)
   mut pi = 16 while(pi < 232){
      def r = (pi - 16) / 36 def g = ((pi - 16) % 36) / 6 def b = (pi - 16) % 6
      store32(p, color_pack(float(r)/5.0, float(g)/5.0, float(b)/5.0, 1.0), pi * 4) pi += 1
   }
   pi = 232 while(pi < 256){
      def v = float(pi - 232) / 24.0
      store32(p, color_pack(v, v, v, 1.0), pi * 4) pi += 1
   }
}

fn _clear_grid(ptr, cols, rows, fg, bg){
   mut i = 0 def n = cols * rows
   while(i < n){
      def off = i * 16 store32(ptr, 32, off) store32(ptr, fg, off + 4)
      store32(ptr, bg, off + 8) store32(ptr, 0, off + 12) i += 1
   }
}

fn open(vt, cmd="/bin/sh", args=[]){
   mut fds = malloc(8)
   def res = sys.sys_openpty(fds)
   if(is_err(res)){ free(fds) return res }
   def m = load32(fds, 0) def s = load32(fds, 4) free(fds)
   _resize_pty(m, dict_get(vt, "cols"), dict_get(vt, "rows"))
   def pid = __fork()
   if(pid == 0){
      __close(m) __setsid() __dup2(s, 0) __dup2(s, 1) __dup2(s, 2)
      mut _ = __ioctl(s, 0x540E, 0)
      ;; Set IUTF8 termios flag (0x004000) for correct kernel-level UTF-8 handling
      mut tm = malloc(64) if(__ioctl(s, 0x5401, tm) == 0){
         def iflag = load32(tm, 0) store32(tm, iflag | 0x4000, 0) _ = __ioctl(s, 0x5402, tm)
      } free(tm)
      if(s > 2){ __close(s) }
      
      mut shell_name = cmd
      def last_slash = str.find_last(cmd, "/")
      if(last_slash != -1){ shell_name = str.str_slice(cmd, last_slash + 1, str.len(cmd)) }
      mut av = ["-" + shell_name] ;; Proper LOGIN shell indicator
      mut j = 0 while(j < len(args)){ av = append(av, get(args, j)) j += 1 }
      
      def el = environ()
      mut ne = [] mut ft = false
      mut i = 0 while(i < len(el)){
         def e = get(el, i)
         if(str.startswith(e, "TERM=")){ ne = append(ne, "TERM=xterm-256color") ft = true }
         elif(str.startswith(e, "COLORTERM=")){ ne = append(ne, e) }
         elif(str.startswith(e, "LANG=")){ ne = append(ne, "LANG=en_US.UTF-8") }
         else { ne = append(ne, e) }
         i += 1
      }
      if(!ft){ ne = append(ne, "TERM=xterm-256color") }
      ne = append(ne, "COLORTERM=truecolor")
      ne = append(ne, "LANG=en_US.UTF-8")
      ne = append(ne, "LC_ALL=en_US.UTF-8")
      ne = append(ne, "LC_CTYPE=en_US.UTF-8")
      __execve(cmd, av, ne) __exit(1)
   }
   __close(s)
   mut nvt = dict_set(vt, "master_fd", m) nvt = dict_set(nvt, "pid", pid)
   def sh = dict_get(nvt, "shared") store64(sh, 1, 32) store64(sh, m, 40)
   thread_spawn(fn(){
      def lk = load64(sh, 0) def fd = load64(sh, 40)
      mut t = malloc(32768)
      while(load64(sh, 32) != 0){
         def r = __sys_read_off(fd, t, 32768, 0)
         if(r == 0){ store64(sh, 0, 32) break }
         elif(r < 0){ if(r == -11 || r == -4){ msleep(1) continue } store64(sh, 0, 32) break }
         mutex_lock(lk)
         mut sz = load64(sh, 16)
         mut cap = load64(sh, 24)
         mut buf = load64(sh, 8)
         ;; Grow buffer if needed
         if(sz + r > cap){
            def new_cap = max(cap * 2, sz + r + 65536)
            def new_buf = malloc(new_cap)
            if(sz > 0){ memcpy(new_buf, buf, sz) }
            free(buf)
            store64(sh, new_buf, 8)
            store64(sh, new_cap, 24)
            buf = new_buf cap = new_cap
         }
         memcpy(buf + sz, t, r)
         store64(sh, sz + r, 16)
         mutex_unlock(lk)
         msleep(1)
      }
      free(t)
   })
   ok(nvt)
}

fn close(vt){
   def sh = dict_get(vt, "shared") store64(sh, 0, 32)
   def m = dict_get(vt, "master_fd") if(m >= 0){ __close(m) }
}

fn is_running(vt){ def sh = dict_get(vt, "shared") load64(sh, 32) != 0 }
fn get_title(vt){ dict_get(vt, "title") }

fn update(vt){
   def sh = dict_get(vt, "shared") def lk = load64(sh, 0)
   mutex_lock(lk)
   def sz = load64(sh, 16)
   if(sz == 0){ mutex_unlock(lk) return vt }
   def bp = load64(sh, 8) 
   def st = dict_get(vt, "state")
   
   mut t_buf = malloc(sz)
   memcpy(t_buf, bp, sz)
   store64(sh, 0, 16)
   mutex_unlock(lk)
   
   mut u_len = load32(st, OFF_UTF8_LEN)
   mut p = 0 mut nvt = vt
   _active_vt_hack = dict_set(_active_vt_hack, 0, nvt)
   
   ;; Cache metrics for the run
   mut co = dict_get(nvt, "cols") mut ro = dict_get(nvt, "rows")
   mut g = dict_get(nvt, "grid") mut pal = dict_get(nvt, "palette")
   mut dfg = dict_get(nvt, "def_fg") mut dbg = dict_get(nvt, "def_bg")

   while(p < sz){
      def b = load8(t_buf, p) & 255
      if(u_len > 0){
         if((b & 0xC0) == 0x80){
            store8(st + OFF_UTF8_BUF, b, u_len)
            u_len += 1
            def b0 = load8(st + OFF_UTF8_BUF, 0) & 255
            mut exp = 0
            if((b0 & 0xE0) == 0xC0){ exp = 2 }
            elif((b0 & 0xF0) == 0xE0){ exp = 3 }
            elif((b0 & 0xF8) == 0xF0){ exp = 4 }
            
            if(exp == u_len){
               nvt = _tputc_fast(nvt, st, co, ro, g, pal, dfg, dbg, str._utf8_decode_at(st + OFF_UTF8_BUF, 0, u_len))
               u_len = 0
               ;; Refresh if changed (rare: screen switch)
               if(dict_get(nvt, "grid") != g){
                  co = dict_get(nvt, "cols") ro = dict_get(nvt, "rows")
                  g = dict_get(nvt, "grid") pal = dict_get(nvt, "palette")
                  dfg = dict_get(nvt, "def_fg") dbg = dict_get(nvt, "def_bg")
               }
            } elif(u_len >= 4 || exp == 0){
               nvt = _tputc_fast(nvt, st, co, ro, g, pal, dfg, dbg, 63)
               u_len = 0
            }
         } else {
            nvt = _tputc_fast(nvt, st, co, ro, g, pal, dfg, dbg, 63)
            u_len = 0 p -= 1
         }
      } else {
         if((b & 0x80) == 0){
            nvt = _tputc_fast(nvt, st, co, ro, g, pal, dfg, dbg, b)
            if(dict_get(nvt, "grid") != g){
               co = dict_get(nvt, "cols") ro = dict_get(nvt, "rows")
               g = dict_get(nvt, "grid") pal = dict_get(nvt, "palette")
               dfg = dict_get(nvt, "def_fg") dbg = dict_get(nvt, "def_bg")
            }
         } elif((b & 0xE0) == 0xC0 || (b & 0xF0) == 0xE0 || (b & 0xF8) == 0xF0){
            store8(st + OFF_UTF8_BUF, b, 0)
            u_len = 1
         } else {
            nvt = _tputc_fast(nvt, st, co, ro, g, pal, dfg, dbg, 63)
         }
      }
      p += 1
   }
   store32(st, u_len, OFF_UTF8_LEN)
   free(t_buf)
   nvt
}

fn _wcwidth(u){
   ;; http://www.cl.cam.ac.uk/~mgk25/ucs/wcwidth.c
   ;; Return -1 for null and C0/C1 control characters
   if (u < 32 || (u >= 0x7f && u < 0xa0)) { return -1 }
   ;; Combining characters (zero-width)
   if ((u >= 0x0300 && u <= 0x036F) || (u >= 0x0483 && u <= 0x0489) ||
       (u >= 0x0591 && u <= 0x05BD) || u == 0x05BF || (u >= 0x05C1 && u <= 0x05C2) ||
       u == 0x05C4 || (u >= 0x064B && u <= 0x0655) || u == 0x0670 ||
       (u >= 0x06D6 && u <= 0x06DC) || (u >= 0x06DD && u <= 0x06DF) ||
       (u >= 0x06E0 && u <= 0x06E4) || (u >= 0x06E7 && u <= 0x06E8) ||
       (u >= 0x06EA && u <= 0x06ED) || u == 0x0711 || (u >= 0x0730 && u <= 0x074A) ||
       (u >= 0x07A6 && u <= 0x07B0) || (u >= 0x07EB && u <= 0x07F3) ||
       (u >= 0x0901 && u <= 0x0902) || u == 0x093C || (u >= 0x0941 && u <= 0x0948) ||
       u == 0x094D || (u >= 0x0951 && u <= 0x0954) || (u >= 0x0962 && u <= 0x0963) ||
       u == 0x0981 || u == 0x09BC || (u >= 0x09C1 && u <= 0x09C4)) { return 0 }
   if (u == 0x09CD || (u >= 0x09E2 && u <= 0x09E3) || u == 0x0A02 || u == 0x0A3C ||
       (u >= 0x0A41 && u <= 0x0A42) || (u >= 0x0A47 && u <= 0x0A48) ||
       (u >= 0x0A4B && u <= 0x0A4D) || (u >= 0x0A70 && u <= 0x0A71) ||
       (u >= 0x0A81 && u <= 0x0A82) || u == 0x0ABC || (u >= 0x0AC1 && u <= 0x0AC5) ||
       (u >= 0x0AC7 && u <= 0x0AC8) || u == 0x0ACD || (u >= 0x0AE2 && u <= 0x0AE3) ||
       u == 0x0B01 || u == 0x0B3C || u == 0x0B3F || (u >= 0x0B41 && u <= 0x0B43) ||
       u == 0x0B4D || u == 0x0B56 || u == 0x0B82 || u == 0x0BC0 || u == 0x0BCD ||
       (u >= 0x0C3E && u <= 0x0C40) || (u >= 0x0C46 && u <= 0x0C48) ||
       (u >= 0x0C4A && u <= 0x0C4D) || (u >= 0x0C55 && u <= 0x0C56)) { return 0 }
   if (u == 0x0CBF || u == 0x0CC6 || (u >= 0x0CCA && u <= 0x0CCB) ||
       (u >= 0x0CD5 && u <= 0x0CD6) || (u >= 0x0D41 && u <= 0x0D43) ||
       u == 0x0D4D || u == 0x0DCA || (u >= 0x0DD2 && u <= 0x0DD4) || u == 0x0DD6 ||
       u == 0x0E31 || (u >= 0x0E34 && u <= 0x0E3A) || (u >= 0x0E47 && u <= 0x0E4E) ||
       u == 0x0EB1 || (u >= 0x0EB4 && u <= 0x0EB9) || (u >= 0x0EBB && u <= 0x0EBC) ||
       (u >= 0x0EC8 && u <= 0x0ECD) || (u >= 0x0F18 && u <= 0x0F19) ||
       u == 0x0F35 || u == 0x0F37 || u == 0x0F39 || (u >= 0x0F71 && u <= 0x0F7E) ||
       (u >= 0x0F80 && u <= 0x0F84) || (u >= 0x0F86 && u <= 0x0F87) ||
       (u >= 0x0F90 && u <= 0x0F97) || (u >= 0x0F99 && u <= 0x0FBC)) { return 0 }
   if (u == 0x0FC6 || (u >= 0x102D && u <= 0x1030) || u == 0x1032 ||
       (u >= 0x1036 && u <= 0x1037) || u == 0x1039 || (u >= 0x1058 && u <= 0x1059) ||
       (u >= 0x1160 && u <= 0x11FF) || (u >= 0x1712 && u <= 0x1714) ||
       (u >= 0x1732 && u <= 0x1734) || (u >= 0x1752 && u <= 0x1753) ||
       (u >= 0x1772 && u <= 0x1773) || (u >= 0x17B4 && u <= 0x17B5) ||
       (u >= 0x17B7 && u <= 0x17BD) || u == 0x17C6 || (u >= 0x17C9 && u <= 0x17D3) ||
       u == 0x17DD || (u >= 0x180B && u <= 0x180D) || u == 0x18A9 ||
       (u >= 0x1920 && u <= 0x1922) || (u >= 0x1927 && u <= 0x1928)) { return 0 }
   if (u == 0x1932 || (u >= 0x1939 && u <= 0x193B) || (u >= 0x1A17 && u <= 0x1A18) ||
       (u >= 0x1B00 && u <= 0x1B03) || u == 0x1B34 || (u >= 0x1B36 && u <= 0x1B3A) ||
       u == 0x1B3C || u == 0x1B42 || (u >= 0x1B6B && u <= 0x1B73) ||
       (u >= 0x1DC0 && u <= 0x1DCA) || (u >= 0x1DFE && u <= 0x1DFF) ||
       (u >= 0x200B && u <= 0x200F) || (u >= 0x202A && u <= 0x202E) ||
       (u >= 0x2060 && u <= 0x2063) || (u >= 0x206A && u <= 0x206F) ||
       (u >= 0x20D0 && u <= 0x20EF) || (u >= 0x302A && u <= 0x302F) ||
       (u >= 0x3099 && u <= 0x309A) || u == 0xFB1E || (u >= 0xFE00 && u <= 0xFE0F) ||
       (u >= 0xFE20 && u <= 0xFE23) || u == 0xFEFF || (u >= 0xFFF9 && u <= 0xFFFB)) { return 0 }
   if ((u >= 0x1D167 && u <= 0x1D169) || (u >= 0x1D173 && u <= 0x1D17A) ||
       (u >= 0x1D185 && u <= 0x1D18B) || (u >= 0x1D1AA && u <= 0x1D1AD) ||
       (u >= 0x1D242 && u <= 0x1D244) || u == 0xE0001 ||
       (u >= 0xE0020 && u <= 0xE007F) || (u >= 0xE0100 && u <= 0xE01EF)) {
     return 0;
   }
   ;; Regional Indicator Symbols (Flags). Treat each RI codepoint as 2 columns so that
   ;; a pair of them (forming one flag glyph) occupies exactly 4 columns total and the
   ;; ATTR_WIDE path correctly allocates a dummy cell for each one.
   if(u >= 0x1F1E6 && u <= 0x1F1FF){ return 2 }
   ;; Wide CJK blocks -- only the blocks that are truly double-width in terminals.
   ;; Hangul Jamo, CJK Unified Ideographs, Hangul Syllables, CJK Compatibility Ideographs
   ;; CJK Radicals, Kangxi, Bopomofo, Hiragana, Katakana, Enclosed CJK, CJK Compatibility
   if(u >= 0x1100 && u <= 0x115F){ return 2 }    ;; Hangul Jamo
   if(u == 0x2329 || u == 0x232A){ return 2 }    ;; Angle brackets
   if(u >= 0x2E80 && u <= 0x303E){ return 2 }    ;; CJK Radicals, Kangxi, Bopomofo
   if(u >= 0x3041 && u <= 0x33FF){ return 2 }    ;; Hiragana, Katakana, Bopomofo, Enclosed CJK
   if(u >= 0x3400 && u <= 0x4DBF){ return 2 }    ;; CJK Extension A
   if(u >= 0x4E00 && u <= 0x9FFF){ return 2 }    ;; CJK Unified Ideographs
   if(u >= 0xA000 && u <= 0xA4CF){ return 2 }    ;; Yi Syllables
   if(u >= 0xAC00 && u <= 0xD7A3){ return 2 }    ;; Hangul Syllables
   if(u >= 0xF900 && u <= 0xFAFF){ return 2 }    ;; CJK Compatibility Ideographs
   if(u >= 0xFE10 && u <= 0xFE19){ return 2 }    ;; Vertical forms
   if(u >= 0xFE30 && u <= 0xFE6F){ return 2 }    ;; CJK Compatibility Forms
   if(u >= 0xFF00 && u <= 0xFF60){ return 2 }    ;; Fullwidth Forms
   if(u >= 0xFFE0 && u <= 0xFFE6){ return 2 }    ;; Fullwidth Signs
   if(u >= 0x20000 && u <= 0x2FFFD){ return 2 }  ;; CJK Extension B-F
   if(u >= 0x30000 && u <= 0x3FFFD){ return 2 }  ;; CJK Extension G+
   ;; Nerd Font / Private Use Area -- single-width glyphs
   if(u >= 0xE000 && u <= 0xF8FF){ return 1 }
   ;; Box Drawing, Block Elements, Braille, Geometric Shapes -- all single-width
   if(u >= 0x2500 && u <= 0x28FF){ return 1 }
   ;; Modern Emojis and Pictographs (SMP)
   if(u >= 0x1F300 && u <= 0x1FAFF){ return 2 }
   ;; Everything else is single-width
   return 1
}

fn _tputc_fast(vt, st, co, ro, g, pal, dfg, dbg, u){
   mut s_state = load32(st, OFF_ESC_STATE)
   if(s_state == 0){
      if(u < 32 || u == 127){ return _tcontrolcode_fast(vt, st, co, ro, g, pal, dfg, dbg, u) }
      else { return _tputc_raw_fast(vt, st, co, ro, g, u) }
   } elif(s_state == ESC_START){
      if(u == 91){ store32(st, ESC_CSI, OFF_ESC_STATE) store8(st, 0, OFF_CSI_PRIV) return dict_set(vt, "esc_buf", "") }
      elif(u == 40 || u == 41 || u == 42 || u == 43){ store32(st, ESC_ALTCHARSET, OFF_ESC_STATE) return vt }
      elif(u == 80 || u == 93 || u == 94 || u == 95){ store32(st, ESC_STR, OFF_ESC_STATE) return dict_set(vt, "esc_buf", "") }
      elif(u == 99){ 
         _tresetattr_fast(st, dfg, dbg) _tclearregion_fast(st, co, ro, g, dfg, dbg, 0, 0, co - 1, ro - 1)
         store32(st, 0, OFF_CX) store32(st, 0, OFF_CY)
         store32(st, 0, OFF_TOP) store32(st, ro - 1, OFF_BOT)
         store32(st, MODE_WRAP | MODE_UTF8, OFF_MODE) store32(st, 0, OFF_ESC_STATE) return vt
      } elif(u == 68){ mut nvt = _tnewline_fast(vt, st, co, ro, g, dfg, dbg, false) store32(st, 0, OFF_ESC_STATE) return nvt }
      elif(u == 69){ mut nvt = _tnewline_fast(vt, st, co, ro, g, dfg, dbg, true) store32(st, 0, OFF_ESC_STATE) return nvt }
      elif(u == 77){
         mut cy = load32(st, OFF_CY)
         mut nvt = vt
         if(cy == load32(st, OFF_TOP)){ nvt = _tscrolldown_fast(vt, st, co, ro, g, dfg, dbg, load32(st, OFF_TOP), 1) }
         else { store32(st, cy - 1, OFF_CY) }
         store32(st, 0, OFF_ESC_STATE) return nvt
      } elif(u == 55){ 
         store32(st, load32(st, OFF_CX), OFF_SAVED_CX) store32(st, load32(st, OFF_CY), OFF_SAVED_CY)
         store32(st, load32(st, OFF_CUR_FG), OFF_SAVED_FG) store32(st, load32(st, OFF_CUR_BG), OFF_SAVED_BG)
         store32(st, load32(st, OFF_CUR_MODE), OFF_SAVED_MODE) store32(st, 0, OFF_ESC_STATE) return vt
      } elif(u == 56){ 
         store32(st, load32(st, OFF_SAVED_CX), OFF_CX) store32(st, load32(st, OFF_SAVED_CY), OFF_CY)
         store32(st, load32(st, OFF_SAVED_FG), OFF_CUR_FG) store32(st, load32(st, OFF_SAVED_BG), OFF_CUR_BG)
         store32(st, load32(st, OFF_SAVED_MODE), OFF_CUR_MODE) store32(st, 0, OFF_ESC_STATE) return vt
      }
      store32(st, 0, OFF_ESC_STATE) return vt
   } elif(s_state == ESC_CSI){
      if(u >= 64 && u <= 126){ mut nvt = _tcsihandle_fast(vt, st, co, ro, g, pal, dfg, dbg, u, dict_get(vt, "esc_buf")) store32(st, 0, OFF_ESC_STATE) return nvt }
      else { 
         if(u == 63 || u == 62){ store8(st, 1, OFF_CSI_PRIV) } 
         elif((u >= 48 && u <= 57) || u == 59){ return dict_set(vt, "esc_buf", dict_get(vt, "esc_buf") + str.chr(u)) }
         return vt
      }
   } elif(s_state == ESC_STR){
      if(u == 7 || u == 27 || u == 0x9c){ mut nvt = _tstrhandle(vt, dict_get(vt, "esc_buf")) store32(st, 0, OFF_ESC_STATE) return nvt }
      elif(u == 0x18 || u == 0x1a){ store32(st, 0, OFF_ESC_STATE) return vt }
      else { return dict_set(vt, "esc_buf", dict_get(vt, "esc_buf") + str.chr(u)) }
   }
   store32(st, 0, OFF_ESC_STATE) vt
}

fn _tcontrolcode_fast(vt, st, co, ro, g, pal, dfg, dbg, u){
   mut nvt = vt
   if(u == 7){ return vt } ;; BEL - ignore (no audio yet)
   elif(u == 8){ ;; BS
      mut cx = load32(st, OFF_CX) if(cx > 0){ store32(st, cx - 1, OFF_CX) }
   } elif(u == 9){ ;; HT tab
      mut cx = load32(st, OFF_CX) cx = (cx + 8) & ~7 if(cx >= co){ cx = co - 1 } store32(st, cx, OFF_CX)
   } elif(u == 10 || u == 11 || u == 12){ ;; LF VT FF
      nvt = _tnewline_fast(vt, st, co, ro, g, dfg, dbg, false)
   } elif(u == 13){ ;; CR
      store32(st, 0, OFF_CX)
   } elif(u == 14 || u == 15){ ;; SO/SI - charset switch, ignore for now
      return vt
   } elif(u == 24 || u == 26){ ;; CAN/SUB - cancel escape
      store32(st, 0, OFF_ESC_STATE)
   } elif(u == 27){ ;; ESC
      store32(st, ESC_START, OFF_ESC_STATE) return vt
   }
   nvt
}

fn _tnewline_fast(vt, st, co, ro, g, dfg, dbg, first_col){
   mut cy = load32(st, OFF_CY)
   mut nvt = vt
   if(cy == load32(st, OFF_BOT)){ nvt = _tscrollup_fast(vt, st, co, ro, g, dfg, dbg, load32(st, OFF_TOP), 1) cy = load32(st, OFF_CY) }
   else { store32(st, cy + 1, OFF_CY) cy += 1 }
   if(first_col){ store32(st, 0, OFF_CX) }
   nvt
}

fn _tmoveto_fast(st, co, ro, x, y){
   mut nx = x mut ny = y if(nx < 0){ nx = 0 } elif(nx >= co){ nx = co - 1 }
   if(ny < 0){ ny = 0 } elif(ny >= ro){ ny = ro - 1 }
   store32(st, nx, OFF_CX) store32(st, ny, OFF_CY)
}

fn _tputc_raw_fast(vt, st, co, ro, g, u){
   mut cx = load32(st, OFF_CX) mut cy = load32(st, OFF_CY)
   def w = _wcwidth(u)
   if(w == 0){ return vt }
   
   mut nvt = vt
   if(cx + w > co){
      if((load32(st, OFF_MODE) & MODE_WRAP) != 0){
         store32(st, 0, OFF_CX)
         if(cy == load32(st, OFF_BOT)){ nvt = _tscrollup_fast(vt, st, co, ro, g, load32(st, OFF_CUR_FG), load32(st, OFF_CUR_BG), load32(st, OFF_TOP), 1) }
         else { store32(st, cy + 1, OFF_CY) }
         cy = load32(st, OFF_CY) cx = 0
      } else { cx = co - w store32(st, cx, OFF_CX) }
   }
   mut md = load32(st, OFF_CUR_MODE)
   if(w == 2){ md = md | ATTR_WIDE }
   
   def off = (cy * co + cx) * 16
   store32(g, u, off)
   store32(g, load32(st, OFF_CUR_FG), off + 4)
   store32(g, load32(st, OFF_CUR_BG), off + 8)
   store32(g, md, off + 12)
   store32(st, u, OFF_LAST_CHAR_C) ;; track for REP (CSI b)

   if(w == 2 && cx + 1 < co){
      def off2 = off + 16 store32(g, 0, off2) store32(g, load32(st, OFF_CUR_FG), off2 + 4)
      store32(g, load32(st, OFF_CUR_BG), off2 + 8) store32(g, md | ATTR_WDUMMY, off2 + 12)
   }
   store32(st, cx + w, OFF_CX)
   nvt
}

fn _tscrollup_fast(vt, st, co, ro, g, dfg, dbg, orig, n){
   def bo = load32(st, OFF_BOT) mut nn = n if(nn > bo - orig + 1){ nn = bo - orig + 1 }
   if(nn <= 0){ return vt }
   
   ;; History push logic: Only when scrolling full screen areas on main grid
   if(orig == load32(st, OFF_TOP) && bo == ro - 1 && (load32(st, OFF_MODE) & MODE_ALTSCREEN) == 0){
      mut h = dict_get(vt, "history", [])
      def max_h = dict_get(vt, "max_history", 5000)
      mut i = 0 while(i < nn){
         def line_ptr = malloc(co * 16)
         memcpy(line_ptr, g + ((orig + i) * co * 16), co * 16)
         h = append(h, line_ptr)
         i += 1
      }
      if(len(h) > max_h){
         def discarded = slice(h, 0, len(h) - max_h)
         mut di = 0 while(di < len(discarded)){ free(get(discarded, di)) di += 1 }
         h = slice(h, len(h) - max_h, len(h))
      }
      vt = dict_set(vt, "history", h)
      if(load32(st, OFF_SCROLL) > 0){ store32(st, load32(st, OFF_SCROLL) + nn, OFF_SCROLL) }
   }

   def ds = g + (orig * co * 16) def sc = g + ((orig + nn) * co * 16)
   def by = (bo - orig - nn + 1) * co * 16 if(by > 0){ memcpy(ds, sc, by) }
   mut y = bo - nn + 1 while(y <= bo){ _tclearline_fast(st, co, g, dfg, dbg, y, 0, co - 1) y += 1 }
   vt
}

fn _tscrolldown_fast(vt, st, co, ro, g, dfg, dbg, orig, n){
   def bo = load32(st, OFF_BOT) mut nn = n if(nn > bo - orig + 1){ nn = bo - orig + 1 }
   if(nn > 0){
      def ds = g + ((orig + nn) * co * 16) def sc = g + (orig * co * 16)
      def by = (bo - orig - nn + 1) * co * 16
      if(by > 0){ mut tm = malloc(by) memcpy(tm, sc, by) memcpy(ds, tm, by) free(tm) }
      mut y = orig while(y < orig + nn){ _tclearline_fast(st, co, g, dfg, dbg, y, 0, co - 1) y += 1 }
   }
   vt
}

fn _tclearline_fast(st, co, g, dfg, dbg, y, x1, x2){
   def bg = load32(st, OFF_CUR_BG)
   mut x = x1 while(x <= x2){ def off = (y * co + x) * 16 store32(g, 32, off) store32(g, dfg, off+4) store32(g, bg, off+8) store32(g, 0, off+12) x += 1 }
}

fn _tclearregion_fast(st, co, ro, g, dfg, dbg, x1, y1, x2, y2){
   mut y = y1 while(y <= y2){ mut mx1 = (y == y1) ? x1 : 0 mut mx2 = (y == y2) ? x2 : co-1 _tclearline_fast(st, co, g, dfg, dbg, y, mx1, mx2) y += 1 }
}

fn _tcsihandle_fast(vt, st, co, ro, g, pal, dfg, dbg, u, buf){
   def pt = str.split(buf, ";") mut ag = [] mut i = 0 while(i < len(pt)){ def p = get(pt, i) ag = append(ag, (len(p)>0)?int(str.atof(p)):0) i += 1 }
   def pr = load8(st, OFF_CSI_PRIV) != 0 def a0 = (len(ag)>0)?get(ag,0):0 def a1 = (len(ag)>1)?get(ag,1):0
   if(u == 65){ _tmoveto_fast(st, co, ro, load32(st, OFF_CX), load32(st, OFF_CY) - ((a0 == 0) ? 1 : a0)) }
   elif(u == 66 || u == 101){ _tmoveto_fast(st, co, ro, load32(st, OFF_CX), load32(st, OFF_CY) + ((a0 == 0) ? 1 : a0)) }
   elif(u == 67 || u == 97){ _tmoveto_fast(st, co, ro, load32(st, OFF_CX) + ((a0 == 0) ? 1 : a0), load32(st, OFF_CY)) }
   elif(u == 68){ _tmoveto_fast(st, co, ro, load32(st, OFF_CX) - ((a0 == 0) ? 1 : a0), load32(st, OFF_CY)) }
   elif(u == 71){ _tmoveto_fast(st, co, ro, (a0 == 0) ? 0 : a0 - 1, load32(st, OFF_CY)) }
   elif(u == 100){ _tmoveto_fast(st, co, ro, load32(st, OFF_CX), (a0 == 0) ? 0 : a0 - 1) }
   elif(u == 115){ store32(st, load32(st, OFF_CX), OFF_SAVED_CX) store32(st, load32(st, OFF_CY), OFF_SAVED_CY) }
   elif(u == 117){ _tmoveto_fast(st, co, ro, load32(st, OFF_SAVED_CX), load32(st, OFF_SAVED_CY)) }
   elif(u == 88){ _tclearline_fast(st, co, g, dfg, dbg, load32(st, OFF_CY), load32(st, OFF_CX), load32(st, OFF_CX) + ((a0 == 0) ? 1 : a0) - 1) }
   elif(u == 83){ vt = _tscrollup_fast(vt, st, co, ro, g, dfg, dbg, load32(st, OFF_TOP), (a0 == 0) ? 1 : a0) }
   elif(u == 84){ vt = _tscrolldown_fast(vt, st, co, ro, g, dfg, dbg, load32(st, OFF_TOP), (a0 == 0) ? 1 : a0) }
   elif(u == 72 || u == 102){ _tmoveto_fast(st, co, ro, (a1 == 0) ? 0 : a1 - 1, (a0 == 0) ? 0 : a0 - 1) }
   elif(u == 74){
      if(a0 == 0){ _tclearregion_fast(st, co, ro, g, dfg, dbg, load32(st, OFF_CX), load32(st, OFF_CY), co-1, load32(st, OFF_CY))
         if(load32(st, OFF_CY) < ro-1){ _tclearregion_fast(st, co, ro, g, dfg, dbg, 0, load32(st, OFF_CY)+1, co-1, ro-1) }
      } elif(a0 == 1){ if(load32(st, OFF_CY) > 0){ _tclearregion_fast(st, co, ro, g, dfg, dbg, 0, 0, co-1, load32(st, OFF_CY)-1) }
         _tclearregion_fast(st, co, ro, g, dfg, dbg, 0, load32(st, OFF_CY), load32(st, OFF_CX), load32(st, OFF_CY))
      } elif(a0 == 2){ _tclearregion_fast(st, co, ro, g, dfg, dbg, 0, 0, co-1, ro-1) 
      } elif(a0 == 3){ 
         ;; ED 3: Clear scrollback / history
         mut h = dict_get(vt, "history", [])
         mut hi = 0 while(hi < len(h)){ free(get(h, hi)) hi += 1 }
         vt = dict_set(vt, "history", [])
         store32(st, 0, OFF_SCROLL)
      }
   } elif(u == 75){
      if(a0 == 0){ _tclearline_fast(st, co, g, dfg, dbg, load32(st, OFF_CY), load32(st, OFF_CX), co-1) }
      elif(a0 == 1){ _tclearline_fast(st, co, g, dfg, dbg, load32(st, OFF_CY), 0, load32(st, OFF_CX)) }
      elif(a0 == 2){ _tclearline_fast(st, co, g, dfg, dbg, load32(st, OFF_CY), 0, co-1) }
   } elif(u == 109){ _tsetattr_fast(st, pal, dfg, dbg, ag) }
   elif(u == 114){ if(!pr){ mut t=(a0==0)?1:a0 mut b=(a1==0)?ro:a1 store32(st, t-1, OFF_TOP) store32(st, b-1, OFF_BOT) _tmoveto_fast(st, co, ro, 0, 0) } }
   elif(u == 104){ vt = _tsetmode_fast(vt, st, co, ro, g, dfg, dbg, pr, true, ag) }
   elif(u == 108){ vt = _tsetmode_fast(vt, st, co, ro, g, dfg, dbg, pr, false, ag) }
   ;; CNL: cursor next N lines (move down + col 0)
   elif(u == 69){ _tmoveto_fast(st, co, ro, 0, load32(st, OFF_CY) + ((a0==0)?1:a0)) }
   ;; CPL: cursor prev N lines (move up + col 0)
   elif(u == 70){ _tmoveto_fast(st, co, ro, 0, load32(st, OFF_CY) - ((a0==0)?1:a0)) }
   ;; ICH: insert N blank chars at cursor (shift right)
   elif(u == 64){
      def cy = load32(st, OFF_CY) def cx = load32(st, OFF_CX) def n = (a0==0)?1:a0
      def room = co - cx mut mv = room - n if(mv < 0){ mv = 0 }
      if(mv > 0){ memmove(g + (cy*co+cx+n)*16, g + (cy*co+cx)*16, mv * 16) }
      _tclearline_fast(st, co, g, dfg, dbg, cy, cx, min(cx+n-1, co-1))
   }
   ;; DCH: delete N chars at cursor (shift left)
   elif(u == 80){
      def cy = load32(st, OFF_CY) def cx = load32(st, OFF_CX) def n = (a0==0)?1:a0
      def mv = co - cx - n if(mv > 0){ memmove(g + (cy*co+cx)*16, g + (cy*co+cx+n)*16, mv * 16) }
      _tclearline_fast(st, co, g, dfg, dbg, cy, co-n, co-1)
   }
   ;; IL: insert N blank lines at cursor row
   elif(u == 76){
      def cy = load32(st, OFF_CY) def n = (a0==0)?1:a0
      vt = _tscrolldown_fast(vt, st, co, ro, g, dfg, dbg, cy, n)
   }
   ;; DL: delete N lines at cursor row
   elif(u == 77){
      def cy = load32(st, OFF_CY) def n = (a0==0)?1:a0
      vt = _tscrollup_fast(vt, st, co, ro, g, dfg, dbg, cy, n)
   }
   ;; ECH: erase N chars at cursor
   elif(u == 88){
      def cy = load32(st, OFF_CY) def cx = load32(st, OFF_CX) def n = (a0==0)?1:a0
      _tclearline_fast(st, co, g, dfg, dbg, cy, cx, min(cx+n-1, co-1))
   }
   ;; REP: repeat last printed char N times
   elif(u == 98){
      def lc = load32(st, OFF_LAST_CHAR_C) def n = (a0==0)?1:a0
      if(lc >= 32){ mut ri = 0 while(ri < n){ vt = _tputc_raw_fast(vt, st, co, ro, g, lc) ri += 1 } }
   }
   ;; DECSCUSR: cursor style (ignore shape, just track visibility)
   elif(u == 113 && pr){
      if(a0 == 0){ store8(st, 1, OFF_CURSOR_VISIBLE) }
      elif(a0 == 1 || a0 == 2 || a0 == 3 || a0 == 4 || a0 == 5 || a0 == 6){ store8(st, 1, OFF_CURSOR_VISIBLE) }
   }
   vt
}

fn _tsetattr_fast(st, pal, dfg, dbg, ag){
   mut i = 0 def n = len(ag) if(n == 0){ _tresetattr_fast(st, dfg, dbg) return }
   while(i < n){
      def a = get(ag, i) mut md = load32(st, OFF_CUR_MODE)
      if(a == 0){ _tresetattr_fast(st, dfg, dbg) }
      elif(a == 1){ store32(st, md | ATTR_BOLD, OFF_CUR_MODE) }
      elif(a == 2){ store32(st, md | ATTR_FAINT, OFF_CUR_MODE) }
      elif(a == 3){ store32(st, md | ATTR_ITALIC, OFF_CUR_MODE) }
      elif(a == 4 || a == 21){ store32(st, md | ATTR_UNDERLINE, OFF_CUR_MODE) }
      elif(a == 7){ store32(st, md | ATTR_REVERSE, OFF_CUR_MODE) }
      elif(a == 22){ store32(st, md & ~(ATTR_BOLD|ATTR_FAINT), OFF_CUR_MODE) }
      elif(a == 23){ store32(st, md & ~ATTR_ITALIC, OFF_CUR_MODE) }
      elif(a == 24){ store32(st, md & ~ATTR_UNDERLINE, OFF_CUR_MODE) }
      elif(a == 27){ store32(st, md & ~ATTR_REVERSE, OFF_CUR_MODE) }
      elif(a == 53){ store32(st, md | ATTR_UNDERLINE, OFF_CUR_MODE) } ;; overline -> underline approx
      elif(a >= 30 && a <= 37){ store32(st, load32(pal, (a-30)*4), OFF_CUR_FG) }
      elif(a == 39){ store32(st, dfg, OFF_CUR_FG) }
      elif(a >= 40 && a <= 47){ store32(st, load32(pal, (a-40)*4), OFF_CUR_BG) }
      elif(a == 49){ store32(st, dbg, OFF_CUR_BG) }
      elif(a >= 90 && a <= 97){ store32(st, load32(pal, (a-90+8)*4), OFF_CUR_FG) }
      elif(a >= 100 && a <= 107){ store32(st, load32(pal, (a-100+8)*4), OFF_CUR_BG) }
      elif(a == 38 || a == 48){
         if(i + 2 < n){
            def t = get(ag, i + 1)
            if(t == 5){ store32(st, load32(pal, get(ag, i+2)*4), (a==38)?OFF_CUR_FG:OFF_CUR_BG) i += 2 }
            elif(t == 2 && i+4 < n){
               def c = color_pack(float(get(ag, i+2))/255.0, float(get(ag, i+3))/255.0, float(get(ag, i+4))/255.0, 1.0)
               store32(st, c, (a==38)?OFF_CUR_FG:OFF_CUR_BG) i += 4
            }
         }
      } i += 1
   }
}

fn _tresetattr_fast(st, dfg, dbg){ store32(st, dfg, OFF_CUR_FG) store32(st, dbg, OFF_CUR_BG) store32(st, 0, OFF_CUR_MODE) }

fn _tsetmode_fast(vt, st, co, ro, g, dfg, dbg, pr, set, ag){
   mut nvt = vt
   mut i = 0 while(i < len(ag)){
      def a = get(ag, i)
      if(pr){
         if(a == 1049 || a == 47 || a == 1047){ nvt = _tswapscreen_fast(nvt, st, co, ro, g, dfg, dbg, set) }
         elif(a == 25){ store8(st, set ? 1 : 0, OFF_CURSOR_VISIBLE) }
         elif(a == 7){ mut m = load32(st, OFF_MODE) store32(st, set ? (m | MODE_WRAP) : (m & ~MODE_WRAP), OFF_MODE) }
         elif(a == 1){ store8(st, set ? 1 : 0, OFF_APPKEYS) }
      } i += 1
   }
   nvt
}

fn _tswapscreen_fast(vt, st, co, ro, g, dfg, dbg, set){
   mut nvt = vt
   mut alt = dict_get(nvt, "alt_grid", 0)
   mut m = load32(st, OFF_MODE)
   if(set && !alt){
      alt = malloc(co * ro * 16)
      _clear_grid(alt, co, ro, dfg, dbg)
      nvt = dict_set(nvt, "alt_grid", alt)
   }
   if(set){
      store32(st, m | MODE_ALTSCREEN, OFF_MODE)
      nvt = dict_set(nvt, "grid", alt)
      nvt = dict_set(nvt, "primary_grid", g)
   } else {
      store32(st, m & ~MODE_ALTSCREEN, OFF_MODE)
      def prim = dict_get(nvt, "primary_grid", 0)
      if(prim){ nvt = dict_set(nvt, "grid", prim) }
   }
   nvt
}

fn _tstrhandle(vt, buf){ 
   if(str.startswith(buf, "0;") || str.startswith(buf, "2;")){ 
      def semi = str.find(buf, ";") if(semi >= 0){ return dict_set(vt, "title", str.str_slice(buf, semi + 1, len(buf))) }
   } vt
}

fn send_input(vt, s){ def m = dict_get(vt, "master_fd") if(m >= 0){ _ = sys.sys_write(m, s, str.len(s)) } }

fn _is_emoji(u){
   ;; Returns true only for pictographic emoji codepoints that need the small emoji font.
   ;; CJK, Hangul, Katakana, Hiragana etc. are excluded -- they render fine at full size.
   ;; Enclosed alphanumerics / pictographs
   if(u >= 0x2600 && u <= 0x27BF){ return true }    ;; Misc Symbols, Dingbats
   if(u >= 0x2B00 && u <= 0x2BFF){ return true }    ;; Misc Symbols and Arrows
   if(u >= 0x1F000 && u <= 0x1F02F){ return true }  ;; Mahjong / Domino
   if(u >= 0x1F0A0 && u <= 0x1F0FF){ return true }  ;; Playing Cards
   if(u >= 0x1F100 && u <= 0x1F1FF){ return true }  ;; Enclosed Alphanumeric Supplement (incl. flags RI)
   if(u >= 0x1F200 && u <= 0x1F2FF){ return true }  ;; Enclosed Ideographic Supplement
   if(u >= 0x1F300 && u <= 0x1F5FF){ return true }  ;; Misc Symbols and Pictographs
   if(u >= 0x1F600 && u <= 0x1F64F){ return true }  ;; Emoticons
   if(u >= 0x1F680 && u <= 0x1F6FF){ return true }  ;; Transport and Map Symbols
   if(u >= 0x1F700 && u <= 0x1F77F){ return true }  ;; Alchemical Symbols
   if(u >= 0x1F780 && u <= 0x1F7FF){ return true }  ;; Geometric Shapes Extended
   if(u >= 0x1F800 && u <= 0x1F8FF){ return true }  ;; Supplemental Arrows-C
   if(u >= 0x1F900 && u <= 0x1F9FF){ return true }  ;; Supplemental Symbols and Pictographs
   if(u >= 0x1FA00 && u <= 0x1FAFF){ return true }  ;; Chess Symbols, Symbols and Pictographs Extended-A
   if(u >= 0x1FB00 && u <= 0x1FBFF){ return true }  ;; Symbols for Legacy Computing
   false
}

mut _scratch_buf = 0
mut _scratch_cap = 0
fn _get_scratch(len){
   if(_scratch_cap < len + 128){
      if(_scratch_buf){ free(_scratch_buf) }
      _scratch_buf = malloc(len + 128) _scratch_cap = len + 128
   } _scratch_buf + 64
}

fn draw(vt, ww, wh){
   def st = dict_get(vt, "state")
   def db = dict_get(vt, "def_bg")
   def co = dict_get(vt, "cols") def ro = dict_get(vt, "rows") def g = dict_get(vt, "grid")
   def fonts = dict_get(vt, "fonts")
    mut f_reg = dict_get(fonts, "regular") if(!f_reg){ f_reg = 0 }
    mut f_bold = dict_get(fonts, "bold") if(!f_bold){ f_bold = f_reg }
    mut f_ital = dict_get(fonts, "italic") if(!f_ital){ f_ital = f_reg }
    mut f_emoji = dict_get(fonts, "emoji") if(!f_emoji){ f_emoji = f_reg }
    ;; Use module-level PADDING_X/Y for consistent alignment
    def f_obj = _font_get(f_reg)
   def f_size = (f_obj != 0) ? float(dict_get(f_obj, "size", 16.0)) : 16.0
   mut ch = dict_get(vt, "char_h", floor(f_size * 1.25))
   mut cw = dict_get(vt, "char_w", 9.0)
   
   ;; Refresh metrics from live font
   def cs = measure_text(f_reg, "A")
   if(get(cs, 0) > 0.1 && (cw <= 0.1 || abs(cw - get(cs, 0)) > 0.01)){ 
      cw = get(cs, 0) 
      ch = floor(f_size * 1.25)
      vt = dict_set(vt, "char_w", cw)
      vt = dict_set(vt, "char_h", ch)
   }
   
   def scroll_off = load32(st, OFF_SCROLL)
   def history = dict_get(vt, "history", [])
   def hist_len = len(history)

    set_unlit(true)
    draw_rectangle(0, 0, ww, wh, db)
    
    def sel_active = load8(st, OFF_SEL_ACTIVE) != 0
    def ssx = load32(st, OFF_SEL_SX) def ssy = load32(st, OFF_SEL_SY)
    def sex = load32(st, OFF_SEL_EX) def sey = load32(st, OFF_SEL_EY)
    ; Normalize selection so s is always before e (abs row/col order)
    mut s_row = ssy mut s_col = ssx mut e_row = sey mut e_col = sex
    if(s_row > e_row || (s_row == e_row && s_col > e_col)){
       s_row = sey s_col = sex e_row = ssy e_col = ssx
    }

    def acache = dict_get(vt, "ascii_cache")

    mut r = 0 while(r < ro){
       def abs_r = (hist_len - scroll_off) + r
       if(abs_r < 0 || abs_r >= hist_len + ro){ r += 1 continue }

       mut line_ptr = 0
       if(abs_r < hist_len){ line_ptr = get(history, abs_r) }
       else { line_ptr = g + ((abs_r - hist_len) * co * 16) }
       
       def ry = float(r) * ch + PADDING_Y

       ;; PASS 1: Backgrounds
       mut c = 0 while(c < co){
          def off = c * 16 
          def fg = load32(line_ptr, off + 4) def bg = load32(line_ptr, off + 8) def md = load32(line_ptr, off + 12)
          mut rbg = bg if((md & ATTR_REVERSE) != 0){ rbg = fg }
          
          mut is_sel = false
          if(sel_active){
             ; Per-row selection test: correct handling of multi-line selection
             if(abs_r > s_row && abs_r < e_row){ is_sel = true }
             elif(abs_r == s_row && abs_r == e_row){ is_sel = (c >= s_col && c <= e_col) }
             elif(abs_r == s_row){ is_sel = (c >= s_col) }
             elif(abs_r == e_row){ is_sel = (c <= e_col) }
          }
          if(is_sel){ rbg = 0xFF4A6FA5 }

          if(rbg != db || is_sel){
             draw_rect_fast(float(c) * cw + PADDING_X, ry, cw, ch, rbg)
          }
          c += 1
       }
       r += 1
    }

    ;; PASS 2: Foregrounds (Batched Run Path)
    r = 0 while(r < ro){
       def abs_r = (hist_len - scroll_off) + r
       if(abs_r < 0 || abs_r >= hist_len + ro){ r += 1 continue }

       mut line_ptr = 0
       if(abs_r < hist_len){ line_ptr = get(history, abs_r) }
       else { line_ptr = g + ((abs_r - hist_len) * co * 16) }
       
       def ry = float(r) * ch + PADDING_Y
       mut c = 0 while(c < co){
          def off = c * 16 
          def cp = load32(line_ptr, off) def fg = load32(line_ptr, off + 4) def bg = load32(line_ptr, off + 8) def md = load32(line_ptr, off + 12)
          if((md & ATTR_WDUMMY) != 0 || cp < 33){ c += 1 continue }
          
          mut rfg = fg if((md & ATTR_REVERSE) != 0){ rfg = bg }
          mut cur_f = f_reg if((md & ATTR_BOLD) != 0){ cur_f = f_bold } elif((md & ATTR_ITALIC) != 0){ cur_f = f_ital }
          if(!cur_f){ cur_f = f_reg }

          ;; Accumulate a run of identical attributes (don't merge wide chars into runs)
          mut run_len = 1
          if((md & ATTR_WIDE) == 0){
             while(c + run_len < co){
                def o2 = (c + run_len) * 16
                def cp2 = load32(line_ptr, o2) def md2 = load32(line_ptr, o2 + 12)
                if(cp2 < 33 || (md2 & (ATTR_WDUMMY|ATTR_WIDE)) != 0 || (md2 & (ATTR_BOLD|ATTR_ITALIC|ATTR_REVERSE)) != (md & (ATTR_BOLD|ATTR_ITALIC|ATTR_REVERSE))){ break }
                def fg2 = load32(line_ptr, o2 + 4) def bg2 = load32(line_ptr, o2 + 8)
                mut rfg2 = fg2 if((md2 & ATTR_REVERSE) != 0){ rfg2 = bg2 }
                if(rfg2 != rfg){ break }
                run_len += 1
             }
          }

          mut rx = float(c) * cw + PADDING_X
          if(run_len == 1){
             mut char_str = (cp < 128) ? dict_get(acache, cp, "?") : str.chr(cp)
             if((md & ATTR_WIDE) != 0 && _is_emoji(cp)){
                ;; Emoji: draw at 0.5x font size, centered in the 2-cell slot
                def slot_w = cw * 2.0
                def e_obj = _font_get(f_emoji)
                def e_size = (e_obj != 0) ? float(dict_get(e_obj, "size", 8.0)) : 8.0
                def draw_rx = rx + (slot_w - e_size) * 0.5
                def draw_ry = ry + (ch - e_size) * 0.5
                draw_text(f_emoji, char_str, draw_rx, draw_ry, rfg)
             } else {
                draw_text(cur_f, char_str, rx, ry, rfg)
             }
          } else {
             ;; For ASCII runs use the fast batch path; for non-ASCII draw each char
             ;; individually at its exact grid column to guarantee monospace alignment.
             mut all_ascii = true
             mut ai = 0 while(ai < run_len){
                if(load32(line_ptr, (c + ai) * 16) >= 128){ all_ascii = false break }
                ai += 1
             }
             if(all_ascii){
                def scratch = _get_scratch(run_len + 2)
                mut s_idx = 0 mut j = 0 while(j < run_len){
                   store8(scratch, load32(line_ptr, (c + j) * 16), s_idx)
                   s_idx += 1 j += 1
                }
                store8(scratch, 0, s_idx)
                draw_text(cur_f, init_str(scratch, s_idx), rx, ry, rfg)
             } else {
                mut j = 0 while(j < run_len){
                   def cp_j = load32(line_ptr, (c + j) * 16)
                   def cs_j = (cp_j < 128) ? dict_get(acache, cp_j, "?") : str.chr(cp_j)
                   draw_text(cur_f, cs_j, float(c + j) * cw + PADDING_X, ry, rfg)
                   j += 1
                }
             }
          }

          if((md & ATTR_UNDERLINE) != 0){
             draw_rectangle(rx, ry + ch - 1.0, cw * float(run_len), 1.0, rfg) 
          }
          c += run_len
       }
       r += 1
    }

    ;; Scrollbar -- always visible when there is history, thumb shows position
    if(hist_len > 0){
       def sb_w = 6.0
       def sb_x = PADDING_X + (float(co) * cw) + 4.0
       def total_lines = float(hist_len + ro)
       def thumb_h = max(12.0, float(wh - PADDING_Y * 2.0) * float(ro) / total_lines)
       ;; thumb_y: scroll_off=0 -> thumb at bottom, scroll_off=hist_len -> thumb at top
       def max_off = float(hist_len)
       def scroll_frac = 1.0 - (float(scroll_off) / max_off) ;; 1=bottom, 0=top
       def thumb_y = PADDING_Y + (float(wh - PADDING_Y * 2.0) - thumb_h) * scroll_frac
       draw_rect_fast(sb_x, PADDING_Y, sb_w, float(wh - PADDING_Y * 2.0), 0x22FFFFFF)
       draw_rect_fast(sb_x, thumb_y, sb_w, thumb_h, (scroll_off > 0) ? 0xCCBBCCDD : 0x55BBCCDD)
    }
   if(scroll_off == 0 && load8(st, OFF_CURSOR_VISIBLE) != 0 && fmod(float(ticks()) / 500000000.0, 2.0) < 1.0){ 
      mut ccx = load32(st, OFF_CX) mut ccy = load32(st, OFF_CY)
      if(ccx >= co){ ccx = co - 1 }
      def rx = float(ccx) * cw + PADDING_X def ry = float(ccy) * ch + PADDING_Y
      draw_rectangle(rx, ry, cw, ch, color_pack(0.8, 0.8, 0.8, 0.9))
      def c_off = (ccy * co + ccx) * 16 def c_cp = load32(g, c_off)
      if(c_cp > 32){ 
         mut char_str = (c_cp < 128) ? dict_get(acache, c_cp, "?") : str.chr(c_cp)
         draw_text(f_reg, char_str, rx, ry, color_pack(0, 0, 0, 1)) 
      }
   }
}

fn handle_event(vt, ty, da){
   def st = dict_get(vt, "state")
   def co = dict_get(vt, "cols")
   def history = dict_get(vt, "history", [])
   def hist_len = len(history)
   def scroll_off = load32(st, OFF_SCROLL)
   def cw = dict_get(vt, "char_w", 9.0)
   def ch = dict_get(vt, "char_h", 18.0)
   
   if(ty == EVENT_MOUSE_BUTTON_PRESSED){
      if(dict_get(da, "button") == 0){
         def g = dict_get(vt, "grid") def ro = dict_get(vt, "rows")
         def now = ticks()
         def last_t = load64(st, OFF_LAST_CLICK_T)
         def lx = load32(st, OFF_CLICK_X) def ly = load32(st, OFF_CLICK_Y)
         def mx_raw = float(dict_get(da, "x")) def my_raw = float(dict_get(da, "y"))
         mut sx = int((mx_raw - PADDING_X) / cw) mut sy = int((my_raw - PADDING_Y) / ch)
         if(sx < 0){ sx = 0 } if(sy < 0){ sy = 0 } if(sx >= co){ sx = co - 1 } if(sy >= ro){ sy = ro - 1 }

          ;; Scrollbar Interaction: click on the far-right edge of the text grid
          def sb_x = PADDING_X + (float(co) * cw) + 4.0
          ;; DEBUG
          ; print("[VTERM] Click: mx=", mx_raw, " my=", my_raw, " sb_x=", sb_x, " hist=", hist_len)
          
          if(hist_len > 0 && mx_raw >= sb_x && mx_raw <= sb_x + 16.0){
             store8(st, 1, OFF_SB_DRAGGING)
             def fy = (my_raw - PADDING_Y) / (float(ro) * ch)
             mut n_off = int((1.0 - fy) * float(hist_len))
             if(n_off < 0){ n_off = 0 } elif(n_off > hist_len){ n_off = hist_len }
             store32(st, n_off, OFF_SCROLL)
             print("[VTERM] SB Drag Start: off=", n_off)
             return vt
          }
         
         def abs_sy = (hist_len - scroll_off) + sy
         mut count = 1
         if(now - last_t < 400000000 && sx == lx && sy == ly){ count = load32(st, OFF_CLICK_COUNT) + 1 }
         if(count > 3){ count = 1 }
         store32(st, count, OFF_CLICK_COUNT) store64(st, now, OFF_LAST_CLICK_T)
         store32(st, sx, OFF_CLICK_X) store32(st, sy, OFF_CLICK_Y)
         
         store8(st, 1, OFF_SEL_ACTIVE) store8(st, 1, OFF_SEL_DRAGGING) store8(st, 0, OFF_SEL_MOVED)
         
         if(count == 1){ ;; Single click: normal selection start
            store32(st, sx, OFF_SEL_SX) store32(st, abs_sy, OFF_SEL_SY)
            store32(st, sx, OFF_SEL_EX) store32(st, abs_sy, OFF_SEL_EY)
         } elif(count == 2){ ;; Double click: select word
            def line_ptr = (abs_sy < hist_len) ? get(history, abs_sy) : (g + (abs_sy - hist_len) * co * 16)
            mut x1 = sx while(x1 > 0){
               def cp = load32(line_ptr, (x1 - 1) * 16)
               if(cp <= 32 || cp == 34 || cp == 39 || cp == 40 || cp == 41 || cp == 44 ||
                  cp == 46 || cp == 58 || cp == 59 || cp == 61 || cp == 91 || cp == 93 ||
                  cp == 96 || cp == 123 || cp == 125){ break }
               x1 -= 1
            }
            mut x2 = sx while(x2 < co - 1){
               def cp = load32(line_ptr, (x2 + 1) * 16)
               if(cp <= 32 || cp == 34 || cp == 39 || cp == 40 || cp == 41 || cp == 44 ||
                  cp == 46 || cp == 58 || cp == 59 || cp == 61 || cp == 91 || cp == 93 ||
                  cp == 96 || cp == 123 || cp == 125){ break }
               x2 += 1
            }
            store32(st, x1, OFF_SEL_SX) 
            store32(st, abs_sy, OFF_SEL_SY)
            store32(st, x2, OFF_SEL_EX) 
            store32(st, abs_sy, OFF_SEL_EY)
         } elif(count == 3){ ;; Triple click: select line
            store32(st, 0, OFF_SEL_SX) 
            store32(st, abs_sy, OFF_SEL_SY)
            store32(st, co - 1, OFF_SEL_EX) 
            store32(st, abs_sy, OFF_SEL_EY)
         }
      }
      return vt
    } elif(ty == EVENT_MOUSE_POS_CHANGED){
       def mx = float(dict_get(da, "x")) def my = float(dict_get(da, "y"))
       if(load8(st, OFF_SB_DRAGGING) != 0){
          def fy = (my - PADDING_Y) / (float(ro) * ch)
          mut n_off = int((1.0 - fy) * float(hist_len))
          if(n_off < 0){ n_off = 0 } elif(n_off > hist_len){ n_off = hist_len }
          store32(st, n_off, OFF_SCROLL)
          return vt
       }
       if(load8(st, OFF_SEL_DRAGGING) != 0){
          mut ex = int((mx - PADDING_X) / cw) mut ey = int((my - PADDING_Y) / ch)
         if(ex < 0){ ex = 0 } elif(ex >= co){ ex = co - 1 }
         
         ;; Auto-scroll logic (updated to read current scroll state)
         mut cur_off = load32(st, OFF_SCROLL)
          mut n_scroll = cur_off
          if(my < PADDING_Y){ n_scroll = min(hist_len, n_scroll + 1) }
          elif(my > (PADDING_Y + float(dict_get(vt, "rows")) * ch)){ n_scroll = max(0, n_scroll - 1) }
          if(n_scroll != cur_off){ store32(st, n_scroll, OFF_SCROLL) }

         mut abs_ey = (hist_len - n_scroll) + ey
         if(abs_ey < 0){ abs_ey = 0 } elif(abs_ey >= hist_len + int(dict_get(vt, "rows"))){ abs_ey = hist_len + int(dict_get(vt, "rows")) - 1 }
         
         store32(st, ex, OFF_SEL_EX) 
         store32(st, abs_ey, OFF_SEL_EY)
         ;; Mark that drag actually moved
         if(ex != load32(st, OFF_SEL_SX) || abs_ey != load32(st, OFF_SEL_SY)){
            store8(st, 1, OFF_SEL_MOVED)
         }
      }
      return vt
   } elif(ty == EVENT_MOUSE_SCROLL){
      def raw_dy = float(dict_get(da, "dy", 0.0))
      def md = dict_get(da, "mod", 0)
      
      mut mult = 3.0 ;; 3 lines per notch (standard)
      if((md & MOD_SHIFT) != 0){ mult = 10.0 }
      
       mut acc = load32_f32(st, OFF_SCROLL_ACC) + (raw_dy * mult)
       mut dy = int(acc)
       acc = acc - float(dy)
      
      if(dy != 0){
         if((load32(st, OFF_MODE) & MODE_ALTSCREEN) != 0){
            def appk = load8(st, OFF_APPKEYS) != 0
            mut i = 0 mut abs_dy = (dy > 0) ? dy : (0 - dy)
            def up_key = appk ? "\033OA" : "\033[A"
            def dn_key = appk ? "\033OB" : "\033[B"
            def out_seq = (dy > 0) ? up_key : dn_key
            while(i < abs_dy){ send_input(vt, out_seq) i += 1 }
         } else {
            mut n_off = load32(st, OFF_SCROLL) + dy
            if(n_off < 0){ n_off = 0 } elif(n_off > hist_len){ n_off = hist_len }
            store32(st, n_off, OFF_SCROLL)
         }
      }
      store32_f32(st, acc, OFF_SCROLL_ACC)
      return vt
   } elif(ty == EVENT_MOUSE_BUTTON_RELEASED){
      if(dict_get(da, "button") == 0){
         store8(st, 0, OFF_SEL_DRAGGING)
         store8(st, 0, OFF_SB_DRAGGING)
         ;; Auto-copy selection to clipboard on release (like most terminals)
         ;; Keep sel_active=1 so the highlight stays visible
         if(load8(st, OFF_SEL_ACTIVE) != 0){
            ;; Only copy if the selection covers at least one cell
            def moved = load8(st, OFF_SEL_MOVED) != 0
            def cnt = load32(st, OFF_CLICK_COUNT)
            if(moved || cnt >= 2){
               ;; Build selected text and put it in clipboard
               def g = dict_get(vt, "grid")
               mut cs_row = load32(st, OFF_SEL_SY) mut cs_col = load32(st, OFF_SEL_SX)
               mut ce_row = load32(st, OFF_SEL_EY) mut ce_col = load32(st, OFF_SEL_EX)
               if(cs_row > ce_row || (cs_row == ce_row && cs_col > ce_col)){
                  mut tmp = cs_row cs_row = ce_row ce_row = tmp
                  tmp = cs_col cs_col = ce_col ce_col = tmp
               }
               mut full_txt = ""
               mut cur_y = cs_row
               while(cur_y <= ce_row){
                  mut row_txt = ""
                  def row_x1 = (cur_y == cs_row) ? cs_col : 0
                  def row_x2 = (cur_y == ce_row) ? ce_col : (co - 1)
                  def line_ptr = (cur_y < hist_len) ? get(history, cur_y) : (g + (cur_y - hist_len) * co * 16)
                  mut cur_x = row_x1
                  while(cur_x <= row_x2){
                     def cp = load32(line_ptr, cur_x * 16)
                     def m  = load32(line_ptr, cur_x * 16 + 12)
                     if((m & ATTR_WDUMMY) == 0){
                        if(cp > 32){ row_txt = row_txt + str.chr(cp) }
                        elif(cp == 32){ row_txt = row_txt + " " }
                     }
                     cur_x += 1
                  }
                  ;; Trim trailing spaces
                  mut ti = str.str_len(row_txt) - 1
                  while(ti >= 0 && load8(row_txt, ti) == 32){ ti -= 1 }
                  if(ti < str.str_len(row_txt) - 1){ row_txt = str.str_slice(row_txt, 0, ti + 1) }
                  full_txt = full_txt + row_txt
                  if(cur_y < ce_row){ full_txt = full_txt + "\n" }
                  cur_y += 1
                }
                if(str.len(full_txt) > 0){ 
                   uiw.set_clipboard(uiw.last(), full_txt) 
                   store8(st, 0, OFF_SEL_ACTIVE)
                }
            } else {
               ;; Plain single click with no drag -- clear selection
               store8(st, 0, OFF_SEL_ACTIVE)
            }
         }
      } elif(dict_get(da, "button") == 1){
         ; Middle-click: paste clipboard at cursor (X11-style)
         def txt = uiw.get_clipboard(uiw.last())
         if(str.len(txt) > 0){ 
            store32(st, 0, OFF_SCROLL)
            send_input(vt, "\033[200~" + txt + "\033[201~")
         }
      } elif(dict_get(da, "button") == 2){
         ; Right-click: clear selection
         store8(st, 0, OFF_SEL_ACTIVE) store8(st, 0, OFF_SEL_DRAGGING) store8(st, 0, OFF_SEL_MOVED)
      }
      return vt
   } elif(ty == EVENT_KEY_CHAR){ 
      store32(st, 0, OFF_SCROLL) ;; Scroll to bottom on input
      def c = dict_get(da, "char")
      if(c >= 32){ 
         if(ticks() - load64(st, OFF_LAST_CHAR_T) > 100000000 || load32(st, OFF_LAST_CHAR_C) != int(c)){
            store64(st, ticks(), OFF_LAST_CHAR_T)
            store32(st, int(c), OFF_LAST_CHAR_C)
            send_input(vt, str.chr(c)) 
         }
      }
      return vt
   } elif(ty == EVENT_KEY_PRESSED){
      def k = dict_get(da, "key") def raw_md = dict_get(da, "mod", 0) def appk = load8(st, OFF_APPKEYS) != 0
      def md = raw_md & 0xFF 
      
      if((md & MOD_CONTROL) != 0 && (md & MOD_SHIFT) != 0){
         if(k == 86){ ;; Ctrl+Shift+V (Paste)
            def txt = uiw.get_clipboard(uiw.last()) 
            if(str.len(txt) > 0){
               store32(st, 0, OFF_SCROLL)
               send_input(vt, "\033[200~" + txt + "\033[201~")
            }
            return vt 
         }
         if(k == 67){ ;; Ctrl+Shift+C (Copy)
            if(load8(st, OFF_SEL_ACTIVE) == 0){ return vt } ;; nothing selected
            def g = dict_get(vt, "grid") 
            mut full_txt = ""
            ; Normalize selection direction
            mut cs_row = load32(st, OFF_SEL_SY) mut cs_col = load32(st, OFF_SEL_SX)
            mut ce_row = load32(st, OFF_SEL_EY) mut ce_col = load32(st, OFF_SEL_EX)
            if(cs_row > ce_row || (cs_row == ce_row && cs_col > ce_col)){
               mut tmp = cs_row cs_row = ce_row ce_row = tmp
               tmp = cs_col cs_col = ce_col ce_col = tmp
            }
            mut cur_y = cs_row
            while(cur_y <= ce_row){
               mut row_txt = ""
               def row_x1 = (cur_y == cs_row) ? cs_col : 0
               def row_x2 = (cur_y == ce_row) ? ce_col : (co - 1)
               def line_ptr = (cur_y < hist_len) ? get(history, cur_y) : (g + (cur_y - hist_len) * co * 16)
               mut cur_x = row_x1
               while(cur_x <= row_x2){
                  def cp = load32(line_ptr, cur_x * 16)
                  def m = load32(line_ptr, cur_x * 16 + 12)
                  if((m & ATTR_WDUMMY) == 0){
                     if(cp > 32){ row_txt = row_txt + str.chr(cp) }
                     elif(cp == 32){ row_txt = row_txt + " " }
                  }
                  cur_x += 1
               }
               ;; Trim trailing spaces
               mut ti = str.len(row_txt) - 1
               while(ti >= 0 && load8(row_txt, ti) == 32){ ti -= 1 }
               if(ti < str.len(row_txt) - 1){ row_txt = str.str_slice(row_txt, 0, ti + 1) }
               full_txt = full_txt + row_txt
               if(cur_y < ce_row){ full_txt = full_txt + "\n" }
               cur_y += 1
            }
            if(str.len(full_txt) > 0){ clipboard.set_text(full_txt) }
            return vt
         }
      }
      
      ; Advanced Shortcuts
      if((md & MOD_CONTROL) != 0){
         if(k >= 65 && k <= 90){ store32(st, 0, OFF_SCROLL) send_input(vt, str.chr(k - 64)) return vt }
         if(k >= 97 && k <= 122){ store32(st, 0, OFF_SCROLL) send_input(vt, str.chr(k - 96)) return vt }
         if(k == 91){ store32(st, 0, OFF_SCROLL) send_input(vt, "\033") return vt }
         if(k == 92){ store32(st, 0, OFF_SCROLL) send_input(vt, "\034") return vt }
         if(k == 93){ store32(st, 0, OFF_SCROLL) send_input(vt, "\035") return vt }
         if(k == 94){ store32(st, 0, OFF_SCROLL) send_input(vt, "\036") return vt }
         if(k == 95){ store32(st, 0, OFF_SCROLL) send_input(vt, "\037") return vt }
      }
      
      if((md & MOD_SHIFT) != 0){
         if(k == 1004){ ;; Shift+PageUp: scroll history up (Full Page)
            mut n_off = scroll_off + max(1, int(dict_get(vt, "rows")) - 2)
            if(n_off > hist_len){ n_off = hist_len }
            store32(st, n_off, OFF_SCROLL) return vt
         }
         if(k == 1005){ ;; Shift+PageDown: scroll history down (Full Page)
            mut n_off = scroll_off - max(1, int(dict_get(vt, "rows")) - 2)
            if(n_off < 0){ n_off = 0 }
            store32(st, n_off, OFF_SCROLL) return vt
         }
      }

      ;; Scroll-to-bottom only on deliberate typing input -- NOT on navigation/repeat keys.
      ;; Arrow keys, F-keys, Escape, Tab etc. must NOT reset scroll because the
      ;; software-repeat system fires them every 40ms, which would make scrolling impossible.
      if(k == 13 || k == 257){ store32(st, 0, OFF_SCROLL) send_input(vt, "\r") return vt }
      elif(k == 8 || k == 259){ store32(st, 0, OFF_SCROLL) send_input(vt, str.chr(127)) return vt }
      elif(k == 9 || k == 258){ send_input(vt, "\t") return vt }
      elif(k == 27 || k == 256){ send_input(vt, "\033") return vt }
      elif(k == 127 || k == 261){ send_input(vt, "\033[3~") return vt }
      elif(k == 1001){ send_input(vt, appk ? "\033OA" : "\033[A") return vt }
      elif(k == 1003){ send_input(vt, appk ? "\033OB" : "\033[B") return vt }
      elif(k == 1002){ send_input(vt, appk ? "\033OC" : "\033[C") return vt }
      elif(k == 1000){ send_input(vt, appk ? "\033OD" : "\033[D") return vt }
      elif(k == 1004){ send_input(vt, "\033[5~") return vt }
      elif(k == 1005){ send_input(vt, "\033[6~") return vt }
      elif(k == 1006){ send_input(vt, "\033[1~") return vt }
      elif(k == 1007){ send_input(vt, "\033[4~") return vt }
      elif(k >= 1008 && k <= 1019){
         def codes = ["\033OP","\033OQ","\033OR","\033OS","\033[15~","\033[17~","\033[18~","\033[19~","\033[20~","\033[21~","\033[23~","\033[24~"]
         send_input(vt, get(codes, k - 1008)) return vt
      }

      ;; KEY_PRESSED fallback for printable chars -- fires when GLFW char callback doesn't.
      ;; Handles standard printable ASCII (32-126) and some layout-specific keys.
      ;; We deduplicate against EVENT_KEY_CHAR using a 100ms window.
      if(k >= 32 && k <= 126 && k != 96 && (md & MOD_CONTROL) == 0 && (md & MOD_ALT) == 0){
         mut kout = k
         if((md & MOD_SHIFT) != 0){
            if(k >= 65 && k <= 90){ kout = k } else { kout = _shift_char(k) }
         } else {
            if(k >= 65 && k <= 90){ kout = k + 32 }
         }
         
         ;; Only send if this character wasn't recently sent by EVENT_KEY_CHAR
         if(ticks() - load64(st, OFF_LAST_CHAR_T) > 100000000 || load32(st, OFF_LAST_CHAR_C) != kout){
            store32(st, 0, OFF_SCROLL)
            store64(st, ticks(), OFF_LAST_CHAR_T)
            store32(st, int(kout), OFF_LAST_CHAR_C)
            send_input(vt, str.chr(kout))
            return vt
         }
      }
      
      ;; Also handle keys > 126 that might be printable in some layouts
      if(k > 126 && (md & MOD_CONTROL) == 0 && (md & MOD_ALT) == 0){
         if(ticks() - load64(st, OFF_LAST_CHAR_T) > 100000000 || load32(st, OFF_LAST_CHAR_C) != k){
            store32(st, 0, OFF_SCROLL)
            store64(st, ticks(), OFF_LAST_CHAR_T)
            store32(st, int(k), OFF_LAST_CHAR_C)
            send_input(vt, str.chr(k))
            return vt
         }
      }
   }
   vt
}

fn _shift_char(k){
   if(k == 49){ return 33 } if(k == 50){ return 64 } if(k == 51){ return 35 } if(k == 52){ return 36 } if(k == 53){ return 37 }
   if(k == 54){ return 94 } if(k == 55){ return 38 } if(k == 56){ return 42 } if(k == 57){ return 40 } if(k == 48){ return 41 }
   if(k == 45){ return 95 } if(k == 61){ return 43 } if(k == 91){ return 123 } if(k == 93){ return 125 } if(k == 92){ return 124 }
   if(k == 59){ return 58 } if(k == 39){ return 34 } if(k == 44){ return 60 } if(k == 46){ return 62 } if(k == 47){ return 63 }
   k
}

fn write(vt, s){ 
   def st = dict_get(vt, "state") def co = dict_get(vt, "cols") def ro = dict_get(vt, "rows") def g = dict_get(vt, "grid")
   def pal = dict_get(vt, "palette") def dfg = dict_get(vt, "def_fg") def dbg = dict_get(vt, "def_bg")
   mut i = 0 def n = len(s) mut nvt = vt
   while(i < n){ 
      def b0 = load8(s, i) & 255 mut cp = b0 mut w = 1 
      if((b0 & 0x80) != 0){ w = str._utf8_seq_len(s, i, n) if(w > 1){ cp = str._utf8_decode_at(s, i, w) } else { w = 1 } } 
      nvt = _tputc_fast(nvt, st, co, ro, g, pal, dfg, dbg, cp) i += w 
   } 
   nvt
}

fn resize(vt, cols, rows){
   def oc = dict_get(vt, "cols") def or = dict_get(vt, "rows")
   if(cols == oc && rows == or){ return vt }
   def dfg = dict_get(vt, "def_fg") def dbg = dict_get(vt, "def_bg")
   def og = dict_get(vt, "grid")
   def ng = malloc(cols * rows * 16)
   _clear_grid(ng, cols, rows, dfg, dbg)

   ;; Copy as many rows as fit, copying min(cols, oc) cells per row
   def copy_rows = min(rows, or)
   def copy_cols = min(cols, oc)
   mut r = 0 while(r < copy_rows){
      memcpy(ng + (r * cols * 16), og + (r * oc * 16), copy_cols * 16)
      r += 1
   }

   mut nvt = dict_set(vt, "grid", ng)
   nvt = dict_set(nvt, "cols", cols)
   nvt = dict_set(nvt, "rows", rows)
   free(og)

   def al = dict_get(nvt, "alt_grid")
   if(al){ free(al) nvt = dict_set(nvt, "alt_grid", 0) }

   def m = dict_get(nvt, "master_fd") if(m >= 0){ _resize_pty(m, cols, rows) }

   def st = dict_get(nvt, "state")
   ;; Clamp scroll region bot to new last row
   store32(st, rows - 1, OFF_BOT)
   ;; Clamp cursor position into new grid
   mut cx = load32(st, OFF_CX) mut cy = load32(st, OFF_CY)
   if(cx >= cols){ cx = cols - 1 } if(cx < 0){ cx = 0 }
   if(cy >= rows){ cy = rows - 1 } if(cy < 0){ cy = 0 }
   store32(st, cx, OFF_CX) store32(st, cy, OFF_CY)

   ;; Keep TOP in range
   mut top = load32(st, OFF_TOP)
   if(top >= rows){ top = 0 } store32(st, top, OFF_TOP)

   ;; Refresh cached cell metrics
   def fr = dict_get(dict_get(nvt, "fonts"), "regular")
   def f_obj = _font_get(fr)
   def f_sz = float(dict_get(f_obj, "size", 16.0))
   def cs = measure_text(fr, "A")
   if(get(cs, 0) > 0.1){
      nvt = dict_set(nvt, "char_w", float(get(cs, 0)))
      nvt = dict_set(nvt, "char_h", floor(f_sz * 1.25))
   }
   nvt
}

fn _resize_pty(m, cols, rows){ 
   mut ws = malloc(8) store16(ws, rows, 0) store16(ws, cols, 2) store16(ws, 0, 4) store16(ws, 0, 6) 
   __ioctl(m, 0x5414, ws) free(ws) 
}
