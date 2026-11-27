;; Keywords: ui animation easing spring tween transition motion interpolation
;; Motion for interfaces: easing curves, a damped spring that is stable at any
;; frame time, and a keyed animator. Widgets ask `animator.value(a, id,
;; target)` every frame and draw with the result; values move toward their
;; targets on their own, and `busy` says whether another frame is needed.
;;
;;   mut a = anim.animator()
;;   ;; each frame:
;;   a = anim.step(a, dt)
;;   def f64 x = anim.value(a, "panel.x", open ? 0.0 : -300.0)
;;   if anim.busy(a) { request_redraw() }
;; References:
;; - https://easings.net
;; - https://www.ryanjuckett.com/damped-springs/
module core.os.ui.anim(
   ease, ease_in_quad, ease_out_quad, ease_in_out_quad, ease_in_cubic, ease_out_cubic,
   ease_in_out_cubic, ease_out_quint, ease_out_expo, ease_in_out_expo, ease_out_back,
   ease_out_elastic, ease_out_bounce, mix, mix_color, spring, spring_step,
   animator, step, value, set_now, busy, tween
)

use core
use core.math (exp, cos, sin, sqrt, pow, min, max, abs)

def PI = 3.141592653589793

fn mix(f64 a, f64 b, f64 t) f64 { "Linear interpolation." a + (b - a) * t }

fn mix_color(list a, list b, f64 t) list {
   "Component-wise interpolation of [r, g, b, a] colours."
   [mix(float(a.get(0, 0.0)), float(b.get(0, 0.0)), t), mix(float(a.get(1, 0.0)), float(b.get(1, 0.0)), t),
      mix(float(a.get(2, 0.0)), float(b.get(2, 0.0)), t), mix(float(a.get(3, 1.0)), float(b.get(3, 1.0)), t)]
}

fn _c(f64 t) f64 { float(max(0.0, min(1.0, t))) }

fn ease_in_quad(f64 t) f64 { def x = _c(t) x * x }

fn ease_out_quad(f64 t) f64 { def x = _c(t) 1.0 - (1.0 - x) * (1.0 - x) }

fn ease_in_out_quad(f64 t) f64 { def x = _c(t) x < 0.5 ? 2.0 * x * x : 1.0 - pow(-2.0 * x + 2.0, 2.0) / 2.0 }

fn ease_in_cubic(f64 t) f64 { def x = _c(t) x * x * x }

fn ease_out_cubic(f64 t) f64 { def x = 1.0 - _c(t) 1.0 - x * x * x }

fn ease_in_out_cubic(f64 t) f64 { def x = _c(t) x < 0.5 ? 4.0 * x * x * x : 1.0 - pow(-2.0 * x + 2.0, 3.0) / 2.0 }

fn ease_out_quint(f64 t) f64 { def x = 1.0 - _c(t) 1.0 - x * x * x * x * x }

fn ease_out_expo(f64 t) f64 { def x = _c(t) x >= 1.0 ? 1.0 : 1.0 - pow(2.0, -10.0 * x) }

fn ease_in_out_expo(f64 t) f64 {
   def x = _c(t)
   if x <= 0.0 { return 0.0 }
   if x >= 1.0 { return 1.0 }
   x < 0.5 ? pow(2.0, 20.0 * x - 10.0) / 2.0 : (2.0 - pow(2.0, -20.0 * x + 10.0)) / 2.0
}

fn ease_out_back(f64 t) f64 {
   def x = _c(t)
   def c1 = 1.70158
   def c3 = c1 + 1.0
   1.0 + c3 * pow(x - 1.0, 3.0) + c1 * pow(x - 1.0, 2.0)
}

fn ease_out_elastic(f64 t) f64 {
   def x = _c(t)
   if x <= 0.0 { return 0.0 }
   if x >= 1.0 { return 1.0 }
   pow(2.0, -10.0 * x) * sin((x * 10.0 - 0.75) * (2.0 * PI / 3.0)) + 1.0
}

fn ease_out_bounce(f64 t) f64 {
   mut x = _c(t)
   def n1 = 7.5625
   def d1 = 2.75
   if x < 1.0 / d1 { return n1 * x * x }
   if x < 2.0 / d1 { x -= 1.5 / d1 return n1 * x * x + 0.75 }
   if x < 2.5 / d1 { x -= 2.25 / d1 return n1 * x * x + 0.9375 }
   x -= 2.625 / d1
   n1 * x * x + 0.984375
}

fn ease(str name, f64 t) f64 {
   "Easing curve by name(linear, quad, cubic, quint, expo, back, elastic, bounce, in-/in-out- variants)."
   case name {
      "in-quad" -> ease_in_quad(t)
      "quad", "out-quad" -> ease_out_quad(t)
      "in-out-quad" -> ease_in_out_quad(t)
      "in-cubic" -> ease_in_cubic(t)
      "cubic", "out-cubic" -> ease_out_cubic(t)
      "in-out-cubic" -> ease_in_out_cubic(t)
      "quint", "out-quint" -> ease_out_quint(t)
      "expo", "out-expo" -> ease_out_expo(t)
      "in-out-expo" -> ease_in_out_expo(t)
      "back", "out-back" -> ease_out_back(t)
      "elastic", "out-elastic" -> ease_out_elastic(t)
      "bounce", "out-bounce" -> ease_out_bounce(t)
      _ -> _c(t)
   }
}

fn tween(f64 from, f64 to, f64 elapsed, f64 duration, str curve="cubic") f64 {
   "Value of a timed transition after `elapsed` seconds."
   if duration <= 0.0 { return to }
   mix(from, to, ease(curve, elapsed / duration))
}

;; ---- springs ---------------------------------------------------------------
fn spring(f64 frequency=2.0, f64 damping=1.0) dict {
   "Spring parameters: natural frequency in Hz(higher is snappier; 2 Hz
   settles in about 0.4 s) and damping ratio(1 = critically damped, no
   overshoot ; below 1 overshoots and wobbles)."
   {"freq": frequency, "damp": damping}
}

fn spring_step(dict sp, f64 pos, f64 vel, f64 target, f64 dt) list {
   "Advances a damped harmonic oscillator exactly(closed form), so large or
   uneven frame times never explode. Returns [pos, vel]."
   if dt <= 0.0 { return [pos, vel] }
   def f64 w = float(max(0.0001, float(sp.get("freq", 2.0)))) * 2.0 * PI
   def f64 z = float(max(0.0, float(sp.get("damp", 1.0))))
   def f64 x0 = pos - target
   if z >= 0.9999 && z <= 1.0001 {
      ;; critically damped
      def f64 e = exp(-w * dt)
      def f64 c2 = vel + w * x0
      def f64 nx = (x0 + c2 * dt) * e
      def f64 nv = (c2 - w * (x0 + c2 * dt)) * e
      return [target + nx, nv]
   }
   if z < 1.0 {
      ;; under-damped
      def f64 wd = w * sqrt(1.0 - z * z)
      def f64 e = exp(-z * w * dt)
      def f64 a = x0
      def f64 b = (vel + z * w * x0) / wd
      def f64 cs = cos(wd * dt)
      def f64 sn = sin(wd * dt)
      def f64 nx = e * (a * cs + b * sn)
      def f64 nv = e * ((b * wd - z * w * a) * cs - (a * wd + z * w * b) * sn)
      return [target + nx, nv]
   }
   ;; over-damped
   def f64 r = w * sqrt(z * z - 1.0)
   def f64 r1 = -z * w + r
   def f64 r2 = -z * w - r
   def f64 c2 = (vel - r1 * x0) / (r2 - r1)
   def f64 c1 = x0 - c2
   def f64 e1 = exp(r1 * dt)
   def f64 e2 = exp(r2 * dt)
   [target + c1 * e1 + c2 * e2, c1 * r1 * e1 + c2 * r2 * e2]
}

;; ---- keyed animator ----------------------------------------------------------
fn animator(dict sp=dict(0)) dict {
   "Creates an animator; `sp` is the default spring(see `spring`)."
   {"spring": sp.len > 0 ? sp : spring(), "vals": dict(32), "dt": 0.0, "busy": false, "eps": 0.01}
}

fn step(dict a, f64 dt) dict {
   "Starts a frame: values requested this frame advance by `dt` seconds."
   a["dt"] = float(max(0.0, min(dt, 0.25)))
   a["busy"] = false
   a
}

fn set_now(dict a, str id, f64 v) dict {
   "Jumps a value to `v` with no motion(first layout, resets)."
   a.get("vals", dict(32))[id] = [v, 0.0]
   a
}

fn value(dict a, str id, f64 target, dict sp=dict(0)) f64 {
   "Current animated value for `id`, moved one frame toward `target`. The
   first request returns `target` directly."
   def vals = a.get("vals", dict(32))
   if !vals.contains(id) {
      vals[id] = [target, 0.0]
      return target
   }
   def cur = vals.get(id, [target, 0.0])
   def f64 pos = float(cur.get(0, target))
   def f64 vel = float(cur.get(1, 0.0))
   def f64 eps = float(a.get("eps", 0.01))
   if abs(pos - target) < eps && abs(vel) < eps {
      vals[id] = [target, 0.0]
      return target
   }
   def nx = spring_step(sp.len > 0 ? sp : a.get("spring", spring()), pos, vel, target, float(a.get("dt", 0.0)))
   vals[id] = nx
   a["busy"] = true
   float(nx.get(0, target))
}

fn busy(dict a) bool {
   "True when some value is still moving(request another frame)."
   bool(a.get("busy", false))
}
