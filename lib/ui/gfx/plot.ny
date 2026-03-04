;; Keywords: ui gfx plot math scientific telemetry
;; Total Architectural Fix for Scientific Plotting (Core-Hardened)

module std.ui.gfx.plot (
   create, set_title,
   line, scatter, bar, area, surface,
   math_fn, show, show_3d
)

use std.core.primitives *
use std.core.dict_mod *
use std.ui.gfx *
use std.math *
use std.str *

;; --- Hardened Low-Level Access ---
@no_inline fn _len(l){ if(!l || __is_int(l)){ return 0 } if(__tagof(l) != 100){ return 0 } __load64_idx(l, 0) }
@no_inline fn _get(l, i){ __load64_idx(l, 16 + i * 8) }
fn _map(v, i0, i1, o0, o1){ if(abs(i1 - i0) < 1e-7){ return o0 } o0 + (float(v) - i0) * (o1 - o0) / (i1 - i0) }

;; --- Context Factory ---
fn create(w, h){
   mut c = dict(64)
   c = dict_set(c, "xmin", 0.0) c = dict_set(c, "xmax", 0.0)
   c = dict_set(c, "ymin", 0.0) c = dict_set(c, "ymax", 0.0)
   c = dict_set(c, "zmin", -1.0) c = dict_set(c, "zmax", 1.0)
   c = dict_set(c, "has_data", false)
   c = dict_set(c, "series",  [])
   c = dict_set(c, "title",  "ANALYTIC HUB")
   c
}

fn set_title(c, t){ dict_set(c, "title", t) }

@no_inline fn _upd_bounds(c, xl, yl, zl=0){
   mut x0 = float(dict_get(c,"xmin",0)) mut x1 = float(dict_get(c,"xmax",0))
   mut y0 = float(dict_get(c,"ymin",0)) mut y1 = float(dict_get(c,"ymax",0))
   mut z0 = float(dict_get(c,"zmin",0)) mut z1 = float(dict_get(c,"zmax",0))
   mut ok = dict_get(c, "has_data", false)
   mut i = 0 def n = _len(xl)
   while(i < n){
      def x = float(_get(xl, i)) def y = float(_get(yl, i))
      mut z = 0.0 if(zl != 0){ z = float(_get(zl, i)) }
      if(!ok){ x0=x x1=x y0=y y1=y z0=z z1=z ok=true }
      else {
         if(x < x0){ x0 = x } if(x > x1){ x1 = x }
         if(y < y0){ y0 = y } if(y > y1){ y1 = y }
         if(z < z0){ z0 = z } if(z > z1){ z1 = z }
      }
      i = i + 1
   }
   c = dict_set(c, "xmin", x0) c = dict_set(c, "xmax", x1)
   c = dict_set(c, "ymin", y0) c = dict_set(c, "ymax", y1)
   c = dict_set(c, "zmin", z0) c = dict_set(c, "zmax", z1)
   c = dict_set(c, "has_data", ok)
   c
}

fn _add(c, t, x, y, col, z=0){
   c = _upd_bounds(c, x, y, z)
   mut s = dict(8) s = dict_set(s, "t", t) s = dict_set(s, "x", x) s = dict_set(s, "y", y) s = dict_set(s, "c", col)
   if(z != 0){ s = dict_set(s, "z", z) }
   c = dict_set(c, "series", append(dict_get(c, "series", []), s))
   c
}

fn line(c, x, y, col=0){ if(!col){ col = [0.4, 0.8, 1, 1] } _add(c, "line", x, y, col) }
fn scatter(c, x, y, col=0){ if(!col){ col = [1, 0.4, 0, 1] } _add(c, "scatter", x, y, col) }
fn bar(c, x, y, col=0){ if(!col){ col = [0.4, 1, 0.5, 0.8] } _add(c, "bar", x, y, col) }
fn area(c, x, y, col=0){ if(!col){ col = [0.3, 0.6, 1, 0.4] } _add(c, "area", x, y, col) }
fn surface(c, x, y, z, col=0){ if(!col){ col = CYAN } _add(c, "surface", x, y, col, z) }

fn math_fn(c, f, x0, x1, res=60, col=0){
   mut xv = [] mut yv = [] mut x = float(x0) def dx = (float(x1)-float(x0))/float(res)
   mut i = 0 while(i <= res){ xv = append(xv, x) yv = append(yv, f(x)) x = x + dx i = i + 1 }
   line(c, xv, yv, col)
}

@no_inline fn _draw_primitive_2d(s, vx0, vx1, vy0, vy1, gx, gy, gw, gh){
   def type = dict_get(s, "t", "")
   def xv = dict_get(s, "x", []) def yv = dict_get(s, "y", []) def col = dict_get(s, "c", WHITE)
   mut lx = 0.0 mut ly = 0.0 mut j = 0 mut n = _len(xv)
   while(j < n){
      def cx = _map(_get(xv, j), vx0, vx1, gx, gx + gw)
      def cy = _map(_get(yv, j), vy0, vy1, gy + gh, gy)
      if(type == "line" && j > 0){ draw_line_2d(lx, ly, cx, cy, col, 2.0) }
      elif(type == "area" && j > 0){
         draw_quad_2d(lx, ly, cx, cy, cx, gy+gh, lx, gy+gh, col)
         draw_line_2d(lx, ly, cx, cy, WHITE, 1.5) ;; Highlight edge
      }
      elif(type == "scatter"){ draw_circle(cx, cy, 3, col, 8) }
      elif(type == "bar"){ def bw = (gw/max(1.0,float(n)))*0.7 draw_rectangle(cx-bw/2, cy, bw, (gy+gh)-cy, col) }
      lx = cx ly = cy j = j + 1
   }
}

fn show(c, px, py, cw, ch){
   mut x0 = float(dict_get(c,"xmin",0)) mut x1 = float(dict_get(c,"xmax",1))
   mut y0 = float(dict_get(c,"ymin",0)) mut y1 = float(dict_get(c,"ymax",1))
   if(abs(x1 - x0) < 1e-4){ x1 = x0 + 1.0 x0 = x0 - 1.0 }
   if(abs(y1 - y0) < 1e-4){ y1 = y0 + 1.0 y0 = y0 - 1.0 }
   def xm = (x1 - x0) * 0.1 def ym = (y1 - y0) * 0.1
   def vx0 = x0-xm def vx1 = x1+xm def vy0 = y0-ym def vy1 = y1+ym

   draw_rect_rounded(px, py, cw, ch, 8, [0.08, 0.09, 0.12, 1.0])
   draw_rectangle(px, py, cw, 28, [0.12, 0.14, 0.18, 1.0])
   draw_text(0, dict_get(c, "title", ""), px + 12, py + 8, [0.7, 0.8, 1, 1])
   def gx = px + cw * 0.12 def gy = py + 45.0 def gw = cw * 0.82 def gh = ch - 85.0
   draw_rectangle(gx, gy, gw, gh, [0.04, 0.04, 0.06, 1.0])
   def skv = dict_get(c, "series", [])
   mut si = 0 while(si < _len(skv)){ _draw_primitive_2d(_get(skv, si), vx0, vx1, vy0, vy1, gx, gy, gw, gh) si = si + 1 }
}

fn show_3d(c, px, py, cw, ch){
   draw_rect_rounded(px, py, cw, ch, 8, [0.05, 0.05, 0.08, 1.0])
   draw_rectangle(px, py, cw, 28, [0.1, 0.1, 0.15, 1.0])
   draw_text(0, dict_get(c, "title", "3D"), px + 12, py + 8, CYAN)
   def cx = px+cw/2 def cy = py+ch/2 def sc = min(cw, ch)*0.4
   def bx0 = float(dict_get(c, "xmin", -1)) def bx1 = float(dict_get(c, "xmax", 1))
   def by0 = float(dict_get(c, "ymin", -1)) def by1 = float(dict_get(c, "ymax", 1))
   def bz0 = float(dict_get(c, "zmin", -1)) def bz1 = float(dict_get(c, "zmax", 1))
   def skv = dict_get(c, "series", [])
   mut si = 0 while(si < _len(skv)){
      def s = _get(skv, si) def xv = dict_get(s, "x", []) def yv = dict_get(s, "y", []) def zv = dict_get(s, "z", 0) def col = dict_get(s, "c", RED)
      mut i = 0 while(i < _len(xv)){
         def nx = _map(_get(xv,i), bx0, bx1, -1, 1) def ny = _map(_get(yv,i), by0, by1, -1, 1)
         def nz = (zv!=0)?_map(_get(zv,i), bz0, bz1, -1, 1):0
         def pz_x = cx + (nx + nz * 0.5) * sc def pz_y = cy + (ny * -1 + nz * 0.4) * sc
         draw_rectangle(pz_x - 2, pz_y - 2, 4, 4, col) i = i + 1
      }
      si = si + 1
   }
}
