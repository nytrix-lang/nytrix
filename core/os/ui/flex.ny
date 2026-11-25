;; Keywords: ui layout flexbox clay sizing grow fit percent padding gap align justify wrap scroll float
;; Flexbox-style layout engine (after Clay): build a tree of nodes, call
;; `solve`, and draw the returned boxes. Renderer-agnostic; text is measured
;; through a callback so any font system can drive it.
;;
;;   def ui = flex.node({"id": "root", "dir": "col", "w": "grow", "h": "grow", "pad": 12, "gap": 8}, [
;;      flex.node({"id": "bar", "w": "grow", "h": 32}),
;;      flex.node({"id": "body", "w": "grow", "h": "grow", "dir": "row", "gap": 8}, [
;;         flex.node({"id": "side", "w": 220, "h": "grow"}),
;;         flex.node({"id": "main", "w": "grow", "h": "grow"})])])
;;   def r = flex.solve(ui, 1280.0, 800.0)
;;   r.get("rects").get("main")   ;; [x, y, w, h]
;;
;; Node properties:
;;   id            name used in the result
;;   dir           "row" (default) or "col": the main axis
;;   w, h          120 (px) | "fit" (default: wrap content) | "grow" |
;;                 "40%" of the parent's content box | {"grow": weight}
;;   min_w max_w min_h max_h   clamps
;;   pad           8 | [vertical, horizontal] | [top, right, bottom, left]
;;   gap           space between children on the main axis
;;   justify       main axis: start center end between around evenly
;;   align         cross axis: start center end stretch
;;   text, wrap    intrinsic size from measure(text) -> [w, h]; wrap=true
;;                 breaks on spaces to fit the final width
;;   scroll_x, scroll_y   offset applied to children (children are clipped)
;;   clip          true clips children to the content box
;;   float         {"x": dx, "y": dy, "anchor": "tl|tr|bl|br|center"}: placed
;;                 relative to the parent box, outside the flow, drawn on top
;;   z             draw order among siblings (higher on top)
;; References:
;; - https://github.com/nicbarker/clay
;; - https://www.w3.org/TR/css-flexbox-1/
module core.os.ui.flex(node, text, solve, rect_of, mono_measure, boxes_at)
use core
use core.str as str
use core.math (max, min, floor)

fn node(dict props, list children=[]) dict {
   "Creates a layout node with `props` and `children`."
   mut dict n = dict(16)
   for k in props.keys() { n[k] = props.get(k) }
   n["children"] = children
   n
}

fn text(str id, str s, dict props=dict(0)) dict {
   "A leaf node sized by its text (fit width, wrapping when wrap=true)."
   mut dict p = {"id": id, "text": s}
   for k in props.keys() { p[k] = props.get(k) }
   node(p, [])
}

fn mono_measure(str s) list {
   "Default text metrics: 8 px per character, 16 px lines."
   [float(str.utf8_len(s)) * 8.0, 16.0]
}

;; ---- property helpers ------------------------------------------------------

fn _pad(dict n) list {
   def p = n.get("pad", 0)
   if is_list(p) {
      if p.len >= 4 { return [float(p.get(0, 0)), float(p.get(1, 0)), float(p.get(2, 0)), float(p.get(3, 0))] }
      if p.len >= 2 { return [float(p.get(0, 0)), float(p.get(1, 0)), float(p.get(0, 0)), float(p.get(1, 0))] }
      if p.len == 1 { def v = float(p.get(0, 0)) return [v, v, v, v] }
      return [0.0, 0.0, 0.0, 0.0]
   }
   def v = float(p)
   [v, v, v, v]
}

fn _row(dict n) bool { to_str(n.get("dir", "row")) != "col" }

;; Sizing kind on an axis: "fixed" "fit" "grow" "pct".
fn _kind(dict n, bool x) str {
   def s = n.get(x ? "w" : "h", "fit")
   if is_dict(s) { return "grow" }
   if is_str(s) {
      if s == "grow" { return "grow" }
      if s == "fit" { return "fit" }
      if str.endswith(s, "%") { return "pct" }
      return "fit"
   }
   "fixed"
}

fn _fixed(dict n, bool x) f64 { float(n.get(x ? "w" : "h", 0)) }

fn _pct(dict n, bool x) f64 {
   def s = to_str(n.get(x ? "w" : "h", "0%"))
   float(str.atof(str.str_slice(s, 0, str.utf8_len(s) - 1))) / 100.0
}

fn _weight(dict n, bool x) f64 {
   def s = n.get(x ? "w" : "h", "grow")
   is_dict(s) ? float(s.get("grow", 1.0)) : 1.0
}

fn _minv(dict n, bool x) f64 { float(n.get(x ? "min_w" : "min_h", 0.0)) }

fn _maxv(dict n, bool x) f64 { float(n.get(x ? "max_w" : "max_h", 1e18)) }

fn _clamp(dict n, bool x, f64 v) f64 { float(max(_minv(n, x), min(_maxv(n, x), v))) }

fn _floating(dict n) bool { is_dict(n.get("float", nil)) }

fn _kids(dict n) list {
   def c = n.get("children", [])
   is_list(c) ? c : []
}

fn _size_key(bool x) str { x ? "_w" : "_h" }

;; ---- text ------------------------------------------------------------------

;; Calls the caller-supplied text measure callback.
@effects(all)
fn _measure(any measure, str s) list {
   if is_nil(measure) { return mono_measure(s) }
   def r = measure(s)
   is_list(r) && r.len >= 2 ? r : mono_measure(s)
}

;; Calls the caller-supplied text measure callback.
@effects(all)
fn _wrap_lines(any measure, str s, f64 width) list {
   ;; Greedy word wrap; a word wider than the line gets its own line.
   mut out = []
   for para in str.split(s, "\n") {
      def words = str.split(to_str(para), " ")
      mut line = ""
      for w in words {
         def word = to_str(w)
         def cand = line.len > 0 ? line + " " + word : word
         if line.len > 0 && float(_measure(measure, cand).get(0, 0.0)) > width + 0.01 {
            out = out.append(line)
            line = word
         } else { line = cand }
      }
      out = out.append(line)
   }
   out
}

;; ---- pass 1: fit sizes (bottom-up) -----------------------------------------

;; Calls the caller-supplied text measure callback.
@effects(all)
fn _fit(dict n, bool x, any measure) f64 {
   def pad = _pad(n)
   def f64 pads = x ? float(pad.get(1, 0.0)) + float(pad.get(3, 0.0)) : float(pad.get(0, 0.0)) + float(pad.get(2, 0.0))
   mut f64 content = 0.0
   if n.contains("text") {
      def s = to_str(n.get("text", ""))
      if x {
         ;; Wrapping text can shrink to its widest word.
         content = float(_measure(measure, s).get(0, 0.0))
         if bool(n.get("wrap", false)) {
            mut f64 widest = 0.0
            for w in str.split(s, " ") { widest = float(max(widest, float(_measure(measure, to_str(w)).get(0, 0.0)))) }
            n["_text_min_w"] = widest
         }
      } else {
         def lines = n.get("_lines", nil)
         def lh = float(_measure(measure, "Ag").get(1, 16.0))
         content = is_list(lines) ? lh * float(lines.len) : float(_measure(measure, s).get(1, lh))
      }
   }
   def kids = _kids(n)
   def bool main = _row(n) == x
   def f64 gap = float(n.get("gap", 0.0))
   mut int flow = 0
   for c in kids {
      def f64 cs = _fit(c, x, measure)
      if _floating(c) { continue }
      flow += 1
      if main { content += cs } else { content = float(max(content, cs)) }
   }
   if main && flow > 1 { content += gap * float(flow - 1) }
   def f64 fit = content + pads
   n[x ? "_fit_w" : "_fit_h"] = fit
   ;; The size this node asks of its parent.
   def k = _kind(n, x)
   mut f64 want = fit
   if k == "fixed" { want = _fixed(n, x) }
   elif k == "grow" || k == "pct" { want = _minv(n, x) }
   want = _clamp(n, x, want)
   n[_size_key(x)] = want
   ;; Smallest acceptable size when the parent must shrink children.
   mut f64 floor_v = k == "fixed" ? want : _minv(n, x)
   if x && n.contains("_text_min_w") { floor_v = float(max(floor_v, float(n.get("_text_min_w", 0.0)) + pads)) }
   n[x ? "_min_w" : "_min_h"] = floor_v
   want
}

;; ---- pass 2: distribute (top-down) -----------------------------------------

fn _grow(dict n, bool x) int {
   def pad = _pad(n)
   def f64 inner = float(n.get(_size_key(x), 0.0)) - (x ? float(pad.get(1, 0.0)) + float(pad.get(3, 0.0)) : float(pad.get(0, 0.0)) + float(pad.get(2, 0.0)))
   def kids = _kids(n)
   def bool main = _row(n) == x
   def f64 gap = float(n.get("gap", 0.0))
   def key = _size_key(x)
   ;; Percent sizes resolve against the parent's content box.
   for c in kids {
      if _kind(c, x) == "pct" { c[key] = _clamp(c, x, float(max(0.0, inner)) * _pct(c, x)) }
   }
   if !main {
      ;; Cross axis: grow (or stretch) fills; everything is clamped to the box.
      def bool stretch = to_str(n.get("align", "start")) == "stretch"
      for c in kids {
         if _floating(c) { continue }
         def k = _kind(c, x)
         if k == "grow" || (stretch && k == "fit") { c[key] = _clamp(c, x, float(max(0.0, inner))) }
      }
   } else {
      mut f64 used = 0.0
      mut int flow = 0
      mut growers = []
      for c in kids {
         if _floating(c) { continue }
         flow += 1
         used += float(c.get(key, 0.0))
         if _kind(c, x) == "grow" { growers = growers.append(c) }
      }
      if flow > 1 { used += gap * float(flow - 1) }
      mut f64 free = inner - used
      ;; Grow: share free space by weight, re-sharing what max clamps refuse.
      mut int rounds = 0
      while free > 0.01 && growers.len > 0 && rounds < 8 {
         mut f64 total_w = 0.0
         for g in growers { total_w += _weight(g, x) }
         if total_w <= 0.0 { break }
         mut next_growers = []
         mut f64 spent = 0.0
         for g in growers {
            def f64 cur = float(g.get(key, 0.0))
            def f64 want = cur + free * _weight(g, x) / total_w
            def f64 got = _clamp(g, x, want)
            spent += got - cur
            g[key] = got
            if got >= want - 0.01 { next_growers = next_growers.append(g) }
         }
         free -= spent
         growers = next_growers
         rounds += 1
      }
      ;; Shrink on overflow: take from the largest shrinkable children first.
      rounds = 0
      while free < -0.01 && rounds < 32 {
         mut f64 largest = 0.0
         mut f64 second = 0.0
         mut int count = 0
         for c in kids {
            if _floating(c) { continue }
            def f64 s = float(c.get(key, 0.0))
            def f64 floor_v = float(c.get(x ? "_min_w" : "_min_h", 0.0))
            if s <= floor_v + 0.01 { continue }
            if s > largest + 0.01 { second = largest largest = s count = 1 }
            elif s >= largest - 0.01 { count += 1 }
            elif s > second { second = s }
         }
         if count == 0 { break }
         def f64 step = float(min(largest - second, -free / float(count)))
         for c in kids {
            if _floating(c) { continue }
            def f64 s = float(c.get(key, 0.0))
            def f64 floor_v = float(c.get(x ? "_min_w" : "_min_h", 0.0))
            if s >= largest - 0.01 && s > floor_v + 0.01 {
               def f64 ns = float(max(floor_v, s - step))
               free += s - ns
               c[key] = ns
            }
         }
         rounds += 1
      }
   }
   ;; Floating children size against the parent like percent of a full box.
   for c in kids {
      if _floating(c) && _kind(c, x) == "grow" { c[key] = _clamp(c, x, float(max(0.0, inner))) }
   }
   for c in kids { _grow(c, x) }
   0
}

;; ---- text wrapping between the passes --------------------------------------

;; Calls the caller-supplied text measure callback.
@effects(all)
fn _wrap(dict n, any measure) int {
   if n.contains("text") && bool(n.get("wrap", false)) {
      def pad = _pad(n)
      def f64 width = float(n.get("_w", 0.0)) - float(pad.get(1, 0.0)) - float(pad.get(3, 0.0))
      n["_lines"] = _wrap_lines(measure, to_str(n.get("text", "")), float(max(1.0, width)))
   } elif n.contains("text") {
      n["_lines"] = str.split(to_str(n.get("text", "")), "\n")
   }
   for c in _kids(n) { _wrap(c, measure) }
   0
}

;; ---- pass 3: positions -----------------------------------------------------

fn _place(dict n, f64 x, f64 y, list clip, int depth, list out, dict rects) list {
   n["_x"] = x
   n["_y"] = y
   def f64 w = float(n.get("_w", 0.0))
   def f64 h = float(n.get("_h", 0.0))
   mut res = out.append({"id": to_str(n.get("id", "")), "x": x, "y": y, "w": w, "h": h, "depth": depth, "clip": clip, "node": n, "lines": n.get("_lines", [])})
   if to_str(n.get("id", "")).len > 0 { rects[to_str(n.get("id", ""))] = [x, y, w, h] }
   def pad = _pad(n)
   def f64 ix = x + float(pad.get(3, 0.0))
   def f64 iy = y + float(pad.get(0, 0.0))
   def f64 iw = float(max(0.0, w - float(pad.get(1, 0.0)) - float(pad.get(3, 0.0))))
   def f64 ih = float(max(0.0, h - float(pad.get(0, 0.0)) - float(pad.get(2, 0.0))))
   def bool scroll = n.contains("scroll_x") || n.contains("scroll_y")
   mut child_clip = clip
   if scroll || bool(n.get("clip", false)) {
      ;; Intersect with the inherited clip.
      def f64 cx0 = clip.len >= 4 ? float(max(ix, float(clip.get(0, ix)))) : ix
      def f64 cy0 = clip.len >= 4 ? float(max(iy, float(clip.get(1, iy)))) : iy
      def f64 cx1 = clip.len >= 4 ? float(min(ix + iw, float(clip.get(0, 0.0)) + float(clip.get(2, 0.0)))) : ix + iw
      def f64 cy1 = clip.len >= 4 ? float(min(iy + ih, float(clip.get(1, 0.0)) + float(clip.get(3, 0.0)))) : iy + ih
      child_clip = [cx0, cy0, float(max(0.0, cx1 - cx0)), float(max(0.0, cy1 - cy0))]
   }
   def f64 sx = -float(n.get("scroll_x", 0.0))
   def f64 sy = -float(n.get("scroll_y", 0.0))
   def kids = _kids(n)
   def bool row = _row(n)
   def f64 gap = float(n.get("gap", 0.0))
   mut flow = []
   for c in kids { if !_floating(c) { flow = flow.append(c) } }
   mut f64 used = 0.0
   for c in flow { used += float(c.get(row ? "_w" : "_h", 0.0)) }
   if flow.len > 1 { used += gap * float(flow.len - 1) }
   def f64 free = float(max(0.0, (row ? iw : ih) - used))
   def just = to_str(n.get("justify", "start"))
   mut f64 lead = 0.0
   mut f64 between = gap
   if just == "center" { lead = free * 0.5 }
   elif just == "end" { lead = free }
   elif just == "between" && flow.len > 1 { between = gap + free / float(flow.len - 1) }
   elif just == "around" && flow.len > 0 { lead = free / float(flow.len) * 0.5 between = gap + free / float(flow.len) }
   elif just == "evenly" && flow.len > 0 { lead = free / float(flow.len + 1) between = gap + free / float(flow.len + 1) }
   def al = to_str(n.get("align", "start"))
   ;; Sort flow children by z for draw order but keep layout order for position.
   mut f64 cursor = lead
   mut placed = []
   for c in flow {
      def f64 cw = float(c.get("_w", 0.0))
      def f64 ch = float(c.get("_h", 0.0))
      def f64 cross_free = row ? ih - ch : iw - cw
      def f64 off = al == "center" ? cross_free * 0.5 : (al == "end" ? cross_free : 0.0)
      def f64 px = row ? ix + cursor : ix + off
      def f64 py = row ? iy + off : iy + cursor
      placed = placed.append([c, px + sx, py + sy])
      cursor += (row ? cw : ch) + between
   }
   ;; Draw order: stable by z.
   mut int zi = 0
   mut zs = []
   for p in placed { zs = zs.append(int(p.get(0).get("z", 0))) }
   mut int lo_z = 0
   mut int hi_z = 0
   for z in zs { lo_z = int(min(lo_z, z)) hi_z = int(max(hi_z, z)) }
   mut int zz = lo_z
   while zz <= hi_z {
      for p in placed {
         def c = p.get(0)
         if int(c.get("z", 0)) == zz { res = _place(c, float(p.get(1)), float(p.get(2)), child_clip, depth + 1, res, rects) }
      }
      zz += 1
   }
   ;; Floating children: anchored to this node's box, on top, unclipped by
   ;; scroll (they are overlays: menus, tooltips, badges).
   for c in kids {
      if !_floating(c) { continue }
      def f = c.get("float", dict(0))
      def anchor = to_str(f.get("anchor", "tl"))
      def f64 cw = float(c.get("_w", 0.0))
      def f64 ch = float(c.get("_h", 0.0))
      mut f64 fx = x
      mut f64 fy = y
      if anchor == "tr" { fx = x + w - cw }
      elif anchor == "bl" { fy = y + h - ch }
      elif anchor == "br" { fx = x + w - cw fy = y + h - ch }
      elif anchor == "center" { fx = x + (w - cw) * 0.5 fy = y + (h - ch) * 0.5 }
      elif anchor == "below" { fy = y + h }
      elif anchor == "above" { fy = y - ch }
      res = _place(c, fx + float(f.get("x", 0.0)), fy + float(f.get("y", 0.0)), clip, depth + 1000, res, rects)
   }
   res
}

;; Calls the caller-supplied text measure callback.
@effects(all)
fn solve(dict root, f64 width, f64 height, any measure=nil) dict {
   "Lays out `root` inside a width x height viewport. Returns
   {rects: {id: [x, y, w, h]}, boxes: [{id, x, y, w, h, depth, clip, node, lines}]}
   with boxes in draw order (parents before children, floats last)."
   ;; Root: grow/pct resolve against the viewport.
   _fit(root, true, measure)
   def kx = _kind(root, true)
   root["_w"] = kx == "grow" ? width : (kx == "pct" ? width * _pct(root, true) : float(root.get("_w", 0.0)))
   _grow(root, true)
   _wrap(root, measure)
   _fit(root, false, measure)
   def ky = _kind(root, false)
   root["_h"] = ky == "grow" ? height : (ky == "pct" ? height * _pct(root, false) : float(root.get("_h", 0.0)))
   _grow(root, false)
   mut rects = dict(32)
   def boxes = _place(root, 0.0, 0.0, [], 0, [], rects)
   {"rects": rects, "boxes": boxes}
}

fn rect_of(dict result, str id) list {
   "The [x, y, w, h] of node `id`, or [] when absent."
   def r = result.get("rects", dict(0)).get(id, nil)
   is_list(r) ? r : []
}

fn boxes_at(dict result, f64 px, f64 py) list {
   "Ids of boxes under a point, topmost last (respecting clips)."
   mut out = []
   for b in result.get("boxes", []) {
      def f64 x = float(b.get("x", 0.0))
      def f64 y = float(b.get("y", 0.0))
      if px < x || py < y || px >= x + float(b.get("w", 0.0)) || py >= y + float(b.get("h", 0.0)) { continue }
      def clip = b.get("clip", [])
      if clip.len >= 4 {
         def f64 cx = float(clip.get(0, 0.0))
         def f64 cy = float(clip.get(1, 0.0))
         if px < cx || py < cy || px >= cx + float(clip.get(2, 0.0)) || py >= cy + float(clip.get(3, 0.0)) { continue }
      }
      out = out.append(to_str(b.get("id", "")))
   }
   out
}
