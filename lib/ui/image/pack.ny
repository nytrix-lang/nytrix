;; Keywords: image rect pack atlas texture
;; Reference:
;; - https://github.com/nothings/stb/blob/master/stb_rect_pack.h

module std.image.pack (
   init, pack, HEURISTIC_BL, HEURISTIC_BF
)

use std.core *

def HEURISTIC_BL = 0
def HEURISTIC_BF = 1

fn _node_new(nodes, x, y, nxt){
   "Internal helper for `node_new`."
   def d = dict(3)
   dict_set(d, "x", x)
   dict_set(d, "y", y)
   dict_set(d, "nxt", nxt)
   def idx = len(nodes)
   append(nodes, d)
   idx
}

fn _n(nodes, idx){
   "Internal helper for `n`."
   get(nodes, idx)
}
fn _nx(nodes, idx){
   "Internal helper for `nx`."
   dict_get(get(nodes, idx), "x")
}
fn _ny(nodes, idx){
   "Internal helper for `ny`."
   dict_get(get(nodes, idx), "y")
}
fn _nn(nodes, idx){
   "Internal helper for `nn`."
   dict_get(get(nodes, idx), "nxt")
}
fn _set_nxt(nodes, idx, v){
   "Internal helper for `set_nxt`."
   dict_set(get(nodes, idx), "nxt", v)
}
fn _set_x(nodes, idx, v){
   "Internal helper for `set_x`."
   dict_set(get(nodes, idx), "x", v)
}
fn _set_y(nodes, idx, v){
   "Internal helper for `set_y`."
   dict_set(get(nodes, idx), "y", v)
}

fn _l2(a, b){
   "Internal helper for `l2`."
   def l = list(2)
   store_item(l, 0, a) store_item(l, 1, b)
   l
}
fn _l3(a, b, c){
   "Internal helper for `l3`."
   def l = list(3)
   store_item(l, 0, a) store_item(l, 1, b) store_item(l, 2, c)
   l
}
fn _l4(a, b, c, d){
   "Internal helper for `l4`."
   def l = list(4)
   store_item(l, 0, a) store_item(l, 1, b) store_item(l, 2, c) store_item(l, 3, d)
   l
}

fn init(width, height, heuristic=HEURISTIC_BL){
   "Creates a new rect-pack context for a bin of `width`×`height`."
   def ctx = dict(8)
   def nodes = list(width + 4)
   _node_new(nodes, 0,     0,        1)
   _node_new(nodes, width, 0x3FFFFFFF, -1)
   dict_set(ctx, "w",      width)
   dict_set(ctx, "h",      height)
   dict_set(ctx, "heur",   heuristic)
   dict_set(ctx, "nodes",  nodes)
   dict_set(ctx, "active", 0)
   dict_set(ctx, "free",   -1)
   dict_set(ctx, "align",  1)
   ctx
}

fn _skyline_find_min_y(nodes, first_idx, x0, width){
   "Returns _l2(min_y, waste) for placing a rect of `width` starting at `x0`."
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
         if(_nx(nodes, node_idx) < x0){
         visited_w += nx2 - x0
         } else {
         visited_w += nx2 - _nx(nodes, node_idx)
         }
      } else {
         mut under_w = nx2 - _nx(nodes, node_idx)
         if(under_w + visited_w > width){ under_w = width - visited_w }
         waste += under_w * (min_y - ny)
         visited_w += under_w
      }
      node_idx = nn_idx
   }
   _l2(min_y, waste)
}

fn _skyline_find_best_pos(ctx, w, h){
   "Returns [found, x, y, prev_idx] for best placement."
   def nodes  = dict_get(ctx, "nodes")
   def cw     = dict_get(ctx, "w")
   def ch     = dict_get(ctx, "h")
   def heur   = dict_get(ctx, "heur")
   def align  = dict_get(ctx, "align")
   mut aw = (w + align - 1)
   aw -= (aw % align)
   if(aw > cw || h > ch){ return _l4(0, 0, 0, -1) }
   mut best_waste = 0x3FFFFFFF
   mut best_x = 0
   mut best_y = 0x3FFFFFFF
   mut best_prev = -2
   mut prev_idx = -1
   mut node_idx = dict_get(ctx, "active")
   while(_nx(nodes, node_idx) + aw <= cw){
      def res = _skyline_find_min_y(nodes, node_idx, _nx(nodes, node_idx), aw)
      def y   = get(res, 0)
      def wst = get(res, 1)
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
      mut tail_idx = dict_get(ctx, "active")
      mut pv2 = -1
      mut nd2 = dict_get(ctx, "active")
      while(_nx(nodes, tail_idx) < aw){ tail_idx = _nn(nodes, tail_idx) }
      while(tail_idx != -1){
         def xpos = _nx(nodes, tail_idx) - aw
         if(xpos < 0){ tail_idx = _nn(nodes, tail_idx) }
         while(_nx(nodes, _nn(nodes, nd2)) <= xpos){
         pv2 = nd2
         nd2 = _nn(nodes, nd2)
         }
         def res = _skyline_find_min_y(nodes, nd2, xpos, aw)
         def y   = get(res, 0)
         def wst = get(res, 1)
         if(y + h <= ch && y <= best_y){
         if(y < best_y || wst < best_waste || (wst == best_waste && xpos < best_x)){
               best_x    = xpos
               best_y    = y
               best_waste = wst
               best_prev = pv2
         }
         }
         tail_idx = _nn(nodes, tail_idx)
      }
   }
   if(best_prev == -2){ return _l4(0, 0, 0, -1) }
   _l4(1, best_x, best_y, best_prev)
}

fn _skyline_pack_one(ctx, w, h){
   "Packs one rect of w×h. Returns [packed, x, y]."
   def nodes   = dict_get(ctx, "nodes")
   def ch      = dict_get(ctx, "h")
   def align   = dict_get(ctx, "align")
   mut aw = (w + align - 1)
   aw -= (aw % align)
   def res = _skyline_find_best_pos(ctx, w, h)
   def found   = get(res, 0)
   def rx      = get(res, 1)
   def ry      = get(res, 2)
   def prev_idx = get(res, 3)
   if(!found || ry + h > ch){ return _l3(0, 0, 0) }
   def new_idx = _node_new(nodes, rx, ry + h, -1)
   def active = dict_get(ctx, "active")
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
      if(prev_idx == -1){
         dict_set(ctx, "active", new_idx)
      } else {
         _set_nxt(nodes, prev_idx, new_idx)
      }
      _set_nxt(nodes, new_idx, cur_idx)
      mut scan = cur_idx
      while(scan != -1 && _nn(nodes, scan) != -1 && _nx(nodes, _nn(nodes, scan)) <= rx + aw){
         scan = _nn(nodes, scan)
      }
      _set_nxt(nodes, new_idx, scan)
      if(scan != -1 && _nx(nodes, scan) < rx + aw){
         _set_x(nodes, scan, rx + aw)
      }
   }
   _l3(1, rx, ry)
}

fn _sort_by_height(rects){
   "Simple insertion sort of rects (list of dicts) by height desc."
   def n = len(rects)
   mut i = 1
   while(i < n){
      def key = get(rects, i)
      def kh = dict_get(key, "h")
      def kw = dict_get(key, "w")
      mut j = i - 1
      while(j >= 0){
         def rj = get(rects, j)
         def rjh = dict_get(rj, "h")
         def rjw = dict_get(rj, "w")
         if(rjh > kh || (rjh == kh && rjw >= kw)){ break }
         store_item(rects, j + 1, rj)
         j -= 1
      }
      store_item(rects, j + 1, key)
      i += 1
   }
}

fn pack(ctx, rects){
   "Packs a list of rect dicts {id, w, h} into ctx. Sets x, y, packed on each."
   def n = len(rects)
   mut i = 0
   while(i < n){
      dict_set(get(rects, i), "_ord", i)
      i += 1
   }
   _sort_by_height(rects)
   mut all_packed = 1
   i = 0
   while(i < n){
      def r  = get(rects, i)
      def rw = dict_get(r, "w")
      def rh = dict_get(r, "h")
      if(rw == 0 || rh == 0){
         dict_set(r, "x", 0)
         dict_set(r, "y", 0)
         dict_set(r, "packed", 1)
      } else {
         def res = _skyline_pack_one(ctx, rw, rh)
         if(get(res, 0)){
         dict_set(r, "x", get(res, 1))
         dict_set(r, "y", get(res, 2))
         dict_set(r, "packed", 1)
         } else {
         dict_set(r, "x", 0)
         dict_set(r, "y", 0)
         dict_set(r, "packed", 0)
         all_packed = 0
         }
      }
      i += 1
   }
   def sorted = list(n)
   i = 0
   while(i < n){ append(sorted, 0) i += 1 }
   i = 0
   while(i < n){
      def r = get(rects, i)
      store_item(sorted, dict_get(r, "_ord"), r)
      i += 1
   }
   i = 0
   while(i < n){ store_item(rects, i, get(sorted, i)) i += 1 }
   all_packed
}

if(comptime{__main()}){
   use std.core *
   use std.image.pack *

   def ctx = init(256, 256)
   def rects = list(3)
   def r0 = dict(3) dict_set(r0,"id",0) dict_set(r0,"w",64) dict_set(r0,"h",64) append(rects,r0)
   def r1 = dict(3) dict_set(r1,"id",1) dict_set(r1,"w",32) dict_set(r1,"h",32) append(rects,r1)
   def r2 = dict(3) dict_set(r2,"id",2) dict_set(r2,"w",128) dict_set(r2,"h",16) append(rects,r2)

   def ok = pack(ctx, rects)
   assert(ok == 1, "all rects packed")
   assert(dict_get(get(rects,0),"packed") == 1, "rect 0 packed")
   assert(dict_get(get(rects,1),"packed") == 1, "rect 1 packed")
   assert(dict_get(get(rects,2),"packed") == 1, "rect 2 packed")
   print("✓ std.image.pack tests passed")
}
