#!/bin/ny
;; Features: Exact Raylib Input, Julia Explorer, Smooth AA, and Safe Memory Layout.

use std.core *
use std.math *
use std.ui.consts *
use std.ui.gfx *
use std.ui.window as window
use std.ui.input as uin
use std.os *
use std.os.time *

;; Exact Nytrix Vertex Layout to prevent memory mismatch
def VERT_SRC = "#version 450
layout(location=0) in vec3 inPos ;
layout(location=1) in vec2 inUV ;
layout(location=2) in vec4 inColor ;
layout(location=3) in uint inTexIndex ;
layout(location=4) in vec3 inNormal ;

layout(push_constant) uniform PC {
   mat4 vp ;
   mat4 model ;
   int isMask ;
   int isUnlit ;

   float cx ; float cy; float zoom; int max_i;
   float aspect ; float mx; float my; float mode;
   float jx ; float jy;
} pc ;

layout(location=0) out vec2 vUV ;

void main(){
  gl_Position = pc.vp * pc.model * vec4(inPos, 1.0) ;
  vUV = inUV ;
}"

def FRAG_SRC = "#version 450
layout(location=0) in vec2 vUV ;

layout(push_constant) uniform PC {
   mat4 vp ;
   mat4 model ;
   int isMask ;
   int isUnlit ;

   float cx ; float cy; float zoom; int max_i;
   float aspect ; float mx; float my; float mode;
   float jx ; float jy;
} pc ;

layout(location=0) out vec4 outColor ;

const float PI = 3.14159265358979323846 ;

void main(){
   vec2 uv = vec2((vUV.x - 0.5) * 3.0 * pc.aspect, (vUV.y - 0.5) * 3.0) ;

   vec2 coord = (uv / pc.zoom) + vec2(pc.cx, pc.cy) ;
   vec2 c, z ;

   if(pc.mode < 0.5){
      c = coord ;
      z = vec2(0.0) ;
   } else {
      z = coord ;
      c = vec2(pc.jx, pc.jy) ;
   }

   float a = z.x ;
   float b = z.y ;
   int iter = 0 ;

   const float escape = 1024.0 ;
   float r2 = 0.0 ;

   while(iter < pc.max_i){
      float aa = a * a ;
      float bb = b * b ;
      r2 = aa + bb ;
      if(r2 > escape) break ;
      b = 2.0 * a * b + c.y ;
      a = aa - bb + c.x ;
      iter++ ;
   }

   if(iter >= pc.max_i){
      outColor = vec4(0.0, 0.0, 0.0, 1.0) ;
   } else {
      float smooth_i = float(iter) - log2(log2(max(r2, 0.0001))) + 4.0 ;
      vec3 col = vec3(
         sin(smooth_i / 55.0 * PI),
         sin(smooth_i / 69.0 * PI),
         sin(smooth_i / 40.0 * PI)
      ) ;

      float d = distance(vUV, vec2(pc.mx, pc.my)) ;
      col += 0.2 * exp(-d * 15.0) ;

      outColor = vec4(clamp(col, 0.0, 1.0), 1.0) ;
   }
}"

mut win = 0
mut pipe = 0
mut pc_data = 0
mut font = 0

def starting_cx = -0.5
def starting_cy = 0.0
def starting_zoom = 0.6

mut cx = starting_cx
mut cy = starting_cy
mut zoom = starting_zoom

mut cur_jx = -0.348827
mut cur_jy = 0.607167

mut mode = 0
mut max_iterations = 333
mut iter_multiplier = 166.5
mut inc_speed = 0.0

def poi_m_cx = [-1.76826775,  0.322004497, -0.748880744, -1.78385007, -0.0985441282,  0.317785531]
def poi_m_cy = [-0.00422996, -0.035709988, -0.056295577, -0.01562006, -0.9246886970, -0.032261222]
def poi_m_zm = [ 28435.9238,  56499.72660,  9237.590820,  14599.5283,  26259.853500,  29297.92580]
def poi_j_cx = [-0.348827, -0.786268, -0.8,   0.285, -0.835, -0.70176]
def poi_j_cy = [ 0.607167,  0.169728,  0.156, 0.0,   -0.2321, -0.3842]

fn run(){
   renderer_config(true, false, "", "", 4)
   win = init_window(1280, 720, "Nytrix Fractal", 0, true, false, 4)
   if(!win){ exit(1) }

   font = font_load("etc/assets/font/monocraft.ttf", 16)
   pipe = create_pipeline(compile_shader(VERT_SRC, "vert"), compile_shader(FRAG_SRC, "frag"), 3, 0, 0, 0, 0, 0, 0)

   pc_data = sys_malloc(128)
   memset(pc_data, 0, 128)

   mut last_t = ticks()

   while(!window.should_close(win)){
      def now = ticks()
      mut dt = float(now - last_t) / 1000000000.0
      if(dt > 0.1 || dt <= 0.0){
         dt = 0.016
      }
      last_t = now

      window.poll_events()
      def size = window.size(win)

      mut ww = float(get(size, 0, 1280))
      mut wh = float(get(size, 1, 720))
      if(ww < 1.0){ ww = 1280.0 }
      if(wh < 1.0){ wh = 720.0 }
      def asp = ww / wh

      def mpos = window.cursor_pos(win)
      def mx = float(get(mpos, 0)) / ww
      def my = float(get(mpos, 1)) / wh

      mut e = window.check_event(win)
      while(e != 0){
         def typ = window.event_type(e)
         if(typ == EVENT_QUIT){
            window.set_should_close(win, true)
         }
         e = window.check_event(win)
      }

      ;; Input Handling
      if(window.key_pressed(win, uin.KEY_TAB)){
         if(mode == 0){ mode = 1 } else { mode = 0 }
      }

      if(window.key_pressed(win, uin.KEY_R)){
         cx = starting_cx
         cy = starting_cy
         zoom = starting_zoom
      }

      if(window.key_pressed(win, uin.KEY_UP)){
         iter_multiplier *= 1.4
      }
      if(window.key_pressed(win, uin.KEY_DOWN)){
         iter_multiplier /= 1.4
      }

      ;; POI Switching
      mut k = 0
      while(k < 6){
         if(window.key_pressed(win, 49 + k)){
            if(mode == 0){
               cx = get(poi_m_cx, k)
               cy = get(poi_m_cy, k)
               zoom = get(poi_m_zm, k)
            } else {
               cur_jx = get(poi_j_cx, k)
               cur_jy = get(poi_j_cy, k)
            }
         }
         k += 1
      }

      ;; Raylib Exact Velocity Zoom
      def zoom_speed = 4.0
      mut zoom_factor = 1.0
      mut zooming = false

      if(window.mouse_down(win, 0)){
         zoom_factor = pow(zoom_speed, dt)
         zooming = true
      }
      if(window.mouse_down(win, 1)){
         zoom_factor = 1.0 / pow(zoom_speed, dt)
         zooming = true
      }

      if(zooming){
         zoom *= zoom_factor

         def offset_speed = 1.5
         def vel_x = (mx - 0.5) * offset_speed * asp / zoom
         def vel_y = (my - 0.5) * offset_speed / zoom

         cx += vel_x * dt
         cy += vel_y * dt
      }

      ;; Panning and Julia Morphing
      if(mode == 1){
         if(window.key_down(win, uin.KEY_RIGHT)){ inc_speed += dt * 2.0 }
         if(window.key_down(win, uin.KEY_LEFT)){ inc_speed -= dt * 2.0 }
         if(window.key_pressed(win, uin.KEY_SPACE)){ inc_speed = 0.0 }

         if(abs(inc_speed) > 0.001){
            cur_jx += dt * inc_speed * 0.1
            cur_jy += dt * inc_speed * 0.1
         }
      } else {
         def pan_speed = 1.5 / zoom
         if(window.key_down(win, uin.KEY_A)){ cx -= pan_speed * dt }
         if(window.key_down(win, uin.KEY_D)){ cx += pan_speed * dt }
         if(window.key_down(win, uin.KEY_W)){ cy -= pan_speed * dt }
         if(window.key_down(win, uin.KEY_S)){ cy += pan_speed * dt }
      }

      ;; Dynamic Iteration Calculation (Syntax Safe)
      mut safe_zoom = zoom
      if(safe_zoom < 1.0){ safe_zoom = 1.0 }

      max_iterations = int((200.0 + log(safe_zoom) * 100.0) * iter_multiplier)
      if(max_iterations < 32){ max_iterations = 32 }
      if(max_iterations > 10000){ max_iterations = 10000 }

      begin_frame_clear([0.0, 0.0, 0.0, 1.0])
      set_ortho_2d(0.0, ww, 0.0, wh)

      if(pipe != 0){
         bind_pipeline(pipe)

         ;; [FIXED]: Store all 10 variables into the byte buffer
         store32_f32(pc_data, cx, 0)
         store32_f32(pc_data, cy, 4)
         store32_f32(pc_data, zoom, 8)
         store32(pc_data, max_iterations, 12)
         store32_f32(pc_data, asp, 16)
         store32_f32(pc_data, mx, 20)
         store32_f32(pc_data, my, 24)
         store32_f32(pc_data, float(mode), 28)
         store32_f32(pc_data, cur_jx, 32)
         store32_f32(pc_data, cur_jy, 36)

         ;; [FIXED]: Push exactly 40 bytes to ensure no variables are dropped!
         push_constants(pc_data, 40, 136)
      }

      draw_rect_tex(0.0, 0.0, ww, wh, -1, 1.0, 1.0, 1.0, 1.0)

      if(pipe != 0){
         reset_pipeline()
      }

      if(font != 0){
         mut mode_str = "Mandelbrot"
         if(mode == 1){ mode_str = "Julia" }

         draw_text(font, mode_str, 20.0, 20.0, YELLOW)
         draw_text(font, "Zoom: " + to_str(zoom), 20.0, 45.0, WHITE)
         draw_text(font, "Iter: " + to_str(max_iterations), 20.0, 70.0, GREEN)

         if(mode == 1){
            draw_text(font, "C: " + to_str(cur_jx) + ", " + to_str(cur_jy), 20.0, 95.0, CYAN)
         }

         draw_text(font, "TAB: Mode | 1-6: POI | R: Reset", 20.0, wh - 50.0, GRAY)
         draw_text(font, "LMB/RMB: Zoom | Arrows: Julia Morph | WASD: Pan", 20.0, wh - 25.0, GRAY)
      }

      end_frame()
   }
}

run()
exit(0)
