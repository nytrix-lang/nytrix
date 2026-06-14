;; Keywords: editor window layout panels chrome os ui render viewer text
;; Editor window layout, title chrome, and panel sizing helpers.
;; References:
;; - std.os.ui.render.viewer.editor.core
;; - std.os.ui.render.viewer.gui
module std.os.ui.render.viewer.editor.window(
   SPLIT_NONE, SPLIT_VERTICAL, SPLIT_HORIZONTAL,
   leaf, root, split_right, split_below, pane_layout, leaves,
   selected, select_next, select_at, delete_selected, balance, resize_selected
)

use std.core
use std.math (max, min)

def SPLIT_NONE = "none"
def SPLIT_VERTICAL = "vertical"
def SPLIT_HORIZONTAL = "horizontal"
def MIN_PANE = 96.0

fn leaf(any buffer=0, bool active=false) dict {
   {"split": SPLIT_NONE, "ratio": 0.5, "buffer": buffer, "active": active, "x": 0.0, "y": 0.0, "w": 0.0, "h": 0.0, "scroll": 0}
}

fn root(any buffer=0) dict {
   leaf(buffer, true)
}

fn _child(dict node, str key) dict {
   node.get(key, leaf())
}

fn _clear_active(dict node) dict {
   if to_str(node.get("split", SPLIT_NONE)) == SPLIT_NONE {
      node["active"] = false
      return node
   }
   node["left"] = _clear_active(_child(node, "left"))
   node["right"] = _clear_active(_child(node, "right"))
   node
}

fn _split_selected(dict node, str kind, any new_buffer) list {
   if to_str(node.get("split", SPLIT_NONE)) == SPLIT_NONE {
      if bool(node.get("active", false)) {
         def old = node
         old["active"] = false
         return [{
               "split": kind, "ratio": 0.5, "active": false,
               "left": old, "right": leaf(new_buffer, true),
               "x": float(node.get("x", 0.0)), "y": float(node.get("y", 0.0)),
               "w": float(node.get("w", 0.0)), "h": float(node.get("h", 0.0))
         }, true]
      }
      return [node, false]
   }
   def left_res = _split_selected(_child(node, "left"), kind, new_buffer)
   node["left"] = left_res.get(0)
   if bool(left_res.get(1, false)) { return [node, true] }
   def right_res = _split_selected(_child(node, "right"), kind, new_buffer)
   node["right"] = right_res.get(0)
   [node, bool(right_res.get(1, false))]
}

fn split_right(dict node, any new_buffer=0) dict {
   _split_selected(node, SPLIT_VERTICAL, new_buffer).get(0)
}

fn split_below(dict node, any new_buffer=0) dict {
   _split_selected(node, SPLIT_HORIZONTAL, new_buffer).get(0)
}

fn pane_layout(dict node, f64 x, f64 y, f64 w, f64 h) dict {
   node["x"] = x
   node["y"] = y
   node["w"] = w
   node["h"] = h
   def split = to_str(node.get("split", SPLIT_NONE))
   if split == SPLIT_NONE { return node }
   def ratio = min(max(float(node.get("ratio", 0.5)), 0.15), 0.85)
   if split == SPLIT_VERTICAL {
      def lw = max(MIN_PANE, w * ratio)
      node["left"] = pane_layout(_child(node, "left"), x, y, lw, h)
      node["right"] = pane_layout(_child(node, "right"), x + lw, y, max(MIN_PANE, w - lw), h)
   } else {
      def th = max(MIN_PANE, h * ratio)
      node["left"] = pane_layout(_child(node, "left"), x, y, w, th)
      node["right"] = pane_layout(_child(node, "right"), x, y + th, w, max(MIN_PANE, h - th))
   }
   node
}

fn _leaves_into(dict node, list out) list {
   if to_str(node.get("split", SPLIT_NONE)) == SPLIT_NONE { return out.append(node) }
   out = _leaves_into(_child(node, "left"), out)
   _leaves_into(_child(node, "right"), out)
}

fn leaves(dict node) list {
   _leaves_into(node, [])
}

fn selected(dict node) dict {
   def rows = leaves(node)
   mut i = 0
   while i < rows.len {
      def row = rows.get(i)
      if bool(row.get("active", false)) { return row }
      i += 1
   }
   rows.len > 0 ? rows.get(0) : leaf()
}

fn _select_buffer(dict node, any buffer) dict {
   if to_str(node.get("split", SPLIT_NONE)) == SPLIT_NONE {
      node["active"] = node.get("buffer", 0) == buffer
      return node
   }
   node["left"] = _select_buffer(_child(node, "left"), buffer)
   node["right"] = _select_buffer(_child(node, "right"), buffer)
   node
}

fn select_next(dict node, int dir=1) dict {
   def rows = leaves(node)
   if rows.len <= 0 { return node }
   mut active = 0
   mut i = 0
   while i < rows.len {
      def row = rows.get(i, dict(0))
      if is_dict(row) && bool(row.get("active", false)) { active = i }
      i += 1
   }
   def next = (active + dir + rows.len) % rows.len
   def next_row = rows.get(next, dict(0))
   _select_buffer(_clear_active(node), is_dict(next_row) ? next_row.get("buffer", next) : next)
}

fn select_at(dict node, f64 x, f64 y) dict {
   def rows = leaves(node)
   mut picked = -1
   mut i = 0
   while i < rows.len {
      def r = rows.get(i)
      if x >= float(r.get("x", 0.0)) && x <= float(r.get("x", 0.0)) + float(r.get("w", 0.0)) &&
      y >= float(r.get("y", 0.0)) && y <= float(r.get("y", 0.0)) + float(r.get("h", 0.0)){ picked = i }
      i += 1
   }
   if picked < 0 { return node }
   def picked_row = rows.get(picked, dict(0))
   _select_buffer(_clear_active(node), is_dict(picked_row) ? picked_row.get("buffer", picked) : picked)
}

fn delete_selected(dict node) dict {
   if to_str(node.get("split", SPLIT_NONE)) == SPLIT_NONE { return node }
   def left = _child(node, "left")
   def right = _child(node, "right")
   if to_str(left.get("split", SPLIT_NONE)) == SPLIT_NONE && bool(left.get("active", false)) {
      right["active"] = true
      return right
   }
   if to_str(right.get("split", SPLIT_NONE)) == SPLIT_NONE && bool(right.get("active", false)) {
      left["active"] = true
      return left
   }
   node["left"] = delete_selected(left)
   node["right"] = delete_selected(right)
   node
}

fn balance(dict node) dict {
   if to_str(node.get("split", SPLIT_NONE)) == SPLIT_NONE { return node }
   node["ratio"] = 0.5
   node["left"] = balance(_child(node, "left"))
   node["right"] = balance(_child(node, "right"))
   node
}

fn _resize(dict node, f64 delta) dict {
   if to_str(node.get("split", SPLIT_NONE)) == SPLIT_NONE { return node }
   def left = _child(node, "left")
   def right = _child(node, "right")
   if bool(selected(left).get("active", false)) {
      node["ratio"] = min(max(float(node.get("ratio", 0.5)) + delta, 0.15), 0.85)
      node["left"] = _resize(left, delta)
   } elif bool(selected(right).get("active", false)) {
      node["ratio"] = min(max(float(node.get("ratio", 0.5)) - delta, 0.15), 0.85)
      node["right"] = _resize(right, delta)
   }
   node
}

fn resize_selected(dict node, f64 delta=0.05) dict {
   _resize(node, delta)
}

#main {
   mut tree = root(1)
   tree = split_right(tree, 2)
   tree = pane_layout(tree, 0.0, 0.0, 800.0, 480.0)
   assert(leaves(tree).len == 2 && int(selected(tree).get("buffer", 0)) == 2, "split selected")
   tree = select_next(tree, 1)
   assert(int(selected(tree).get("buffer", 0)) == 1, "select next")
   tree = balance(tree)
   assert(float(tree.get("ratio", 0.0)) == 0.5, "balance")
}
