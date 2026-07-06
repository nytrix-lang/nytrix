module std.os.ui.render.prims(draw_sphere_3d, draw_cylinder_3d, draw_cylinder_between_3d, draw_plane_3d, draw_box_3d)
use std.math as math
use std.math.vector
use std.os.ui.render as render

; Helper: convert Vector3-like `v` to a list of three floats.
fn _v3f(any v) list { [float(vector.x(v)), float(vector.y(v)), float(vector.z(v))] }

fn draw_sphere_3d(any center, any radius, any color) {
   "Draw a UV-sphere by tessellating rings and segments.
   Precomputes per-segment sin/cos and batches triangles into a single
   `render.draw_triangles` call to reduce draw overhead."
   def c = _v3f(center)
   def cx = c.get(0)
   def cy = c.get(1)
   def cz = c.get(2)
   def r = float(radius)
   def rings = 8
   def segs = 14
   ; precompute segment trig
   mut cu = []
   mut su = []
   mut si = 0
   while si <= segs {
      def u = 2.0 * PI * float(si) / float(segs)
         cu = cu.append(math.cos(u))
         su = su.append(math.sin(u))
      si += 1
   }
   mut tris = []
   mut ri = 0
   while ri < rings {
      def v0 = -PI * 0.5 + PI * float(ri) / float(rings)
      def v1 = -PI * 0.5 + PI * float(ri + 1) / float(rings)
      def c0 = math.cos(v0)
      def c1 = math.cos(v1)
      def s0 = math.sin(v0)
      def s1 = math.sin(v1)
      mut sj = 0
      while sj < segs {
         def p00 = [cx + c0*cu.get(sj)*r, cy + s0*r, cz + c0*su.get(sj)*r]
         def p01 = [cx + c0*cu.get(sj + 1)*r, cy + s0*r, cz + c0*su.get(sj + 1)*r]
         def p11 = [cx + c1*cu.get(sj + 1)*r, cy + s1*r, cz + c1*su.get(sj + 1)*r]
         def p10 = [cx + c1*cu.get(sj)*r, cy + s1*r, cz + c1*su.get(sj)*r]
         tris = tris.append([p00, p01, p11])
         tris = tris.append([p00, p11, p10])
         sj += 1
      }
      ri += 1
   }
   render.draw_triangles(tris, color)
}

fn draw_cylinder_3d(any center, any radiusTop, any radiusBottom, any height, any sides, any color) {
   "Draw a capped cylinder centered at `center` with given `height`.
   Precomputes per-side trig to avoid repeated cos/sin and batches triangles."
   def c = _v3f(center)
   def cx = c.get(0)
   def cy = c.get(1)
   def cz = c.get(2)
   def rt = float(radiusTop)
   def rb = float(radiusBottom)
   def y0 = cy - float(height) * 0.5
   def y1 = cy + float(height) * 0.5
   def n = math.max(3, int(sides))
   mut ca = []
   mut sa = []
   mut i = 0
   while i <= n {
      def a = 2.0 * PI * float(i) / float(n)
         ca = ca.append(math.cos(a))
         sa = sa.append(math.sin(a))
      i += 1
   }
   mut tris = []
   mut j = 0
   while j < n {
      def ca0 = ca.get(j)
      def ca1 = ca.get(j + 1)
      def sa0 = sa.get(j)
      def sa1 = sa.get(j + 1)
      def b0 = [cx + ca0*rb, y0, cz + sa0*rb]
      def b1 = [cx + ca1*rb, y0, cz + sa1*rb]
      def t1 = [cx + ca1*rt, y1, cz + sa1*rt]
      def t0 = [cx + ca0*rt, y1, cz + sa0*rt]
      tris = tris.append([b0, b1, t1])
      tris = tris.append([b0, t1, t0])
      tris = tris.append([[cx, y1, cz], t0, t1])
      tris = tris.append([[cx, y0, cz], b1, b0])
      j += 1
   }
   render.draw_triangles(tris, color)
}

fn draw_cylinder_between_3d(any start, any finish, any radiusStart, any radiusEnd, any sides, any color) {
   "Draw a generalized cylinder between two endpoints.
   Falls back to a sphere when length is near-zero. Uses a local frame and
   batches triangles for the lateral surface."
   def axis = vector.sub(finish, start)
   def h = vector.magnitude(axis)
   if h <= 0.0001 {
      draw_sphere_3d(start, math.max(float(radiusStart), float(radiusEnd)), color)
      return
   }
   def dir = vector.normalize3(axis)
   def ref = if math.abs(float(vector.y(dir))) < 0.90 { Vector3(0, 1, 0) } else { Vector3(1, 0, 0) }
   def right = vector.normalize3(vector.cross3(ref, dir))
   def fwd = vector.cross3(dir, right)
   def n = math.max(3, int(sides))
   mut ca = []
   mut sa = []
   mut i = 0
   while i <= n {
      def a = 2.0 * PI * float(i) / float(n)
         ca = ca.append(math.cos(a))
         sa = sa.append(math.sin(a))
      i += 1
   }
   mut tris = []
   mut j = 0
   while j < n {
      def ca0 = ca.get(j)
      def ca1 = ca.get(j + 1)
      def sa0 = sa.get(j)
      def sa1 = sa.get(j + 1)
      def r0a = vector.add(vector.scale(right, ca0 * float(radiusStart)), vector.scale(fwd, sa0 * float(radiusStart)))
      def r0b = vector.add(vector.scale(right, ca1 * float(radiusStart)), vector.scale(fwd, sa1 * float(radiusStart)))
      def r1b = vector.add(vector.scale(right, ca1 * float(radiusEnd)), vector.scale(fwd, sa1 * float(radiusEnd)))
      def r1a = vector.add(vector.scale(right, ca0 * float(radiusEnd)), vector.scale(fwd, sa0 * float(radiusEnd)))
      def p0 = vector.add(start, r0a)
      def p1 = vector.add(start, r0b)
      def p2 = vector.add(finish, r1b)
      def p3 = vector.add(finish, r1a)
      tris = tris.append([_v3f(p0), _v3f(p1), _v3f(p2)])
      tris = tris.append([_v3f(p0), _v3f(p2), _v3f(p3)])
      j += 1
   }
   render.draw_triangles(tris, color)
}

fn draw_plane_3d(any center, any normal, any size, any color) {
   "Draw an oriented square plane centered at `center` with given `normal`.
   This constructs a local right/fwd frame orthogonal to `normal`."
   def s = float(size) * 0.5
   def up = if math.abs(float(vector.y(normal))) < 0.9 { Vector3(0, 1, 0) } else { Vector3(1, 0, 0) }
   def right = vector.normalize3(vector.cross3(up, normal))
   def fwd = vector.normalize3(vector.cross3(normal, right))
   def p0 = vector.add(center, vector.add(vector.scale(right, -s), vector.scale(fwd, -s)))
   def p1 = vector.add(center, vector.add(vector.scale(right, s), vector.scale(fwd, -s)))
   def p2 = vector.add(center, vector.add(vector.scale(right, s), vector.scale(fwd, s)))
   def p3 = vector.add(center, vector.add(vector.scale(right, -s), vector.scale(fwd, s)))
   render.draw_quad(_v3f(p0), _v3f(p1), _v3f(p2), _v3f(p3), color)
}

fn draw_box_3d(any center, any w, any h, any d, any color) {
   "Draw an axis-aligned box centered at `center` with width/height/depth.
   Generates two triangles per face and batches into `draw_triangles`."
   def c = _v3f(center)
   def cx = c.get(0)
   def cy = c.get(1)
   def cz = c.get(2)
   def hx = float(w) * 0.5
   def hy = float(h) * 0.5
   def hz = float(d) * 0.5
   def p000 = [cx - hx, cy - hy, cz - hz]
   def p001 = [cx - hx, cy - hy, cz + hz]
   def p010 = [cx - hx, cy + hy, cz - hz]
   def p011 = [cx - hx, cy + hy, cz + hz]
   def p100 = [cx + hx, cy - hy, cz - hz]
   def p101 = [cx + hx, cy - hy, cz + hz]
   def p110 = [cx + hx, cy + hy, cz - hz]
   def p111 = [cx + hx, cy + hy, cz + hz]
   mut tris = []
   ; Top
   tris = tris.append([p010, p110, p111])
   tris = tris.append([p010, p111, p011])
   ; Bottom
   tris = tris.append([p000, p001, p101])
   tris = tris.append([p000, p101, p100])
   ; Left
   tris = tris.append([p000, p010, p011])
   tris = tris.append([p000, p011, p001])
   ; Right
   tris = tris.append([p100, p101, p111])
   tris = tris.append([p100, p111, p110])
   ; Front (+Z)
   tris = tris.append([p001, p011, p111])
   tris = tris.append([p001, p111, p101])
   ; Back (-Z)
   tris = tris.append([p000, p100, p110])
   tris = tris.append([p000, p110, p010])
   render.draw_triangles(tris, color)
}
