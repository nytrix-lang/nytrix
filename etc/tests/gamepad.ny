#!/bin/ny
;; Nytrix Gamepad

use std.core *
use std.ui.gfx *
use std.ui.window as window
use std.ui.window.input.gamepad as gamepad
use std.str as str
use std.ui.consts *

mut font = 0
mut _win_w = 1280.0
mut _win_h = 720.0

fn draw_dashboard(jid, ww, wh){
   def ref_w = 800.0 def ref_h = 450.0
   def sx = ww / ref_w def sy = wh / ref_h
   def s = (sx < sy) ? sx : sy
   def ref_cx = 405.0 def ref_cy = 220.0
   def off_x = ww * 0.5 - ref_cx * s
   def off_y = wh * 0.5 - ref_cy * s
   def p = fn(v){ off_x + v * s }
   def q = fn(v){ off_y + v * s }
   def r = fn(v){ v * s }

   def name = gamepad.get_gamepad_name(jid)
   def is_mapped = gamepad.is_mapped(jid)

   def c_panel  = color_hex("#080808")
   def c_idle   = color_hex("#1a1a1a")
   def c_lit    = color_hex("#bb86fc")
   def c_black  = color_hex("#000000")
   def c_mid    = color_hex("#333333")
   def c_stk    = color_hex("#0d0d0d")

   def lx = gamepad.get_gamepad_axis(jid, "LEFTX")
   def ly = gamepad.get_gamepad_axis(jid, "LEFTY")
   def rx = gamepad.get_gamepad_axis(jid, "RIGHTX")
   def ry = gamepad.get_gamepad_axis(jid, "RIGHTY")
   mut lt = gamepad.get_gamepad_axis(jid, "LEFTTRIGGER")
   mut rt = gamepad.get_gamepad_axis(jid, "RIGHTTRIGGER")
   if(!is_mapped){
      lt = gamepad.get_gamepad_axis(jid, 2)
      rt = gamepad.get_gamepad_axis(jid, 5)
   }

   draw_rect_rounded(p(175), q(110), r(460), r(220), r(33), c_panel)

   def lb_col = gamepad.get_gamepad_button(jid, "LEFT_BUMPER")  ? c_lit : c_idle
   def rb_col = gamepad.get_gamepad_button(jid, "RIGHT_BUMPER") ? c_lit : c_idle
   draw_rect_rounded(p(215), q(98),  r(100), r(10), r(5), lb_col)
   draw_rect_rounded(p(495), q(98),  r(100), r(10), r(5), rb_col)

   draw_rect_rounded(p(151), q(110), r(15), r(70), r(5), c_idle)
   draw_rect_rounded(p(644), q(110), r(15), r(70), r(5), c_idle)
   def lt_h = ((1.0 + lt) / 2.0) * r(70)
   def rt_h = ((1.0 + rt) / 2.0) * r(70)
   if(lt_h > r(5) * 2.0){ draw_rect_rounded(p(151), q(110), r(15), lt_h, r(5), c_lit) }
   if(rt_h > r(5) * 2.0){ draw_rect_rounded(p(644), q(110), r(15), rt_h, r(5), c_lit) }

   def back_col  = gamepad.get_gamepad_button(jid, "BACK")  ? c_lit : c_idle
   def guide_col = gamepad.get_gamepad_button(jid, "GUIDE") ? c_lit : c_idle
   def start_col = gamepad.get_gamepad_button(jid, "START") ? c_lit : c_idle
   draw_circle(p(365), q(170), r(12), c_mid)
   draw_circle(p(405), q(170), r(12), c_mid)
   draw_circle(p(445), q(170), r(12), c_mid)
   draw_circle(p(365), q(170), r(9), back_col)
   draw_circle(p(405), q(170), r(9), guide_col)
   draw_circle(p(445), q(170), r(9), start_col)

   def sq_col  = gamepad.get_gamepad_button(jid, "SQUARE")   ? c_lit : c_idle
   def cr_col  = gamepad.get_gamepad_button(jid, "CROSS")    ? c_lit : c_idle
   def ci_col  = gamepad.get_gamepad_button(jid, "CIRCLE")   ? c_lit : c_idle
   def tr_col  = gamepad.get_gamepad_button(jid, "TRIANGLE") ? c_lit : c_idle
   draw_circle(p(516), q(191), r(17), c_mid) draw_circle(p(516), q(191), r(14), sq_col)
   draw_circle(p(551), q(227), r(17), c_mid) draw_circle(p(551), q(227), r(14), cr_col)
   draw_circle(p(587), q(191), r(17), c_mid) draw_circle(p(587), q(191), r(14), ci_col)
   draw_circle(p(551), q(155), r(17), c_mid) draw_circle(p(551), q(155), r(14), tr_col)

   draw_rect_rounded(p(245), q(145), r(28), r(88), r(4), c_mid)
   draw_rect_rounded(p(215), q(174), r(88), r(29), r(4), c_mid)
   draw_rect_rounded(p(247), q(147), r(24), r(84), r(4), c_idle)
   draw_rect_rounded(p(217), q(176), r(84), r(25), r(4), c_idle)

   def dc_x = p(259) def dc_y = q(188.5)

   mut col_up = gamepad.get_gamepad_button(jid, "DPAD_UP") ? c_lit : c_idle
   if(col_up == c_lit){
      draw_rect_rounded(p(247), q(147), r(24), r(29), r(4), col_up)
      draw_rectangle(p(247), q(158), r(24), r(18), col_up)
      draw_triangle([dc_x, dc_y, 0.0], [p(247), q(176), 0.0], [p(271), q(176), 0.0], col_up)
   }

   mut col_dn = gamepad.get_gamepad_button(jid, "DPAD_DOWN") ? c_lit : c_idle
   if(col_dn == c_lit){
      draw_rect_rounded(p(247), q(201), r(24), r(30), r(4), col_dn)
      draw_rectangle(p(247), q(201), r(24), r(16), col_dn)
      draw_triangle([dc_x, dc_y, 0.0], [p(271), q(201), 0.0], [p(247), q(201), 0.0], col_dn)
   }

   mut col_lf = gamepad.get_gamepad_button(jid, "DPAD_LEFT") ? c_lit : c_idle
   if(col_lf == c_lit){
      draw_rect_rounded(p(217), q(176), r(30), r(25), r(4), col_lf)
      draw_rectangle(p(232), q(176), r(15), r(25), col_lf)
      draw_triangle([dc_x, dc_y, 0.0], [p(247), q(201), 0.0], [p(247), q(176), 0.0], col_lf)
   }

   mut col_rt = gamepad.get_gamepad_button(jid, "DPAD_RIGHT") ? c_lit : c_idle
   if(col_rt == c_lit){
      draw_rect_rounded(p(271), q(176), r(30), r(25), r(4), col_rt)
      draw_rectangle(p(271), q(176), r(15), r(25), col_rt)
      draw_triangle([dc_x, dc_y, 0.0], [p(271), q(176), 0.0], [p(271), q(201), 0.0], col_rt)
   }

   mut lj_knob = gamepad.get_gamepad_button(jid, "LEFT_THUMB") ? c_lit : c_stk
   draw_circle(p(345), q(260), r(40), c_black)
   draw_circle(p(345), q(260), r(35), c_idle)
   draw_circle(p(345) + (lx*r(20)), q(260) + (ly*r(20)), r(25), lj_knob)

   mut rj_knob = gamepad.get_gamepad_button(jid, "RIGHT_THUMB") ? c_lit : c_stk
   draw_circle(p(465), q(260), r(40), c_black)
   draw_circle(p(465), q(260), r(35), c_idle)
   draw_circle(p(465) + (rx*r(20)), q(260) + (ry*r(20)), r(25), rj_knob)

   def mode = is_mapped ? "MAPPED" : "RAW"
   def col  = is_mapped ? c_lit : color_hex("#555555")
   draw_text(font, name + "  [" + mode + "]", 30.0, 30.0, col)

   def mid_y = wh * 0.5

   draw_rect_rounded(30.0, mid_y, 100.0, 30.0, 5.0, c_lit)
   draw_text(font, "VIBRATE", 42.0, mid_y + 7.0, c_black)

   def axis_count = gamepad.get_gamepad_axis_count(jid)
   def right_anchor = ww - 300.0

   draw_text(font, "DETECTED AXIS [" + to_str(axis_count) + "]:", right_anchor, 30.0, color_hex("#555555"))
   mut i = 0 while(i < axis_count){
      def val = gamepad.get_gamepad_axis(jid, i)
      draw_text(font, "AXIS " + to_str(i) + ": " + to_str(val), right_anchor + 10.0, 60.0 + 25.0 * i, color_hex("#999999"))
      i = i + 1
   }

   mut last_btn = -1
   mut b = 0 while(b < gamepad.get_gamepad_button_count(jid)){
      if(gamepad.get_gamepad_button(jid, b)){ last_btn = b }
      b = b + 1
   }

   if(last_btn != -1){
      draw_text(font, "DETECTED BUTTON: " + to_str(last_btn), right_anchor, wh - 50.0, c_lit)
   } else {
      draw_text(font, "DETECTED BUTTON: NONE", right_anchor, wh - 50.0, color_hex("#444444"))
   }
}

fn update_events(win){
   mut e = window.check_event(win)
   while(e != 0){
      def typ = window.event_type(e)
      if(typ == EVENT_KEY_PRESSED){
         if(dict_get(window.event_data(e), "key") == KEY_ESCAPE){
         if(env("NYTRIX_AUTO_DUMP")){ snapshot("build/release/fb_dump.tga") }
         window.set_should_close(win, true)
         }
      }
      if(typ == EVENT_QUIT){ window.set_should_close(win, true) }
      if(typ == EVENT_WINDOW_RESIZED){
         def d = window.event_data(e)
         _win_w = float(dict_get(d, "w", 1280))
         _win_h = float(dict_get(d, "h", 720))
      }
      e = window.check_event(win)
   }
}

def win = init_window(1280, 720, "Gamepad", 0, true, false, 8)
if(!win){ return 0 }
font = font_load("etc/assets/fonts/monocraft.ttf", 16)

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

   window.poll_events()
   update_events(win)
   begin_frame()

   clear_background(color_hex("#000000"))

   def joysticks = gamepad.get_joysticks()

   mut best_jid = -1
   mut best_score = -1000
   mut i = 0
   while(i < len(joysticks)){
      def jid = get(joysticks, i)
      def lname = str.lower(gamepad.get_gamepad_name(jid))
      mut score = 0
      if(gamepad.is_mapped(jid)){ score = score + 100 }
      if(str.find(lname, "controller") != -1){ score = score + 50 }
      if(str.find(lname, "dual") != -1){ score = score + 60 }
      if(str.find(lname, "sony") != -1){ score = score + 60 }
      if(str.find(lname, "keyboard") != -1){ score = score - 200 }
      if(str.find(lname, "k400") != -1){ score = score - 300 }
      if(str.find(lname, "mouse") != -1){ score = score - 200 }
      if(score > best_score){ best_score = score best_jid = jid }
      i = i + 1
   }

   if(best_jid != -1 && best_score > -100){
      draw_dashboard(best_jid, _win_w, _win_h)

      def left_anchor = 30.0

      def device_count = len(joysticks) - 1
      def list_height = device_count * 25.0
      def start_y = _win_h - 50.0 - list_height

      draw_text(font, "OTHER DEVICES:", left_anchor, start_y - 30.0, color_hex("#555555"))

      mut oy2 = start_y
      mut j = 0 while(j < len(joysticks)){
         def jid2 = get(joysticks, j)
         if(jid2 != best_jid){
         draw_text(font, "[" + to_str(jid2) + "] " + gamepad.get_gamepad_name(jid2), left_anchor + 10.0, oy2, color_hex("#888888"))
         oy2 = oy2 + 25.0
         }
         j = j + 1
      }
   } else {
      draw_text(font, "CONNECT A GAMEPAD", _win_w * 0.5 - 100.0, _win_h * 0.5, color_hex("#bb86fc"))
   }

   end_frame()
}
window.close(win)
