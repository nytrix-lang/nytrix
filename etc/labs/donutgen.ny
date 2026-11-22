#!/usr/bin/env ny
;; Keywords: cli terminal donut generator art lab
;; donutgen - source-art generator for etc/labs/donut.ny.
;;
;; Takes the compact golfed donut program, ray-marches the torus silhouette for
;; a viewpoint, and packs the program's tokens into the mask's ink runs so the
;; source text itself draws the donut. Everything is computed: the mask from
;; the pose, the layout from the mask, the shade characters from the mask.
;;
;; usage: donutgen.ny [-W WIDTH] [-H HEIGHT] [--pose A B] [--margin N]
;;                    [--fill F] [--mask] [--preview] [--stats]
;;                    [--out PATH] [--dry-run]

use core
use core.str as sx
use core.os (file_write, file_read, file_exists)
use core.os.subprocess (run_capture)
use core.os.time (now_ms)

def HEADER = "#!/usr/bin/env ny\n" +
   ";; Keywords: cli terminal donut\n" +
   ";; Donut - https://www.a1k0n.net/2006/09/15/obfuscated-c-donut.html\n" +
   "\n"

;; The input program (golfed, not the art): the generator packs this.
def PROG = """
use core
use core.term
mut A,B=.55,.35
fn R(u,v,a,b,c,d,L,M,N){
 g=4.41-u*u-v*v
 if g<=0{return}
 mut t=sqrt(g)+.01
 for _ in 0..56{
  p=c*u+d*t x=d*u-c*t
  y=b*v+a*p z=b*p-a*v
  r=sqrt(x*x+y*y)
  q=r-1.48
  e=sqrt(q*q+z*z)
  if e<.632{
   l=q*(x*L+y*M)+z*N*r
   return l<.2*e*r?1:l<.65*e*r?2:3
  }
  t-=e-.62
 }
 0
}
fn D(any c,z,w,h)int{
 def i,j=int(w),int(h)
 a,b,d,e=sin(A),cos(A),sin(B),cos(B)
 z=.65*e-.45*d
 L,M,N=-.45*e-.65*d,.65*b+z*a,z*b-.65*a
 mut f64 s=min(i+1,j*2+1)
 s*=.18
 v=(j-1)/s
 for y in 0..j{
  u=(1-i)*.5/s
  for x in 0..i{
   k=R(u,v,a,b,d,e,L,M,N)
   canvas_set(c,x,y," ░▒▓"[k],[0,4,5,7][k],k/3)
   u+=1/s
  }
  v-=2/s
 }
 canvas_refresh(c)
 A+=.035 B+=.021
 0
}
tui_canvas_loop(D)
"""

def PALETTE = {1: ".", 2: "-", 3: "="}
def SEP = " "
def MIN_HOLE = 6
def OUT_DEFAULT = "etc/labs/donut.ny"
def MULTI_OPS = ["..", "+=", "-=", "*=", "/=", "==", "!=", "<=", ">=", "&&", "||"]
def OPENERS = ["(", "[", "{"]
def CLOSERS = [")", "]", "}"]
def NL_AFTER = [",", ":", "?", "=", "+", "-", "*", "/", "<", ">", "==", "!=",
                "<=", ">=", "&&", "||", "..", "+=", "-=", "*=", "/="]
def NL_BEFORE = [")", "]", "}", ",", ":"]

mut SET_OPEN = {}
mut SET_CLOSE = {}
mut SET_NA = {}
mut SET_NB = {}
mut MEMO = {}
mut MEMO_HAS = {}

;; ------------------------------------------------------------------- bytes
fn clen(str s) int {
   mut n = 0
   mut i = 0
   while i < s.len {
      if (load8(s, i) & 0xC0) != 0x80 { n += 1 }
      i += 1
   }
   n
}

fn is_digit_byte(int b) bool { b >= 48 && b <= 57 }

fn is_word_byte(int b) bool {
   (b >= 48 && b <= 57) || (b >= 65 && b <= 90) || (b >= 97 && b <= 122) || b == 95
}

fn is_single_op_byte(int b) bool {
   b == 61 || b == 43 || b == 45 || b == 42 || b == 47 || b == 60 || b == 62 ||
   b == 63 || b == 58 || b == 33 || b == 40 || b == 41 || b == 123 || b == 125 ||
   b == 91 || b == 93 || b == 44 || b == 46
}

fn spaces(int n) str {
   mut s = ""
   mut i = 0
   while i < n { s = s + " " i += 1 }
   s
}

fn make_set(list items) dict {
   mut d = {}
   mut i = 0
   while i < items.len {
      d[to_str(items.get(i, ""))] = 1
      i += 1
   }
   d
}

fn in_set(dict d, str v) bool { int(d.get(v, 0)) == 1 }

fn copy_list(list a) list {
   mut o = []
   mut i = 0
   while i < a.len {
      o = o.append(a.get(i, 0))
      i += 1
   }
   o
}

fn kv_index(list m, int k) int {
   mut i = 0
   while i < m.len {
      def e = m.get(i, [])
      if int(e.get(0, -1)) == k { return i }
      i += 1
   }
   -1
}

fn kv_get(list m, int k, any d) any {
   def at = kv_index(m, k)
   if at < 0 { return d }
   def e = m.get(at, [])
   e.get(1, d)
}

fn kv_put(list m, int k, any v) list {
   if kv_index(m, k) >= 0 { return m }
   m.append([k, v])
}

fn entry4(int si, int t0, int t1, int code) list {
   mut e = []
   e = e.append(si)
   e = e.append(t0)
   e = e.append(t1)
   e = e.append(code)
   e
}

fn stats_list(int ink, int hole, list bb) list {
   mut out = []
   out = out.append(ink)
   out = out.append(hole)
   out = out.append(bb)
   out
}

;; ---------------------------------------------------------------- torus mask
fn ray(f64 u, f64 v, f64 a, f64 b, f64 c, f64 d, f64 L, f64 M, f64 N) int {
   def g = u * u + v * v
   if g >= 4.41 { return 0 }
   def X = d * u
   def Y = b * v + a * c * u
   def Z = b * c * u - a * v
   mut t = sqrt(4.41 - g) + 0.01
   mut i = 0
   while i < 56 {
      if t <= -2.2 { return 0 }
      def x = X - c * t
      def y = Y + a * d * t
      def z = Z + b * d * t
      def r = sqrt(x * x + y * y)
      def q = r - 1.48
      def e = sqrt(q * q + z * z)
      if e < 0.632 {
         def l = q * (x * L + y * M) + z * N * r
         def m = e * r
         if l < 0.2 * m { return 1 }
         if l < 0.65 * m { return 2 }
         return 3
      }
      t = t - (e - 0.62)
      i += 1
   }
   0
}

fn render_mask(int w, int h, f64 A, f64 B, int margin, f64 fill) list {
   def mw = float(w - 2 * margin) / 4.2
   def mh = float(h - 2 * margin) / 2.1
   def s = float(min(mw, mh)) * fill
   if s <= 0.0 { return [] }
   def cx = float(w - 1) / 2.0
   def cy = float(h - 1) / 2.0
   def sa = sin(A)
   def ca = cos(A)
   def sb = sin(B)
   def cb = cos(B)
   def zz = 0.65 * cb - 0.45 * sb
   def L = -0.45 * cb - 0.65 * sb
   def M = 0.65 * ca + zz * sa
   def N = zz * ca - 0.65 * sa
   mut grid = []
   mut y = 0
   while y < h {
      def v = (cy - float(y)) * 2.0 / s
      mut row = []
      mut x = 0
      while x < w {
         row = row.append(ray((float(x) - cx) / s, v, sa, ca, sb, cb, L, M, N))
         x += 1
      }
      grid = grid.append(row)
      y += 1
   }
   grid
}

fn mask_stats(list g) list {
   def h = g.len
   if h == 0 { return stats_list(0, 0, []) }
   def w = g.get(0, []).len
   mut y0 = -1
   mut y1 = -1
   mut y = 0
   while y < h {
      def row = g.get(y, [])
      mut any = false
      mut x = 0
      while x < row.len {
         if int(row.get(x, 0)) != 0 { any = true }
         x += 1
      }
      if any {
         if y0 < 0 { y0 = y }
         y1 = y
      }
      y += 1
   }
   if y0 < 0 { return stats_list(0, 0, []) }
   mut x0 = -1
   mut x1 = -1
   mut x = 0
   while x < w {
      mut any = false
      y = y0
      while y <= y1 {
         if int(g.get(y, []).get(x, 0)) != 0 { any = true }
         y += 1
      }
      if any {
         if x0 < 0 { x0 = x }
         x1 = x
      }
      x += 1
   }
   mut ink = 0
   mut seen = {}
   mut q = []
   x = x0
   while x <= x1 {
      q = q.append(y0 * 1000 + x)
      q = q.append(y1 * 1000 + x)
      x += 1
   }
   y = y0
   while y <= y1 {
      q = q.append(y * 1000 + x0)
      q = q.append(y * 1000 + x1)
      y += 1
   }
   mut qi = 0
   while qi < q.len {
      def code = int(q.get(qi, 0))
      qi += 1
      def cy = code / 1000
      def cx = code - cy * 1000
      if cx < x0 || cx > x1 || cy < y0 || cy > y1 { continue }
      def key = to_str(code)
      if int(seen.get(key, 0)) == 1 { continue }
      if int(g.get(cy, []).get(cx, 0)) != 0 { continue }
      seen[key] = 1
      if cx < x1 { q = q.append(cy * 1000 + cx + 1) }
      if cx > x0 { q = q.append(cy * 1000 + cx - 1) }
      if cy < y1 { q = q.append((cy + 1) * 1000 + cx) }
      if cy > y0 { q = q.append((cy - 1) * 1000 + cx) }
   }
   mut hole = 0
   y = y0
   while y <= y1 {
      x = x0
      while x <= x1 {
         if int(g.get(y, []).get(x, 0)) != 0 {
            ink += 1
         } elif int(seen.get(to_str(y * 1000 + x), 0)) == 0 {
            hole += 1
         }
         x += 1
      }
      y += 1
   }
   mut bb = []
   bb = bb.append(x0)
   bb = bb.append(y0)
   bb = bb.append(x1)
   bb = bb.append(y1)
   stats_list(ink, hole, bb)
}

fn hole_rows(list g) int {
   def h = g.len
   if h == 0 { return 0 }
   def w = g.get(0, []).len
   mut seen = {}
   mut q = []
   mut x = 0
   while x < w {
      q = q.append(x)
      q = q.append((h - 1) * 1000 + x)
      x += 1
   }
   mut y = 0
   while y < h {
      q = q.append(y * 1000)
      q = q.append(y * 1000 + w - 1)
      y += 1
   }
   mut qi = 0
   while qi < q.len {
      def code = int(q.get(qi, 0))
      qi += 1
      def cy = code / 1000
      def cx = code - cy * 1000
      if cx < 0 || cx >= w || cy < 0 || cy >= h { continue }
      def key = to_str(code)
      if int(seen.get(key, 0)) == 1 { continue }
      if int(g.get(cy, []).get(cx, 0)) != 0 { continue }
      seen[key] = 1
      if cx + 1 < w { q = q.append(cy * 1000 + cx + 1) }
      if cx > 0 { q = q.append(cy * 1000 + cx - 1) }
      if cy + 1 < h { q = q.append((cy + 1) * 1000 + cx) }
      if cy > 0 { q = q.append((cy - 1) * 1000 + cx) }
   }
   mut rows = {}
   y = 0
   while y < h {
      x = 0
      while x < w {
         if int(g.get(y, []).get(x, 0)) == 0 {
            if int(seen.get(to_str(y * 1000 + x), 0)) == 0 { rows[to_str(y)] = 1 }
         }
         x += 1
      }
      y += 1
   }
   rows.len
}

fn crop(list g) list {
   def st = mask_stats(g)
   def bb = st.get(2, [])
   if bb.len == 0 { return g }
   def x0 = int(bb.get(0, 0))
   def y0 = int(bb.get(1, 0))
   def x1 = int(bb.get(2, 0))
   def y1 = int(bb.get(3, 0))
   mut out = []
   mut y = y0
   while y <= y1 {
      mut row = []
      mut x = x0
      while x <= x1 {
         row = row.append(int(g.get(y, []).get(x, 0)))
         x += 1
      }
      out = out.append(row)
      y += 1
   }
   out
}

fn spans_of(list g) list {
   mut out = []
   mut y = 0
   while y < g.len {
      def row = g.get(y, [])
      mut sp = []
      mut run = -1
      mut x = 0
      while x < row.len {
         if int(row.get(x, 0)) != 0 && run < 0 {
            run = x
         } elif int(row.get(x, 0)) == 0 && run >= 0 {
            mut pair = []
            pair = pair.append(run)
            pair = pair.append(x)
            sp = sp.append(pair)
            run = -1
         }
         x += 1
      }
      if run >= 0 {
         mut pair = []
         pair = pair.append(run)
         pair = pair.append(row.len)
         sp = sp.append(pair)
      }
      out = out.append(sp)
      y += 1
   }
   out
}

;; ------------------------------------------------------------------ lexing
fn lex_line(str src) list {
   mut toks = []
   mut i = 0
   def n = clen(src)
   while i < n {
      def before = i
      def ch = to_str(slice(src, i, i + 1))
      def b = load8(ch, 0)
      if ch == " " || ch == "\t" {
         i += 1
      } elif ch == ";" {
         i = n
      } elif ch == "\"" {
         def start = i
         i += 1
         mut closed = false
         while i < n {
            def c = to_str(slice(src, i, i + 1))
            if c == "\\" && i + 1 < n {
               i += 2
            } elif c == "\"" {
               i += 1
               closed = true
               break
            } else {
               i += 1
            }
         }
         if !closed { die("unterminated string") }
         toks = toks.append(to_str(slice(src, start, i)))
      } elif is_digit_byte(b) || (ch == "." && i + 1 < n && is_digit_byte(load8(to_str(slice(src, i + 1, i + 2)), 0))) {
         def start = i
         if ch == "." {
            i += 1
            while i < n && is_digit_byte(load8(to_str(slice(src, i, i + 1)), 0)) { i += 1 }
         } else {
            while i < n && is_digit_byte(load8(to_str(slice(src, i, i + 1)), 0)) { i += 1 }
            if i + 1 < n && to_str(slice(src, i, i + 1)) == "." && is_digit_byte(load8(to_str(slice(src, i + 1, i + 2)), 0)) {
               i += 1
               while i < n && is_digit_byte(load8(to_str(slice(src, i, i + 1)), 0)) { i += 1 }
            }
         }
         toks = toks.append(to_str(slice(src, start, i)))
      } elif is_word_byte(b) {
         def start = i
         while i < n && is_word_byte(load8(to_str(slice(src, i, i + 1)), 0)) { i += 1 }
         toks = toks.append(to_str(slice(src, start, i)))
      } else {
         mut op = ""
         mut k = 0
         while k < MULTI_OPS.len {
            def cand = to_str(MULTI_OPS.get(k, ""))
            def cn = clen(cand)
            if op.len == 0 && i + cn <= n && to_str(slice(src, i, i + cn)) == cand { op = cand }
            k += 1
         }
         if op.len == 0 {
            if !is_single_op_byte(b) { die("unexpected character") }
            op = ch
         }
         toks = toks.append(op)
         i += clen(op)
      }
      if i == before { die("lexer stall") }
   }
   toks
}

fn build_stmts(str text) list {
   mut stmts = []
   def lines = sx.split(text, "\n")
   mut i = 0
   while i < lines.len {
      def line = sx.strip(to_str(lines.get(i, "")))
      if line.len > 0 { stmts = stmts.append(lex_line(line)) }
      i += 1
   }
   stmts
}

fn join_class(str a, str b) str {
   if a == ".." || b == ".." { return "MUSTJOIN" }
   def ab = load8(a, a.len - 1)
   def bb = load8(b, 0)
   if is_word_byte(ab) && is_word_byte(bb) { return "SEP4" }
   if is_digit_byte(ab) && bb == 46 { return "SEP4" }
   if ab == 46 && bb == 46 { return "SEP4" }
   "JOIN"
}

fn token_stream(list stmts) list {
   mut tokens = []
   mut starts = []
   mut sep = {}
   mut cant = {}
   mut s = 0
   while s < stmts.len {
      def line = stmts.get(s, [])
      starts = starts.append(tokens.len)
      mut i = 0
      while i < line.len {
         tokens = tokens.append(to_str(line.get(i, "")))
         i += 1
      }
      s += 1
   }
   mut i = 1
   while i < tokens.len {
      def c = join_class(to_str(tokens.get(i - 1, "")), to_str(tokens.get(i, "")))
      if c == "SEP4" { sep[to_str(i)] = 1 }
      if c == "MUSTJOIN" { cant[to_str(i)] = 1 }
      i += 1
   }
   mut out = []
   out = out.append(tokens)
   out = out.append(starts)
   out = out.append(sep)
   out = out.append(cant)
   out
}

fn newline_legal(list tokens, int i) bool {
   if i <= 0 || i >= tokens.len { return true }
   def a = to_str(tokens.get(i - 1, ""))
   def b = to_str(tokens.get(i, ""))
   if in_set(SET_OPEN, a) || in_set(SET_CLOSE, b) { return true }
   if in_set(SET_NA, a) || in_set(SET_NB, b) { return true }
   false
}

;; ----------------------------------------------------------------- packing
fn span_options(list tokens, dict sep, dict cant, int start, int w, bool last) list {
   mut opts = []
   mut cells = 0
   mut j = start
   while j < tokens.len {
      def add = clen(to_str(tokens.get(j, ""))) + (j > start ? int(sep.get(to_str(j), 0)) : 0)
      cells += add
      if cells > w { break }
      def nxt = j + 1
      if nxt == tokens.len || int(cant.get(to_str(nxt), 0)) != 1 || last {
         mut o = []
         o = o.append(nxt)
         o = o.append(cells)
         opts = opts.append(o)
      }
      j += 1
   }
   opts
}

fn row_transitions(int li, int t, list sp, list tokens, dict sep, dict cant, dict legal) list {
   def key = to_str(li) + ":" + to_str(t)
   if int(MEMO_HAS.get(key, 0)) == 1 { return MEMO.get(key, []) }
   def spans = sp.get(li, [])
   mut states = kv_put([], t, [])
   mut si = 0
   while si < spans.len && states.len > 0 {
      def span = spans.get(si, [])
      def a = int(span.get(0, 0))
      def b = int(span.get(1, 0))
      def w = b - a
      def last = si == spans.len - 1
      mut nxt = []
      mut k = 0
      while k < states.len {
         def st = states.get(k, [])
         def t0 = int(st.get(0, 0))
         def path = st.get(1, [])
         def opts = span_options(tokens, sep, cant, t0, w, last)
         mut oi = 0
         while oi < opts.len {
            def o = opts.get(oi, [])
            def t1 = int(o.get(0, 0))
            if kv_index(nxt, t1) < 0 {
               mut np = copy_list(path)
               np = np.append(entry4(si, t0, t1, 1))
               nxt = kv_put(nxt, t1, np)
            }
            oi += 1
         }
         if kv_index(nxt, t0) < 0 {
            mut na = copy_list(path)
            na = na.append(entry4(si, t0, t0, 0))
            nxt = kv_put(nxt, t0, na)
         }
         k += 1
      }
      states = nxt
      si += 1
   }
   mut out = []
   mut i = 0
   while i < states.len {
      def st = states.get(i, [])
      def t1 = int(st.get(0, 0))
      if int(legal.get(to_str(t1), 0)) == 1 { out = out.append(st) }
      i += 1
   }
   MEMO[key] = out
   MEMO_HAS[key] = 1
   out
}

fn pack(list stmts, list grid) dict {
   MEMO = {}
   MEMO_HAS = {}
   def ts = token_stream(stmts)
   def tokens = ts.get(0, [])
   def starts = ts.get(1, [])
   def sep = ts.get(2, {})
   def cant = ts.get(3, {})
   def ntok = tokens.len
   mut legal = {}
   mut i = 0
   while i < starts.len {
      legal[to_str(int(starts.get(i, 0)))] = 1
      i += 1
   }
   i = 1
   while i < ntok {
      if int(legal.get(to_str(i), 0)) == 0 && newline_legal(tokens, i) {
         legal[to_str(i)] = 1
      }
      i += 1
   }
   legal[to_str(ntok)] = 1
   def sp = spans_of(grid)
   if sp.len == 0 || stmts.len == 0 { return {} }
   mut states = kv_put([], int(starts.get(0, 0)), [])
   mut parents = []
   mut li = 0
   while li < sp.len {
      mut nxt = []
      mut k = 0
      while k < states.len {
         def st = states.get(k, [])
         def t0 = int(st.get(0, 0))
         def tr = row_transitions(li, t0, sp, tokens, sep, cant, legal)
         mut j = 0
         while j < tr.len {
            def e = tr.get(j, [])
            def t1 = int(e.get(0, 0))
            if kv_index(nxt, t1) < 0 {
               mut par = []
               par = par.append(li)
               par = par.append(t0)
               nxt = kv_put(nxt, t1, par)
            }
            j += 1
         }
         k += 1
      }
      parents = parents.append(nxt)
      states = nxt
      if kv_index(states, ntok) >= 0 { break }
      li += 1
   }
   if kv_index(states, ntok) < 0 { return {} }
   mut ranges = []
   mut t1 = ntok
   mut r = parents.len - 1
   while r >= 0 {
      def par = kv_get(parents.get(r, []), t1, [])
      def t0 = int(par.get(1, 0))
      mut rng = []
      rng = rng.append(r)
      rng = rng.append(t0)
      rng = rng.append(t1)
      ranges = ranges.append(rng)
      t1 = t0
      r -= 1
   }
   mut packed = {}
   mut idx = ranges.len - 1
   while idx >= 0 {
      def rng = ranges.get(idx, [])
      def li2 = int(rng.get(0, 0))
      def t0 = int(rng.get(1, 0))
      def tl = int(rng.get(2, 0))
      def tr = row_transitions(li2, t0, sp, tokens, sep, cant, legal)
      packed[to_str(li2)] = kv_get(tr, tl, [])
      idx -= 1
   }
   packed
}

;; ----------------------------------------------------------------- emitting
fn span_text(list tokens, int start, int end, dict sep) str {
   mut out = ""
   mut j = start
   while j < end {
      if j > start && int(sep.get(to_str(j), 0)) != 0 { out = out + SEP }
      out = out + to_str(tokens.get(j, ""))
      j += 1
   }
   out
}

fn art_for(list row, int a, int b) str {
   mut out = ""
   mut x = a
   while x < b {
      out = out + to_str(PALETTE.get(int(row.get(x, 0)), " "))
      x += 1
   }
   out
}

fn paint_span(list row, int a, int b, bool last, str code) str {
   def w = b - a
   def cw = clen(code)
   if code.len == 0 {
      if last { return ";" + art_for(row, a + 1, b) }
      if w <= 4 { return spaces(w) }
      return ";[" + art_for(row, a + 2, b - 2) + "];"
   }
   def rest = w - cw
   if rest < 0 { die("span overrun " + to_str(cw) + " > " + to_str(w)) }
   if rest == 0 { return code }
   if last { return code + ";" + art_for(row, a + cw + 1, b) }
   if rest <= 4 { return code + spaces(rest) }
   code + ";[" + art_for(row, a + cw + 2, b - 2) + "];"
}

fn build_lines(list stmts, list grid, dict packed, list tokens, dict sep) list {
   mut stmt_set = {}
   mut n = 0
   mut s = 0
   while s < stmts.len {
      stmt_set[to_str(n)] = 1
      n += stmts.get(s, []).len
      s += 1
   }
   def all_sp = spans_of(grid)
   mut lines = []
   mut li = 0
   while li < grid.len {
      def row = grid.get(li, [])
      def sp = all_sp.get(li, [])
      def entries = packed.get(to_str(li), [])
      mut by_span = {}
      mut e = 0
      while e < entries.len {
         def ent = entries.get(e, [])
         by_span[to_str(int(ent.get(0, 0)))] = ent
         e += 1
      }
      mut first_code = -1
      mut continuation = false
      mut q = 0
      while q < entries.len {
         def ent = entries.get(q, [])
         if int(ent.get(3, 0)) != 0 && first_code < 0 {
            first_code = int(ent.get(0, 0))
            continuation = int(stmt_set.get(to_str(int(ent.get(1, 0))), 0)) != 1
         }
         q += 1
      }
      mut text = ""
      mut prev_end = 0
      mut si = 0
      while si < sp.len {
         def span = sp.get(si, [])
         def a = int(span.get(0, 0))
         def b = int(span.get(1, 0))
         def last = si == sp.len - 1
         def ent = by_span.get(to_str(si), [])
         mut piece = ""
         if continuation && first_code >= 0 && si < first_code {
            piece = spaces(b - a)
         } elif ent.len == 0 || int(ent.get(3, 0)) == 0 {
            piece = paint_span(row, a, b, last, "")
         } else {
            def code = span_text(tokens, int(ent.get(1, 0)), int(ent.get(2, 0)), sep)
            piece = paint_span(row, a, b, last, code)
         }
         text = text + spaces(a - prev_end) + piece
         prev_end = b
         si += 1
      }
      lines = lines.append(text)
      li += 1
   }
   lines
}

;; ------------------------------------------------------------------ attempt
fn attempt(list stmts, int w, int h, f64 A, f64 B, int margin, f64 fill) dict {
   def g0 = render_mask(w, h, A, B, margin, fill)
   if g0.len == 0 { return {} }
   def g = crop(g0)
   def st = mask_stats(g)
   def ink = int(st.get(0, 0))
   def hole = int(st.get(1, 0))
   if hole < MIN_HOLE { return {} }
   def packed = pack(stmts, g)
   if packed.len == 0 { return {} }
   def ts = token_stream(stmts)
   def tokens = ts.get(0, [])
   def sep = ts.get(2, {})
   def lines = build_lines(stmts, g, packed, tokens, sep)
   mut text = HEADER
   mut i = 0
   while i < lines.len {
      text = text + to_str(lines.get(i, "")) + (i + 1 < lines.len ? "\n" : "")
      i += 1
   }
   text = text + "\n"
   mut width = 0
   i = 0
   while i < lines.len {
      def n = clen(to_str(lines.get(i, "")))
      if n > width { width = n }
      i += 1
   }
   mut r = {}
   r["W"] = w
   r["H"] = h
   r["A"] = A
   r["B"] = B
   r["fill"] = fill
   r["grid"] = g
   r["text"] = text
   r["bytes"] = text.len
   r["ink"] = ink
   r["hole"] = hole
   r["hole_rows"] = hole_rows(g)
   r["width"] = width
   r["rows"] = lines.len
   r
}

;; ---------------------------------------------------------------- optimize
;; Coarse (A,B) search for filled donuts with a visible, closed hole.
fn pose_candidates(int cw, int ch, int margin, f64 fill, int keep) list {
   mut scored = []
   mut ai = 12
   while ai < 25 {
      mut bi = -2
      while bi < 6 {
         def A = float(ai) * 0.05
         def B = float(bi) * 0.05
         def g = render_mask(cw, ch, A, B, margin, fill)
         if g.len > 0 {
            def st = mask_stats(g)
            def ink = int(st.get(0, 0))
            def hole = int(st.get(1, 0))
            def bb = st.get(2, [])
            if bb.len == 4 && hole >= MIN_HOLE {
               def area = (int(bb.get(2, 0)) - int(bb.get(0, 0)) + 1) *
                          (int(bb.get(3, 0)) - int(bb.get(1, 0)) + 1)
               if area > 0 && float(hole) / float(area) <= 0.25 {
                  scored = scored.append([ink, A, B])
               }
            }
         }
         bi += 1
      }
      ai += 1
   }
   mut used = {}
   mut out = []
   while out.len < keep && out.len < scored.len {
      mut best_i = -1
      mut best_ink = -1
      mut j = 0
      while j < scored.len {
         if int(used.get(to_str(j), 0)) == 0 {
            def ink = int(scored.get(j, []).get(0, 0))
            if ink > best_ink {
               best_ink = ink
               best_i = j
            }
         }
         j += 1
      }
      if best_i >= 0 {
         used[to_str(best_i)] = 1
         def e = scored.get(best_i, [])
         out = out.append([e.get(1, 0.0), e.get(2, 0.0)])
      }
   }
   out
}

fn optimize(list stmts, int w, int h, int margin, f64 fill_hint, int budget_s) dict {
   mut poses = [[0.85, 0.0], [0.90, 0.0], [0.90, 0.05], [0.915, 0.0],
                [0.936, 0.0], [0.95, -0.05], [0.95, 0.0], [0.95, 0.05],
                [0.95, 0.10], [1.00, 0.0], [1.00, 0.05], [1.00, 0.10],
                [1.05, 0.0], [1.05, 0.05], [1.10, 0.0], [1.10, 0.05],
                [1.10, 0.10]]
   def extra = pose_candidates(48, 24, margin, fill_hint, 8)
   mut p = 0
   while p < extra.len {
      poses = poses.append(extra.get(p, []))
      p += 1
   }
   mut seen_pose = {}
   mut uniq = []
   p = 0
   while p < poses.len {
      def e = poses.get(p, [])
      def key = to_str(e.get(0, 0.0)) + "," + to_str(e.get(1, 0.0))
      if int(seen_pose.get(key, 0)) == 0 {
         seen_pose[key] = 1
         uniq = uniq.append(e)
      }
      p += 1
   }
   mut fills = []
   mut i = 0
   while i < 46 {
      fills = fills.append(0.55 + float(i) * 0.01)
      i += 1
   }
   fills = fills.append(0.6715)
   fills = fills.append(0.7025)
   def deadline = now_ms() + budget_s * 1000
   mut results = []
   mut seen_text = {}
   mut tried = 0
   mut stop = false
   mut pi = 0
   while pi < uniq.len && !stop {
      def pe = uniq.get(pi, [])
      def A = float(pe.get(0, 0.0))
      def B = float(pe.get(1, 0.0))
      mut fi = 0
      while fi < fills.len && !stop {
         if now_ms() >= deadline { stop = true }
         if !stop {
            def r = attempt(stmts, w, h, A, B, margin, float(fills.get(fi, 0.55)))
            tried += 1
            if r.len > 0 {
               def t = to_str(r.get("text", ""))
               if int(seen_text.get(t, 0)) == 0 {
                  seen_text[t] = 1
                  results = results.append(r)
               }
            }
         }
         fi += 1
      }
      pi += 1
   }
   print(to_str(results.len) + " feasible layouts on " + to_str(w) + "x" + to_str(h) +
         " (" + to_str(tried) + " tried, " + to_str(budget_s) + "s budget):")
   mut used = {}
   mut ordered = []
   mut k = 0
   while k < 10 && k < results.len {
      mut best_i = -1
      mut best_bytes = 0
      mut best_ink = 0
      mut j = 0
      while j < results.len {
         if int(used.get(to_str(j), 0)) == 0 {
            def rr = results.get(j, {})
            def b = int(rr.get("bytes", 0))
            def ink = int(rr.get("ink", 0))
            if best_i < 0 || b < best_bytes || (b == best_bytes && ink < best_ink) {
               best_i = j
               best_bytes = b
               best_ink = ink
            }
         }
         j += 1
      }
      if best_i < 0 { break }
      used[to_str(best_i)] = 1
      def rr = results.get(best_i, {})
      ordered = ordered.append(rr)
      print("  " + to_str(int(rr.get("width", 0))) + "x" + to_str(int(rr.get("rows", 0))) +
            " A=" + fmt(rr.get("A", 0.0)) + " B=" + fmt(rr.get("B", 0.0)) +
            " fill=" + fmt4(rr.get("fill", 0.0)) +
            " bytes=" + to_str(int(rr.get("bytes", 0))) +
            " ink=" + to_str(int(rr.get("ink", 0))) +
            " hole=" + to_str(int(rr.get("hole", 0))) +
            " holeRows=" + to_str(int(rr.get("hole_rows", 0))))
      k += 1
   }
   if ordered.len == 0 { return {} }
   ordered.get(0, {})
}

;; ------------------------------------------------------------------ verify
fn same_tokens(list a, list b) bool {
   if a.len != b.len { return false }
   mut i = 0
   while i < a.len {
      if to_str(a.get(i, "")) != to_str(b.get(i, "")) { return false }
      i += 1
   }
   true
}

;; Fillers are comments (`;[art];` or a trailing `;`), and the program never
;; uses ';', so dropping each comment segment leaves exactly the code tokens.
fn strip_fillers(str line) str {
   def n = clen(line)
   mut out = ""
   mut i = 0
   while i < n {
      def ch = to_str(slice(line, i, i + 1))
      if ch == ";" {
         i += 1
         while i < n && to_str(slice(line, i, i + 1)) != ";" { i += 1 }
         i += 1
      } else {
         out = out + ch
         i += 1
      }
   }
   out
}

fn prog_tokens(str text) list {
   def lines = sx.split(text, "\n")
   mut stmts = []
   mut i = 0
   while i < lines.len {
      def l = sx.strip(to_str(lines.get(i, "")))
      if l.len > 0 {
         def c = to_str(slice(l, 0, 1))
         if c != "#" && c != ";" {
            def code = sx.strip(strip_fillers(l))
            if code.len > 0 { stmts = stmts.append(lex_line(code)) }
         }
      }
      i += 1
   }
   token_stream(stmts).get(0, [])
}

fn verify_text(str text, str ref) dict {
   def got = prog_tokens(text)
   def want = prog_tokens(ref)
   mut out = {}
   out["tokens_equal"] = same_tokens(got, want) ? 1 : 0
   out["token_count"] = want.len
   out["got_count"] = got.len
   out
}

fn verify_compile(str path) dict {
   mut r = run_capture(["ny", "--run=none", path], [], nil, false)
   mut code = int(r.get("code", 1))
   if code != 0 && file_exists("build/release/ny") {
      r = run_capture(["build/release/ny", "--run=none", path], [], nil, false)
      code = int(r.get("code", 1))
   }
   mut lines = 0
   def err = sx.split(to_str(r.get("stderr", "")), "\n")
   mut i = 0
   while i < err.len {
      if to_str(err.get(i, "")).len > 0 { lines += 1 }
      i += 1
   }
   mut out = {}
   out["code"] = code
   out["lines"] = lines
   out
}

;; --------------------------------------------------------------------- CLI
fn die(str msg) any {
   print("donutgen: " + msg)
   exit(1)
}

fn usage() {
   print("usage: donutgen.ny [-W WIDTH] [-H HEIGHT] [--pose A B] [--margin N]")
   print("                   [--fill F] [--mask] [--preview] [--stats]")
   print("                   [--verify] [--optimize] [--budget N] [--out PATH] [--dry-run]")
}

fn main() int {
   SET_OPEN = make_set(OPENERS)
   SET_CLOSE = make_set(CLOSERS)
   SET_NA = make_set(NL_AFTER)
   SET_NB = make_set(NL_BEFORE)
   mut opt_w = 80
   mut opt_h = 40
   mut pose_a = 0.85
   mut pose_b = 0.0
   mut margin = 1
   mut fill = 0.64
   mut do_mask = false
   mut do_preview = false
   mut do_stats = false
   mut do_verify = false
   mut do_optimize = false
   mut opt_budget = 60
   mut out_path = OUT_DEFAULT
   mut dry = false
   mut i = 1
   while i < argc() {
      def a = argv(i)
      if a == "-W" && i + 1 < argc() {
         i += 1
         opt_w = int(str.atof(argv(i)))
      } elif a == "-H" && i + 1 < argc() {
         i += 1
         opt_h = int(str.atof(argv(i)))
      } elif a == "--pose" && i + 2 < argc() {
         pose_a = float(str.atof(argv(i + 1)))
         pose_b = float(str.atof(argv(i + 2)))
         i += 2
      } elif a == "--margin" && i + 1 < argc() {
         i += 1
         margin = int(str.atof(argv(i)))
      } elif a == "--fill" && i + 1 < argc() {
         i += 1
         fill = float(str.atof(argv(i)))
      } elif a == "--mask" {
         do_mask = true
      } elif a == "--preview" {
         do_preview = true
      } elif a == "--stats" {
         do_stats = true
      } elif a == "--verify" {
         do_verify = true
      } elif a == "--optimize" {
         do_optimize = true
      } elif a == "--budget" && i + 1 < argc() {
         i += 1
         opt_budget = int(str.atof(argv(i)))
      } elif a == "--out" && i + 1 < argc() {
         i += 1
         out_path = argv(i)
      } elif a == "--dry-run" {
         dry = true
      } elif a == "--help" || a == "-h" {
         usage()
         return 0
      } else {
         die("unknown argument " + a)
      }
      i += 1
   }
   def stmts = build_stmts(PROG)
   mut r = {}
   if do_optimize {
      r = optimize(stmts, opt_w, opt_h, margin, fill, opt_budget)
      if r.len == 0 {
         print("no feasible combo found")
         return 1
      }
   } else {
      r = attempt(stmts, opt_w, opt_h, pose_a, pose_b, margin, fill)
      if r.len == 0 {
         print("no feasible packing at " + to_str(opt_w) + "x" + to_str(opt_h) +
               " for the requested pose; try --optimize")
         return 1
      }
   }
   if do_mask {
      def grid = r.get("grid", [])
      mut y = 0
      while y < grid.len {
         def row = grid.get(y, [])
         mut line = ""
         mut x = 0
         while x < row.len {
            line = line + to_str(PALETTE.get(int(row.get(x, 0)), " "))
            x += 1
         }
         print(line)
         y += 1
      }
   }
   if do_preview { print(to_str(r.get("text", ""))) }
   if do_stats {
      print("program tokens : " + to_str(token_stream(stmts).get(0, []).len))
      print("file dims      : " + to_str(int(r.get("width", 0))) + "x" + to_str(int(r.get("rows", 0))))
      print("ink cells      : " + to_str(int(r.get("ink", 0))) +
            "  hole cells: " + to_str(int(r.get("hole", 0))) +
            " in " + to_str(int(r.get("hole_rows", 0))) + " rows")
      print("file bytes     : " + to_str(int(r.get("bytes", 0))))
      print("viewpoint      : A=" + fmt(r.get("A", 0.0)) + " B=" + fmt(r.get("B", 0.0)) +
            " fill=" + fmt(r.get("fill", 0.0)))
   }
   if !dry {
      file_write(out_path, to_str(r.get("text", "")))
      print("wrote " + out_path + " (" + to_str(int(r.get("bytes", 0))) + " bytes, " +
            to_str(int(r.get("width", 0))) + "x" + to_str(int(r.get("rows", 0))) + ")")
   }
   if do_verify {
      def text = to_str(r.get("text", ""))
      mut vpath = out_path
      if dry {
         vpath = "/tmp/donutgen-verify.ny"
         file_write(vpath, text)
      }
      def vt = verify_text(text, PROG)
      def vc = verify_compile(vpath)
      def code = int(vc.get("code", 1))
      def teq = int(vt.get("tokens_equal", 0))
      print("verify: compile rc=" + to_str(code) +
            " stderr_lines=" + to_str(int(vc.get("lines", 0))))
      print("verify: tokens_equal=" + (teq == 1 ? "true" : "false") +
            " (" + to_str(int(vt.get("got_count", 0))) + "/" +
            to_str(int(vt.get("token_count", 0))) + " tokens)")
      if code != 0 || teq == 0 {
         print("verify: FAILED")
         return 1
      }
      print("verify: OK")
   }
   0
}

fn fmt4(any v) str {
   def x = float(v)
   def q = int(x * 10000.0 + (x < 0.0 ? -0.5 : 0.5))
   mut s = to_str(q)
   while s.len < 5 { s = "0" + s }
   def n = s.len
   to_str(slice(s, 0, n - 4)) + "." + to_str(slice(s, n - 4, n))
}

fn fmt(any v) str {
   def x = float(v)
   mut s = to_str(int(x * 1000.0 + (x < 0.0 ? -0.5 : 0.5)))
   if s.len <= 3 {
      mut pad = "0"
      mut k = s.len
      while k < 3 { pad = pad + "0" k += 1 }
      s = pad + s
   }
   to_str(slice(s, 0, s.len - 3)) + "." + to_str(slice(s, s.len - 3, s.len))
}

exit(main())
