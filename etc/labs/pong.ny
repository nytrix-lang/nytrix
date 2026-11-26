#!/usr/bin/env ny

;; Keywords: ui pong game example
;; Single-player arcade Pong

use core
use core.os (exit)
use core.os.args as process_args
use core.os.fixture as fixture
use core.common as common
use core.math as math
use core.os.ui.render as gfx
use core.os.ui.render.dump as ui_dump
use core.os.ui.window as window
use core.os.ui.window.consts (KEY_W, KEY_S, KEY_UP, KEY_DOWN, KEY_ENTER, KEY_ESCAPE, KEY_SPACE, KEY_R, WINDOW_MAXIMIZE)

def int startup_started = ticks()
def bool frame_times = common.env_truthy("NYTRIX_PONG_FRAME_TIMES")

def WIN_W, WIN_H = 1280, 720
def f64 VIEW_W, VIEW_H = 1280.0, 720.0
def f64 BALL_SIZE, BALL_SPEED = 16.0, 440.0
def f64 PADDLE_W, PADDLE_H, PADDLE_MARGIN = 14.0, 120.0, 64.0
def f64 PADDLE_SPEED, OPPONENT_SPEED, OPPONENT_ERROR, SERVE_TIME = 560.0, 360.0, 19.0, 0.75
def f64 MAX_BOUNCE_ANGLE, SPEEDUP_PER_HIT, MAX_SPEEDUP = 0.9, 0.03, 1.6
def int NET_COUNT = 51
def f64 FIXED = 1.0/240.0
def f64 DEATH_TIME = 0.42
def int FX_CAP, FX_STRIDE, TRAIL_CAP = 24,24,14
def PLAYER = gfx.color_hex("#70E6BB")
def CPU = gfx.color_hex("#B7A0FF")
def int WHITE, GREEN, PURPLE = 0xeeeeee,0xbbe670,0xffa0b7

def BG = gfx.color_hex("#000000")
def FG = gfx.color_hex("#EEEEEE")
def MUTED = gfx.color_hex("#777777")

fn lim(f64 value, f64 lower, f64 upper) f64 {
   float(math.min(math.max(value, lower), upper))
}

fn opponent_goal(f64 bx, f64 by, f64 vx, f64 vy, f64 ball, f64 h,
                 f64 paddle_h, f64 plane, int rally) f64 {
   def f64 center = by + ball * 0.5
   if vx <= 0.0 { return (h - paddle_h) * 0.5 }
   def f64 t = float(math.max((plane - bx) / vx, 0.0))
   def f64 span = h - ball
   if span <= 0.0 { return 0.0 }
   def f64 period = span * 2.0
   mut f64 folded = (center + vy * t - ball * 0.5) % period
   if folded < 0.0 { folded += period }
   if folded > span { folded = period - folded }
   def f64 variation = float(rally % 5 - 2) * OPPONENT_ERROR
   lim(ball * 0.5 + folded + variation - paddle_h * 0.5, 0.0, h - paddle_h)
}

mut int player_score = 0
mut int opponent_score = 0
mut f64 scale = 1.0
mut f64 ball_x = 0.0
mut f64 ball_y = 0.0
mut f64 ball_vx = 0.0
mut f64 ball_vy = 0.0
mut f64 serve_dir = 1.0
mut int hits = 0
mut f64 player_y = 0.0
mut f64 opponent_y = 0.0
mut f64 serve_t = SERVE_TIME
mut bool paused = false
mut bool previous_pause = false
mut bool previous_reset = false
mut bool first_frame = true
mut int rally = 0
mut int replay_frame = 0
mut bool dump_done = false
mut f64 court_w = VIEW_W
mut f64 court_h = VIEW_H
mut f64 acc = 0.0
mut f64 prev_x = 0.0
mut f64 prev_y = 0.0
mut f64 prev_player = 0.0
mut f64 prev_cpu = 0.0
mut f64 player_v = 0.0
mut f64 cpu_v = 0.0
mut f64 intro = 0.0
mut f64 death_t = 0.0
mut f64 pause_fade = 0.0
mut f64 player_flash = 0.0
mut f64 cpu_flash = 0.0
mut f64 player_pop = 0.0
mut f64 cpu_pop = 0.0
mut int fx_head = 0
mut int trail_head = 0
mut int trail_count = 0
mut int trail_tick = 0
def ptr effects = malloc(FX_CAP*FX_STRIDE)
def ptr trail = malloc(TRAIL_CAP*8)
if !effects || !trail { panic("Pong: allocation failed") }

fn ease(f64 t) f64 { def f64 u = 1.0-lim(t,0.0,1.0) 1.0-u*u*u }
fn smooth(f64 t) f64 { def f64 u = lim(t,0.0,1.0) u*u*(3.0-2.0*u) }
fn mix(f64 a,f64 b,f64 t) f64 { a+(b-a)*t }
fn absf(f64 v) f64 { float(math.abs(v)) }
fn margin() f64 { lim(PADDLE_MARGIN,court_w*0.05,court_w*0.12) }
fn left_face() f64 { margin()+PADDLE_W }
fn right_face() f64 { court_w-margin()-PADDLE_W-BALL_SIZE }
fn rgba(int c,f64 a) int { (c&0xffffff)|(int(lim(a,0.0,1.0)*255.0+0.5)<<24) }
fn putq(ptr p,int o,f64 v) { store32(p,int(lim(v,0.0,1.0)*1048576.0+0.5),o) }
fn getq(ptr p,int o) f64 { float(load32(p,o))/1048576.0 }
fn sync_pose() { prev_x,prev_y,prev_player,prev_cpu = ball_x,ball_y,player_y,opponent_y }
fn clear_trail() { trail_head,trail_count,trail_tick = 0,0,0 }
fn record_trail() {
   putq(trail,trail_head*8,(ball_x+BALL_SIZE*0.5)/court_w)
   putq(trail,trail_head*8+4,(ball_y+BALL_SIZE*0.5)/court_h)
   trail_head = (trail_head+1)%TRAIL_CAP
   if trail_count < TRAIL_CAP { trail_count += 1 }
}
fn emit(f64 x,f64 y,int kind,int color) {
   def int o = fx_head*FX_STRIDE
   putq(effects,o,x/court_w) putq(effects,o+4,y/court_h)
   store32(effects,0,o+8) store32(effects,kind,o+12)
   store32(effects,color,o+16) store32(effects,(rally*37+fx_head*71)%997,o+20)
   fx_head = (fx_head+1)%FX_CAP
}
fn age_effects() {
   mut int i = 0
   while i < FX_CAP {
      def int o = i*FX_STRIDE
      def int kind = load32(effects,o+12)
      if kind > 0 {
         def int age = load32(effects,o+8)+1
         if age >= (kind == 3 ? 144 : 72) { store32(effects,0,o+12) }
         else { store32(effects,age,o+8) }
      }
      i += 1
   }
}
fn park() {
   ball_x = (court_w-BALL_SIZE)*0.5
   ball_y = (court_h-BALL_SIZE)*0.5
   prev_x,prev_y = ball_x,ball_y
}
fn prepare_serve() {
   hits = 0
   ball_vx,ball_vy = 0.0,0.0
   serve_t = SERVE_TIME
   clear_trail()
   park()
}
fn reset_game() {
   player_score,opponent_score,rally,hits = 0,0,0,0
   player_y = (court_h-PADDLE_H)*0.5
   opponent_y = player_y
   player_v,cpu_v,death_t,intro,pause_fade,acc = 0.0,0.0,0.0,0.0,0.0,0.0
   player_flash,cpu_flash,player_pop,cpu_pop = 0.0,0.0,0.0,0.0
   paused = false
   serve_dir = 1.0
   fx_head = 0
   memset(effects,0,FX_CAP*FX_STRIDE)
   prepare_serve()
   sync_pose()
}
fn resize_court(f64 w,f64 h) {
   if w == court_w && h == court_h { return }
   ball_x = lim((ball_x+BALL_SIZE*0.5)*w/court_w-BALL_SIZE*0.5,-BALL_SIZE,w)
   ball_y = lim((ball_y+BALL_SIZE*0.5)*h/court_h-BALL_SIZE*0.5,0.0,h-BALL_SIZE)
   player_y = lim((player_y+PADDLE_H*0.5)*h/court_h-PADDLE_H*0.5,0.0,h-PADDLE_H)
   opponent_y = lim((opponent_y+PADDLE_H*0.5)*h/court_h-PADDLE_H*0.5,0.0,h-PADDLE_H)
   court_w,court_h = w,h
   if serve_t > 0.0 || death_t > 0.0 { park() }
   sync_pose()
}
fn launch() {
   serve_t = 0.0
   def f64 a = rally%4 < 2 ? 0.24 : -0.24
   ball_vx = serve_dir*BALL_SPEED*float(math.cos(a))
   ball_vy = BALL_SPEED*float(math.sin(a))
   record_trail()
   emit(ball_x+BALL_SIZE*0.5,ball_y+BALL_SIZE*0.5,1,WHITE)
}
fn bounce(f64 dir,f64 py,f64 pv) {
   def f64 offset = lim((ball_y+BALL_SIZE*0.5-py-PADDLE_H*0.5)/(PADDLE_H*0.5+BALL_SIZE*0.5),-1.0,1.0)
   def f64 angle = lim(offset*MAX_BOUNCE_ANGLE+pv/PADDLE_SPEED*0.12,-1.02,1.02)
   hits += 1
   def f64 speed = BALL_SPEED*lim(1.0+float(hits)*SPEEDUP_PER_HIT,1.0,MAX_SPEEDUP)
   ball_vx = dir*speed*float(math.cos(angle))
   ball_vy = speed*float(math.sin(angle))
   rally += 1
   if dir > 0.0 { player_flash = 0.18 }
   else { cpu_flash = 0.18 }
   emit(dir > 0.0 ? left_face() : right_face()+BALL_SIZE,ball_y+BALL_SIZE*0.5,2,dir > 0.0 ? GREEN : PURPLE)
}
fn point(bool player) {
   if player { player_score += 1 player_pop = 0.55 serve_dir = 1.0 }
   else { opponent_score += 1 cpu_pop = 0.55 serve_dir = -1.0 }
   emit(player ? court_w-3.0 : 3.0,ball_y+BALL_SIZE*0.5,3,player ? GREEN : PURPLE)
   rally += 1
   death_t = DEATH_TIME
   prepare_serve()
}
fn move_ball(f64 dt) int {
   mut f64 remaining = dt
   mut int n = 0
   while remaining > 0.00000001 && n < 6 {
      mut f64 t = remaining
      mut int event = 0
      if ball_vy < 0.0 {
         def f64 k = -ball_y/ball_vy
         if k >= 0.0 && k <= t { t = k event = 1 }
      } elif ball_vy > 0.0 {
         def f64 k = (court_h-BALL_SIZE-ball_y)/ball_vy
         if k >= 0.0 && k <= t { t = k event = 2 }
      }
      if ball_vx < 0.0 {
         def f64 k = (left_face()-ball_x)/ball_vx
         def f64 y = ball_y+ball_vy*k
         if k >= 0.0 && k <= t && y+BALL_SIZE >= player_y && y <= player_y+PADDLE_H { t = k event = 3 }
         def f64 goal = (-BALL_SIZE-ball_x)/ball_vx
         if goal >= 0.0 && goal <= t { t = goal event = 5 }
      } elif ball_vx > 0.0 {
         def f64 k = (right_face()-ball_x)/ball_vx
         def f64 y = ball_y+ball_vy*k
         if k >= 0.0 && k <= t && y+BALL_SIZE >= opponent_y && y <= opponent_y+PADDLE_H { t = k event = 4 }
         def f64 goal = (court_w-ball_x)/ball_vx
         if goal >= 0.0 && goal <= t { t = goal event = 6 }
      }
      ball_x += ball_vx*t
      ball_y += ball_vy*t
      remaining -= t
      if event == 0 { return 0 }
      if event == 1 || event == 2 {
         ball_y = event == 1 ? 0.0 : court_h-BALL_SIZE
         ball_vy = -ball_vy
         emit(ball_x+BALL_SIZE*0.5,event == 1 ? 0.0 : court_h,1,WHITE)
      } elif event == 3 {
         ball_x = left_face()
         bounce(1.0,player_y,player_v)
      } elif event == 4 {
         ball_x = right_face()
         bounce(-1.0,opponent_y,cpu_v)
      } else { point(event == 6) return 0 }
      n += 1
   }
   0
}
fn step(f64 dir) int {
   pause_fade = lim(pause_fade+(paused ? FIXED : -FIXED)*8.0,0.0,1.0)
   if paused { return 0 }
   sync_pose()
   age_effects()
   intro = lim(intro+FIXED,0.0,0.65)
   player_flash = lim(player_flash-FIXED,0.0,0.18)
   cpu_flash = lim(cpu_flash-FIXED,0.0,0.18)
   player_pop = lim(player_pop-FIXED,0.0,0.55)
   cpu_pop = lim(cpu_pop-FIXED,0.0,0.55)
   player_v = mix(player_v,dir*PADDLE_SPEED,0.16)
   player_y = lim(player_y+player_v*FIXED,0.0,court_h-PADDLE_H)
   if player_y <= 0.0 || player_y >= court_h-PADDLE_H { player_v = 0.0 }
   def f64 goal = opponent_goal(ball_x,ball_y,ball_vx,ball_vy,BALL_SIZE,court_h,PADDLE_H,right_face(),rally)
   cpu_v = mix(cpu_v,lim((goal-opponent_y)*8.0,-OPPONENT_SPEED,OPPONENT_SPEED),0.1)
   opponent_y = lim(opponent_y+cpu_v*FIXED,0.0,court_h-PADDLE_H)
   if opponent_y <= 0.0 || opponent_y >= court_h-PADDLE_H { cpu_v = 0.0 }
   if death_t > 0.0 { death_t = lim(death_t-FIXED,0.0,DEATH_TIME) return 0 }
   if serve_t > 0.0 {
      serve_t = lim(serve_t-FIXED,0.0,SERVE_TIME)
      if serve_t <= 0.0 { launch() }
      return 0
   }
   move_ball(FIXED)
   if death_t <= 0.0 {
      trail_tick += 1
      if trail_tick >= 3 { trail_tick = 0 record_trail() }
   }
   0
}
fn update(f64 dt,f64 dir) {
   acc = lim(acc+lim(dt,0.0,0.05),0.0,0.05)
   mut int n = 0
   while acc >= FIXED && n < 12 { acc -= FIXED step(dir) n += 1 }
}
fn toggle_pause() { paused = !paused sync_pose() }
fn release() { free(effects) free(trail) }
fn check(bool ok,str label) {
   if !ok { eprint("PONG_FAIL "+label) release() exit(1) }
}
fn selftest() {
   reset_game()
   check(ball_x == (VIEW_W-BALL_SIZE)*0.5 && serve_t == SERVE_TIME,"initial serve")
   mut int i = 0
   while i < 200 { step(0.0) i += 1 }
   check(serve_t == 0.0 && ball_vx > 0.0,"automatic serve")
   reset_game()
   serve_t = 0.0
   ball_x,ball_y = left_face()+2.0,player_y+40.0
   ball_vx,ball_vy = -700.0,0.0
   move_ball(0.03)
   check(ball_vx > 0.0 && hits == 1,"swept left hit")
   ball_x,ball_y = right_face()-2.0,opponent_y+40.0
   ball_vx,ball_vy = 700.0,0.0
   move_ball(0.03)
   check(ball_vx < 0.0 && hits == 2,"swept right hit")
   ball_x,ball_y = court_w*0.5,1.0
   ball_vx,ball_vy = 100.0,-500.0
   move_ball(0.02)
   check(ball_y > 0.0 && ball_vy > 0.0,"wall residual time")
   ball_x,ball_y = -BALL_SIZE+1.0,0.0
   ball_vx,ball_vy = -500.0,0.0
   move_ball(0.01)
   check(opponent_score == 1 && death_t == DEATH_TIME && trail_count == 0,"score and destruction")
   def f64 old_y = player_y
   def f64 old_death = death_t
   toggle_pause()
   update(0.05,1.0)
   check(player_y == old_y && death_t == old_death,"pause freezes all gameplay")
   toggle_pause()
   resize_court(1280.0,1800.0)
   check(ball_x == (court_w-BALL_SIZE)*0.5 && player_y >= 0.0,"portrait resize")
   resize_court(2800.0,720.0)
   check(ball_y == (court_h-BALL_SIZE)*0.5 && opponent_y <= court_h-PADDLE_H,"wide resize")
   reset_game()
   i = 0
   while i < FX_CAP+5 { emit(court_w*0.5,court_h*0.5,3,GREEN) i += 1 }
   check(fx_head == 5,"effect wrap")
   i = 0
   while i < 145 { age_effects() i += 1 }
   check(load32(effects,12) == 0,"effect expiry")
   check(ease(0.0) == 0.0 && ease(1.0) == 1.0,"easing endpoints")
   reset_game()
   check(player_score == 0 && opponent_score == 0 && fx_head == 0 && trail_count == 0,"reset clears state")
   print("PONG_OK")
   release()
   exit(0)
}
if process_args.positionals().contains("--selftest") { selftest() }

fn rect(f64 x,f64 y,f64 w,f64 h,int c) {
   def f64 l = lim(x,0.0,court_w)
   def f64 t = lim(y,0.0,court_h)
   def f64 r = lim(x+w,0.0,court_w)
   def f64 b = lim(y+h,0.0,court_h)
   if r > l && b > t { gfx.draw_rect_fast(l*scale,t*scale,(r-l)*scale,(b-t)*scale,c) }
}
fn draw_effects(f64 blend) {
   mut int i = 0
   while i < FX_CAP {
      def int o = i*FX_STRIDE
      def int kind = load32(effects,o+12)
      if kind > 0 {
         def f64 age = (float(load32(effects,o+8))+(paused ? 0.0 : blend))/240.0
         def f64 t = lim(age/(kind == 3 ? 0.6 : 0.3),0.0,1.0)
         def f64 x = getq(effects,o)*court_w
         def f64 y = getq(effects,o+4)*court_h
         def int count = kind == 3 ? 32 : (kind == 2 ? 12 : 8)
         def int seed = load32(effects,o+20)
         def int color = rgba(load32(effects,o+16),(1.0-t)*(1.0-t))
         mut int j = 0
         while j < count {
            def f64 a = float(j)*6.283185307179586/float(count)+float(seed%17)*0.073
            def f64 speed = (kind == 3 ? 240.0 : 120.0)*(0.45+float((seed+j*47)%73)/100.0)
            mut f64 dx = float(math.cos(a))*speed
            mut f64 dy = float(math.sin(a))*speed
            if x < 4.0 { dx = absf(dx) }
            elif x > court_w-4.0 { dx = -absf(dx) }
            if y < 4.0 { dy = absf(dy) }
            elif y > court_h-4.0 { dy = -absf(dy) }
            def f64 travel = age*(1.0-0.42*t)
            def f64 size = (kind == 3 ? 7.0 : 4.0)*(0.25+0.75*(1.0-t))
            rect(x+dx*travel-size*0.5,y+dy*travel-size*0.5,size,size,color)
            j += 1
         }
      }
      i += 1
   }
}
fn draw_paddle(f64 x,f64 y,f64 flash,int color) {
   def f64 t = ease(intro/0.48)
   def f64 k = flash/0.18
   def f64 h = PADDLE_H*t*(1.0+0.08*k)
   def f64 w = PADDLE_W*t*(1.0-0.18*k)
   rect(x+(PADDLE_W-w)*0.5-3.0,y+(PADDLE_H-h)*0.5-3.0,w+6.0,h+6.0,rgba(color,0.12*t))
   rect(x+(PADDLE_W-w)*0.5,y+(PADDLE_H-h)*0.5,w,h,rgba(color,t))
}
fn draw_ball(f64 blend) {
   if death_t > 0.0 { return }
   if serve_t > 0.0 {
      def f64 t = ease(1.0-serve_t/SERVE_TIME)
      def f64 size = BALL_SIZE*(0.25+0.75*t)
      def f64 x = court_w*0.5
      def f64 y = court_h*0.5
      def f64 spread = 76.0*(1.0-t)
      rect(x-size*0.5-spread,y-1.0,spread,2.0,rgba(WHITE,0.45*t*(1.0-t)))
      rect(x+size*0.5,y-1.0,spread,2.0,rgba(WHITE,0.45*t*(1.0-t)))
      rect(x-size*0.5,y-size*0.5,size,size,rgba(WHITE,t))
      return
   }
   mut int j = trail_count-1
   while j >= 0 {
      def int k = (trail_head-1-j+TRAIL_CAP)%TRAIL_CAP
      def f64 t = 1.0-float(j+1)/float(TRAIL_CAP+1)
      def f64 size = BALL_SIZE*(0.2+0.7*t)
      rect(getq(trail,k*8)*court_w-size*0.5,getq(trail,k*8+4)*court_h-size*0.5,size,size,rgba(WHITE,0.24*t*t))
      j -= 1
   }
   def f64 x = mix(prev_x,ball_x,blend)
   def f64 y = mix(prev_y,ball_y,blend)
   rect(x-3.0,y-3.0,BALL_SIZE+6.0,BALL_SIZE+6.0,rgba(WHITE,0.12))
   rect(x,y,BALL_SIZE,BALL_SIZE,rgba(WHITE,1.0))
}
fn draw_scene(int font,int font_small) {
   def f64 blend = paused ? 1.0 : lim(acc/FIXED,0.0,1.0)
   def f64 mid = court_w*0.5
   def f64 reveal = ease(intro/0.65)
   def f64 dash_h = court_h/float(NET_COUNT)
   mut int i = 0
   while i < 26 {
      def f64 y = float(i*2)*dash_h
      def f64 fade = lim(reveal*2.0-absf(y/court_h-0.5)*2.0,0.0,1.0)
      rect(mid-1.0,y,2.0,dash_h*0.52,rgba(0x696969,fade*0.46))
      i += 1
   }
   draw_paddle(margin(),mix(prev_player,player_y,blend),player_flash,GREEN)
   draw_paddle(court_w-margin()-PADDLE_W,mix(prev_cpu,opponent_y,blend),cpu_flash,PURPLE)
   draw_ball(blend)
   draw_effects(blend)
   if pause_fade > 0.0 { rect(0.0,0.0,court_w,court_h,rgba(0,0.78*smooth(pause_fade))) }
   def f64 gap = lim(court_w*0.15,100.0,170.0)
   if font_small > 0 {
      gfx.draw_text_centered(font_small,"YOU",(mid-gap)*scale,16.0*scale,PLAYER)
      gfx.draw_text_centered(font_small,"CPU",(mid+gap)*scale,16.0*scale,CPU)
      gfx.draw_text_centered(font_small,"W/S OR ARROWS  /  SPACE PAUSE  /  R RESET",mid*scale,(court_h-30.0)*scale,MUTED)
   }
   if font > 0 {
      gfx.draw_text_centered(font,to_str(player_score),(mid-gap)*scale,(52.0-9.0*player_pop/0.55)*scale,player_pop > 0.0 ? PLAYER : FG)
      gfx.draw_text_centered(font,to_str(opponent_score),(mid+gap)*scale,(52.0-9.0*cpu_pop/0.55)*scale,cpu_pop > 0.0 ? CPU : FG)
      if pause_fade > 0.0 { gfx.draw_text_centered(font,"PAUSED",mid*scale,(court_h*0.5-24.0)*scale,gfx.color_alpha(FG,smooth(pause_fade))) }
   }
   if font_small > 0 && !paused && serve_t > 0.0 && death_t <= 0.0 {
      gfx.draw_text_centered(font_small,"GET READY",mid*scale,(court_h*0.5+48.0)*scale,gfx.color_alpha(MUTED,smooth(1.0-serve_t/SERVE_TIME)))
   }
}

gfx.apply_backend_env()
gfx.apply_backend_argv()

mut win = gfx.init_window(WIN_W, WIN_H, "Pong - Nytrix", WINDOW_MAXIMIZE, true, false, 1)
if !win { panic("could not create the Pong window") }
window.set_cursor_mode(win,window.CURSOR_NORMAL)
if is_dict(win) && !!win.get("_mock", false) && !common.env_truthy("NYTRIX_UI_REPLAY") {
   if !gfx.begin_frame_clear(BG) { panic("Pong renderer could not begin a frame") }
   if !gfx.end_frame() { panic("Pong: presentation failed") }
   gfx.close_window()
   release()
   exit(0)
}

fixture.ready("pong")
def headless = gfx.get_active_backend_name() == "mock"
def int legacy_frames = common.env_int_clamped("NYTRIX_PONG_MAX_FRAMES", 0, 0, 1000000)
def int dump_delay = ui_dump.auto_dump_delay_frames(8)
def int legacy_timeout = common.env_int_clamped("NYTRIX_PONG_TIMEOUT_MS", headless ? 1000 : 0, 0, 3600000)
def fx = fixture.start_ui(legacy_frames, legacy_timeout, headless && !common.env_present("NYTRIX_PONG_MAX_FRAMES"))
def bool replay = common.env_truthy("NYTRIX_UI_REPLAY")
def bool trace = common.env_truthy("NYTRIX_PONG_TRACE")
def int trace_every = common.env_int_clamped("NYTRIX_PONG_TRACE_EVERY", 30, 1, 600)
mut int font = 0
mut int font_small = 0
mut int score_font_size = 0
mut int label_font_size = 0

mut int last_tick = ticks()
mut int surface_failures = 0
mut int font_retry = 0
reset_game()

while !gfx.window_should_close(win) && fixture.tick_ui(fx) {
   def int frame_started = ticks()
   window.poll_events()
   if fixture.quit_key(fixture.next_ui_key(fx)) { break }
   def fb = window.get_framebuffer_size(win)
   def f64 w = float(fb.get(0,WIN_W))
   def f64 h = float(fb.get(1,WIN_H))
   if w <= 0.0 || h <= 0.0 { last_tick = ticks() msleep(16) continue }
   scale = float(math.min(w/VIEW_W,h/VIEW_H))
   resize_court(w/scale,h/scale)
   def int next_score_size = int(lim(float(math.floor(48.0*scale+0.5)),1.0,256.0))
   def int next_label_size = int(lim(float(math.floor(19.0*scale+0.5)),1.0,128.0))
   if font_retry > 0 { font_retry -= 1 }
   if font_retry == 0 && (next_score_size != score_font_size || next_label_size != label_font_size) {
      def nf = gfx.font_load_first(["etc/assets/fonts/maplemono.ttf","etc/assets/fonts/monocraft.ttf"],next_score_size,gfx.FONT_FILTER_DEFAULT,"0123456789PAUSED")
      def ns = gfx.font_load_first(["etc/assets/fonts/maplemono.ttf","etc/assets/fonts/monocraft.ttf"],next_label_size,gfx.FONT_FILTER_DEFAULT,"YOU CPU W/S OR ARROWS SPACE PAUSE R RESET GET READY /")
      if nf > 0 && ns > 0 {
         if font > 0 { gfx.font_destroy(font) }
         if font_small > 0 { gfx.font_destroy(font_small) }
         font,font_small = nf,ns
         score_font_size,label_font_size = next_score_size,next_label_size
         gfx.font_prepare(font,"0123456789PAUSED")
         gfx.font_prepare(font_small,"YOU CPU W/S OR ARROWS SPACE PAUSE R RESET GET READY /")
      } else {
         if nf > 0 { gfx.font_destroy(nf) }
         if ns > 0 { gfx.font_destroy(ns) }
         font_retry = 30
      }
   }
   if !gfx.begin_frame_clear(BG) {
      surface_failures += 1
      last_tick = ticks()
      if surface_failures >= 120 { eprint("Pong: surface unavailable") break }
      msleep(16) continue
   }
   surface_failures = 0
   gfx.set_ortho_2d(0.0,w,h,0.0)
   def int now = ticks()
   def f64 dt = replay ? 1.0/120.0 : lim(float(now-last_tick)/1000000000.0,0.0,0.05)
   last_tick = now
   def bool pause_down = window.key_down(win,KEY_SPACE) || window.key_down(win,KEY_ENTER)
   def bool reset_down = window.key_down(win,KEY_R)
   if window.key_down(win,KEY_ESCAPE) { window.set_should_close(win,true) }
   if reset_down && !previous_reset { reset_game() }
   elif pause_down && !previous_pause { toggle_pause() }
   previous_pause,previous_reset = pause_down,reset_down
   mut f64 dir = 0.0
   if window.key_down(win,KEY_W) || window.key_down(win,KEY_UP) { dir -= 1.0 }
   if window.key_down(win,KEY_S) || window.key_down(win,KEY_DOWN) { dir += 1.0 }
   update(dt,dir)
   draw_scene(font,font_small)

   if replay || (trace && replay_frame % trace_every == 0) {
      def live_window_size = window.size(win)
      def live_framebuffer_size = window.get_framebuffer_size(win)
      print("NYTRIX_UI_APP_STATE demo=pong frame=" + to_str(replay_frame + 1) +
         " scale=" + to_str(scale) + " ball=" + to_str(ball_x*scale) + "," + to_str(ball_y*scale) +
         " velocity=" + to_str(ball_vx*scale) + "," + to_str(ball_vy*scale) +
         " paddles=" + to_str(player_y*scale) + "," + to_str(opponent_y*scale) +
         " score=" + to_str(player_score) + ":" + to_str(opponent_score) +
         " serve=" + to_str(serve_t) + " rally=" + to_str(rally) +
         " fonts=" + to_str(score_font_size) + "," + to_str(label_font_size) +
         " dash=" + to_str(h/51.0) + " size=" + to_str(w) + "x" + to_str(h) +
         " api=" + to_str(live_window_size.get(0, 0)) + "x" + to_str(live_window_size.get(1, 0)) +
         " fb=" + to_str(live_framebuffer_size.get(0, 0)) + "x" + to_str(live_framebuffer_size.get(1, 0)) +
         " paused=" + to_str(paused) + " w=" + to_str(window.key_down(win, KEY_W)) +
         " s=" + to_str(window.key_down(win, KEY_S)))
   }
   ui_dump.auto_dump_pre_frame(dump_done, replay_frame, dump_delay)
   if !gfx.end_frame() { eprint("Pong: presentation failed") surface_failures = 120 break }
   replay_frame += 1
   dump_done = ui_dump.auto_dump_post_frame(win, dump_done, replay_frame, dump_delay, "build/cache/fb/ui/pong.png")
   if first_frame {
      if !window.show(win) { eprint("Pong: window could not be shown") surface_failures = 120 break }
      first_frame = false
   }
   if frame_times {
      print("PONG_FRAME_TIME frame=" + to_str(replay_frame) +
         " elapsed_ns=" + to_str(ticks() - startup_started))
   }
   if !replay {
      def int remaining = 8333333 - (ticks() - frame_started)
      if remaining > 0 { msleep((remaining + 999999) / 1000000) }
   }
}

release()
if font > 0 { gfx.font_destroy(font) }
if font_small > 0 { gfx.font_destroy(font_small) }
gfx.close_window()
exit(surface_failures >= 120 ? 1 : fixture.exit_status(fx))
