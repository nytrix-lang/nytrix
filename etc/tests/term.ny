#!/bin/ny
;; Terminal Emulator

use std.core *
use std.os *
use std.ui.consts *
use std.ui.gfx *
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

def FONT_REG_DEFAULT = "/usr/share/fonts/TTF/DejaVuSansMono.ttf"
def FONT_REG_CANDIDATES = [
   "/usr/share/fonts/TTF/JetBrainsMonoNerdFontMono-Regular.ttf",
   "/usr/share/fonts/TTF/JetBrainsMonoNLNerdFontMono-Regular.ttf",
   "/usr/share/fonts/TTF/MesloLGSNerdFontMono-Regular.ttf",
   "/usr/share/fonts/OTF/FiraMonoNerdFontMono-Regular.otf",
   "/usr/share/fonts/TTF/DejaVuSansMono.ttf"
]

fn startup(){
   win = init_window(int(win_w), int(win_h), "Nytrix Terminal", WINDOW_MAXIMIZE | WINDOW_CENTER_CURSOR)
   if(!win){ print("Failed to create window") exit(1) }
   window.set_exit_key(win, KEY_NULL)
   window.set_input_exclusive(win, true)
   window.focus(win)

   _reload_fonts()

   def sz = window.size(win)
   win_w = float(get(sz, 0))
   win_h = float(get(sz, 1))

   _init_vt()
   window.set_cursor_mode(win, window.CURSOR_NORMAL)
}

fn _init_vt(){
   def char_sz = measure_text(font, "A")
   mut cw = get(char_sz, 0) if(cw <= 0.0){ cw = 9.0 }
   def ch = floor(font_size * 1.25)

   mut cols = int((win_w - 4.0) / cw)
   mut rows = int((win_h - 4.0) / ch)
   if(cols <= 0){ cols = 80 } if(rows <= 0){ rows = 24 }

   mut fonts = dict()
   fonts = dict_set(fonts, "regular", font)
   fonts = dict_set(fonts, "bold", font_bold)
   fonts = dict_set(fonts, "italic", font_italic)
   fonts = dict_set(fonts, "emoji", font_emoji)

   vt = vterm.new(cols, rows, fonts)

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
   mut cw = get(char_sz, 0) if(cw <= 0.0){ cw = 9.0 }
   def ch = floor(font_size * 1.25)
   def cols = int((win_w - 4.0) / cw)
   def rows = int((win_h - 4.0) / ch)
   if(cols > 0 && rows > 0){ vt = vterm.resize(vt, cols, rows) }
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
while(!window.should_close(win)){
   def now = ticks()
   def dt = float(now - last_t) / 1e9
   last_t = now
   update(dt)
   draw()
}
vterm.close(vt)
exit(0)
