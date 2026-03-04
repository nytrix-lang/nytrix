#!/bin/ny
;; Terminal Emulator

use std.core *
use std.os *
use std.ui.consts *
use std.ui.gfx *
use std.ui.window.native as native
use std.ui.window as window
use std.ui.gfx.vterm as vterm
use std.str as str

mut win = 0
mut font = 0
mut font_bold = 0
mut font_italic = 0
mut font_emoji = 0
mut vt = 0
mut win_w = 1280.0
mut win_h = 720.0
def START_FONT_SIZE = 18.0
mut font_size = 18.0
mut last_esc_ms = 0
mut _has_framebuffer_transparency = false

def DEFAULT_TERM_BG = 0x88323232

def FONT_REG_DEFAULT = "/usr/share/fonts/TTF/DejaVuSansMono.ttf"
def FONT_REG_CANDIDATES = [
   "/usr/share/fonts/TTF/JetBrainsMonoNerdFontMono-Regular.ttf",
   "/usr/share/fonts/TTF/JetBrainsMonoNLNerdFontMono-Regular.ttf",
   "/usr/share/fonts/TTF/MesloLGSNerdFontMono-Regular.ttf",
   "/usr/share/fonts/OTF/FiraMonoNerdFontMono-Regular.otf",
   "/usr/share/fonts/TTF/DejaVuSansMono.ttf"
]

fn _clamp_byte(v){
   if(v < 0){ return 0 }
   if(v > 255){ return 255 }
   v
}

fn _with_alpha_abgr(c, a8){
   ((a8 & 255) << 24) | (c & 0x00ffffff)
}

fn _opaque_abgr(c){
   0xff000000 | (c & 0x00ffffff)
}

fn _rgb_to_abgr(rgb){
   def r = (rgb >> 16) & 255
   def g = (rgb >> 8) & 255
   def b = rgb & 255
   (255 << 24) | (b << 16) | (g << 8) | r
}

fn _argb_to_abgr(argb){
   def a = (argb >> 24) & 255
   def r = (argb >> 16) & 255
   def g = (argb >> 8) & 255
   def b = argb & 255
   (a << 24) | (b << 16) | (g << 8) | r
}

fn _parse_term_bg(raw, def_val){
   if(!raw || !is_str(raw)){ return def_val }
   mut s = str.strip(raw)
   if(str.len(s) == 0){ return def_val }

   mut order = ""
   if(str.startswith(str.lower(s), "argb:")){ order = "argb" s = str.str_slice(s, 5, str.len(s)) }
   elif(str.startswith(str.lower(s), "abgr:")){ order = "abgr" s = str.str_slice(s, 5, str.len(s)) }
   if(str.startswith(s, "0x")){ s = str.str_slice(s, 2, str.len(s)) }
   elif(str.startswith(s, "#")){ s = str.str_slice(s, 1, str.len(s)) }

   mut hex = 0
   mut n = 0
   while(n < str.len(s)){
      def c = load8(s, n)
      mut v = -1
      if(c >= 48 && c <= 57){ v = c - 48 }
      elif(c >= 65 && c <= 70){ v = c - 55 }
      elif(c >= 97 && c <= 102){ v = c - 87 }
      else { break }
      hex = (hex << 4) | v
      n += 1
   }

   if(n == 6){
      return _with_alpha_abgr(_rgb_to_abgr(hex), (DEFAULT_TERM_BG >> 24) & 255)
   }
   if(n == 8){
      if(order == "argb"){ return _argb_to_abgr(hex) }
      return hex
   }
   def_val
}

fn _default_bg_alpha(){
   def raw = env("NY_TERM_BG_ALPHA")
   if(!raw || !is_str(raw) || str.len(str.strip(raw)) == 0){ return (DEFAULT_TERM_BG >> 24) & 255 }
   def f = str.atof(raw)
   if(f <= 1.0){ return _clamp_byte(int(f * 255.0 + 0.5)) }
   _clamp_byte(int(f))
}

fn _term_bg_color(){
   def c = _parse_term_bg(env("NY_TERM_BG"), DEFAULT_TERM_BG)
   if(!_has_framebuffer_transparency){ return _opaque_abgr(c) }
   _with_alpha_abgr(c, _default_bg_alpha())
}

fn _window_has_real_transparency(){
   if(!win){ return false }
   def has_transparency = native.get_window_attrib(window.id(win), native.TRANSPARENT_FRAMEBUFFER) != 0
   print("term: TRANSPARENT_FRAMEBUFFER attrib = " + to_str(has_transparency))
   return has_transparency
}

fn _fallback_window_opacity(){
   def raw = env("NY_TERM_WINDOW_OPACITY")
   if(!raw || !is_str(raw) || str.len(str.strip(raw)) == 0){ return 0.94 }
   def v = str.atof(raw)
   if(v < 0.0){ return 0.0 }
   if(v > 1.0){ return 1.0 }
   v
}

fn _allow_window_opacity_fallback(){
   def raw = env("NY_TERM_ALLOW_WINDOW_OPACITY")
   if(!raw || !is_str(raw) || str.len(str.strip(raw)) == 0){ return true }
   str.atof(raw) != 0.0
}

fn _maybe_report_transparency(){
   _has_framebuffer_transparency = _window_has_real_transparency()
   if(_has_framebuffer_transparency){
      print("term: per-pixel transparency ENABLED")
      return
   }
   print("term: per-pixel transparency check failed")
   if(_allow_window_opacity_fallback()){
      def opacity = _fallback_window_opacity()
      native.set_window_opacity(window.id(win), opacity)
      print("term: using whole-window opacity fallback: " + to_str(opacity))
   } else {
      print("term: per-pixel background transparency unavailable on this platform/compositor; background-only transparency cannot work here")
      print("term: set NY_TERM_ALLOW_WINDOW_OPACITY=1 to enable opacity fallback, or NY_TERM_WINDOW_OPACITY=0.9 to set custom opacity")
   }
}

fn startup(){
   def flags = WINDOW_CENTER | WINDOW_ALLOW_DND | WINDOW_TRANSPARENT | WINDOW_NO_BORDER
   print("term: creating window with flags=0x" + to_hex(flags) + " (TRANSPARENT=" + to_hex(WINDOW_TRANSPARENT) + ")")
   win = init_window(int(win_w), int(win_h), "Nytrix Terminal", flags)
   if(!win){ print("Failed to create window") exit(1) }
   print("term: window created: 0x" + to_hex(window.id(win)))
   set_clear_color([0.0, 0.0, 0.0, 0.0])
   window.set_exit_key(win, KEY_NULL)
   window.set_input_exclusive(win, true)
   window.focus(win)

   _reload_fonts()

   def sz = window.size(win)
   win_w = float(get(sz, 0))
   win_h = float(get(sz, 1))
   _maybe_report_transparency()

   _init_vt()
   window.set_cursor_mode(win, window.CURSOR_NORMAL)
   _resize_term()
}

fn _init_vt(){
   def char_sz = measure_text(font, "A")
   mut cw = get(char_sz, 0)
   mut ch = font_size
   if(cw <= 1.0){ cw = font_size * 0.6 }
   if(ch <= 1.0){ ch = 20.0 }

   mut cols = int(win_w / cw)
   mut rows = int(win_h / ch)
   if(cols <= 0){ cols = 80 } if(rows <= 0){ rows = 24 }

   mut fonts = dict()
   fonts = dict_set(fonts, "regular", font)
   fonts = dict_set(fonts, "bold", font_bold)
   fonts = dict_set(fonts, "italic", font_italic)
   fonts = dict_set(fonts, "emoji", font_emoji)

   vt = vterm.new(cols, rows, fonts, _term_bg_color())
   ;; TILING: Force cells to fill every pixel of the window
   vt = dict_set(vt, "char_w", win_w / float(cols))
   vt = dict_set(vt, "char_h", win_h / float(rows))

   vt = dict_set(vt, "window_id", window.id(win))
   def shell = env("SHELL")
   def shell_path = (shell != 0) ? shell : "/bin/bash"
   def res = vterm.open(vt, shell_path, ["--login"])
   if(is_err(res)){
      print("Failed to open vterm:", unwrap_err(res))
      exit(1)
   }
   vt = unwrap(res)
}

fn _reload_fonts(){
   def reg_path = env("NY_TERM_FONT_REG")
   def bold_path = env("NY_TERM_FONT_BOLD")
   def ital_path = env("NY_TERM_FONT_ITAL")
   def emoji_path = env("NY_TERM_FONT_EMOJI")

   mut reg_pick = reg_path ? reg_path : ""
   if(!reg_pick || !is_str(reg_pick) || str.len(reg_pick) == 0){
      mut i = 0
      while(i < len(FONT_REG_CANDIDATES) && (!reg_pick || str.len(reg_pick) == 0)){
         def p = get(FONT_REG_CANDIDATES, i)
         if(file_exists(p)){ reg_pick = p }
         i += 1
      }
   }
   if(!reg_pick || str.len(reg_pick) == 0){ reg_pick = FONT_REG_DEFAULT }
   mut f = font_load(reg_pick, int(font_size))
   if(!f){ f = font_load(FONT_REG_DEFAULT, int(font_size)) }
   font = f

   if(bold_path && is_str(bold_path) && str.len(bold_path) > 0){
      f = font_load(bold_path, int(font_size))
      font_bold = f ? f : font
   } else {
      font_bold = font
   }

   if(ital_path && is_str(ital_path) && str.len(ital_path) > 0){
      f = font_load(ital_path, int(font_size))
      font_italic = f ? f : font
   } else {
      font_italic = font
   }

   def emoji_env = env("NY_TERM_EMOJI")
   def emoji_on = (emoji_env == 0) ? true : (str.atof(emoji_env) != 0)
   if(emoji_on){
      def emoji_default = "/usr/share/fonts/noto/NotoColorEmoji.ttf"
      def ep = (emoji_path && is_str(emoji_path) && str.len(emoji_path) > 0) ? emoji_path : emoji_default
      f = font_load(ep, int(font_size))
      font_emoji = f ? f : font
   } else {
      font_emoji = font
   }

   if(vt != 0){
      mut fonts = dict()
      fonts = dict_set(fonts, "regular", font)
      fonts = dict_set(fonts, "bold", font_bold)
      fonts = dict_set(fonts, "italic", font_italic)
      fonts = dict_set(fonts, "emoji", font_emoji)
      vt = dict_set(vt, "fonts", fonts)
      _resize_term()
   }
}

fn update(dt){
   def sz = window.size(win)
   if(get(sz, 0) != int(win_w) || get(sz, 1) != int(win_h)){
      win_w = float(get(sz, 0))
      win_h = float(get(sz, 1))
      _resize_term()
   }

   mut e = window.check_event(win)
   mut event_count = 0
   while(e != 0){
      def typ = window.event_type(e)
      def data = window.event_data(e)

      if(typ == EVENT_WINDOW_RESIZED){
         win_w = float(dict_get(data, "w", 1024))
         win_h = float(dict_get(data, "h", 768))
         set_win_size(win_w, win_h)
         _resize_term()
      } elif(typ == EVENT_KEY_PRESSED){
         def k = dict_get(data, "key")
         def md = dict_get(data, "mod", 0)

         if(env("NY_TERM_ESC_CLOSE") != 0){
         if(k == KEY_ESCAPE && (md & (MOD_SHIFT|MOD_CONTROL|MOD_ALT|MOD_SUPER|MOD_META)) == 0 && vt != 0){
               def st = dict_get(vt, "state", 0)
               if(st != 0){
                  def mode = load32(st, 32)
                  if((mode & 2) == 0){
                     def now_ms = int(ticks() / 1000000)
                     if(last_esc_ms != 0 && (now_ms - last_esc_ms) < 400){
                  if(env("NYTRIX_AUTO_DUMP")){ snapshot("build/release/fb_dump.tga") }
                  window.set_should_close(win, true)
                  e = window.check_event(win)
                  continue
                     }
                     last_esc_ms = now_ms
                  } else {
                     last_esc_ms = 0
                  }
               }
         }
         }
         if((md & MOD_CONTROL) != 0){
         if(k == 61 || k == 334){ font_size += 1.0 _reload_fonts() e = window.check_event(win) continue }
         elif(k == 45 || k == 333){ if(font_size > 4.0){ font_size -= 1.0 _reload_fonts() } e = window.check_event(win) continue }
         elif(k == 48 || k == 320){ font_size = START_FONT_SIZE _reload_fonts() e = window.check_event(win) continue }
         }
      }

      mut ev_data = data
      if(is_dict(ev_data)){
         ev_data = dict_set(ev_data, "ww", win_w)
         ev_data = dict_set(ev_data, "wh", win_h)
      }
      vt = vterm.handle_event(vt, typ, ev_data)
      if(typ == EVENT_QUIT){ window.set_should_close(win, true) }
      e = window.check_event(win)
      event_count += 1
   }

   mut updates = 0
   while(updates < 100){
      mut nvt = vterm.update(vt)
      if(nvt == vt){ break }
      vt = nvt
      updates += 1
   }

   window.set_title(win, vterm.get_title(vt))

   if(!vterm.is_running(vt)){ window.set_should_close(win, true) }
   if(event_count == 0 && updates == 0){ msleep(1) }
}

fn _resize_term(){
   def char_sz = measure_text(font, "A")
   mut cw = get(char_sz, 0)
   mut ch = font_size
   if(cw <= 1.0){ cw = font_size * 0.6 }
   if(ch <= 1.0){ ch = 20.0 }
   def cols = int(win_w / cw)
   def rows = int(win_h / ch)
   if(cols > 0 && rows > 0){
      vt = vterm.resize(vt, cols, rows)
      ;; TILING: Perfect window fit
      vt = dict_set(vt, "char_w", win_w / float(cols))
      vt = dict_set(vt, "char_h", win_h / float(rows))
   }
   set_win_size(win_w, win_h)
}

fn draw(){
   begin_frame()
   set_ortho_2d(0, win_w, win_h, 0)
   vterm.draw(vt, win_w, win_h)
   end_frame()
}

startup()
mut last_t = ticks()
mut startup_ticks = ticks()
while(!window.should_close(win)){
   def now = ticks()
   def env_t = env("NY_UI_TIMEOUT")
   if(env_t){
      def timeout_ns = int(str.atof(env_t) * 1e9)
      if(now - startup_ticks >= timeout_ns){
         window.set_should_close(win, true)
      }
   }
   def dt = float(now - last_t) / 1e9
   last_t = now
   update(dt)
   draw()
}
vterm.close(vt)
exit(0)
