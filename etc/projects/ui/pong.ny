#!/usr/bin/env ny

;; Keywords: ui pong game example
;; Pong example

use std.core
use std.math as math
use std.os.ui.render as gfx
use std.os.ui.window as window
use std.os.ui.window.consts (KEY_W, KEY_S, KEY_UP, KEY_DOWN, KEY_ESCAPE)
use std.os.ui.window.input (key_down, key_pressed)

def WIN_W, WIN_H = 1920, 1080
def START_W, START_H = 1920.0, 1080.0

def PAD_W, PAD_H = 22.0, 146.0
def PLAYER_MAX, PLAYER_ACCEL, PLAYER_DRAG = 1900.0, 22000.0, 30000.0
def ENEMY_MAX, ENEMY_ACCEL, ENEMY_DRAG = 640.0, 10500.0, 19500.0
def ENEMY_ERR, ENEMY_ZONE, ENEMY_STEER = 55.0, 9.0, 140.0

def BALL = 20.0
def BALL_START, BALL_X_MAX, BALL_Y_MAX = 660.0, 950.0, 420.0
def BALL_ACCEL, ENGLISH = 1.01, 0.052

def BORDER, NET_W, NET_H, NET_STEP = 4.0, 4.0, 18.0, 42.0
def SCORE_Y, SCORE_ADV, SCORE_GAP = 36.0, 22.0, 18.0

fn lim(a, b) { math.max(a - b, 0.0) }
fn mid(a, b) { lim(a, b) * 0.5 }

fn approach(x, goal, step) {
   if x < goal { math.min(x + step, goal) } else { math.max(x - step, goal) }
}

fn axis(v, dir, max_v, accel, drag, dt) {
   approach(v, dir * max_v, (if math.abs(dir) < 0.001 { drag } else { accel }) * dt)
}

fn wall_v(y, v, max_y) {
   if y <= 0.0 && v < 0.0 { 0.0 } else { if y >= max_y && v > 0.0 { 0.0 } else { v } }
}

fn control_axis() {
   mut a = 0.0
   if key_down(KEY_W) || key_down(KEY_UP) { a -= 1.0 }
   if key_down(KEY_S) || key_down(KEY_DOWN) { a += 1.0 }
   a
}

fn steer(y, target, zone, width) {
   def d = target - y
   if math.abs(d) <= zone { 0.0 } else { math.clamp(d / width, -1.0, 1.0) }
}

fn digits(int n) {
   if n < 10 { 1.0 } else { if n < 100 { 2.0 } else { if n < 1000 { 3.0 } else { 4.0 } } }
}

fn overlap(ax, ay, aw, ah, bx, by, bw, bh) bool {
   ax < bx + bw && ax + aw > bx && ay < by + bh && ay + ah > by
}

fn sweep(px, py, x, y, plane, pad_y) bool {
   def dx = x - px
   if math.abs(dx) <= 0.0001 { return false }

   def t = (plane - px) / dx
   if t < 0.0 || t > 1.0 { return false }

   def iy = py + (y - py) * t
   iy + BALL >= pad_y && iy <= pad_y + PAD_H
}

fn reflect(p, lo, hi) {
   ;; Reflect p into [lo,hi] with bounce folding (for multi wall hits in predict).
   def span = hi - lo
   if span <= 0.0 { return lo }
   mut x = (p - lo) % (span * 2.0)
   if x < 0.0 { x += span * 2.0 }
   if x > span { hi - (x - span) } else { lo + x }
}

fn predict(bx, by, vx, vy, plane, h) {
   ;; Predict ball center y when its x reaches the plane, folding over top/bottom walls.
   if vx <= 0.0 || plane <= bx {
      return by + BALL * 0.5
   }
   def t = (plane - bx) / vx
   def py = by + BALL * 0.5 + vy * t
   reflect(py, BALL * 0.5, h - BALL * 0.5)
}

fn bias(int rally, int score) {
   ;; small varying error to keep enemy human-like, not perfect
   def offs = [-1.0, -0.6, -0.25, 0.0, 0.25, 0.6, 1.0]
   offs[(rally * 7 + score) % 7]
}

fn enemy_goal(int rally, int score, bx, by, vx, vy, plane, h) {
   ;; return desired paddle top y; track ball y with lag and error when approaching
   def pad_c = PAD_H * 0.5
   def ball_c = by + BALL * 0.5
   mut aim = h * 0.5

   if vx > 0.0 {
      ;; crude direct predict (no full reflect for reliability)
      def t = (plane - bx) / (vx + 0.00001)
      aim = ball_c + vy * t
      aim = aim + bias(rally, score) * ENEMY_ERR
   } else {
      ;; return toward center slowly when ball is with player
      aim = math.lerp(h * 0.5, ball_c, 0.15)
   }

   aim = math.clamp(aim, BALL * 0.5, h - BALL * 0.5)
   def min_y = 6.0
   def max_y = math.max(min_y, lim(h, PAD_H) - 6.0)
   math.clamp(aim - pad_c, min_y, max_y)
}

fn serve_x(int n) { if n % 2 == 0 { BALL_START } else { -BALL_START } }
fn serve_y(int n) { if n % 4 < 2 { -BALL_START * 0.19 } else { BALL_START * 0.19 } }

fn bounce_y(by, py, old_py, dt) {
   def pad_v = (py - old_py) / dt
   def hit_y = ((by + BALL * 0.5) - (py + PAD_H * 0.5)) / (PAD_H * 0.5)
   math.clamp(hit_y * BALL_START + pad_v * ENGLISH, -BALL_Y_MAX, BALL_Y_MAX)
}

fn draw_field(w, h) {
   gfx.draw_rect(0.0, 0.0, w, BORDER, gfx.WHITE)
   gfx.draw_rect(0.0, lim(h, BORDER), w, BORDER, gfx.WHITE)

   mut y = 18.0
   while y < h {
      gfx.draw_rect(w * 0.5 - NET_W * 0.5, y, NET_W, NET_H, gfx.WHITE)
      y += NET_STEP
   }
}

fn draw_score(int font, int player, int enemy, w) {
   def cx = w * 0.5
   gfx.draw_text(font, f"{player}", cx - SCORE_GAP - digits(player) * SCORE_ADV, SCORE_Y, gfx.WHITE)
   gfx.draw_text(font, "-", cx - SCORE_ADV * 0.5, SCORE_Y, gfx.WHITE)
   gfx.draw_text(font, f"{enemy}", cx + SCORE_GAP, SCORE_Y, gfx.WHITE)
}

def win = gfx.init_window(WIN_W, WIN_H, "Nytrix - Pong", 0, true, false, 1)
if !win { panic("could not create the Pong window") }
defer { gfx.close_window() }

def font = gfx.font_load_first(["etc/assets/fonts/monocraft.ttf"], 35) ?? 0
defer { if font { gfx.font_destroy(font) } }

mut player_score, enemy_score = 0, 0
mut player_x, player_y, player_v = 0.0, mid(START_H, PAD_H), 0.0
mut enemy_x, enemy_y, enemy_v = lim(START_W, PAD_W), mid(START_H, PAD_H), 0.0
mut ball_x, ball_y = mid(START_W, BALL), mid(START_H, BALL)
mut ball_vx, ball_vy = BALL_START, BALL_START * 0.19
mut enemy_target = enemy_y
mut rally = 0

while !gfx.window_should_close() {
   if key_pressed(KEY_ESCAPE) {
      window.set_should_close(win, true)
   }

   gfx.begin_frame_clear(gfx.BLACK)

   def fb = gfx.framebuffer_size_f64()
   def w, h = fb[0], fb[1]
   def dt = math.clamp(gfx.get_frame_time(), 1.0 / 240.0, 1.0 / 30.0)
   def pad_max = lim(h, PAD_H)
   def ball_max = lim(h, BALL)

   gfx.set_ortho_2d(0.0, w, h, 0.0)

   player_x, enemy_x = 0.0, lim(w, PAD_W)

   def old_py = player_y
   def old_ey = enemy_y
   def old_bx = ball_x
   def old_by = ball_y
   def player_plane = player_x + PAD_W
   def enemy_plane = enemy_x - BALL

   player_v = axis(player_v, control_axis(), PLAYER_MAX, PLAYER_ACCEL, PLAYER_DRAG, dt)
   player_y = math.clamp(player_y + player_v * dt, 0.0, pad_max)
   player_v = wall_v(player_y, player_v, pad_max)

   enemy_target = math.lerp(
      enemy_target,
      enemy_goal(rally, player_score + enemy_score, ball_x, ball_y, ball_vx, ball_vy, enemy_plane, h),
      if ball_vx > 0.0 { 16.0 * dt } else { 5.0 * dt }
   )

   enemy_v = axis(enemy_v, steer(enemy_y, enemy_target, ENEMY_ZONE, ENEMY_STEER), ENEMY_MAX, ENEMY_ACCEL, ENEMY_DRAG, dt)
   enemy_y = math.clamp(enemy_y + enemy_v * dt, 0.0, pad_max)
   enemy_v = wall_v(enemy_y, enemy_v, pad_max)

   ball_x += ball_vx * dt
   ball_y += ball_vy * dt

   if ball_y <= 0.0 && ball_vy < 0.0 {
      ball_y = 0.0
      ball_vy = math.abs(ball_vy)
   }

   if ball_y >= ball_max && ball_vy > 0.0 {
      ball_y = ball_max
      ball_vy = -math.abs(ball_vy)
   }

   if ball_vx < 0.0 && (
      overlap(ball_x, ball_y, BALL, BALL, player_x, player_y, PAD_W, PAD_H) ||
      sweep(old_bx, old_by, ball_x, ball_y, player_plane, player_y)
   ) {
      ball_x = player_plane
      ball_vx = math.min(math.abs(ball_vx) * BALL_ACCEL, BALL_X_MAX)
      ball_vy = bounce_y(ball_y, player_y, old_py, dt)
      rally += 1
   }

   if ball_vx > 0.0 && (
      overlap(ball_x, ball_y, BALL, BALL, enemy_x, enemy_y, PAD_W, PAD_H) ||
      sweep(old_bx, old_by, ball_x, ball_y, enemy_plane, enemy_y)
   ) {
      ball_x = enemy_plane
      ball_vx = -math.min(math.abs(ball_vx) * BALL_ACCEL, BALL_X_MAX)
      ball_vy = bounce_y(ball_y, enemy_y, old_ey, dt)
      rally += 1
   }

   if ball_x + BALL < 0.0 || ball_x > w {
      if ball_x > w { player_score += 1 } else { enemy_score += 1 }

      def total = player_score + enemy_score
      ball_x, ball_y = mid(w, BALL), mid(h, BALL)
      ball_vx, ball_vy = serve_x(total), serve_y(total)
      player_v, enemy_v = 0.0, 0.0
      enemy_target = mid(h, PAD_H)
      rally = 0
   }

   ball_y = math.clamp(ball_y, 0.0, ball_max)

   draw_field(w, h)
   gfx.draw_rect(player_x, player_y, PAD_W, PAD_H, gfx.WHITE)
   gfx.draw_rect(enemy_x, enemy_y, PAD_W, PAD_H, gfx.WHITE)
   gfx.draw_circle(ball_x + BALL * 0.5, ball_y + BALL * 0.5, BALL * 0.5, gfx.WHITE)
   draw_score(font, player_score, enemy_score, w)

   gfx.end_frame()
}
