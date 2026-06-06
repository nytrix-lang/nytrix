;; Keywords: viewer gizmo transform axis rotate os ui render
;; Shared transform gizmo geometry, handles, and drawing helpers.
;; References:
;; - std.os.ui.render.viewer.gui
;; - std.os.ui.render.matrix
module std.os.ui.render.viewer.gizmo(compact_view, metrics, draw_box, draw_box_corners, draw_axes, project_point, hit_test, draw_overlay)
use std.core
use std.math (PI, abs, clamp, cos, max, min, sin, sqrt)
use std.os.ui.render.dump as ui_profile
use std.os.ui.render as render

fn compact_view(win_w, win_h) bool {
   "Returns whether the viewport needs compact gizmos."
   min(float(win_w), float(win_h)) < 560.0
}

fn draw_box(minx, miny, minz, maxx, maxy, maxz, col, thickness) bool {
   "Draws a world-space bounds box."
   render.draw_line([minx, miny, minz], [maxx, miny, minz], col, thickness)
   render.draw_line([minx, maxy, minz], [maxx, maxy, minz], col, thickness)
   render.draw_line([minx, miny, maxz], [maxx, miny, maxz], col, thickness)
   render.draw_line([minx, maxy, maxz], [maxx, maxy, maxz], col, thickness)
   render.draw_line([minx, miny, minz], [minx, maxy, minz], col, thickness)
   render.draw_line([maxx, miny, minz], [maxx, maxy, minz], col, thickness)
   render.draw_line([minx, miny, maxz], [minx, maxy, maxz], col, thickness)
   render.draw_line([maxx, miny, maxz], [maxx, maxy, maxz], col, thickness)
   render.draw_line([minx, miny, minz], [minx, miny, maxz], col, thickness)
   render.draw_line([maxx, miny, minz], [maxx, miny, maxz], col, thickness)
   render.draw_line([minx, maxy, minz], [minx, maxy, maxz], col, thickness)
   render.draw_line([maxx, maxy, minz], [maxx, maxy, maxz], col, thickness)
   true
}

fn _draw_corner_lines(f64 x, f64 y, f64 z, f64 sx, f64 sy, f64 sz, f64 lx, f64 ly, f64 lz, any col, any thickness) bool {
   render.draw_line([x, y, z], [x + sx * lx, y, z], col, thickness)
   render.draw_line([x, y, z], [x, y + sy * ly, z], col, thickness)
   render.draw_line([x, y, z], [x, y, z + sz * lz], col, thickness)
   true
}

fn draw_box_corners(minx, miny, minz, maxx, maxy, maxz, col, thickness, any corner_len=0.0) bool {
   "Draws a world-space bounds box with emphasized corners."
   def dx = max(0.001, abs(float(maxx) - float(minx)))
   def dy = max(0.001, abs(float(maxy) - float(miny)))
   def dz = max(0.001, abs(float(maxz) - float(minz)))
   def span = max(dx, max(dy, dz))
   def want = float(corner_len) > 0.0 ? float(corner_len) : clamp(span * 0.16, 0.22, span * 0.34)
   def lx = min(dx * 0.42, want)
   def ly = min(dy * 0.42, want)
   def lz = min(dz * 0.42, want)
   _draw_corner_lines(minx, miny, minz, 1.0, 1.0, 1.0, lx, ly, lz, col, thickness)
   _draw_corner_lines(maxx, miny, minz, -1.0, 1.0, 1.0, lx, ly, lz, col, thickness)
   _draw_corner_lines(minx, maxy, minz, 1.0, -1.0, 1.0, lx, ly, lz, col, thickness)
   _draw_corner_lines(maxx, maxy, minz, -1.0, -1.0, 1.0, lx, ly, lz, col, thickness)
   _draw_corner_lines(minx, miny, maxz, 1.0, 1.0, -1.0, lx, ly, lz, col, thickness)
   _draw_corner_lines(maxx, miny, maxz, -1.0, 1.0, -1.0, lx, ly, lz, col, thickness)
   _draw_corner_lines(minx, maxy, maxz, 1.0, -1.0, -1.0, lx, ly, lz, col, thickness)
   _draw_corner_lines(maxx, maxy, maxz, -1.0, -1.0, -1.0, lx, ly, lz, col, thickness)
   true
}

fn _draw_axis(px, py, pz, ax, ay, az, length, col, thickness, handle_size, compact) bool {
   def ex, ey = px + ax * length, py + ay * length
   def ez = pz + az * length
   render.draw_line([px, py, pz], [ex, ey, ez], col, thickness)
   if(!compact){ render.draw_cube([ex, ey, ez], handle_size, col) }
   true
}

fn _draw_move_tip(px, py, pz, ax, ay, az, length, col, thickness, size, compact) bool {
   if(compact){ return true }
   def ex, ey = px + ax * length, py + ay * length
   def ez = pz + az * length
   def back = max(size * 1.8, length * 0.075)
   def side = back * 0.42
   if(abs(ax) > 0.5){
      render.draw_line([ex, ey, ez], [ex - ax * back, ey + side, ez], col, thickness)
      render.draw_line([ex, ey, ez], [ex - ax * back, ey - side, ez], col, thickness)
      render.draw_line([ex, ey, ez], [ex - ax * back, ey, ez + side], col, thickness)
      render.draw_line([ex, ey, ez], [ex - ax * back, ey, ez - side], col, thickness)
   } elif(abs(ay) > 0.5){
      render.draw_line([ex, ey, ez], [ex + side, ey - ay * back, ez], col, thickness)
      render.draw_line([ex, ey, ez], [ex - side, ey - ay * back, ez], col, thickness)
      render.draw_line([ex, ey, ez], [ex, ey - ay * back, ez + side], col, thickness)
      render.draw_line([ex, ey, ez], [ex, ey - ay * back, ez - side], col, thickness)
   } else {
      render.draw_line([ex, ey, ez], [ex + side, ey, ez - az * back], col, thickness)
      render.draw_line([ex, ey, ez], [ex - side, ey, ez - az * back], col, thickness)
      render.draw_line([ex, ey, ez], [ex, ey + side, ez - az * back], col, thickness)
      render.draw_line([ex, ey, ez], [ex, ey - side, ez - az * back], col, thickness)
   }
   true
}

fn _draw_move_axis(px, py, pz, ax, ay, az, length, col, thickness, handle_size, compact) bool {
   def ex, ey = px + ax * length, py + ay * length
   def ez = pz + az * length
   render.draw_line([px, py, pz], [ex, ey, ez], col, thickness)
   _draw_move_tip(px, py, pz, ax, ay, az, length, col, thickness, handle_size, compact)
}

fn _ring_steps(win_w, win_h) int {
   if(ui_profile.env_present_cached("NY_UI_GIZMO_RING_STEPS")){
      return ui_profile.env_int_cached("NY_UI_GIZMO_RING_STEPS", 48, 12, 96)
   }
   def px = max(1.0, min(float(win_w), float(win_h)))
   if(px < 520.0){ return 24 }
   if(px < 900.0){ return 36 }
   48
}

fn _draw_ring(px, py, pz, radius, plane, col, thickness, win_w, win_h) bool {
   def steps = _ring_steps(win_w, win_h)
   mut i = 0
   mut prev = [px, py, pz]
   while(i <= steps){
      def a = (float(i) / float(steps)) * (PI * 2.0)
      mut p = [px, py, pz]
      if(plane == 0){ p = [px, py + cos(a) * radius, pz + sin(a) * radius] }
      elif(plane == 1){ p = [px + cos(a) * radius, py, pz + sin(a) * radius] }
      else { p = [px + cos(a) * radius, py + sin(a) * radius, pz] }
      if(i > 0){ render.draw_line(prev, p, col, thickness) }
      prev = p
      i += 1
   }
   true
}

fn _draw_ring_band(px, py, pz, radius, plane, col, thickness, win_w, win_h) bool {
   def halo = [float(col.get(0, 1.0)), float(col.get(1, 1.0)), float(col.get(2, 1.0)), float(col.get(3, 1.0)) * 0.22]
   _draw_ring(px, py, pz, radius, plane, halo, thickness * 2.40, win_w, win_h)
   _draw_ring(px, py, pz, radius, plane, col, thickness, win_w, win_h)
   true
}

fn _screen_norm(f64 dx, f64 dy) list {
   def len2 = dx * dx + dy * dy
   if(len2 <= 0.000001){ return [0.0, 0.0] }
   def inv = 1.0 / sqrt(len2)
   [dx * inv, dy * inv]
}

fn _screen_dist2(f64 px, f64 py, f64 x0, f64 y0, f64 x1, f64 y1) f64 {
   def vx, vy = x1 - x0, y1 - y0
   def wx, wy = px - x0, py - y0
   def len2 = vx * vx + vy * vy
   if(len2 <= 0.000001){ return wx * wx + wy * wy }
   def t = clamp((wx * vx + wy * vy) / len2, 0.0, 1.0)
   def cx, cy = x0 + vx * t, y0 + vy * t
   def dx, dy = px - cx, py - cy
   dx * dx + dy * dy
}

fn _hit_threshold(any win_w, any win_h) f64 {
   clamp(min(float(win_w), float(win_h)) * 0.018, 10.0, 22.0)
}

fn _pick_axis_line(any mvp, any win_w, any win_h, f64 mx, f64 my, f64 px, f64 py, f64 pz, f64 ax, f64 ay, f64 az, f64 length, int axis, f64 best_d2) dict {
   def a = project_point(mvp, win_w, win_h, px, py, pz)
   def b = project_point(mvp, win_w, win_h, px + ax * length, py + ay * length, pz + az * length)
   if(!is_list(a) || !is_list(b)){ return {"axis": -1, "dist2": best_d2, "screen_axis_x": 0.0, "screen_axis_y": 0.0} }
   def x0, y0 = float(a.get(0, 0.0)), float(a.get(1, 0.0))
   def x1, y1 = float(b.get(0, 0.0)), float(b.get(1, 0.0))
   def d2 = _screen_dist2(mx, my, x0, y0, x1, y1)
   if(d2 >= best_d2){ return {"axis": -1, "dist2": best_d2, "screen_axis_x": 0.0, "screen_axis_y": 0.0} }
   def n = _screen_norm(x1 - x0, y1 - y0)
   {"axis": axis, "dist2": d2, "screen_axis_x": float(n.get(0, 0.0)), "screen_axis_y": float(n.get(1, 0.0))}
}

fn _ring_point(f64 px, f64 py, f64 pz, f64 r, int plane, f64 a) list {
   if(plane == 0){ return [px, py + cos(a) * r, pz + sin(a) * r] }
   if(plane == 1){ return [px + cos(a) * r, py, pz + sin(a) * r] }
   [px + cos(a) * r, py + sin(a) * r, pz]
}

fn _pick_ring(any mvp, any win_w, any win_h, f64 mx, f64 my, f64 px, f64 py, f64 pz, f64 radius, int plane, int axis, f64 best_d2) dict {
   def steps = _ring_steps(win_w, win_h)
   mut i = 1
   mut prev_world = _ring_point(px, py, pz, radius, plane, 0.0)
   mut prev = project_point(mvp, win_w, win_h, prev_world.get(0, px), prev_world.get(1, py), prev_world.get(2, pz))
   mut out = {"axis": -1, "dist2": best_d2, "screen_axis_x": 0.0, "screen_axis_y": 0.0}
   while(i <= steps){
      def a = (float(i) / float(steps)) * (PI * 2.0)
      def cur_world = _ring_point(px, py, pz, radius, plane, a)
      def cur = project_point(mvp, win_w, win_h, cur_world.get(0, px), cur_world.get(1, py), cur_world.get(2, pz))
      if(is_list(prev) && is_list(cur)){
         def x0, y0 = float(prev.get(0, 0.0)), float(prev.get(1, 0.0))
         def x1, y1 = float(cur.get(0, 0.0)), float(cur.get(1, 0.0))
         def d2 = _screen_dist2(mx, my, x0, y0, x1, y1)
         if(d2 < float(out.get("dist2", best_d2))){
            def n = _screen_norm(x1 - x0, y1 - y0)
            out = {"axis": axis, "dist2": d2, "screen_axis_x": float(n.get(0, 0.0)), "screen_axis_y": float(n.get(1, 0.0))}
         }
      }
      prev = cur
      i += 1
   }
   out
}

fn metrics(bounds, mode) list {
   "Computes center, span, thickness, and handle sizes for gizmos."
   def minx, miny = bounds.get(0, 0.0), bounds.get(1, 0.0)
   def minz = bounds.get(2, 0.0)
   def maxx, maxy = bounds.get(3, 0.0), bounds.get(4, 0.0)
   def maxz = bounds.get(5, 0.0)
   def px, py = (minx + maxx) * 0.5, (miny + maxy) * 0.5
   def pz = (minz + maxz) * 0.5
   def span_x, span_y = max(0.001, maxx - minx), max(0.001, maxy - miny)
   def span_z = max(0.001, maxz - minz)
   def span = max(max(span_x, span_y), span_z)
   def axis_len = max(0.28, span * 0.52)
   def axis_thick = clamp(axis_len * 0.0058, 0.0020, max(0.014, span * 0.0040))
   def box_thick = clamp(axis_thick * 0.45, 0.0010, 0.0040)
   def handle_size = clamp(axis_len * (int(mode) == 2 ? 0.040 : 0.028), 0.018, max(0.065, span * 0.030))
   def center_size = clamp(axis_len * 0.032, 0.014, max(0.052, span * 0.024))
   [px, py, pz, span, axis_len, axis_thick, box_thick, handle_size, center_size]
}

fn _axis_col(any col, int axis, int active_axis) list {
   if(active_axis <= 0 || active_axis == axis){
      return col
   }
   [
      float(col.get(0, 1.0)) * 0.58,
      float(col.get(1, 1.0)) * 0.58,
      float(col.get(2, 1.0)) * 0.58,
      float(col.get(3, 1.0)) * 0.34
   ]
}

fn draw_axes(px, py, pz, span, axis_len, axis_thick, handle_size, red, green, blue, mode, win_w, win_h, axis=0) bool {
   "Draws translate/rotate/scale gizmo axes."
   def compact = compact_view(win_w, win_h)
   def center = [0.92, 0.94, 0.92, 0.82]
   def active_axis = int(axis)
   def red_c = _axis_col(red, 1, active_axis)
   def green_c = _axis_col(green, 2, active_axis)
   def blue_c = _axis_col(blue, 3, active_axis)
   if(!compact){ render.draw_cube([px, py, pz], max(handle_size * 0.72, axis_thick * 2.4), center) }
   if(int(mode) == 1){
      def ring_r = max(axis_len * 0.92, span * 0.58)
      def ring_thick = axis_thick * 1.22
      _draw_ring_band(px, py, pz, ring_r, 0, red_c, ring_thick, win_w, win_h)
      _draw_ring_band(px, py, pz, ring_r, 1, green_c, ring_thick, win_w, win_h)
      _draw_ring_band(px, py, pz, ring_r, 2, blue_c, ring_thick, win_w, win_h)
      if(compact){ return true }
      _draw_axis(px, py, pz, 1.0, 0.0, 0.0, axis_len * 0.30, red_c, axis_thick * 0.62, handle_size * 0.48, compact)
      _draw_axis(px, py, pz, 0.0, 1.0, 0.0, axis_len * 0.30, green_c, axis_thick * 0.62, handle_size * 0.48, compact)
      _draw_axis(px, py, pz, 0.0, 0.0, 1.0, axis_len * 0.30, blue_c, axis_thick * 0.62, handle_size * 0.48, compact)
      return true
   }
   if(int(mode) == 0){
      _draw_move_axis(px, py, pz, 1.0, 0.0, 0.0, axis_len, red_c, axis_thick, handle_size, compact)
      _draw_move_axis(px, py, pz, 0.0, 1.0, 0.0, axis_len, green_c, axis_thick, handle_size, compact)
      _draw_move_axis(px, py, pz, 0.0, 0.0, 1.0, axis_len, blue_c, axis_thick, handle_size, compact)
      return true
   }
   _draw_axis(px, py, pz, 1.0, 0.0, 0.0, axis_len, red_c, axis_thick, handle_size, compact)
   _draw_axis(px, py, pz, 0.0, 1.0, 0.0, axis_len, green_c, axis_thick, handle_size, compact)
   _draw_axis(px, py, pz, 0.0, 0.0, 1.0, axis_len, blue_c, axis_thick, handle_size, compact)
   if(int(mode) == 2){
      _draw_axis(px, py, pz, -1.0, 0.0, 0.0, axis_len * 0.46, red_c, axis_thick * 0.72, handle_size * 0.82, compact)
      _draw_axis(px, py, pz, 0.0, -1.0, 0.0, axis_len * 0.46, green_c, axis_thick * 0.72, handle_size * 0.82, compact)
      _draw_axis(px, py, pz, 0.0, 0.0, -1.0, axis_len * 0.46, blue_c, axis_thick * 0.72, handle_size * 0.82, compact)
   }
   true
}

fn hit_test(any mvp, any win_w, any win_h, any bounds, any mode, any mouse_x, any mouse_y) dict {
   "Hit-tests the projected world gizmo. Returns mode, axis, and screen drag tangent."
   if(!is_list(bounds) || bounds.len < 6){ return {"hit": false, "axis": 0, "mode": int(mode)} }
   def m = metrics(bounds, mode)
   def px, py = float(m.get(0, 0.0)), float(m.get(1, 0.0))
   def pz = float(m.get(2, 0.0))
   def axis_len = float(m.get(4, 1.0))
   def center = project_point(mvp, win_w, win_h, px, py, pz)
   if(!is_list(center)){ return {"hit": false, "axis": 0, "mode": int(mode)} }
   def mx, my = float(mouse_x), float(mouse_y)
   def cx, cy = float(center.get(0, 0.0)), float(center.get(1, 0.0))
   def threshold = _hit_threshold(win_w, win_h)
   def center_dx, center_dy = mx - cx, my - cy
   def center_r = max(8.0, threshold * 0.72)
   if(center_dx * center_dx + center_dy * center_dy <= center_r * center_r){
      return {"hit": true, "axis": 0, "mode": int(mode), "screen_axis_x": 0.0, "screen_axis_y": 0.0, "dist2": 0.0}
   }
   mut best_d2 = threshold * threshold
   mut best = {"axis": -1, "dist2": best_d2, "screen_axis_x": 0.0, "screen_axis_y": 0.0}
   if(int(mode) == 1){
      def ring_r = max(axis_len * 0.92, float(m.get(3, 1.0)) * 0.58)
      def rx = _pick_ring(mvp, win_w, win_h, mx, my, px, py, pz, ring_r, 0, 1, best_d2)
      if(int(rx.get("axis", -1)) >= 0){ best = rx best_d2 = float(rx.get("dist2", best_d2)) }
      def ry = _pick_ring(mvp, win_w, win_h, mx, my, px, py, pz, ring_r, 1, 2, best_d2)
      if(int(ry.get("axis", -1)) >= 0){ best = ry best_d2 = float(ry.get("dist2", best_d2)) }
      def rz = _pick_ring(mvp, win_w, win_h, mx, my, px, py, pz, ring_r, 2, 3, best_d2)
      if(int(rz.get("axis", -1)) >= 0){ best = rz }
   } else {
      def ax = _pick_axis_line(mvp, win_w, win_h, mx, my, px, py, pz, 1.0, 0.0, 0.0, axis_len, 1, best_d2)
      if(int(ax.get("axis", -1)) >= 0){ best = ax best_d2 = float(ax.get("dist2", best_d2)) }
      def ay = _pick_axis_line(mvp, win_w, win_h, mx, my, px, py, pz, 0.0, 1.0, 0.0, axis_len, 2, best_d2)
      if(int(ay.get("axis", -1)) >= 0){ best = ay best_d2 = float(ay.get("dist2", best_d2)) }
      def az = _pick_axis_line(mvp, win_w, win_h, mx, my, px, py, pz, 0.0, 0.0, 1.0, axis_len, 3, best_d2)
      if(int(az.get("axis", -1)) >= 0){ best = az best_d2 = float(az.get("dist2", best_d2)) }
      if(int(mode) == 2){
         def nx = _pick_axis_line(mvp, win_w, win_h, mx, my, px, py, pz, -1.0, 0.0, 0.0, axis_len * 0.46, 1, best_d2)
         if(int(nx.get("axis", -1)) >= 0){ best = nx best_d2 = float(nx.get("dist2", best_d2)) }
         def ny = _pick_axis_line(mvp, win_w, win_h, mx, my, px, py, pz, 0.0, -1.0, 0.0, axis_len * 0.46, 2, best_d2)
         if(int(ny.get("axis", -1)) >= 0){ best = ny best_d2 = float(ny.get("dist2", best_d2)) }
         def nz = _pick_axis_line(mvp, win_w, win_h, mx, my, px, py, pz, 0.0, 0.0, -1.0, axis_len * 0.46, 3, best_d2)
         if(int(nz.get("axis", -1)) >= 0){ best = nz }
      }
   }
   def axis = int(best.get("axis", -1))
   if(axis < 0){ return {"hit": false, "axis": 0, "mode": int(mode)} }
   {
      "hit": true,
      "axis": axis,
      "mode": int(mode),
      "screen_axis_x": float(best.get("screen_axis_x", 0.0)),
      "screen_axis_y": float(best.get("screen_axis_y", 0.0)),
      "dist2": float(best.get("dist2", 0.0))
   }
}

fn project_point(mvp, win_w, win_h, px, py, pz) any {
   "Projects a world-space point to screen coordinates."
   def x = float(px)
   def y = float(py)
   def z = float(pz)
   def cx = x * float(mvp.get(2, 1.0)) + y * float(mvp.get(6, 0.0)) + z * float(mvp.get(10, 0.0)) + float(mvp.get(14, 0.0))
   def cy = x * float(mvp.get(3, 0.0)) + y * float(mvp.get(7, 1.0)) + z * float(mvp.get(11, 0.0)) + float(mvp.get(15, 0.0))
   def cz = x * float(mvp.get(4, 0.0)) + y * float(mvp.get(8, 0.0)) + z * float(mvp.get(12, 1.0)) + float(mvp.get(16, 0.0))
   def cw = x * float(mvp.get(5, 0.0)) + y * float(mvp.get(9, 0.0)) + z * float(mvp.get(13, 0.0)) + float(mvp.get(17, 1.0))
   if(cw <= 0.000001 || abs(cw) <= 0.000001){ return 0 }
   def nx, ny = cx / cw, cy / cw
   def sx = (nx * 0.5 + 0.5) * float(win_w)
   def sy = (1.0 - (ny * 0.5 + 0.5)) * float(win_h)
   if(sx < -160.0 || sx > float(win_w) + 160.0 || sy < -160.0 || sy > float(win_h) + 160.0){ return 0 }
   [sx, sy, cz / cw]
}

fn _draw_overlay_axis(mvp, win_w, win_h, px, py, pz, ax, ay, az, length, int col) bool {
   def a = project_point(mvp, win_w, win_h, px, py, pz)
   def b = project_point(mvp, win_w, win_h, px + ax * length, py + ay * length, pz + az * length)
   if(!is_list(a) || !is_list(b)){ return false }
   def x0, y0 = float(a.get(0, 0.0)), float(a.get(1, 0.0))
   def x1, y1 = float(b.get(0, 0.0)), float(b.get(1, 0.0))
   render.draw_line_2d(x0, y0, x1, y1, col, 2.0)
   render.draw_circle(x1, y1, 3.4, col)
   true
}

fn draw_overlay(mvp, win_w, win_h, px, py, pz, axis_len, mode) bool {
   "Draws a compact 2D gizmo overlay for small viewports."
   if(!compact_view(win_w, win_h)){ return false }
   def c = project_point(mvp, win_w, win_h, px, py, pz)
   if(!is_list(c)){ return false }
   def cx = float(c.get(0, 0.0))
   def cy = float(c.get(1, 0.0))
   def red = render.color_pack(0.98, 0.18, 0.14, 0.92)
   def green = render.color_pack(0.18, 0.92, 0.34, 0.92)
   def blue = render.color_pack(0.20, 0.50, 1.00, 0.92)
   def white = render.color_pack(0.92, 0.96, 0.96, 0.90)
   render.draw_circle(cx, cy, 3.8, white)
   if(int(mode) == 1){
      render.draw_circle_lines(cx, cy, 17.0, red, 2.0, 40)
      render.draw_circle_lines(cx, cy, 21.0, green, 2.0, 40)
      render.draw_circle_lines(cx, cy, 25.0, blue, 2.0, 40)
      return true
   }
   def l = axis_len * (int(mode) == 2 ? 0.68 : 0.78)
   _draw_overlay_axis(mvp, win_w, win_h, px, py, pz, 1.0, 0.0, 0.0, l, red)
   _draw_overlay_axis(mvp, win_w, win_h, px, py, pz, 0.0, 1.0, 0.0, l, green)
   _draw_overlay_axis(mvp, win_w, win_h, px, py, pz, 0.0, 0.0, 1.0, l, blue)
   true
}

#main {
   assert(compact_view(320, 800) && !compact_view(800, 800), "viewer gizmo compact")
   assert(metrics([0, 0, 0, 2, 2, 2], 0).len == 9, "viewer gizmo metrics")
   def id = [4, 4, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0]
   assert(bool(hit_test(id, 100, 100, [-1, -1, -1, 1, 1, 1], 0, 50, 50).get("hit", false)), "viewer gizmo center hit")
   assert(int(hit_test(id, 100, 100, [-1, -1, -1, 1, 1, 1], 0, 80, 50).get("axis", 0)) == 1, "viewer gizmo x hit")
   print("✓ std.os.ui.render.viewer.gizmo self-test passed")
}
