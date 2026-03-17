;; Keywords: render image pack
;; Reference:
;; - https://github.com/nothings/stb/blob/master/stb_rect_pack.h
module std.os.ui.render.img.pack(init, pack, HEURISTIC_BL, HEURISTIC_BF)
use std.core

def HEURISTIC_BL = 0
def HEURISTIC_BF = 1

fn _node_new(list: nodes, any: x, any: y, any: nxt): int {
   def d = [x, y, nxt]
   def idx = nodes.len
   nodes.append(d)
   idx
}

fn _nx(list: nodes, any: idx): any { nodes.get(idx).get(0) }

fn _ny(list: nodes, any: idx): any { nodes.get(idx).get(1) }

fn _nn(list: nodes, any: idx): any { nodes.get(idx).get(2) }

fn _set_nxt(list: nodes, any: idx, any: v): any { nodes.get(idx).set(2, v) }

fn _set_x(list: nodes, any: idx, any: v): any { nodes.get(idx).set(0, v) }

fn init(any: width, any: height, any: heuristic=0): dict {
   "Creates a new rect-pack context for a bin of `width`×`height`."
   def nodes = list(width + 4)
   _node_new(nodes, 0,     0,        1)
   _node_new(nodes, width, 0x3FFFFFFF, -1)
   return {
      "w": width,
      "h": height,
      "heur": heuristic,
      "nodes": nodes,
      "active": 0,
      "free": -1,
      "align": 1
   }
}

fn _skyline_find_min_y(list: nodes, any: first_idx, any: x0, any: width): list {
   def x1 = x0 + width
   mut node_idx = first_idx
   mut min_y = 0
   mut waste = 0
   mut visited_w = 0
   while(_nx(nodes, node_idx) < x1){
      def ny = _ny(nodes, node_idx)
      def nn_idx = _nn(nodes, node_idx)
      def nx2 = _nx(nodes, nn_idx)
      if(ny > min_y){
         waste += visited_w * (ny - min_y)
         min_y = ny
         if(_nx(nodes, node_idx) < x0){ visited_w += nx2 - x0 } else { visited_w += nx2 - _nx(nodes, node_idx) }
      } else {
         mut under_w = nx2 - _nx(nodes, node_idx)
         if(under_w + visited_w > width){ under_w = width - visited_w }
         waste += under_w * (min_y - ny)
         visited_w += under_w
      }
      node_idx = nn_idx
   }
   [min_y, waste]
}

fn _skyline_find_best_pos(dict: ctx, any: w, any: h): list {
   def nodes  = ctx.get("nodes")
   def cw     = ctx.get("w")
   def ch     = ctx.get("h")
   def heur   = ctx.get("heur")
   def align  = ctx.get("align")
   mut aw = (w + align - 1)
   aw -= (aw % align)
   if(aw > cw || h > ch){ return [0, 0, 0, -1] }
   mut best_waste = 0x3FFFFFFF
   mut best_x = 0
   mut best_y = 0x3FFFFFFF
   mut best_prev = -2
   mut prev_idx = -1
   mut node_idx = ctx.get("active")
   while(_nx(nodes, node_idx) + aw <= cw){
      def res = _skyline_find_min_y(nodes, node_idx, _nx(nodes, node_idx), aw)
      def y   = res.get(0)
      def wst = res.get(1)
      if(heur == HEURISTIC_BL){
         if(y < best_y){
            best_y    = y
            best_prev = prev_idx
            best_x    = _nx(nodes, node_idx)
         }
      } else {
         if(y + h <= ch){
            if(y < best_y || (y == best_y && wst < best_waste)){
               best_y    = y
               best_waste = wst
               best_prev = prev_idx
               best_x    = _nx(nodes, node_idx)
            }
         }
      }
      prev_idx = node_idx
      node_idx = _nn(nodes, node_idx)
   }
   if(heur == HEURISTIC_BF){
      mut tail_idx = ctx.get("active")
      mut pv2 = -1
      mut nd2 = ctx.get("active")
      while(_nx(nodes, tail_idx) < aw){ tail_idx = _nn(nodes, tail_idx) }
      while(tail_idx != -1){
         def xpos = _nx(nodes, tail_idx) - aw
         if(xpos < 0){ tail_idx = _nn(nodes, tail_idx) }
         while(_nx(nodes, _nn(nodes, nd2)) <= xpos){
            pv2 = nd2
            nd2 = _nn(nodes, nd2)
         }
         def res = _skyline_find_min_y(nodes, nd2, xpos, aw)
         def y   = res.get(0)
         def wst = res.get(1)
         if(y + h <= ch && y <= best_y){
            if(y < best_y || wst < best_waste || (wst == best_waste && xpos < best_x)){
               best_x, best_y = xpos, y
               best_waste = wst
               best_prev = pv2
            }
         }
         tail_idx = _nn(nodes, tail_idx)
      }
   }
   if(best_prev == -2){ return [0, 0, 0, -1] }
   [1, best_x, best_y, best_prev]
}

fn _skyline_pack_one(dict: ctx, any: w, any: h): list {
   def nodes   = ctx.get("nodes")
   def ch      = ctx.get("h")
   def align   = ctx.get("align")
   mut aw = (w + align - 1)
   aw -= (aw % align)
   def res = _skyline_find_best_pos(ctx, w, h)
   def found   = res.get(0)
   def rx      = res.get(1)
   def ry      = res.get(2)
   def prev_idx = res.get(3)
   if(!found || ry + h > ch){ return [0, 0, 0] }
   def new_idx = _node_new(nodes, rx, ry + h, -1)
   def active = ctx.get("active")
   def cur_idx = (prev_idx == -1) ? active : _nn(nodes, prev_idx)
   if(_nx(nodes, cur_idx) < rx){
      def after = _nn(nodes, cur_idx)
      _set_nxt(nodes, cur_idx, new_idx)
      _set_nxt(nodes, new_idx, after)
      mut scan = after
      while(scan != -1 && _nn(nodes, scan) != -1 && _nx(nodes, _nn(nodes, scan)) <= rx + aw){
         def next = _nn(nodes, scan)
         scan = next
      }
      _set_nxt(nodes, new_idx, scan)
   } else {
      if(prev_idx == -1){ ctx.set("active", new_idx) } else { _set_nxt(nodes, prev_idx, new_idx) }
      _set_nxt(nodes, new_idx, cur_idx)
      mut scan = cur_idx
      while(scan != -1 && _nn(nodes, scan) != -1 && _nx(nodes, _nn(nodes, scan)) <= rx + aw){ scan = _nn(nodes, scan) }
      _set_nxt(nodes, new_idx, scan)
      if(scan != -1 && _nx(nodes, scan) < rx + aw){ _set_x(nodes, scan, rx + aw) }
   }
   [1, rx, ry]
}

fn _sort_by_height(list: rects): any {
   def n = rects.len
   mut i = 1
   while(i < n){
      def key = rects.get(i)
      def kh = key.get("h")
      def kw = key.get("w")
      mut j = i - 1
      while(j >= 0){
         def rj = rects.get(j)
         def rjh = rj.get("h")
         def rjw = rj.get("w")
         if(rjh > kh || (rjh == kh && rjw >= kw)){ break }
         rects.set(j + 1, rj)
         j -= 1
      }
      rects.set(j + 1, key)
      i += 1
   }
}

fn pack(dict: ctx, list: rects): int {
   "Packs a list of rect dicts {id, w, h} into ctx. Sets x, y, packed on each."
   def n = rects.len
   mut i = 0
   while(i < n){
      rects.get(i).set("_ord", i)
      i += 1
   }
   _sort_by_height(rects)
   mut all_packed = 1
   i = 0
   while(i < n){
      def r  = rects.get(i)
      def rw = r.get("w")
      def rh = r.get("h")
      if(rw == 0 || rh == 0){
         r.set("x", 0)
         r.set("y", 0)
         r.set("packed", 1)
      } else {
         def res = _skyline_pack_one(ctx, rw, rh)
         if(res.get(0)){
            r.set("x", res.get(1))
            r.set("y", res.get(2))
            r.set("packed", 1)
         } else {
            r.set("x", 0)
            r.set("y", 0)
            r.set("packed", 0)
            all_packed = 0
         }
      }
      i += 1
   }
   def sorted = list(n)
   i = 0
   while(i < n){ sorted.append(0) i += 1 }
   i = 0
   while(i < n){
      def r = rects.get(i)
      sorted.set(r.get("_ord"), r)
      i += 1
   }
   i = 0
   while(i < n){ rects.set(i, sorted.get(i)) i += 1 }
   all_packed
}
