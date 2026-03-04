#!/bin/ny
;; Pong

use std.core *
use std.os *
use std.math *
use std.math.random *
use std.ui.consts *
use std.ui.gfx *
use std.ui.window as window
use std.ui.input as uin

def WIN_W = 1280.0
def WIN_H = 720.0
def PADDLE_W = 20.0
def PADDLE_H = 120.0
def BALL_SIZE = 16.0
def BALL_SPEED_START = 650.0
def PADDLE_SPEED = 850.0

def COLOR_WHITE = 0xFFFFFFFF
def COLOR_BG = [0.01, 0.01, 0.015, 1.0]

def M_ID = mat4_identity()

mut win = 0
mut res_font = 0

mut p1_y = 300.0
mut p2_y = 300.0
mut ball_x = 640.0
mut ball_y = 360.0
mut ball_vx = 650.0
mut ball_vy = 650.0
mut time_acc = 0.0

mut score_l = 0
mut score_r = 0

mut _serve_dir = 1.0
mut _game_active = false

fn reset_ball(){
   ball_x = WIN_W / 2.0
   ball_y = WIN_H / 2.0
   ball_vx = _serve_dir * BALL_SPEED_START
   ball_vy = (float(rand() % 100) / 50.0 - 1.0) * BALL_SPEED_START * 0.5
   _serve_dir = 0.0 - _serve_dir
}

fn is_inside(px, py, rx, ry, rw, rh){
   px >= rx && px <= (rx + rw) && py >= ry && py <= (ry + rh)
}

fn startup(){
   win = init_window(1280, 720, "Pong", 0, true, false)
   if(win == 0){ exit(1) }
   set_window_pos(win, 100, 100)
   res_font = font_load("etc/assets/font/monocraft.ttf", 24)
   set_clear_color(COLOR_BG)
}

fn update(dt){
   time_acc = time_acc + dt

   if(!_game_active){
      if(win != 0){
         if(window.key_down(win, uin.KEY_SPACE)){
         _game_active = true
         }
      }
      return 0
   }

   if(win != 0){
      if(window.key_down(win, uin.KEY_W) || window.key_down(win, uin.KEY_UP)){
         p1_y = p1_y - (PADDLE_SPEED * dt)
      }
      if(window.key_down(win, uin.KEY_S) || window.key_down(win, uin.KEY_DOWN)){
         p1_y = p1_y + (PADDLE_SPEED * dt)
      }
   }

   p1_y = clamp(p1_y, 0.0, WIN_H - PADDLE_H)

   def ai_target = ball_y + (BALL_SIZE / 2.0) - (PADDLE_H / 2.0)
   p2_y = lerp(p2_y, ai_target, 0.040)
   p2_y = clamp(p2_y, 0.0, WIN_H - PADDLE_H)

   ball_x = ball_x + (ball_vx * dt)
   ball_y = ball_y + (ball_vy * dt)

   if(ball_y <= 0.0 && ball_vy < 0.0){
      ball_vy = 0.0 - ball_vy
      ball_vy = ball_vy + (float(rand() % 100) / 500.0 - 0.1)
   } elif(ball_y >= WIN_H - BALL_SIZE && ball_vy > 0.0){
      ball_vy = 0.0 - ball_vy
      ball_vy = ball_vy + (float(rand() % 100) / 500.0 - 0.1)
   }

   def px_l = 60.0
   def px_r = WIN_W - 60.0 - PADDLE_W
   def MAX_BALL_SPEED = 1800.0

   if(ball_vx < 0.0){
      if(is_inside(ball_x, ball_y + (BALL_SIZE / 2.0), px_l, p1_y, PADDLE_W, PADDLE_H)){
         ball_vx = abs(ball_vx) * 1.05
         if(ball_vx > MAX_BALL_SPEED){ ball_vx = MAX_BALL_SPEED }
         def hit_offset = (ball_y + (BALL_SIZE / 2.0)) - (p1_y + (PADDLE_H / 2.0))
         ball_vy = hit_offset * 10.0
         ball_x = px_l + PADDLE_W + 1.0
      }
   } else {
      if(is_inside(ball_x + BALL_SIZE, ball_y + (BALL_SIZE / 2.0), px_r, p2_y, PADDLE_W, PADDLE_H)){
         ball_vx = (0.0 - abs(ball_vx)) * 1.05
         if(ball_vx < (0.0 - MAX_BALL_SPEED)){ ball_vx = (0.0 - MAX_BALL_SPEED) }
         def hit_offset = (ball_y + (BALL_SIZE / 2.0)) - (p2_y + (PADDLE_H / 2.0))
         ball_vy = hit_offset * 10.0
         ball_x = px_r - BALL_SIZE - 1.0
      }
   }

   if(ball_x < 0.0){
      score_r = score_r + 1
      reset_ball()
   } elif(ball_x > WIN_W){
      score_l = score_l + 1
      reset_ball()
   }

   if(win != 0){
      if(window.key_down(win, uin.KEY_ESCAPE)){
         window.set_should_close(win, true)
      }
   }
}

fn draw(){
   begin_frame()
   set_ortho_2d(0.0, WIN_W, 0.0, WIN_H)
   set_unlit(true)
   set_model_matrix(M_ID)

   draw_rect_fast(639.0, 0.0, 2.0, WIN_H, 0x22FFFFFF)
   draw_rect_fast(60.0, p1_y, PADDLE_W, PADDLE_H, COLOR_WHITE)
   draw_rect_fast(WIN_W - 60.0 - PADDLE_W, p2_y, PADDLE_W, PADDLE_H, COLOR_WHITE)

   def r = 0.5 + 0.5 * cos(time_acc * 4.0)
   def g = 0.5 + 0.5 * cos(time_acc * 4.0 + 2.0)
   def b = 0.5 + 0.5 * cos(time_acc * 4.0 + 4.0)
   def ball_col = color_pack(r, g, b, 1.0)
   draw_rect_fast(ball_x, ball_y, BALL_SIZE, BALL_SIZE, ball_col)

   if(res_font != 0){
      draw_text(res_font, f"{score_l}", 580.0, 60.0, COLOR_WHITE)
      draw_text(res_font, f"{score_r}", 680.0, 60.0, COLOR_WHITE)
      if(!_game_active){
         draw_text(res_font, "PRESS SPACE TO SERVE", 500.0, 450.0, COLOR_WHITE)
      }
   }
   end_frame()
}

startup()
mut last_upd_t = ticks()

while(win != 0){
   if(window.should_close(win)){ break }
   def now = ticks()

   mut dt = float(now - last_upd_t) / 1e9
   if(dt > 0.1){ dt = 0.016 }
   if(dt <= 0.0){ dt = 0.001 }
   last_upd_t = now

   window.poll_events()

   mut e = window.check_event(win)
   while(e != 0){
      def typ = window.event_type(e)
      if(typ == EVENT_QUIT){
         window.set_should_close(win, true)
      }
      e = window.check_event(win)
   }

   update(dt)
   draw()
}

if(win != 0){
   window.set_cursor_mode(win, window.CURSOR_NORMAL)
}
exit(0)
