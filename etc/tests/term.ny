#!/bin/ny
;; Nytrix Terminal Emulator

use std.core *
use std.os *
use std.ui.consts *
use std.ui.gfx *
use std.ui.window as window
use std.ui.glfw as ui_backend
use std.ui.vterm as vterm
use std.str as str

mut win = 0
mut font = 0
mut font_bold = 0
mut font_italic = 0
mut font_emoji = 0
mut vt = 0
mut win_w = 1280.0
mut win_h = 720.0
def START_FONT_SIZE = 16.0
mut font_size = 16.0

def FONT_REG = "/usr/share/fonts/TTF/FiraCode-Regular.ttf"
def FONT_BOLD = "/usr/share/fonts/TTF/MesloLGSNerdFontMono-Bold.ttf"
def FONT_ITAL = "/usr/share/fonts/TTF/MesloLGSNerdFontMono-Italic.ttf"

fn startup(){
   renderer_config(false, false, "", "", 2)
   win = window.create(int(win_w), int(win_h), "Nytrix Terminal", WINDOW_MAXIMIZE | WINDOW_CENTER_CURSOR)
   if(!win){ print("Failed to create window") exit(1) }
   render_init(win)
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

   def cols = int(win_w / cw)
   def rows = int(win_h / ch)

   mut fonts = dict()
   fonts = dict_set(fonts, "regular", font)
   fonts = dict_set(fonts, "bold", font_bold)
   fonts = dict_set(fonts, "italic", font_italic)
   fonts = dict_set(fonts, "emoji", font_emoji)

   vt = vterm.new(cols, rows, fonts)
   def shell = env("SHELL")
   def shell_path = (shell != 0) ? shell : "/bin/bash"
   def res = vterm.open(vt, shell_path, ["--login"])
   if(is_err(res)){
      print("Failed to open vterm:", unwrap_err(res))
      exit(1)
   }
   vt = unwrap(res)
   ;; TEST UTF-8 during startup
   ;vterm.send_input(vt, "echo 'ターミナル🫪'\r\n")
   ;vterm.send_input(vt, "neofetch\r\n")
}

fn _reload_fonts(){
   mut f = font_load(FONT_REG, int(font_size))
   if(!f){ f = font_load("/usr/share/fonts/TTF/DejaVuSansMono.ttf", int(font_size)) }
   font = f
   f = font_load(FONT_BOLD, int(font_size)) if(!f){ f = font } font_bold = f
   f = font_load(FONT_ITAL, int(font_size)) if(!f){ f = font } font_italic = f
   ;; Emoji font at 0.5x size so wide glyphs fit neatly inside their 2-cell slot
   def emoji_sz = max(4.0, floor(font_size * 0.5))
   f = font_load(FONT_REG, int(emoji_sz)) if(!f){ f = font } font_emoji = f
   
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
   ui_backend.poll_events()
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
         if((md & MOD_CONTROL) != 0){
            if(k == 61 || k == 334){ font_size += 1.0 _reload_fonts() e = window.check_event(win) continue }
            elif(k == 45 || k == 333){ if(font_size > 4.0){ font_size -= 1.0 _reload_fonts() } e = window.check_event(win) continue }
            elif(k == 48 || k == 320){ font_size = START_FONT_SIZE _reload_fonts() e = window.check_event(win) continue }
         }
      }
      
      vt = vterm.handle_event(vt, typ, data)
      if(typ == EVENT_QUIT){ window.set_should_close(win, true) }
      e = window.check_event(win)
      event_count += 1
   }
   
   mut updates = 0
   while(updates < 100){ ;; Aggressive burst for high output/repeats
      mut nvt = vterm.update(vt)
      if(nvt == vt){ break }
      vt = nvt
      updates += 1
   }
   
   if(!vterm.is_running(vt)){ window.set_should_close(win, true) }
   if(event_count == 0 && updates == 0){ msleep(1) }
}

fn _resize_term(){
   def char_sz = measure_text(font, "A")
   mut cw = get(char_sz, 0) if(cw <= 0.0){ cw = 9.0 }
   def ch = floor(font_size * 1.25)
   def cols = int(win_w / cw)
   def rows = int(win_h / ch)
   if(cols > 0 && rows > 0){ vt = vterm.resize(vt, cols, rows) }
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
