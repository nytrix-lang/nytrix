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
;;   aspect        preferred width / height when one dimension is fit
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
;;   background    [r, g, b, a] emitted as a rectangle command
;;   border        {color: [r, g, b, a], width: px} emitted as a border command
;;   image         renderer-owned image payload emitted as an image command
;;   color         text RGBA; font, font_size and letter_spacing pass through
;;   transition    true or {spring: anim.spring(...), properties: [x, y, w, h]}
;; References:
;; - https://github.com/nicbarker/clay
;; - https://www.w3.org/TR/css-flexbox-1/
module core.os.ui.flex(node, text, from_xml, watch_xml, poll_xml, classes, styled_node, reconcile, grid, solve, solve_animated, rect_of, mono_measure, boxes_at, render_commands)
use core
use core.str as str
use core.math (max, min, floor)
use core.os.ui.anim as anim
use core.math.parse.data.xml as xml
use core.os as os

fn node(dict props, list children=[]) dict {
   "Creates a layout node with `props` and `children`."
   mut dict n = dict(16)
   for k in props.keys() { n[k] = props.get(k) }
   n["children"] = children
   n
}

fn text(str id, str s, dict props=dict(0)) dict {
   "A leaf node sized by its text(fit width, wrapping when wrap=true)."
   mut dict p = {"id": id, "text": s}
   for k in props.keys() { p[k] = props.get(k) }
   node(p, [])
}

fn _xml_layout_node(dict element) any {
   def tag = to_str(element.get("name", ""))
   def interactive = tag == "button" || tag == "checkbox"
   if tag != "row" && tag != "column" && tag != "box" && tag != "text" && !interactive { return nil }
   def attrs = element.get("attr", dict(0))
   mut props = classes(attrs.get("class", ""))
   if tag == "row" || tag == "column" { props["dir"] = tag == "row" ? "row" : "col" }
   for attr in attrs.keys() {
      def key = to_str(attr)
      if key == "class" { continue }
      def value = to_str(attrs.get(attr, ""))
      if ["id", "align", "justify", "font"].contains(key) || (interactive && key == "action") {
         props[key] = value
      } elif key == "w" || key == "h" {
         if value == "grow" || value == "fit" || str.endswith(value, "%") { props[key] = value }
         else { props[key] = float(str.atof(value)) }
      } elif ["min_w", "max_w", "min_h", "max_h", "pad", "gap", "aspect", "font_size", "letter_spacing", "line_height", "scroll_x", "scroll_y", "z", "radius"].contains(key) {
         props[key] = float(str.atof(value))
      } elif key == "wrap" || key == "clip" || (tag == "checkbox" && key == "checked") {
         if value != "true" && value != "false" { return nil }
         props[key] = value == "true"
      } else { return nil }
   }
   def raw_children = element.get("children", [])
   def label = to_str(element.get("text", ""))
   if tag == "text" || interactive {
      if raw_children.len > 0 { return nil }
      props["text"] = label
   } elif label.len > 0 { return nil }
   if interactive {
      if to_str(props.get("id", "")).len == 0 || to_str(props.get("action", "")).len == 0 { return nil }
      props["widget"] = tag
      if !props.contains("pad") { props["pad"] = tag == "checkbox" ? [6.0, 6.0, 6.0, 26.0] : 6.0 }
   }
   mut children = []
   for raw_child in raw_children {
      if !is_dict(raw_child) { return nil }
      def child = _xml_layout_node(raw_child)
      if is_nil(child) { return nil }
      children = children.append(child)
   }
   node(props, children)
}

fn from_xml(str source) any {
   "Reads a declarative flex layout with row, column, box, text, button and checkbox elements. Interactive controls require a stable id and named action. The class attribute accepts flex utility classes. Element direction and explicit attributes override classes, and descendants inherit text styles. Returns nil for an unsupported layout."
   def root = xml.parse(source, true)
   if !is_dict(root) { return nil }
   def tree = _xml_layout_node(root)
   if !is_dict(tree) { return nil }
   tree.set("children", _inherit_text_styles(_kids(tree), tree))
}

fn watch_xml(str path) dict {
   "Loads a file and returns its watch state. Read tree for the current layout and call poll_xml from your frame loop or timer."
   mut watch = {"path": path, "source": "", "tree": nil, "status": "unreadable"}
   poll_xml(watch)
   watch
}

fn poll_xml(dict watch) str {
   "Checks a watched XML file. Returns updated, unchanged, invalid, or unreadable. Invalid edits and read failures preserve the last valid tree."
   mut status = "unreadable"
   match os.file_read(to_str(watch.get("path", ""))) {
      ok(contents) -> {
         def source = to_str(contents)
         if !is_nil(watch.get("tree", nil)) && source == watch.get("source", "") {
            status = "unchanged"
         } else {
            def next = from_xml(source)
            if is_dict(next) {
               def result = reconcile(watch.get("tree", nil), next)
               watch["tree"] = result.get("tree")
               watch["source"] = source
               status = "updated"
            } else { status = "invalid" }
         }
      }
      err(_) -> {}
   }
   watch["status"] = status
   status
}

fn _tw_scale(str value) f64 {
   if value == "px" { return 1.0 }
   float(str.atof(value)) * 4.0
}

fn _tw_size(str value) any {
   if value == "full" { return "100%" }
   if value == "1/2" { return "50%" }
   if value == "1/3" { return "33.333333%" }
   if value == "2/3" { return "66.666667%" }
   if value == "1/4" { return "25%" }
   if value == "3/4" { return "75%" }
   if value == "screen" { return "100%" }
   _tw_scale(value)
}

fn _tw_apply(dict out, str token) dict {
   if token == "flex" || token == "flex-row" { out["dir"] = "row" }
   elif token == "flex-col" { out["dir"] = "col" }
   elif token == "grow" || token == "flex-1" { out["w"] = {"grow": 1.0} }
   elif token == "items-start" { out["align"] = "start" }
   elif token == "items-center" { out["align"] = "center" }
   elif token == "items-end" { out["align"] = "end" }
   elif token == "items-stretch" { out["align"] = "stretch" }
   elif token == "justify-start" { out["justify"] = "start" }
   elif token == "justify-center" { out["justify"] = "center" }
   elif token == "justify-end" { out["justify"] = "end" }
   elif token == "justify-between" { out["justify"] = "between" }
   elif token == "justify-around" { out["justify"] = "around" }
   elif token == "justify-evenly" { out["justify"] = "evenly" }
   elif token == "clip" || token == "overflow-hidden" { out["clip"] = true }
   elif token == "rounded" { out["radius"] = 4.0 }
   elif token == "rounded-sm" { out["radius"] = 2.0 }
   elif token == "rounded-md" { out["radius"] = 6.0 }
   elif token == "rounded-lg" { out["radius"] = 8.0 }
   elif token == "rounded-xl" { out["radius"] = 12.0 }
   elif token == "rounded-full" { out["radius"] = 999.0 }
   elif token == "border" { out["border"] = {"color": [76, 76, 92, 255], "width": 1.0} }
   elif token == "border-0" { out["border"] = {"color": [76, 76, 92, 255], "width": 0.0} }
   elif token == "border-2" { out["border"] = {"color": [76, 76, 92, 255], "width": 2.0} }
   elif token == "bg-transparent" { out["background"] = [0, 0, 0, 0] }
   elif token == "bg-surface" { out["background"] = [17, 17, 21, 255] }
   elif token == "bg-surface-muted" { out["background"] = [27, 27, 34, 255] }
   elif token == "bg-accent" { out["background"] = [112, 95, 220, 255] }
   elif token == "bg-danger" { out["background"] = [220, 72, 70, 255] }
   elif token == "bg-success" { out["background"] = [60, 170, 105, 255] }
   elif token == "text-primary" { out["color"] = [240, 240, 245, 255] }
   elif token == "text-muted" { out["color"] = [160, 160, 172, 255] }
   elif token == "text-accent" { out["color"] = [160, 145, 255, 255] }
   elif token == "text-danger" { out["color"] = [255, 112, 105, 255] }
   elif str.startswith(token, "gap-") { out["gap"] = _tw_scale(str.str_slice(token, 4, token.len)) }
   elif str.startswith(token, "p-") { out["pad"] = _tw_scale(str.str_slice(token, 2, token.len)) }
   elif str.startswith(token, "px-") {
      def v = _tw_scale(str.str_slice(token, 3, token.len))
      out["pad"] = [0.0, v, 0.0, v]
   }
   elif str.startswith(token, "py-") {
      def v = _tw_scale(str.str_slice(token, 3, token.len))
      out["pad"] = [v, 0.0, v, 0.0]
   }
   elif str.startswith(token, "w-") { out["w"] = _tw_size(str.str_slice(token, 2, token.len)) }
   elif str.startswith(token, "h-") { out["h"] = _tw_size(str.str_slice(token, 2, token.len)) }
   elif token == "text-xs" { out["font_size"] = 12 }
   elif token == "text-sm" { out["font_size"] = 14 }
   elif token == "text-base" { out["font_size"] = 16 }
   elif token == "text-lg" { out["font_size"] = 18 }
   elif token == "text-xl" { out["font_size"] = 20 }
   out
}

fn classes(any class_names, dict base=dict(0)) dict {
   "Builds flex/render properties from Tailwind-like utility classes. Supported groups include flex direction, grow, alignment, justification, spacing, dimensions, clipping, radius, border, palette colors and text size. Base properties are copied first; later classes override them."
   mut dict out = dict(24)
   if is_dict(base) { for k in base.keys() { out[k] = base.get(k) } }
   mut tokens = []
   if is_str(class_names) { tokens = str.split(to_str(class_names), " ") }
   elif is_list(class_names) { tokens = class_names }
   for token in tokens {
      def t = to_str(token)
      if t.len > 0 { _tw_apply(out, t) }
   }
   out
}

fn _inherit_text_styles(list children, dict inherited) list {
   mut out = []
   for child in children {
      if !is_dict(child) { out = out.append(child) continue }
      mut dict copied = dict(16)
      for k in child.keys() { copied[k] = child.get(k) }
      mut dict next_inherited = dict(8)
      for key in ["color", "font", "font_size", "letter_spacing", "line_height"] {
         if copied.contains(key) { next_inherited[key] = copied.get(key) }
         elif inherited.contains(key) {
            copied[key] = inherited.get(key)
            next_inherited[key] = inherited.get(key)
         }
      }
      copied["children"] = _inherit_text_styles(_kids(copied), next_inherited)
      out = out.append(copied)
   }
   out
}

fn styled_node(str id, any class_names, list children=[], dict props=dict(0)) dict {
   "Creates a layout/render element from utility classes plus explicit property overrides."
   mut dict merged = classes(class_names)
   for k in props.keys() { merged[k] = props.get(k) }
   merged["id"] = id
   node(merged, _inherit_text_styles(children, merged))
}

fn _state_merge(any old_state, any new_state) any {
   if !is_dict(old_state) { return new_state }
   mut dict merged = dict(16)
   for k in old_state.keys() { merged[k] = old_state.get(k) }
   if is_dict(new_state) { for k in new_state.keys() { merged[k] = new_state.get(k) } }
   merged
}

fn _props_changed(dict old_node, dict new_node) bool {
   for k in new_node.keys() {
      def key = to_str(k)
      if key == "children" || key == "state" || str.startswith(key, "_") { continue }
      if !old_node.contains(k) || old_node.get(k) != new_node.get(k) { return true }
   }
   for k in old_node.keys() {
      def key = to_str(k)
      if key == "children" || key == "state" || str.startswith(key, "_") { continue }
      if !new_node.contains(k) { return true }
   }
   false
}

fn _collect_removed(dict old_node, str parent_path, list removed) list {
   def id = to_str(old_node.get("id", ""))
   def path = parent_path.len > 0 ? parent_path + "/" + id : id
   removed = removed.append(path)
   for child in _kids(old_node) {
      if is_dict(child) { removed = _collect_removed(child, path, removed) }
   }
   removed
}

fn _reconcile_node(any old_any, dict next_node, str parent_path, dict changes) dict {
   def id = to_str(next_node.get("id", ""))
   def path = parent_path.len > 0 ? parent_path + "/" + id : id
   def old_node = is_dict(old_any) ? old_any : dict(0)
   def exists = is_dict(old_any) && id.len > 0 && to_str(old_node.get("id", "")) == id
   if !exists { changes["added"] = changes.get("added", []).append(path) }
   elif _props_changed(old_node, next_node) { changes["updated"] = changes.get("updated", []).append(path) }
   mut dict out = dict(24)
   for k in next_node.keys() {
      if to_str(k) != "children" && to_str(k) != "state" { out[k] = next_node.get(k) }
   }
   out["state"] = exists ? _state_merge(old_node.get("state", nil), next_node.get("state", nil)) : next_node.get("state", dict(0))
   def old_children = exists ? _kids(old_node) : []
   def next_children = _kids(next_node)
   mut used = dict(8)
   mut reconciled = []
   for child in next_children {
      def child_id = is_dict(child) ? to_str(child.get("id", "")) : ""
      mut found = -1
      mut i = 0
      while i < old_children.len {
         if child_id.len > 0 && !used.contains(to_str(i)) && is_dict(old_children.get(i, nil)) && to_str(old_children.get(i, dict(0)).get("id", "")) == child_id {
            found = i
            break
         }
         i += 1
      }
      mut old_child = nil
      if found >= 0 {
         used[to_str(found)] = true
         old_child = old_children.get(found, nil)
      }
      if is_dict(child) { reconciled = reconciled.append(_reconcile_node(old_child, child, path, changes)) }
      else { reconciled = reconciled.append(child) }
   }
   mut i = 0
   while i < old_children.len {
      if !used.contains(to_str(i)) && is_dict(old_children.get(i, nil)) {
         changes["removed"] = _collect_removed(old_children.get(i, dict(0)), path, changes.get("removed", []))
      }
      i += 1
   }
   out["children"] = reconciled
   out
}

fn reconcile(any previous, dict next_tree) dict {
   "Reconciles freshly described nodes against a previous tree by stable IDs. Matching nodes retain local state keys while new state keys override; the result includes added, removed and updated node paths. Pass `tree` to solve for the next frame."
   mut changes = {"added": [], "removed": [], "updated": []}
   def previous_node = is_dict(previous) ? previous.get("tree", previous) : nil
   def next_id = to_str(next_tree.get("id", ""))
   def previous_id = is_dict(previous_node) ? to_str(previous_node.get("id", "")) : ""
   if previous_id.len > 0 && previous_id != next_id {
      changes["removed"] = _collect_removed(previous_node, "", changes.get("removed", []))
      previous_node = nil
   }
   def tree = _reconcile_node(previous_node, next_tree, "", changes)
   {"tree": tree, "added": changes.get("added", []), "removed": changes.get("removed", []), "updated": changes.get("updated", [])}
}

fn _grid_track_base(str value) any {
   if value == "auto" { return "fit" }
   if str.endswith(value, "fr") {
      def amount = str.str_slice(value, 0, value.len - 2)
      def weight = amount.len == 0 ? 1.0 : max(0.01, float(str.atof(amount)))
      return {"grow": weight}
   }
   if str.endswith(value, "%") { return value }
   float(str.atof(value))
}

fn _grid_track_parts(any track) dict {
   if !is_str(track) { return {"size": track, "min": 0.0, "max": 1e18} }
   def value = str.strip(to_str(track))
   if !str.startswith(value, "minmax(") || !str.endswith(value, ")") { return {"size": _grid_track_base(value), "min": 0.0, "max": 1e18} }
   def body = str.str_slice(value, 7, value.len - 1)
   def parts = str.split(body, ",")
   if parts.len < 2 { return {"size": _grid_track_base(value), "min": 0.0, "max": 1e18} }
   def min_spec = str.strip(to_str(parts.get(0, "0")))
   def max_spec = str.strip(to_str(parts.get(1, "1fr")))
   def min_num = str.endswith(min_spec, "px") ? str.str_slice(min_spec, 0, min_spec.len - 2) : min_spec
   def max_num = str.endswith(max_spec, "px") ? str.str_slice(max_spec, 0, max_spec.len - 2) : max_spec
   def min_v = (str.endswith(min_spec, "fr") || min_spec == "auto" || str.endswith(min_spec, "%")) ? 0.0 : float(max(0.0, float(str.atof(min_num))))
   def max_v = (str.endswith(max_spec, "fr") || max_spec == "auto" || str.endswith(max_spec, "%")) ? 1e18 : float(max(min_v, float(str.atof(max_num))))
   mut size = _grid_track_base(max_spec)
   if !is_dict(size) { size = {"grow": 1.0} }
   {"size": size, "min": min_v, "max": max_v}
}

fn _grid_track(any track) any {
   if !is_str(track) { return track }
   def parts = _grid_track_parts(track)
   parts.get("size", "fit")
}

fn _grid_child(any child, bool fill_height) dict {
   mut dict copied = dict(16)
   if is_dict(child) { for k in child.keys() { copied[k] = child.get(k) } }
   if fill_height && (!copied.contains("h") || to_str(copied.get("h", "")) == "fit") { copied["h"] = "grow" }
   copied
}

fn _grid_span_track(list columns, int start, int span, f64 gap) any {
   if span <= 1 { return _grid_track(columns[start]) }
   mut f64 fixed = gap * float(span - 1)
   mut f64 weight = 0.0
   mut f64 percent = 0.0
   mut int i = 0
   while i < span {
      def spec = _grid_track(columns[start + i])
      if is_dict(spec) {
         weight += float(spec.get("grow", 1.0))
      }
      elif is_str(spec) && str.endswith(to_str(spec), "%") {
         percent += float(str.atof(str.str_slice(to_str(spec), 0, to_str(spec).len - 1)))
      } else {
         if is_str(spec) { return "fit" }
         fixed += float(spec)
      }
      i += 1
   }
   if weight == 0.0 && percent == 0.0 { return fixed }
   {"grow": weight, "grid_base": fixed, "grid_percent": percent / 100.0}
}

fn _grid_slot_fits(dict slots, int slot, int span, int columns) bool {
   if slot % columns + span > columns { return false }
   mut offset = 0
   while offset < span {
      if slots.contains(to_str(slot + offset)) { return false }
      offset += 1
   }
   true
}

fn grid(str id, list columns, list children, list rows=[], f64 column_gap=0.0, f64 row_gap=0.0, dict props=dict(0)) dict {
   "Builds an auto-flow grid as a retained flex tree. Tracks accept pixels, percentages, intrinsic auto, weighted fr, and minmax(min, max) with pixel bounds. Children can set 1-based grid_row/grid_column and grid_column_span; other children auto-place in the first free cell. Column spans combine compatible fixed, percentage, or fr tracks."
   if columns.len <= 0 { return node({"id": id, "w": "grow", "h": "grow"}, []) }
   def column_count = columns.len
   mut row_count = max(rows.len, int((children.len + column_count - 1) / column_count))
   mut slots = dict(children.len * 2 + 4)
   mut next_auto = 0
   mut positioned = []
   mut row_positioned = []
   mut remaining = []
   for child in children {
      def row = is_dict(child) ? int(float(child.get("grid_row", 0))) : 0
      def column = is_dict(child) ? int(float(child.get("grid_column", 0))) : 0
      if row > 0 && column > 0 && column <= column_count {
         positioned = positioned.append(child)
      } elif row > 0 {
         row_positioned = row_positioned.append(child)
      } else {
         remaining = remaining.append(child)
      }
   }
   for child in row_positioned { positioned = positioned.append(child) }
   for child in remaining { positioned = positioned.append(child) }
   for child in positioned {
      def requested_row = is_dict(child) ? int(float(child.get("grid_row", 0))) : 0
      def requested_col = is_dict(child) ? int(float(child.get("grid_column", 0))) : 0
      def requested_span = is_dict(child) ? max(1, int(float(child.get("grid_column_span", 1)))) : 1
      def span = min(requested_span, column_count)
      def col_span = min(span, max(1, column_count - requested_col + 1))
      mut slot = -1
      if requested_row > 0 && requested_col > 0 && requested_col <= column_count {
         slot = (requested_row - 1) * column_count + requested_col - 1
      } elif requested_row > 0 {
         mut c = 0
         while c < column_count {
            def candidate_slot = (requested_row - 1) * column_count + c
            if _grid_slot_fits(slots, candidate_slot, span, column_count) { slot = candidate_slot break }
            c += 1
         }
      } elif requested_col > 0 && requested_col <= column_count {
         mut r = 0
         while r < row_count + children.len {
            def candidate_slot = r * column_count + requested_col - 1
            if _grid_slot_fits(slots, candidate_slot, col_span, column_count) { slot = candidate_slot break }
            r += 1
         }
      }
      mut placed_span = requested_col > 0 && requested_col <= column_count ? col_span : span
      if slot < 0 || !_grid_slot_fits(slots, slot, placed_span, column_count) {
         placed_span = span
         while !_grid_slot_fits(slots, next_auto, placed_span, column_count) { next_auto += 1 }
         slot = next_auto
         next_auto += placed_span
      }
      slots[to_str(slot)] = child
      if placed_span > 1 {
         mut covered = 1
         while covered < placed_span {
            slots[to_str(slot + covered)] = true
            covered += 1
         }
      }
      row_count = max(row_count, int(slot / column_count) + 1)
   }
   mut row_nodes = []
   mut row_index = 0
   while row_index < row_count {
      mut cells = []
      def row_spec = row_index < rows.len ? rows.get(row_index, "auto") : "1fr"
      def row_parts = _grid_track_parts(row_spec)
      def row_track = row_parts.get("size", "fit")
      def fill_height = is_dict(row_track) || to_str(row_track) == "grow"
      mut column_index = 0
      while column_index < column_count {
         def child_index = row_index * column_count + column_index
         def source_child = slots.get(to_str(child_index), nil)
         if is_bool(source_child) && bool(source_child) { column_index += 1 continue }
         def child = is_nil(source_child) ? node({"id": id + ":empty:" + to_str(child_index), "w": "grow", "h": "grow"}, []) : _grid_child(source_child, fill_height)
         def span = is_dict(source_child) ? min(max(1, int(float(source_child.get("grid_column_span", 1)))), column_count - column_index) : 1
         def col_size = _grid_span_track(columns, column_index, span, column_gap)
         mut dict cell_props = {"id": id + ":cell:" + to_str(child_index), "dir": "col", "w": col_size, "h": fill_height ? "grow" : "fit", "align": "stretch"}
         if is_dict(col_size) && col_size.contains("grid_percent") {
            cell_props["_grid_span_percent"] = col_size.get("grid_percent")
         }
         def col_parts = _grid_track_parts(columns[column_index])
         def gap_base = column_gap * float(span - 1)
         def span_base = is_dict(col_size) ? float(col_size.get("grid_base", gap_base)) : gap_base
         def span_min = span_base + float(col_parts.get("min", 0.0))
         if span_min > 0.0 { cell_props["min_w"] = span_min }
         if float(col_parts.get("max", 1e18)) < 1e18 { cell_props["max_w"] = col_parts.get("max") }
         cells = cells.append(node(cell_props, [child]))
         column_index += span
      }
      mut dict row_props = {"id": id + ":row:" + to_str(row_index), "dir": "row", "w": "grow", "h": row_track, "gap": column_gap, "align": "stretch"}
      if float(row_parts.get("min", 0.0)) > 0.0 { row_props["min_h"] = row_parts.get("min") }
      if float(row_parts.get("max", 1e18)) < 1e18 { row_props["max_h"] = row_parts.get("max") }
      row_nodes = row_nodes.append(node(row_props, cells))
      row_index += 1
   }
   mut dict container = {"id": id, "dir": "col", "w": "grow", "h": "grow", "gap": row_gap, "align": "stretch"}
   for k in props.keys() { container[k] = props.get(k) }
   node(container, row_nodes)
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
fn _measure(any measure, str s, dict node) list {
   if is_nil(measure) { return mono_measure(s) }
   if is_dict(measure) {
      def fnptr styled = measure.get("styled", 0)
      if !styled { return mono_measure(s) }
      def result = styled(s, node)
      return is_list(result) && result.len >= 2 ? result : mono_measure(s)
   }
   def r = measure(s)
   is_list(r) && r.len >= 2 ? r : mono_measure(s)
}

;; Calls the caller-supplied text measure callback.
@effects(all)
fn _wrap_lines(any measure, str s, f64 width, dict node) list {
   ;; Greedy word wrap; a word wider than the line gets its own line.
   mut out = []
   for para in str.split(s, "\n") {
      def words = str.split(to_str(para), " ")
      mut line = ""
      for w in words {
         def word = to_str(w)
         def cand = line.len > 0 ? line + " " + word : word
         if line.len > 0 && float(_measure(measure, cand, node).get(0, 0.0)) > width + 0.01 {
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
         content = float(_measure(measure, s, n).get(0, 0.0))
         if bool(n.get("wrap", false)) {
            mut f64 widest = 0.0
            for w in str.split(s, " ") { widest = float(max(widest, float(_measure(measure, to_str(w), n).get(0, 0.0)))) }
            n["_text_min_w"] = widest
         }
      } else {
         def lines = n.get("_lines", nil)
         def lh = float(n.get("line_height", _measure(measure, "Ag", n).get(1, 16.0)))
         n["_text_line_height"] = lh
         content = is_list(lines) ? lh * float(lines.len) : float(_measure(measure, s, n).get(1, lh))
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
   ;; Width is solved first. A fixed height can provide an intrinsic width;
   ;; on the height pass the resolved width provides the intrinsic height.
   def f64 aspect = float(n.get("aspect", 0.0))
   if k == "fit" && aspect > 0.0 {
      if x && _kind(n, false) == "fixed" { want = _clamp(n, false, _fixed(n, false)) * aspect }
      elif !x { want = float(n.get("_w", 0.0)) / aspect }
   }
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
      if _kind(c, x) == "pct" {
         c[key] = _clamp(c, x, float(max(0.0, inner)) * _pct(c, x))
      } elif x && c.contains("_grid_span_percent") {
         def f64 basis = _minv(c, true)
         def f64 share = float(max(0.0, inner)) * float(c.get("_grid_span_percent", 0.0))
         c[key] = _clamp(c, x, basis + share)
      }
   }
   if !main {
      ;; Cross axis: grow (or stretch) fills; everything is clamped to the box.
      def bool stretch = to_str(n.get("align", "start")) == "stretch"
      for c in kids {
         if _floating(c) { continue }
         def k = _kind(c, x)
         if k == "grow" || (stretch && k == "fit" && float(c.get("aspect", 0.0)) <= 0.0) { c[key] = _clamp(c, x, float(max(0.0, inner))) }
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

@effects(all)
fn _refit_widths(dict n, any measure) f64 {
   ;; Recompute fit widths bottom-up after heights are distributed. This lets
   ;; aspect widths propagate through any number of fit-sized ancestors.
   def kids = _kids(n)
   for c in kids { _refit_widths(c, measure) }
   def pad = _pad(n)
   def f64 pads = float(pad.get(1, 0.0)) + float(pad.get(3, 0.0))
   mut f64 content = 0.0
   if n.contains("text") {
      def s = to_str(n.get("text", ""))
      content = float(_measure(measure, s, n).get(0, 0.0))
      if bool(n.get("wrap", false)) {
         mut f64 widest = 0.0
         for word in str.split(s, " ") { widest = float(max(widest, float(_measure(measure, to_str(word), n).get(0, 0.0)))) }
         n["_text_min_w"] = widest
      }
   }
   def bool main = _row(n)
   def f64 gap = float(n.get("gap", 0.0))
   mut int flow = 0
   for c in kids {
      if _floating(c) { continue }
      def f64 child_w = float(c.get("_w", 0.0))
      if main { content += child_w } else { content = float(max(content, child_w)) }
      flow += 1
   }
   if main && flow > 1 { content += gap * float(flow - 1) }
   def f64 fit = content + pads
   def kind = _kind(n, true)
   mut f64 want = fit
   if kind == "fixed" { want = _fixed(n, true) }
   elif kind == "grow" || kind == "pct" { want = _minv(n, true) }
   def f64 aspect = float(n.get("aspect", 0.0))
   if kind == "fit" && aspect > 0.0 { want = float(n.get("_h", 0.0)) * aspect }
   want = _clamp(n, true, want)
   n["_fit_w"] = fit
   n["_w"] = want
   mut f64 floor_v = kind == "fixed" ? want : _minv(n, true)
   if n.contains("_text_min_w") { floor_v = float(max(floor_v, float(n.get("_text_min_w", 0.0)) + pads)) }
   n["_min_w"] = floor_v
   want
}

;; ---- text wrapping between the passes --------------------------------------
;; Calls the caller-supplied text measure callback.
@effects(all)
fn _wrap(dict n, any measure) int {
   if n.contains("text") && bool(n.get("wrap", false)) {
      def pad = _pad(n)
      def f64 width = float(n.get("_w", 0.0)) - float(pad.get(1, 0.0)) - float(pad.get(3, 0.0))
      n["_lines"] = _wrap_lines(measure, to_str(n.get("text", "")), float(max(1.0, width)), n)
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
   mut f64 used = 0.0
   mut int flow_count = 0
   for c in kids {
      if _floating(c) { continue }
      used += float(c.get(row ? "_w" : "_h", 0.0))
      flow_count += 1
   }
   if flow_count > 1 { used += gap * float(flow_count - 1) }
   def f64 free = float(max(0.0, (row ? iw : ih) - used))
   def just = to_str(n.get("justify", "start"))
   mut f64 lead = 0.0
   mut f64 between = gap
   if just == "center" { lead = free * 0.5 }
   elif just == "end" { lead = free }
   elif just == "between" && flow_count > 1 { between = gap + free / float(flow_count - 1) }
   elif just == "around" && flow_count > 0 { lead = free / float(flow_count) * 0.5 between = gap + free / float(flow_count) }
   elif just == "evenly" && flow_count > 0 { lead = free / float(flow_count + 1) between = gap + free / float(flow_count + 1) }
   def al = to_str(n.get("align", "start"))
   ;; Sort flow children by z for draw order but keep layout order for position.
   mut f64 cursor = lead
   mut z_levels = []
   mut z_buckets = dict(8)
   for c in kids {
      if _floating(c) { continue }
      def f64 cw = float(c.get("_w", 0.0))
      def f64 ch = float(c.get("_h", 0.0))
      def f64 cross_free = row ? ih - ch : iw - cw
      def f64 off = al == "center" ? cross_free * 0.5 : (al == "end" ? cross_free : 0.0)
      def f64 px = row ? ix + cursor : ix + off
      def f64 py = row ? iy + off : iy + cursor
      c["_flex_place_x"] = px + sx
      c["_flex_place_y"] = py + sy
      def int z = int(c.get("z", 0))
      def key = to_str(z)
      if !z_buckets.contains(key) {
         z_levels = z_levels.append(z)
         z_buckets[key] = []
      }
      z_buckets[key] = z_buckets.get(key, []).append(c)
      cursor += (row ? cw : ch) + between
   }
   ;; Group once, then visit only occupied z levels in stable input order.
   ;; Iterating every integer between min and max makes sparse z values a
   ;; denial-of-service on layout time (and is unnecessary for ordering).
   z_levels = sort(z_levels)
   for z in z_levels {
      for c in z_buckets.get(to_str(z), []) {
         res = _place(c, float(c.get("_flex_place_x", 0.0)), float(c.get("_flex_place_y", 0.0)), child_clip, depth + 1, res, rects)
      }
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
   with boxes in draw order(parents before children, floats last). The measure
   option is a text callback or a dict with a styled callback taking text and node."
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
   def f64 root_width = float(root.get("_w", 0.0))
   _refit_widths(root, measure)
   if kx != "fit" { root["_w"] = root_width }
   _grow(root, true)
   mut rects = dict(32)
   def boxes = _place(root, 0.0, 0.0, [], 0, [], rects)
   {"rects": rects, "boxes": boxes}
}

fn _transition_has(list properties, str property) bool {
   if !is_list(properties) { return true }
   properties.contains(property)
}

fn solve_animated(dict root, f64 width, f64 height, dict state, f64 dt, any measure=nil) dict {
   "Solves layout and springs opted-in box geometry from its prior value toward this frame's target. Pass the returned animation_state into the next frame."
   anim.step(state, dt)
   def result = solve(root, width, height, measure)
   def boxes = result.get("boxes", [])
   def rects = result.get("rects", dict(0))
   for box in boxes {
      def node = box.get("node", dict(0))
      def transition = node.get("transition", false)
      mut bool enabled = false
      mut properties = ["x", "y", "w", "h"]
      mut spring = dict(0)
      if is_dict(transition) {
         enabled = true
         properties = transition.get("properties", properties)
         spring = transition.get("spring", spring)
      } elif bool(transition) { enabled = true }
      if !enabled { continue }
      def id = to_str(box.get("id", ""))
      if id.len == 0 { continue }
      mut f64 x = float(box.get("x", 0.0))
      mut f64 y = float(box.get("y", 0.0))
      mut f64 w = float(box.get("w", 0.0))
      mut f64 h = float(box.get("h", 0.0))
      if _transition_has(properties, "x") { x = anim.value(state, "flex:" + id + ":x", x, spring) }
      if _transition_has(properties, "y") { y = anim.value(state, "flex:" + id + ":y", y, spring) }
      if _transition_has(properties, "w") { w = anim.value(state, "flex:" + id + ":w", w, spring) }
      if _transition_has(properties, "h") { h = anim.value(state, "flex:" + id + ":h", h, spring) }
      box["x"] = x box["y"] = y box["w"] = w box["h"] = h
      node["_x"] = x node["_y"] = y node["_w"] = w node["_h"] = h
      rects[id] = [x, y, w, h]
   }
   result["animation_state"] = state
   result["animation_busy"] = anim.busy(state)
   result
}

fn rect_of(dict result, str id) list {
   "The [x, y, w, h] of node `id`, or [] when absent."
   def r = result.get("rects", dict(0)).get(id, nil)
   is_list(r) ? r : []
}

fn boxes_at(dict result, f64 px, f64 py) list {
   "Ids of boxes under a point, topmost last(respecting clips)."
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

fn _render_command(str kind, any box, any payload) dict {
   def node = box.get("node", dict(0))
   {"type": kind, "id": to_str(box.get("id", "")), "rect": [float(box.get("x", 0.0)), float(box.get("y", 0.0)), float(box.get("w", 0.0)), float(box.get("h", 0.0))], "clip": box.get("clip", []), "z": int(node.get("z", 0)), "data": payload}
}

fn _with_clip(list out, any box, dict command) list {
   def clip = box.get("clip", [])
   if is_list(clip) && clip.len >= 4 {
      out = out.append(_render_command("scissor_start", box, {"rect": clip, "horizontal": true, "vertical": true}))
   }
   out = out.append(command)
   if is_list(clip) && clip.len >= 4 { out = out.append(_render_command("scissor_end", box, {})) }
   out
}

fn render_commands(dict result) list {
   "Builds renderer-neutral rect, border, text, image, widget and scissor records in layout draw order. Widget records carry a named action for the caller's event dispatch."
   def boxes = result.get("boxes", [])
   if !is_list(boxes) { return [] }
   mut out = []
   for box in boxes {
      if !is_dict(box) { continue }
      def node = box.get("node", dict(0))
      def widget = to_str(node.get("widget", ""))
      if widget == "button" || widget == "checkbox" {
         def payload = {"text": node.get("text", ""), "action": node.get("action", ""), "checked": node.get("checked", false)}
         out = _with_clip(out, box, _render_command(widget, box, payload))
         continue
      }
      if node.contains("background") {
         out = _with_clip(out, box, _render_command("rect", box, {"color": node.get("background"), "radius": node.get("radius", 0.0)}))
      }
      def border = node.get("border", nil)
      if is_dict(border) {
         out = _with_clip(out, box, _render_command("border", box, {"color": border.get("color", [0, 0, 0, 255]), "width": border.get("width", 1.0), "radius": node.get("radius", 0.0)}))
      }
      if node.contains("image") {
         out = _with_clip(out, box, _render_command("image", box, {"image": node.get("image"), "tint": node.get("image_tint", [255, 255, 255, 255]), "radius": node.get("radius", 0.0)}))
      }
      if node.contains("text") {
         def pad = _pad(node)
         def f64 tx = float(box.get("x", 0.0)) + float(pad.get(3, 0.0))
         def f64 ty = float(box.get("y", 0.0)) + float(pad.get(0, 0.0))
         mut lines = box.get("lines", [])
         if !is_list(lines) || lines.len == 0 { lines = [to_str(node.get("text", ""))] }
         def f64 line_height = float(node.get("_text_line_height", node.get("line_height", 16.0)))
         mut int li = 0
         for line in lines {
            def payload = {"text": to_str(line), "color": node.get("color", [255, 255, 255, 255]), "font": node.get("font", 0), "font_size": node.get("font_size", 16), "letter_spacing": node.get("letter_spacing", 0), "line_height": line_height, "x": tx, "y": ty + float(li) * line_height}
            out = _with_clip(out, box, _render_command("text", box, payload))
            li += 1
         }
      }
   }
   out
}
