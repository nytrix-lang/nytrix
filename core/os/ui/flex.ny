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
module core.os.ui.flex(node, text, from_xml, watch_xml, poll_xml, classes, styled_node, reconcile, grid, solve, solve_animated, rect_of, mono_measure, boxes_at, render_commands, fast_node, fast_solve, fast_solve_into, fast_boxes, fast_rect)
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

fn _widget_tag(str tag) bool {
   tag == "button" || tag == "checkbox" || tag == "slider" || tag == "input" || tag == "spinbox" || tag == "combo" || tag == "menu"
}

fn _xml_attrs(dict element) dict {
   def attrs = element.get("attr", dict(0))
   is_dict(attrs) ? attrs : dict(0)
}

fn _xml_component_scan(any element, dict out, int depth) int {
   if !is_dict(element) || depth > 32 { return 0 }
   def tag = to_str(element.get("name", ""))
   if tag == "component" {
      def name = to_str(_xml_attrs(element).get("name", ""))
      def kids = element.get("children", [])
      if name.len == 0 || !is_list(kids) || kids.len != 1 || !is_dict(kids.get(0)) { return 0 }
      out[name] = kids.get(0)
      for kid in kids { _xml_component_scan(kid, out, depth + 1) }
      return 0
   }
   def kids = element.get("children", [])
   if is_list(kids) { for kid in kids { _xml_component_scan(kid, out, depth + 1) } }
   0
}

fn _xml_slot(dict element) bool {
   to_str(element.get("name", "")) == "slot" && to_str(_xml_attrs(element).get("name", "")).len == 0
}

fn _xml_expand(any element, dict components, int depth) any {
   "Expands use references against component definitions, applying use attributes as overrides and replacing slot children."
   if !is_dict(element) || depth > 24 { return nil }
   def tag = to_str(element.get("name", ""))
   if tag == "component" { return nil }
   def attrs = _xml_attrs(element)
   if tag == "use" {
      def name = to_str(attrs.get("component", ""))
      if name.len == 0 || !components.contains(name) { return nil }
      def base = components.get(name)
      if !is_dict(base) { return nil }
      mut dict merged = dict(16)
      for k in _xml_attrs(base).keys() { merged[k] = _xml_attrs(base).get(k) }
      for k in attrs.keys() { if to_str(k) != "component" { merged[k] = attrs.get(k) } }
      def use_kids = element.get("children", [])
      mut list kids = []
      mut bool slotted = false
      for kid in base.get("children", []) {
         if is_dict(kid) && _xml_slot(kid) {
            for use_kid in use_kids { kids = kids.append(use_kid) }
            slotted = true
         } else { kids = kids.append(kid) }
      }
      if use_kids.len > 0 && !slotted { for use_kid in use_kids { kids = kids.append(use_kid) } }
      return _xml_expand({"name": base.get("name", ""), "attr": merged, "text": base.get("text", ""), "children": kids}, components, depth + 1)
   }
   mut list kids = []
   for kid in element.get("children", []) {
      if is_dict(kid) && to_str(kid.get("name", "")) == "component" { continue }
      def expanded = _xml_expand(kid, components, depth + 1)
      if is_nil(expanded) { return nil }
      kids = kids.append(expanded)
   }
   {"name": tag, "attr": attrs, "text": element.get("text", ""), "children": kids}
}

fn _xml_placement(str value) any {
   def v = str.strip(value)
   mut int i = v.len > 0 && (load8(v, 0) & 255) == 45 ? 1 : 0
   mut bool digits = v.len > i
   while i < v.len {
      def int c = load8(v, i) & 255
      if c < 48 || c > 57 { digits = false break }
      i += 1
   }
   digits ? int(str.atoi(v)) : v
}

fn _xml_layout_node(dict element) any {
   def tag = to_str(element.get("name", ""))
   def interactive = _widget_tag(tag)
   if tag != "row" && tag != "column" && tag != "box" && tag != "text" && !interactive { return nil }
   def attrs = _xml_attrs(element)
   mut props = classes(attrs.get("class", ""))
   if tag == "row" || tag == "column" { props["dir"] = tag == "row" ? "row" : "col" }
   for attr in attrs.keys() {
      def key = to_str(attr)
      if key == "class" { continue }
      def value = to_str(attrs.get(attr, ""))
      if ["id", "align", "justify", "font", "justify_items", "align_items", "justify_self", "align_self", "grid_auto_flow"].contains(key) || ((interactive || tag == "menu") && key == "action") {
         props[key] = value
      } elif tag == "input" && (key == "value" || key == "placeholder") {
         props[key] = value
      } elif (tag == "slider" || tag == "spinbox") && (key == "value" || key == "min" || key == "max") {
         props[key] = float(str.atof(value))
      } elif tag == "spinbox" && key == "step" {
         def step = float(str.atof(value))
         if step <= 0.0 { return nil }
         props[key] = step
      } elif (tag == "spinbox" && key == "decimals") || (tag == "combo" && (key == "selected" || key == "max_visible")) {
         props[key] = int(str.atoi(value))
      } elif key == "grid_row" || key == "grid_column" {
         props[key] = _xml_placement(value)
      } elif key == "grid_row_span" || key == "grid_column_span" {
         def span = int(str.atoi(value))
         if span <= 0 { return nil }
         props[key] = span
      } elif key == "w" || key == "h" {
         if value == "grow" || value == "fit" || value == "auto" || value == "min-content" || value == "max-content" || str.endswith(value, "%") { props[key] = value }
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
   if tag == "menu" {
      mut list items = []
      for raw in raw_children {
         if !is_dict(raw) || raw.get("name", "") != "item" || raw.get("children", []).len != 0 { return nil }
         def iattrs = _xml_attrs(raw)
         def caption = to_str(raw.get("text", ""))
         def action = to_str(iattrs.get("action", ""))
         if caption.len == 0 || action.len == 0 { return nil }
         mut dict item = {"text": caption, "action": action, "id": to_str(iattrs.get("id", ""))}
         if iattrs.contains("shortcut") { item["shortcut"] = to_str(iattrs.get("shortcut")) }
         if iattrs.contains("checked") {
            def checked = to_str(iattrs.get("checked"))
            if checked != "true" && checked != "false" { return nil }
            item["checked"] = checked == "true"
         }
         for ik in iattrs.keys() {
            def k = to_str(ik)
            if k != "action" && k != "id" && k != "shortcut" && k != "checked" { return nil }
         }
         items = items.append(item)
      }
      if items.len == 0 || to_str(props.get("id", "")).len == 0 { return nil }
      props["items"] = items
      props["text"] = label
      props["widget"] = "menu"
      if !props.contains("pad") { props["pad"] = 6.0 }
      if !props.contains("min_w") { props["min_w"] = 96.0 }
      if !props.contains("min_h") { props["min_h"] = 28.0 }
      return node(props, [])
   }
   if tag == "combo" {
      mut list options = []
      for option in raw_children {
         if !is_dict(option) || option.get("name", "") != "option" || option.get("children", []).len != 0 || option.get("attr", dict(0)).len != 0 { return nil }
         def caption = to_str(option.get("text", ""))
         if caption.len == 0 { return nil }
         options = options.append(caption)
      }
      if options.len == 0 { return nil }
      props["options"] = options
      props["text"] = label
   } elif tag == "text" || interactive {
      if raw_children.len > 0 { return nil }
      props["text"] = label
   } elif label.len > 0 { return nil }
   if interactive {
      if to_str(props.get("id", "")).len == 0 || to_str(props.get("action", "")).len == 0 { return nil }
      props["widget"] = tag
      if !props.contains("pad") { props["pad"] = tag == "checkbox" ? [6.0, 6.0, 6.0, 26.0] : 6.0 }
      if tag == "slider" || tag == "input" || tag == "spinbox" || tag == "combo" {
         if !props.contains("min_w") { props["min_w"] = 96.0 }
         if !props.contains("min_h") { props["min_h"] = tag != "slider" && label.len > 0 ? 48.0 : 36.0 }
      }
      if tag == "combo" { return node(props, []) }
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
   "Reads rows, columns, boxes, text, buttons, checkboxes, sliders, inputs, spinboxes, and combos with option children. Interactive controls require a stable id and named action. The class attribute accepts flex utility classes. Element direction and explicit attributes override classes, and descendants inherit text styles. Returns nil for an unsupported layout."
   def root = xml.parse(source, true)
   if !is_dict(root) { return nil }
   mut dict components = dict(8)
   _xml_component_scan(root, components, 0)
   def expanded = _xml_expand(root, components, 0)
   if !is_dict(expanded) { return nil }
   def tree = _xml_layout_node(expanded)
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
   if value == "auto" || value == "fit" || value == "min-content" || value == "max-content" { return value }
   if str.endswith(value, "fr") {
      def amount = str.str_slice(value, 0, value.len - 2)
      def weight = amount.len == 0 ? 1.0 : max(0.01, float(str.atof(amount)))
      return {"grow": weight}
   }
   if str.endswith(value, "%") { return value }
   float(str.atof(value))
}

fn _grid_track_parts(any track) list {
   if !is_str(track) { return [track, 0.0, 1e18] }
   def value = str.strip(to_str(track))
   if !str.startswith(value, "minmax(") || !str.endswith(value, ")") { return [_grid_track_base(value), 0.0, 1e18] }
   def body = str.str_slice(value, 7, value.len - 1)
   def parts = str.split(body, ",")
   if parts.len != 2 { return [_grid_track_base(value), 0.0, 1e18] }
   def lower = _grid_track_base(str.strip(to_str(parts.get(0))))
   def upper = _grid_track_base(str.strip(to_str(parts.get(1))))
   if is_dict(upper) || _grid_intrinsic(upper) { return [upper, lower, 1e18] }
   [{"grow": 1.0}, lower, upper]
}

fn _grid_track_parse(any track) dict {
   "Splits a track spec into boundary line names and the sizing function. `[name]` before the size names the starting line; later names name the following line."
   if !is_str(track) { return {"pre": [], "size": _grid_track_parts(track), "post": []} }
   def spec = str.strip(to_str(track))
   mut list pre = []
   mut list post = []
   mut str body = ""
   mut bool seen_size = false
   mut int i = 0
   while i < spec.len {
      def int c = load8(spec, i) & 255
      if c == 91 {
         def int close = str.find(str.str_slice(spec, i + 1, spec.len), "]")
         if close < 0 { break }
         def name = str.strip(str.str_slice(spec, i + 1, i + 1 + close))
         if name.len > 0 { if seen_size { post = post.append(name) } else { pre = pre.append(name) } }
         i = i + close + 2
      } else {
         def ch = str.str_slice(spec, i, i + 1)
         if ch != " " && ch != "\t" { seen_size = true }
         body = body + ch
         i += 1
      }
   }
   def size = body.len > 0 ? _grid_track_parts(str.strip(body)) : _grid_track_parts("fit")
   {"pre": pre, "size": size, "post": post}
}

fn _grid_numeric(str value) bool {
   def v = str.strip(value)
   mut int i = v.len > 0 && (load8(v, 0) & 255) == 45 ? 1 : 0
   if i >= v.len { return false }
   while i < v.len {
      def int c = load8(v, i) & 255
      if c < 48 || c > 57 { return false }
      i += 1
   }
   true
}

fn _grid_line_of(any value, int lines, list names) int {
   "Resolves a 1-based, negative-from-the-end, or named grid line to a 0-based line index."
   if is_str(value) {
      def name = str.strip(to_str(value))
      if name.len == 0 { return -1 }
      if _grid_numeric(name) { return _grid_line_of(int(str.atoi(name)), lines, names) }
      for i in range(0, names.len) {
         if is_list(names.get(i, [])) && names.get(i, []).contains(name) { return min(i, lines - 1) }
      }
      return -1
   }
   def int v = int(float(value))
   if v > 0 { return min(lines - 1, v - 1) }
   if v < 0 { return max(0, lines + v) }
   -1
}

fn _grid_placement(any spec, any span_value, int lines, list names) list {
   "Resolves `start`, `start / end`, a number, or a name pair into [0-based start line, span]."
   if is_str(spec) {
      def s = str.strip(to_str(spec))
      def at = str.find(s, "/")
      if at >= 0 {
         def start = _grid_line_of(str.strip(str.str_slice(s, 0, at)), lines, names)
         def end = _grid_line_of(str.strip(str.str_slice(s, at + 1, s.len)), lines, names)
         if start >= 0 && end > start { return [start, end - start] }
         if start >= 0 { return [start, 1] }
         return [-1, 0]
      }
      if !_grid_numeric(s) {
         def start = _grid_line_of(s, lines, names)
         return start >= 0 ? [start, max(1, int(float(span_value)))] : [-1, 0]
      }
   }
   def start = _grid_line_of(spec, lines, names)
   start >= 0 ? [start, max(1, int(float(span_value)))] : [-1, 0]
}

fn _grid_order(int columns, int limit, bool by_column) list {
   mut out = []
   if by_column {
      for c in range(0, columns) {
         for r in range(0, limit) { out = out.append(r * columns + c) }
      }
   } else {
      for r in range(0, limit) {
         for c in range(0, columns) { out = out.append(r * columns + c) }
      }
   }
   out
}

fn _grid_occupy(dict slots, dict entry, int columns) int {
   def child = entry.get("child")
   def int row = int(entry.get("row", -1))
   def int col = int(entry.get("col", -1))
   def int cs = int(entry.get("cs", 1))
   def int rs = int(entry.get("rs", 1))
   mut int slot = -1
   if row >= 0 && col >= 0 { slot = row * columns + col }
   elif col >= 0 {
      slot = col
      while !_grid_slot_fits(slots, slot, cs, rs, columns) { slot += columns }
   } else {
      mut int c = 0
      while c + cs <= columns {
         def int candidate = row * columns + c
         if _grid_slot_fits(slots, candidate, cs, rs, columns) { slot = candidate break }
         c += 1
      }
      if slot < 0 { slot = (row + 1) * columns }
   }
   def int placed_row = slot / columns
   child["_grid_col"] = slot % columns
   child["_grid_row"] = placed_row
   child["_grid_cs"] = cs
   child["_grid_rs"] = rs
   for r in range(0, rs) {
      for c in range(0, cs) { slots[to_str(slot + r * columns + c)] = true }
   }
   placed_row + rs
}

fn _grid_slot_fits(dict slots, int slot, int cs, int rs, int columns) bool {
   if slot % columns + cs > columns { return false }
   for r in range(0, rs) {
      for c in range(0, cs) {
         if slots.contains(to_str(slot + r * columns + c)) { return false }
      }
   }
   true
}

fn grid(str id, list columns, list children, list rows=[], f64 column_gap=0.0, f64 row_gap=0.0, dict props=dict(0)) dict {
   "Builds a retained grid with shared row and column tracks. Items accept 1-based grid_row/grid_column and grid_row_span/grid_column_span. Explicit rectangles may overlap; auto-placement avoids occupied cells. Align within cells with justify_items/align_items or justify_self/align_self."
   if columns.len <= 0 { return node({"id": id, "w": "grow", "h": "grow"}, []) }
   def column_count = columns.len
   mut list col_tracks = []
   mut list col_names = []
   mut list pending_names = []
   for track in columns {
      def parsed = _grid_track_parse(track)
      col_tracks = col_tracks.append(parsed.get("size"))
      for name in parsed.get("pre", []) { pending_names = pending_names.append(name) }
      col_names = col_names.append(pending_names)
      pending_names = parsed.get("post", [])
   }
   col_names = col_names.append(pending_names)
   mut list row_tracks = []
   mut list row_names = []
   pending_names = []
   for track in rows {
      def parsed = _grid_track_parse(track)
      row_tracks = row_tracks.append(parsed.get("size"))
      for name in parsed.get("pre", []) { pending_names = pending_names.append(name) }
      row_names = row_names.append(pending_names)
      pending_names = parsed.get("post", [])
   }
   row_names = row_names.append(pending_names)
   def int declared_rows = row_tracks.len
   mut int row_count = declared_rows
   mut slots = dict(children.len * 2 + 4)
   mut kids = []
   for child in children {
      if is_dict(child) { kids = kids.append(node(child, _kids(child))) }
   }
   def flow = to_str(props.get("grid_auto_flow", "row"))
   def bool by_column = str.startswith(flow, "column")
   def bool dense = str.find(flow, "dense") >= 0
   ;; Resolve every explicit rectangle before automatic placement so the
   ;; implicit flow avoids all occupied cells.
   mut list auto_items = []
   mut list both_items = []
   mut list partial_items = []
   for child in kids {
      if _floating(child) { continue }
      mut int row = -1
      mut int col = -1
      mut int rs = 1
      mut int cs = 1
      if child.contains("grid_column") {
         def p = _grid_placement(child.get("grid_column", 0), child.get("grid_column_span", 1), column_count + 1, col_names)
         if p.get(0) >= 0 { col = int(p.get(0)) cs = min(column_count - col, max(1, int(p.get(1)))) }
      }
      if child.contains("grid_row") {
         def p = _grid_placement(child.get("grid_row", 0), child.get("grid_row_span", 1), max(1, declared_rows + 1), row_names)
         if p.get(0) >= 0 { row = int(p.get(0)) rs = max(1, int(p.get(1))) }
      }
      if row < 0 && col < 0 { auto_items = auto_items.append(child) }
      elif row >= 0 && col >= 0 { both_items = both_items.append({"child": child, "row": row, "col": col, "cs": cs, "rs": rs}) }
      else { partial_items = partial_items.append({"child": child, "row": row, "col": col, "cs": cs, "rs": rs}) }
   }
   ;; CSS auto-placement order: definite rectangles first, then items with one
   ;; definite axis, then fully automatic items.
   for entry in both_items { row_count = max(row_count, _grid_occupy(slots, entry, column_count)) }
   for entry in partial_items { row_count = max(row_count, _grid_occupy(slots, entry, column_count)) }
   ;; Automatic items follow the requested flow. Dense flow restarts the
   ;; cursor for every item; sparse flow continues past the previous one.
   def int limit = max(4, children.len + declared_rows + 2)
   def order = _grid_order(column_count, limit, by_column)
   mut int next_index = 0
   for child in auto_items {
      def int cs = min(column_count, max(1, int(float(child.get("grid_column_span", 1)))))
      def int rs = max(1, int(float(child.get("grid_row_span", 1))))
      mut int slot = -1
      mut int i = dense ? 0 : next_index
      while i < order.len {
         def int candidate = int(order.get(i))
         if _grid_slot_fits(slots, candidate, cs, rs, column_count) {
            slot = candidate
            if !dense { next_index = i + 1 }
            break
         }
         i += 1
      }
      if slot < 0 {
         slot = row_count * column_count
         while !_grid_slot_fits(slots, slot, cs, rs, column_count) { slot += column_count }
      }
      def int placed_row = slot / column_count
      child["_grid_col"] = slot % column_count
      child["_grid_row"] = placed_row
      child["_grid_cs"] = cs
      child["_grid_rs"] = rs
      for r in range(0, rs) {
         for c in range(0, cs) { slots[to_str(slot + r * column_count + c)] = true }
      }
      row_count = max(row_count, placed_row + rs)
   }
   mut dict container = {"id": id, "w": "grow", "h": "grow"}
   for k in props.keys() { container[k] = props.get(k) }
   def implicit = _grid_track_parts("1fr")
   while row_tracks.len < row_count { row_tracks = row_tracks.append(implicit) }
   while row_names.len < row_tracks.len + 1 { row_names = row_names.append([]) }
   container["_grid_x"] = col_tracks
   container["_grid_y"] = row_tracks
   container["_grid_cgap"] = max(0.0, column_gap)
   container["_grid_rgap"] = max(0.0, row_gap)
   node(container, kids)
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

fn _first_baseline(dict n) any {
   "First text baseline of a subtree, relative to its box top."
   def pad = _pad(n)
   if n.contains("_baseline") { return float(pad.get(0, 0.0)) + float(n.get("_baseline", 0.0)) }
   for c in _kids(n) {
      if !is_dict(c) || _floating(c) { continue }
      def b = _first_baseline(c)
      if !is_nil(b) { return float(pad.get(0, 0.0)) + float(b) }
   }
   nil
}

fn _kids(dict n) list {
   def c = n.get("children", [])
   is_list(c) ? c : []
}

fn _size_key(bool x) str { x ? "_w" : "_h" }

fn _grid_intrinsic(any value) bool {
   value == "auto" || value == "fit" || value == "min-content" || value == "max-content"
}

fn _grid_value(any value, f64 inner) f64 {
   if is_str(value) {
      if !str.endswith(value, "%") { return 0.0 }
      return float(str.atof(str.str_slice(value, 0, value.len - 1))) * inner / 100.0
   }
   is_dict(value) ? 0.0 : float(max(0.0, float(value)))
}

fn _grid_gap(dict n, bool x) f64 { float(n.get(x ? "_grid_cgap" : "_grid_rgap", 0.0)) }

fn _grid_start(dict c, bool x) int { int(c.get(x ? "_grid_col" : "_grid_row", 0)) }

fn _grid_span(dict c, bool x) int { int(c.get(x ? "_grid_cs" : "_grid_rs", 1)) }

fn _grid_sum(list sizes, int first, int end, f64 gap) f64 {
   mut f64 total = gap * float(max(0, end - first - 1))
   for i in range(first, end) { total += float(sizes.get(i)) }
   total
}

fn _grid_share(list sizes, list limits, list weights, int first, int end, f64 free, bool intrinsic=false) int {
   while free > 0.001 {
      mut f64 total = 0.0
      for i in range(first, end) {
         if float(sizes.get(i)) < float(limits.get(i)) {
            def f64 weight = float(weights.get(i))
            total += intrinsic ? 1.0 : weight
         }
      }
      if total <= 0.0 { break }
      mut f64 spent = 0.0
      for i in range(first, end) {
         def f64 size = float(sizes.get(i))
         def f64 headroom = float(limits.get(i)) - size
         def f64 weight = intrinsic ? 1.0 : float(weights.get(i))
         if headroom <= 0.0 || weight <= 0.0 { continue }
         def f64 add = float(min(headroom, free * weight / total))
         sizes[i] = size + add
         spent += add
      }
      if spent <= 0.001 { break }
      free -= spent
   }
   0
}

fn _grid_axis(dict n, bool x, f64 inner, bool distribute) list {
   def tracks = n.get(x ? "_grid_x" : "_grid_y", [])
   def kids = _kids(n)
   def gap = _grid_gap(n, x)
   mut sizes = []
   mut limits = []
   mut weights = []
   mut content_limits = []
   for track in tracks {
      def spec = track.get(0)
      def lower = track.get(1)
      def bool flexible = is_dict(spec)
      def bool intrinsic = _grid_intrinsic(spec) || _grid_intrinsic(lower)
      def f64 base = float(max(_grid_value(spec, inner), _grid_value(lower, inner)))
      def f64 cap = flexible || intrinsic ? float(max(base, _grid_value(track.get(2), inner))) : base
      sizes = sizes.append(base)
      limits = limits.append(cap)
      weights = weights.append(flexible ? max(0.0, float(spec.get("grow", 1.0))) : 0.0)
      content_limits = content_limits.append(intrinsic ? cap : base)
   }
   ;; Single-track contributions establish shared sizes first. Then a span
   ;; distributes only its unmet contribution, respecting fixed tracks/caps.
   for phase in range(0, 2) {
      for c in kids {
         if _floating(c) { continue }
         def int span = _grid_span(c, x)
         if (phase == 0) != (span == 1) { continue }
         def int first = _grid_start(c, x)
         def int end = first + span
         def spec = tracks.get(first).get(0)
         def lower = tracks.get(first).get(1)
         def bool min_content = spec == "min-content" || lower == "min-content" || lower == "auto"
         def f64 measured = float(c.get(x ? "_fit_w" : "_fit_h", 0.0))
         def f64 contribution = min_content ? float(c.get(x ? "_cmin_w" : "_cmin_h", measured)) : measured
         def f64 want = _kind(c, x) == "fixed" ? float(c.get(_size_key(x), 0.0)) : _clamp(c, x, contribution)
         if span == 1 {
            sizes[first] = max(float(sizes.get(first)), min(float(content_limits.get(first)), want))
         } else {
            _grid_share(sizes, content_limits, weights, first, end, want - _grid_sum(sizes, first, end, gap), true)
         }
      }
   }
   if distribute { _grid_share(sizes, limits, weights, 0, tracks.len, inner - _grid_sum(sizes, 0, tracks.len, gap)) }
   sizes
}

fn _grid_fit_extent(dict n, bool x) f64 {
   def sizes = _grid_axis(n, x, 0.0, false)
   _grid_sum(sizes, 0, sizes.len, _grid_gap(n, x))
}

fn _grid_grow(dict n, bool x, f64 inner) int {
   def sizes = _grid_axis(n, x, inner, true)
   def f64 gap = _grid_gap(n, x)
   def mode = to_str(n.get(x ? "justify_content" : "align_content", "start"))
   def f64 free = max(0.0, inner - _grid_sum(sizes, 0, sizes.len, gap))
   def int count = sizes.len
   mut f64 step = gap
   mut f64 cursor = 0.0
   if mode == "center" { cursor = free * 0.5 }
   elif mode == "end" { cursor = free }
   elif mode == "between" && count > 1 { step += free / float(count - 1) }
   elif mode == "around" && count > 0 { cursor = free / float(count) * 0.5 step += cursor * 2.0 }
   elif mode == "evenly" { cursor = free / float(count + 1) step += cursor }
   mut starts = []
   for size in sizes {
      starts = starts.append(cursor)
      cursor += float(size) + step
   }
   def key = _size_key(x)
   def default_align = n.get(x ? "justify_items" : "align_items", "stretch")
   ;; Baseline groups align the first text baseline of items sharing a row.
   mut dict row_baseline = dict(8)
   if !x {
      for c in _kids(n) {
         if _floating(c) || _grid_span(c, false) != 1 { continue }
         def b = _first_baseline(c)
         if is_nil(b) { continue }
         def rk = to_str(_grid_start(c, false))
         row_baseline[rk] = max(float(row_baseline.get(rk, -1e18)), float(b))
      }
   }
   for c in _kids(n) {
      def k = _kind(c, x)
      if _floating(c) {
         if k == "grow" { c[key] = _clamp(c, x, inner) }
         elif k == "pct" { c[key] = _clamp(c, x, inner * _pct(c, x)) }
      } else {
         def int first = _grid_start(c, x)
         def int last = first + _grid_span(c, x) - 1
         def f64 start = float(starts.get(first, 0.0))
         def f64 extent = float(starts.get(last, 0.0)) - start + float(sizes.get(last, 0.0))
         def align = c.get(x ? "justify_self" : "align_self", default_align)
         if k == "grow" || (align == "stretch" && k == "fit" && float(c.get("aspect", 0.0)) <= 0.0) {
            c[key] = _clamp(c, x, extent)
         } elif k == "pct" { c[key] = _clamp(c, x, extent * _pct(c, x)) }
         def f64 remainder = extent - float(c.get(key, 0.0))
         mut f64 offset = align == "end" ? remainder : (align == "center" ? remainder * 0.5 : 0.0)
         if !x && align == "baseline" && _grid_span(c, false) == 1 {
            def b = _first_baseline(c)
            def rb = row_baseline.get(to_str(first), nil)
            if !is_nil(b) && !is_nil(rb) { offset = float(rb) - float(b) }
         }
         c[x ? "_grid_offset_x" : "_grid_offset_y"] = start + offset
      }
      _grow(c, x)
   }
   0
}

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
         n["_baseline"] = lh * 0.8
         content = is_list(lines) ? lh * float(lines.len) : float(_measure(measure, s, n).get(1, lh))
      }
   }
   def kids = _kids(n)
   def bool grid_parent = n.contains("_grid_x")
   def bool main = _row(n) == x
   def f64 gap = float(n.get("gap", 0.0))
   mut int flow = 0
   for c in kids {
      def f64 cs = _fit(c, x, measure)
      if grid_parent || _floating(c) { continue }
      flow += 1
      if main { content += cs } else { content = float(max(content, cs)) }
   }
   if grid_parent { content = _grid_fit_extent(n, x) }
   elif main && flow > 1 { content += gap * float(flow - 1) }
   ;; Intrinsic minimum: wrapping text floors at its widest word, containers
   ;; combine their children, and fixed boxes keep their declared size.
   mut f64 content_min = content
   if n.contains("text") {
      if x { content_min = bool(n.get("wrap", false)) ? float(n.get("_text_min_w", content)) : content }
      else { content_min = float(n.get("_text_line_height", content)) }
   } elif !grid_parent {
      content_min = 0.0
      for c in kids {
         if _floating(c) { continue }
         def f64 cm = float(c.get(x ? "_cmin_w" : "_cmin_h", 0.0))
         if main { content_min += cm } else { content_min = float(max(content_min, cm)) }
      }
      if main && flow > 1 { content_min += gap * float(flow - 1) }
   }
   def km = _kind(n, x)
   n[x ? "_cmin_w" : "_cmin_h"] = km == "fixed" ? _clamp(n, x, _fixed(n, x)) : _clamp(n, x, content_min + pads)
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
   if n.contains("_grid_x") { return _grid_grow(n, x, float(max(0.0, inner))) }
   def kids = _kids(n)
   def bool main = _row(n) == x
   def f64 gap = float(n.get("gap", 0.0))
   def key = _size_key(x)
   ;; Percent sizes resolve against the parent's content box.
   for c in kids {
      if _kind(c, x) == "pct" {
         c[key] = _clamp(c, x, float(max(0.0, inner)) * _pct(c, x))
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
   def bool grid_parent = n.contains("_grid_x")
   def bool main = _row(n)
   def f64 gap = float(n.get("gap", 0.0))
   mut int flow = 0
   for c in kids {
      if grid_parent || _floating(c) { continue }
      def f64 child_w = float(c.get("_w", 0.0))
      if main { content += child_w } else { content = float(max(content, child_w)) }
      flow += 1
   }
   if grid_parent { content = _grid_fit_extent(n, true) }
   elif main && flow > 1 { content += gap * float(flow - 1) }
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
   def bool grid_parent = n.contains("_grid_x")
   def bool row = _row(n)
   def f64 gap = float(n.get("gap", 0.0))
   mut f64 used = 0.0
   mut int flow_count = 0
   for c in kids {
      if grid_parent || _floating(c) { continue }
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
      if grid_parent {
         c["_flex_place_x"] = ix + float(c.get("_grid_offset_x", 0.0)) + sx
         c["_flex_place_y"] = iy + float(c.get("_grid_offset_y", 0.0)) + sy
      } else {
         def f64 cw = float(c.get("_w", 0.0))
         def f64 ch = float(c.get("_h", 0.0))
         def f64 cross_free = row ? ih - ch : iw - cw
         def f64 off = al == "center" ? cross_free * 0.5 : (al == "end" ? cross_free : 0.0)
         c["_flex_place_x"] = (row ? ix + cursor : ix + off) + sx
         c["_flex_place_y"] = (row ? iy + off : iy + cursor) + sy
         cursor += (row ? cw : ch) + between
      }
      def int z = int(c.get("z", 0))
      def key = to_str(z)
      if !z_buckets.contains(key) {
         z_levels = z_levels.append(z)
         z_buckets[key] = []
      }
      z_buckets[key] = z_buckets.get(key, []).append(c)
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

;; ---- typed arena fast path ------------------------------------------------
;; Nodes live in one raw native arena (malloc_raw/realloc only when nodes are
;; added). Every property and solved coordinate is an int64 in 1/1024 px, so
;; the measure/resolve/place passes use raw loads and stores: no dict lookups,
;; no list slots, no float boxing and no allocation while solving.
;; Text, grid, float, scroll, aspect and transitions stay on `solve`.
struct FastBox { str id, f64 x, f64 y, f64 w, f64 h }

;; 208-byte stride; offsets documented beside each store.
mut any _fa_mem = nil
mut int _fa_cap = 0
mut int _fa_n = 0
mut int _fa_gen = 0
mut int _fo_n = 0
mut list<str> _fa_ids = []

fn _fx(any v) int { int(float(v) * 1024.0) }

fn _fast_kind(any v) list {
   if is_dict(v) { return [1, float(v.get("grow", 1.0))] }
   if is_str(v) {
      def s = str.strip(to_str(v))
      if s.len == 0 || s == "fit" { return [3, 0.0] }
      if s == "grow" { return [1, 1.0] }
      if str.str_slice(s, s.len - 1, s.len) == "%" { return [2, float(str.atoi(str.str_slice(s, 0, s.len - 1))) / 100.0] }
      return [0, float(str.atoi(s))]
   }
   if v == nil { return [3, 0.0] }
   [0, float(v)]
}

fn _fast_justify(any v) int {
   def s = to_str(v)
   s == "center" ? 1 : (s == "end" ? 2 : (s == "between" ? 3 : (s == "around" ? 4 : (s == "evenly" ? 5 : 0))))
}

fn _fast_align(any v) int {
   def s = to_str(v)
   s == "center" ? 1 : (s == "end" ? 2 : (s == "stretch" ? 3 : 0))
}

fn fast_node(dict props, list children=[]) int {
   "Creates a raw-arena layout node and returns its handle."
   if _fa_n >= _fa_cap {
      mut int cap = _fa_cap * 2
      if cap < 256 { cap = 256 }
      _fa_mem = _fa_mem == nil ? malloc_raw(cap * 208) : realloc(_fa_mem, cap * 208)
      _fa_cap = cap
   }
   def int idx = _fa_n
   _fa_n += 1
   def any p = ptr_add(_fa_mem, idx * 208)
   def wd = _fast_kind(props.get("w", "fit"))
   def hd = _fast_kind(props.get("h", "fit"))
   def pad = _pad(props)
   store64_i(p, to_str(props.get("dir", "row")) == "col" ? 1 : 0, 0)
   store64_i(p, int(wd.get(0)), 8)
   store64_i(p, _fx(wd.get(1)), 16)
   store64_i(p, int(hd.get(0)), 24)
   store64_i(p, _fx(hd.get(1)), 32)
   def any mw = props.get("max_w", nil)
   def any mh = props.get("max_h", nil)
   store64_i(p, _fx(props.get("min_w", 0.0)), 40)
   store64_i(p, mw == nil ? 1152921504606846976 : _fx(mw), 48)
   store64_i(p, _fx(props.get("min_h", 0.0)), 56)
   store64_i(p, mh == nil ? 1152921504606846976 : _fx(mh), 64)
   store64_i(p, _fx(pad.get(1)), 72)
   store64_i(p, _fx(pad.get(0)), 80)
   store64_i(p, _fx(pad.get(3)), 88)
   store64_i(p, _fx(pad.get(2)), 96)
   store64_i(p, _fx(props.get("gap", 0.0)), 104)
   store64_i(p, _fast_justify(props.get("justify", "start")), 112)
   store64_i(p, _fast_align(props.get("align", "start")), 120)
   store64_i(p, -1, 128)
   store64_i(p, -1, 136)
   store64_i(p, children.len, 144)
   store64_i(p, 0, 152)
   store64_i(p, 0, 160)
   store64_i(p, 0, 168)
   store64_i(p, 0, 176)
   store64_i(p, 0, 184)
   store64_i(p, 0, 192)
   store64_i(p, 0, 200)
   _fa_ids = _fa_ids.append(to_str(props.get("id", "")))
   mut int prev = -1
   mut int ci = 0
   while ci < children.len {
      def int child = int(children[ci])
      if prev < 0 { store64_i(p, child, 128) }
      else { store64_i(ptr_add(_fa_mem, prev * 208), child, 136) }
      prev = child
      ci += 1
   }
   idx
}

fn _fa_measure() any {
   mut int i = _fa_n - 1
   while i >= 0 {
      def any p = ptr_add(_fa_mem, i * 208)
      def int nk = load64_i(p, 144)
      def int dir = load64_i(p, 0)
      def int gap = load64_i(p, 104)
      mut int cw = 0
      mut int ch = 0
      mut int c = load64_i(p, 128)
      while c >= 0 {
         def any cp = ptr_add(_fa_mem, c * 208)
         if dir == 0 { cw += load64_i(cp, 152) ch = max(ch, load64_i(cp, 160)) }
         else { cw = max(cw, load64_i(cp, 152)) ch += load64_i(cp, 160) }
         c = load64_i(cp, 136)
      }
      if nk > 1 { if dir == 0 { cw += gap * (nk - 1) } else { ch += gap * (nk - 1) } }
      def int pw = load64_i(p, 72) + load64_i(p, 88)
      def int ph = load64_i(p, 80) + load64_i(p, 96)
      def int fw = load64_i(p, 8) == 0 ? load64_i(p, 16) : cw + pw
      def int fh = load64_i(p, 24) == 0 ? load64_i(p, 32) : ch + ph
      store64_i(p, max(load64_i(p, 40), min(load64_i(p, 48), fw)), 152)
      store64_i(p, max(load64_i(p, 56), min(load64_i(p, 64), fh)), 160)
      i -= 1
   }
   0
}

fn _fa_resolve(int root, f64 width, f64 height) any {
   def any rp = ptr_add(_fa_mem, root * 208)
   def int rw = int(width * 1024.0)
   def int rh = int(height * 1024.0)
   _fa_gen += 1
   store64_i(rp, max(load64_i(rp, 40), min(load64_i(rp, 48), rw)), 168)
   store64_i(rp, max(load64_i(rp, 56), min(load64_i(rp, 64), rh)), 176)
   store64_i(rp, _fa_gen, 200)
   mut int i = _fa_n - 1
   while i >= 0 {
      def any p = ptr_add(_fa_mem, i * 208)
      if load64_i(p, 200) == _fa_gen {
         def int nk = load64_i(p, 144)
         def int dir = load64_i(p, 0)
         def int align = load64_i(p, 120)
         def int w = load64_i(p, 168)
         def int h = load64_i(p, 176)
         def int iw = max(0, w - load64_i(p, 72) - load64_i(p, 88))
         def int ih = max(0, h - load64_i(p, 80) - load64_i(p, 96))
         def int gap = load64_i(p, 104)
         def bool row = dir == 0
         mut int used_w = 0
         mut int gw = 0
         mut int used_h = 0
         mut int gh = 0
         mut int c = load64_i(p, 128)
         while c >= 0 {
            def any cp = ptr_add(_fa_mem, c * 208)
            def int cwk = load64_i(cp, 8)
            def int chk = load64_i(cp, 24)
            if row {
               mut int cw = load64_i(cp, 152)
               if cwk == 0 { cw = load64_i(cp, 16) }
               elif cwk == 2 { cw = iw * load64_i(cp, 16) / 1024 }
               elif cwk == 1 { cw = 0 }
               used_w += max(load64_i(cp, 40), min(load64_i(cp, 48), cw))
               if cwk == 1 { gw += 1 }
            } else {
               mut int chh = load64_i(cp, 160)
               if chk == 0 { chh = load64_i(cp, 32) }
               elif chk == 2 { chh = ih * load64_i(cp, 32) / 1024 }
               elif chk == 1 { chh = 0 }
               used_h += max(load64_i(cp, 56), min(load64_i(cp, 64), chh))
               if chk == 1 { gh += 1 }
            }
            c = load64_i(cp, 136)
         }
         used_w += gap * max(0, nk - 1)
         used_h += gap * max(0, nk - 1)
         def int sw = gw > 0 ? max(0, iw - used_w) / gw : 0
         def int sh = gh > 0 ? max(0, ih - used_h) / gh : 0
         c = load64_i(p, 128)
         while c >= 0 {
            def any cp = ptr_add(_fa_mem, c * 208)
            def int cwk = load64_i(cp, 8)
            def int chk = load64_i(cp, 24)
            mut int tw = load64_i(cp, 152)
            if cwk == 0 { tw = load64_i(cp, 16) }
            elif cwk == 2 { tw = iw * load64_i(cp, 16) / 1024 }
            elif row && cwk == 1 { tw = sw }
            elif !row && (cwk == 1 || (align == 3 && cwk == 3)) { tw = iw }
            mut int th = load64_i(cp, 160)
            if chk == 0 { th = load64_i(cp, 32) }
            elif chk == 2 { th = ih * load64_i(cp, 32) / 1024 }
            elif !row && chk == 1 { th = sh }
            elif row && (chk == 1 || (align == 3 && chk == 3)) { th = ih }
            store64_i(cp, max(load64_i(cp, 40), min(load64_i(cp, 48), tw)), 168)
            store64_i(cp, max(load64_i(cp, 56), min(load64_i(cp, 64), th)), 176)
            store64_i(cp, _fa_gen, 200)
            c = load64_i(cp, 136)
         }
      }
      i -= 1
   }
   0
}

fn _fa_place(list out) any {
   mut int i = _fa_n - 1
   while i >= 0 {
      def any p = ptr_add(_fa_mem, i * 208)
      if load64_i(p, 200) == _fa_gen {
         def int w = load64_i(p, 168)
         def int h = load64_i(p, 176)
         def int x = load64_i(p, 184)
         def int y = load64_i(p, 192)
         if _fo_n < out.len {
            out[_fo_n] = FastBox(_fa_ids[i], float(x) / 1024.0, float(y) / 1024.0, float(w) / 1024.0, float(h) / 1024.0)
         }
         _fo_n += 1
         def int nk = load64_i(p, 144)
         if nk > 0 {
            def int iw = max(0, w - load64_i(p, 72) - load64_i(p, 88))
            def int ih = max(0, h - load64_i(p, 80) - load64_i(p, 96))
            def bool row = load64_i(p, 0) == 0
            def int gap = load64_i(p, 104)
            def int align = load64_i(p, 120)
            def int justify = load64_i(p, 112)
            mut int used = 0
            mut int c = load64_i(p, 128)
            while c >= 0 { used += row ? load64_i(ptr_add(_fa_mem, c * 208), 168) : load64_i(ptr_add(_fa_mem, c * 208), 176) c = load64_i(ptr_add(_fa_mem, c * 208), 136) }
            used += gap * max(0, nk - 1)
            def int free = max(0, (row ? iw : ih) - used)
            mut int lead = 0
            mut int between = gap
            if justify == 1 { lead = free / 2 }
            elif justify == 2 { lead = free }
            elif justify == 3 && nk > 1 { between = gap + free / (nk - 1) }
            elif justify == 4 { lead = free / nk / 2 between = gap + free / nk }
            elif justify == 5 { lead = free / (nk + 1) between = gap + free / (nk + 1) }
            mut int cursor = lead
            c = load64_i(p, 128)
            while c >= 0 {
               def any cp = ptr_add(_fa_mem, c * 208)
               def int cw = load64_i(cp, 168)
               def int ch = load64_i(cp, 176)
               def int cross = row ? ih - ch : iw - cw
               def int off = align == 1 ? cross / 2 : (align == 2 ? cross : 0)
               store64_i(cp, x + load64_i(p, 72) + (row ? cursor : off), 184)
               store64_i(cp, y + load64_i(p, 80) + (row ? off : cursor), 192)
               cursor += (row ? cw : ch) + between
               c = load64_i(cp, 136)
            }
         }
      }
      i -= 1
   }
   0
}

fn fast_boxes(int n) list {
   "Builds a caller-owned box arena with `n` slots for fast_solve_into."
   mut list out = []
   mut i = 0
   while i < n { out = out.append(FastBox("", 0.0, 0.0, 0.0, 0.0)) i += 1 }
   out
}

fn fast_solve_into(int root, f64 width, f64 height, list out) int {
   "Lays out into a caller-owned fast_boxes arena; nothing is allocated per solve. Returns the box count."
   _fo_n = 0
   if _fa_n > 0 {
      _fa_measure()
      _fa_resolve(root, width, height)
      _fa_place(out)
   }
   _fo_n
}

fn fast_solve(int root, f64 width, f64 height) list {
   "Lays out a typed `fast_node` tree in a width x height viewport and returns FastBox records."
   def list out = fast_boxes(_fa_n)
   fast_solve_into(root, width, height, out)
   out
}

fn fast_rect(list boxes, str id) list {
   "Returns [x, y, w, h] for `id` in a fast_solve result, or [] when absent."
   for box in boxes {
      def FastBox b = box
      if b.id == id { return [b.x, b.y, b.w, b.h] }
   }
   []
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
      if _widget_tag(widget) {
         mut payload = {"text": node.get("text", ""), "action": node.get("action", "")}
         if widget == "checkbox" { payload["checked"] = node.get("checked", false) }
         elif widget == "slider" {
            payload["min"] = node.get("min", 0.0)
            payload["max"] = node.get("max", 1.0)
            payload["value"] = node.get("value", payload.get("min"))
         } elif widget == "input" {
            payload["value"] = node.get("value", "")
            payload["placeholder"] = node.get("placeholder", "")
         } elif widget == "spinbox" {
            payload["min"] = node.get("min", 0.0)
            payload["max"] = node.get("max", 0.0)
            payload["step"] = node.get("step", 1.0)
            payload["decimals"] = node.get("decimals", 0)
            payload["value"] = node.get("value", payload.get("min"))
         } elif widget == "combo" {
            payload["options"] = node.get("options", [])
            payload["selected"] = node.get("selected", 0)
            payload["max_visible"] = node.get("max_visible", 6)
         } elif widget == "menu" {
            payload["items"] = node.get("items", [])
         }
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
