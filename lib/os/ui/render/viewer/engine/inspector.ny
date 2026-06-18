;; Keywords: engine inspector properties transform material os ui render viewer scene
;; Inspector UI helpers for transforms, materials, camera, and view settings.
;; References:
;; - std.os.ui.render.viewer.engine.state
;; - std.os.ui.render.scene
module std.os.ui.render.viewer.engine.inspector(TAB_ITEMS, COUNTER_PROPS, draw_body)
use std.core
use std.core.str as str
use std.math (clamp, floor, max)
use std.os.ui.render.viewer.app as ui_app
use std.os.ui.render.viewer.gui as gui
use std.os.ui.render.viewer.editor.colorpicker as colorpicker
use std.os.ui.render.viewer.icons as icons
use std.os.ui.render.dump as ui_profile

def list TAB_ITEMS = ["Scene", "View", "Profile"]
def list COUNTER_PROPS = [
   ["Draws", "draws"], ["Dynamic draws", "dynamic_draws"], ["Static draws", "static_draws"],
   ["Indexed draws", "indexed_draws"], ["Flushes", "flushes"], ["Pipeline binds", "pipeline_binds"],
   ["Descriptor binds", "descriptor_binds"], ["Submitted vertices", "submitted_vertices"]
]

def list ALPHA_MODE_ITEMS = ["OPAQUE", "MASK", "BLEND"]
def list AXIS_ITEMS = ["Free", "X", "Y", "Z"]
def list TRANSFORM_ACTION_ITEMS = ["Actions", "Reset", "Frame", "Unit Scale", "Zero Rotation", "Snap Values"]

fn _bool_text(any v) str { bool(v) ? "on" : "off" }

fn _txt(any value, str fallback="") str {
   if value == nil { return fallback }
   def out = to_str(value)
   out == "<nil>" ? fallback : out
}

fn _num(any value, f64 fallback=0.0) f64 {
   if value == nil { return fallback }
   float(value)
}

fn _intv(any value, int fallback=0) int {
   if value == nil { return fallback }
   int(value)
}

fn _rad_to_deg(any value) f64 { _num(value) * 57.29577951308232 }

fn _deg_to_rad(any value) f64 { _num(value) * 0.017453292519943295 }

fn _safe_id(any label) str {
   str.str_replace(str.str_replace(str.str_replace(to_str(label), "/", "_"), "\\", "_"), ":", "_")
}

fn _prop_value(any value) str {
   if value == nil { return "-" }
   if is_str(value) {
      def s = str.strip(to_str(value))
      def low = str.lower(s)
      if s.len == 0 || s == "<nil>" || s == "<none>" || low == "none" { return "-" }
      return s
   }
   if is_int(value) || is_float(value) || is_bool(value) { return to_str(value) }
   if is_list(value) { return "[" + to_str(value.len) + "]" }
   if is_dict(value) { return "{dict}" }
   "<value>"
}

fn _prop(any label, any value, any tone=[0.74, 0.74, 0.74, 1.0]) any {
   def c = is_list(tone) ? tone : [0.74, 0.74, 0.74, 1.0]
   gui.property_row("prop_" + _safe_id(label), label, _prop_value(value), c)
}

fn _prop_bool(any label, any value) any {
   _prop(label, _bool_text(value), bool(value) ? [0.78, 0.78, 0.78, 1.0] : [0.58, 0.58, 0.58, 1.0])
}

fn _gizmo_label(any mode) str {
   case int(mode){
      1 -> "rotate"
      2 -> "scale"
      _ -> "move"
   }
}

fn _axis_label(any axis) str {
   case int(axis){
      1 -> "X"
      2 -> "Y"
      3 -> "Z"
      _ -> "Free"
   }
}

fn _panel_w(any st, f64 fallback=390.0) f64 {
   def w = _num(is_dict(st) ? st.get("inspector_w", fallback) : fallback, fallback)
   w > 1.0 ? w : fallback
}

fn _packed_cell_w(any st) f64 {
   clamp((_panel_w(st) - 56.0) / 3.0, 92.0, 132.0)
}

fn _wide_panel(any st) bool { _panel_w(st) >= 430.0 }

fn _snap_value(any value, f64 step) f64 {
   def s = max(float(step), 0.000001)
   floor((_num(value) / s) + 0.5) * s
}

fn _slider3(str idp, str title, f64 x0, f64 y0, f64 z0, f64 lo, f64 hi, f64 cell_w, bool packed) list {
   gui.text_colored(title, [0.70, 0.70, 0.70, 0.94])
   if packed {
      def x1 = gui.slider_float(idp + "_x", "X", x0, lo, hi, cell_w)
      gui.same_line(4.0)
      def y1 = gui.slider_float(idp + "_y", "Y", y0, lo, hi, cell_w)
      gui.same_line(4.0)
      def z1 = gui.slider_float(idp + "_z", "Z", z0, lo, hi, cell_w)
      return [x1, y1, z1]
   }
   [
      gui.slider_float(idp + "_x", "X", x0, lo, hi),
      gui.slider_float(idp + "_y", "Y", y0, lo, hi),
      gui.slider_float(idp + "_z", "Z", z0, lo, hi)
   ]
}

fn _mode_button(str id, str icon_name, str label, bool selected) bool {
   def icon = ui_profile.env_toggle_cached("NY_UI_SVG_INSPECTOR_ICONS", false) ? icons.icon_sprite(icon_name) : -1
   gui.icon_button(id, icon, label, 96.0, 30.0, selected)
}

fn _axis_button(str id, str label, bool selected) bool {
   gui.icon_button(id, -1, label, 56.0, 30.0, selected)
}

fn _props_int_dict(any src, any specs) any {
   mut i = 0
   def n = is_list(specs) ? specs.len : 0
   while i < n {
      def row = specs.get(i, [])
      _prop(row.get(0, ""), _intv(src.get(to_str(row.get(1, "")), 0)))
      i += 1
   }
}

fn _action(any state, any action) dict {
   mut st = state
   if to_str(action).len > 0 { st["action"] = action }
   st
}

fn _list_field(any obj, str key) list {
   if !is_dict(obj) { return [] }
   def v = obj.get(key, [])
   is_list(v) ? v : []
}

fn _scene_gltf_root(any scene) any {
   if !is_dict(scene) { return 0 }
   def gltf_data = scene.get("gltf_data", 0)
   is_dict(gltf_data) ? gltf_data.get("gltf", 0) : 0
}

fn _scene_materials(any scene) list { _list_field(_scene_gltf_root(scene), "materials") }

fn _scene_mat_records(any scene) list {
   if !is_dict(scene) { return [] }
   def recs = scene.get("mat_records", [])
   is_list(recs) ? recs : []
}

fn _material_items(list materials, int count) list {
   mut out = []
   mut i = 0
   while i < count {
      def mat = materials.get(i, 0)
      def name = is_dict(mat) ? to_str(mat.get("name", "")) : ""
      out = out.append("Mat " + to_str(i) + (name.len > 0 ? ("  " + name) : ""))
      i += 1
   }
   out
}

fn _clamp_index(int idx, int n) int {
   if n <= 0 { return -1 }
   if idx < 0 { return 0 }
   idx >= n ? (n - 1) : idx
}

fn _material_index(any scene, int selected_material_idx) int {
   def materials = _scene_materials(scene)
   def recs = _scene_mat_records(scene)
   def count = max(materials.len, recs.len)
   if count <= 0 { return -1 }
   if selected_material_idx >= 0 { return _clamp_index(selected_material_idx, count) }
   _clamp_index(0, count)
}

fn _raw_material(any scene, int mat_idx) any {
   def materials = _scene_materials(scene)
   (mat_idx >= 0 && mat_idx < materials.len) ? materials.get(mat_idx, 0) : 0
}

fn _mat_record(any scene, int mat_idx) any {
   def recs = _scene_mat_records(scene)
   (mat_idx >= 0 && mat_idx < recs.len) ? recs.get(mat_idx, 0) : 0
}

fn _pbr(any mat) any {
   if !is_dict(mat) { return 0 }
   mat.get("pbrMetallicRoughness", 0)
}

fn _tex_idx(any owner, str key) int {
   if !is_dict(owner) { return -1 }
   def tex = owner.get(key, 0)
   is_dict(tex) ? int(tex.get("index", -1)) : -1
}

fn _u32_rgba(int c) list {
   [
      float(band(c, 255)) / 255.0,
      float(band(bshr(c, 8), 255)) / 255.0,
      float(band(bshr(c, 16), 255)) / 255.0,
      float(band(bshr(c, 24), 255)) / 255.0
   ]
}

fn _material_base_rgba(any raw, any rec) list {
   def pbr = _pbr(raw)
   if is_dict(pbr) && is_list(pbr.get("baseColorFactor", 0)) {
      return colorpicker.rgba(pbr.get("baseColorFactor", [1, 1, 1, 1]), [1, 1, 1, 1])
   }
   is_dict(rec) ? _u32_rgba(int(rec.get("base_color_u32", 0xffffffff))) : [1, 1, 1, 1]
}

fn _material_word_factor(any rec, int shift, f64 fallback) f64 {
   if !is_dict(rec) { return fallback }
   float(band(bshr(int(rec.get("material_u32", 0x0000ff00)), shift), 255)) / 255.0
}

fn _material_metallic(any raw, any rec) f64 {
   if is_dict(rec) && rec.contains("metallic_factor") { return clamp(float(rec.get("metallic_factor", 1.0)), 0.0, 1.0) }
   def pbr = _pbr(raw)
   is_dict(pbr) ? clamp(float(pbr.get("metallicFactor", _material_word_factor(rec, 0, 1.0))), 0.0, 1.0) : _material_word_factor(rec, 0, 1.0)
}

fn _material_roughness(any raw, any rec) f64 {
   if is_dict(rec) && rec.contains("roughness_factor") { return clamp(float(rec.get("roughness_factor", 1.0)), 0.0, 1.0) }
   def pbr = _pbr(raw)
   is_dict(pbr) ? clamp(float(pbr.get("roughnessFactor", _material_word_factor(rec, 8, 1.0))), 0.0, 1.0) : _material_word_factor(rec, 8, 1.0)
}

fn _alpha_mode_code(str mode) int {
   case mode {
      "MASK" -> { return 1 }
      "BLEND" -> { return 2 }
      _ -> { return 0 }
   }
}

fn _alpha_mode_from_code(int code) str {
   case code {
      1 -> { return "MASK" }
      2 -> { return "BLEND" }
      _ -> { return "OPAQUE" }
   }
}

fn _alpha_mode_index(str mode) int {
   case mode {
      "MASK" -> { return 1 }
      "BLEND" -> { return 2 }
      _ -> { return 0 }
   }
}

fn _material_alpha_word(any rec) int { is_dict(rec) ? int(rec.get("alpha_u32", 0)) : 0 }

fn _material_alpha_mode(any raw, any rec) str {
   if is_dict(raw) { return to_str(raw.get("alphaMode", _alpha_mode_from_code(band(_material_alpha_word(rec), 3)))) }
   _alpha_mode_from_code(band(_material_alpha_word(rec), 3))
}

fn _material_alpha_cutoff(any raw, any rec) f64 {
   if is_dict(raw) { return clamp(float(raw.get("alphaCutoff", 0.5)), 0.0, 1.0) }
   clamp(float(band(bshr(_material_alpha_word(rec), 8), 255)) / 255.0, 0.0, 1.0)
}

fn _material_occlusion_strength(any raw, any rec) f64 {
   def occ = is_dict(raw) ? raw.get("occlusionTexture", 0) : 0
   if is_dict(occ) { return clamp(float(occ.get("strength", 1.0)), 0.0, 1.0) }
   def word = _material_alpha_word(rec)
   def packed = band(bshr(word, 16), 255)
   packed <= 0 ? 1.0 : clamp(float(packed) / 255.0, 0.0, 1.0)
}

fn _normal_scale_from_word(int word) f64 {
   def packed = band(bshr(word, 24), 127)
   packed <= 0 ? 1.0 : clamp(float(packed) * 2.0 / 127.0, 0.0, 2.0)
}

fn _material_normal_scale(any raw, any rec) f64 {
   if is_dict(rec) && rec.contains("normal_scale") { return clamp(float(rec.get("normal_scale", 1.0)), 0.0, 2.0) }
   def normal_tex = is_dict(raw) ? raw.get("normalTexture", 0) : 0
   if is_dict(normal_tex) { return clamp(float(normal_tex.get("scale", 1.0)), 0.0, 2.0) }
   _normal_scale_from_word(is_dict(rec) ? int(rec.get("normal_tex_word", 0x80000000)) : 0x80000000)
}

fn _material_double_sided(any raw, any rec) bool {
   if is_dict(rec) && rec.contains("double_sided") { return bool(rec.get("double_sided", false)) }
   is_dict(raw) && bool(raw.get("doubleSided", false))
}

fn _vec3_rgba(any color) list {
   [
      clamp(float(color.get(0, 0.0)), 0.0, 1.0),
      clamp(float(color.get(1, 0.0)), 0.0, 1.0),
      clamp(float(color.get(2, 0.0)), 0.0, 1.0),
      1.0
   ]
}

fn _rgba_vec3(any color) list {
   [
      clamp(float(color.get(0, 0.0)), 0.0, 1.0),
      clamp(float(color.get(1, 0.0)), 0.0, 1.0),
      clamp(float(color.get(2, 0.0)), 0.0, 1.0)
   ]
}

fn _material_emissive_rgba(any raw, any rec) list {
   if is_dict(rec) && is_list(rec.get("emissive_factor", 0)) { return _vec3_rgba(rec.get("emissive_factor", [0.0, 0.0, 0.0])) }
   is_dict(raw) ? _vec3_rgba(raw.get("emissiveFactor", [0.0, 0.0, 0.0])) : [0.0, 0.0, 0.0, 1.0]
}

fn _material_emissive_strength(any raw, any rec) f64 {
   if is_dict(rec) && rec.contains("emissive_strength") { return max(0.0, float(rec.get("emissive_strength", 1.0))) }
   def ext = is_dict(raw) ? raw.get("extensions", 0) : 0
   def es = is_dict(ext) ? ext.get("KHR_materials_emissive_strength", 0) : 0
   is_dict(es) ? max(0.0, float(es.get("emissiveStrength", 1.0))) : 1.0
}

fn _changed_f64(f64 a, f64 b, f64 eps=0.0005) bool { ui_app.app_absf(a - b) > eps }

fn _material_texture_summary(any raw, any rec) any {
   def pbr = _pbr(raw)
   def rows = [
      ["Base texture", "base", pbr, "baseColorTexture"],
      ["MR texture", "metallic_roughness", pbr, "metallicRoughnessTexture"],
      ["Normal texture", "normal", raw, "normalTexture"],
      ["Occlusion texture", "occlusion", raw, "occlusionTexture"],
      ["Emissive texture", "emissive", raw, "emissiveTexture"]
   ]
   mut i = 0
   while i < rows.len {
      def row = rows.get(i, [])
      _prop(row.get(0, ""), is_dict(rec) ? int(rec.get(to_str(row.get(1, "")), _tex_idx(row.get(2, 0), to_str(row.get(3, ""))))) : _tex_idx(row.get(2, 0), to_str(row.get(3, ""))))
      i += 1
   }
}

fn _material_section(any state) dict {
   mut st = state
   def scene = st.get("scene", dict(0))
   def materials = _scene_materials(scene)
   def recs = _scene_mat_records(scene)
   def mat_count = max(materials.len, recs.len)
   if !gui.collapsing_header("inspect_scene_material", "Material", false) { return st }
   if mat_count <= 0 {
      gui.text_colored("No material records.", [0.68, 0.68, 0.68, 1.0])
      return st
   }
   def selected_part_idx = int(st.get("selected_part", -1))
   mut mat_idx = _material_index(scene, int(st.get("selected_material", -1)))
   def items = _material_items(materials, mat_count)
   if items.len > 0 {
      mat_idx = gui.combo_box("inspect_material_select", "Material", items, mat_idx, 0.0, 8)
      st["selected_material"] = mat_idx
   }
   def raw = _raw_material(scene, mat_idx)
   def rec = _mat_record(scene, mat_idx)
   _prop("Selected part", selected_part_idx >= 0 ? selected_part_idx : 0)
   _prop("Material index", mat_idx)
   if is_dict(raw) { _prop("Name", to_str(raw.get("name", ""))) }
   def base0 = _material_base_rgba(raw, rec)
   def metal0 = _material_metallic(raw, rec)
   def rough0 = _material_roughness(raw, rec)
   def alpha_mode0 = _material_alpha_mode(raw, rec)
   def alpha_cutoff0 = _material_alpha_cutoff(raw, rec)
   def occ0 = _material_occlusion_strength(raw, rec)
   def normal0 = _material_normal_scale(raw, rec)
   def double0 = _material_double_sided(raw, rec)
   def emissive0 = _material_emissive_rgba(raw, rec)
   def emissive_strength0 = _material_emissive_strength(raw, rec)
   def base1 = colorpicker.edit4("inspect_material_base", "Base Color", base0, [1, 1, 1, 1])
   _prop("Base hex", colorpicker.rgba_hex(base1))
   def emissive1 = colorpicker.edit4("inspect_material_emissive", "Emissive Color", emissive0, [0, 0, 0, 1])
   def emissive_strength1 = gui.slider_float("inspect_material_emissive_strength", "Emissive Strength", emissive_strength0, 0.0, 8.0)
   def metal1 = gui.slider_float("inspect_material_metallic", "Metallic", metal0, 0.0, 1.0)
   def rough1 = gui.slider_float("inspect_material_roughness", "Roughness", rough0, 0.0, 1.0)
   def alpha_idx = gui.combo_box("inspect_material_alpha_mode", "Alpha Mode", ALPHA_MODE_ITEMS, _alpha_mode_index(alpha_mode0), 0.0, 3)
   def alpha_mode1 = to_str(ALPHA_MODE_ITEMS.get(alpha_idx, "OPAQUE"))
   def alpha_cutoff1 = gui.slider_float("inspect_material_alpha_cutoff", "Alpha Cutoff", alpha_cutoff0, 0.0, 1.0)
   def occ1 = gui.slider_float("inspect_material_occlusion_strength", "Occlusion Strength", occ0, 0.0, 1.0)
   def normal1 = gui.slider_float("inspect_material_normal_scale", "Normal Scale", normal0, 0.0, 2.0)
   def double1 = gui.checkbox("inspect_material_double_sided", "Double Sided", double0)
   if colorpicker.changed(base0, base1) ||
   colorpicker.changed(emissive0, emissive1) ||
   alpha_mode0 != alpha_mode1 ||
   double0 != double1 ||
   _changed_f64(metal0, metal1) ||
   _changed_f64(rough0, rough1) ||
   _changed_f64(alpha_cutoff0, alpha_cutoff1) ||
   _changed_f64(occ0, occ1) ||
   _changed_f64(normal0, normal1) ||
   _changed_f64(emissive_strength0, emissive_strength1){
      st["material_changed"] = true
      st["material_tweak"] = {
         "mat_idx": mat_idx,
         "base_color": base1,
         "emissive_factor": _rgba_vec3(emissive1),
         "emissive_strength": emissive_strength1,
         "metallic": metal1,
         "roughness": rough1,
         "alpha_mode": alpha_mode1,
         "alpha_cutoff": alpha_cutoff1,
         "occlusion_strength": occ1,
         "normal_scale": normal1,
         "double_sided": double1
      }
   }
   _material_texture_summary(raw, rec)
   st
}

fn _scene_tab(any state) dict {
   mut st = state
   def scene = st.get("scene", dict(0))
   def mat_mask = int(st.get("mat_mask", 0))
   if gui.collapsing_header("inspect_scene_summary", "Scene Summary", true) {
      _prop("Loaded", to_str(st.get("scene_name", "")).len > 0 ? st.get("scene_name", "") : "<none>")
      _prop_bool("Visible", bool(st.get("show_scene", false)) && bool(st.get("has_scene", false)))
      _prop("Resolved", st.get("selected_path", "<no selected model>"))
      _prop("Draw parts", int(st.get("part_count", 0)))
      _prop("Optical start", bool(st.get("has_scene", false)) ? int(scene.get("gpu_optical_start", 0)) : 0)
      _prop("Blend start", bool(st.get("has_scene", false)) ? int(scene.get("gpu_blend_start", 0)) : 0)
      _prop_bool("Has optical pass", bool(st.get("has_scene", false)) && bool(scene.get("has_optical", false)))
      _prop_bool("Has blend pass", !bool(st.get("has_scene", false)) || bool(scene.get("has_blend", true)))
      _prop("Lights", bool(st.get("has_scene", false)) ? int(scene.get("scene_lights_count", 0)) : 0)
      _prop("Features", ui_app.app_scene_feature_names(mat_mask))
   }
   if gui.collapsing_header("inspect_scene_anim", "Animation / Rigging", false) {
      _prop("Animation clips", int(st.get("anim_count", 0)))
      _prop("Animation time", f"{float(st.get('anim_time', 0.0)):.3f}" + " / " + f"{float(st.get('anim_duration', 0.0)):.3f}")
      st["anim_enabled"] = gui.checkbox("inspect_anim_enabled", "Animation Enabled", bool(st.get("anim_enabled", false)))
      st["anim_speed"] = gui.slider_float("inspect_anim_speed", "Animation Speed", float(st.get("anim_speed", 1.0)), 0.0, 4.0)
      _prop("Skins", bool(st.get("has_scene", false)) ? int(scene.get("skin_count", 0)) : 0)
      _prop("Morph targets", bool(st.get("has_scene", false)) ? int(scene.get("morph_target_count", 0)) : 0)
   }
   if gui.collapsing_header("inspect_scene_fit", "Fit / Framing", false) {
      _prop("Fit scale", f"{float(scene.get('fit_scale', 1.0)):.5f}")
      _prop("Fit offset", f"{float(scene.get('fit_tx', 0.0)):.3f}" + ", " + f"{float(scene.get('fit_ty', 0.0)):.3f}" + ", " + f"{float(scene.get('fit_tz', 0.0)):.3f}")
      if gui.button("inspect_autofit", "Autofit", 96.0) { return _action(st, "autofit") }
      gui.same_line()
      if gui.button("inspect_lookat", "Look At", 96.0) { return _action(st, "lookat") }
      gui.same_line()
      if gui.button("inspect_unload", "Unload", 96.0) { return _action(st, "unload") }
   }
   st = _material_section(st)
   st
}

fn _object_transform_tab(any state) dict {
   mut st = state
   if !gui.collapsing_header("inspect_object_transform", "Object Transform", true) { return st }
   if !bool(st.get("has_scene", false)) {
      gui.text_colored("No selected scene object.", [0.68, 0.68, 0.68, 1.0])
      return st
   }
   def tx0, ty0, tz0 = _num(st.get("edit_tx", 0.0)), _num(st.get("edit_ty", 0.0)), _num(st.get("edit_tz", 0.0))
   def rx0, ry0, rz0 = _rad_to_deg(st.get("edit_rx", 0.0)), _rad_to_deg(st.get("edit_ry", 0.0)), _rad_to_deg(st.get("edit_rz", 0.0))
   def sx0, sy0, sz0 = _num(st.get("edit_sx", 1.0), 1.0), _num(st.get("edit_sy", 1.0), 1.0), _num(st.get("edit_sz", 1.0), 1.0)
   def packed = _panel_w(st) >= 370.0
   def cell_w = _packed_cell_w(st)
   def pos1 = _slider3("inspect_obj_t", "Position XYZ", tx0, ty0, tz0, -200.0, 200.0, cell_w, packed)
   st["edit_tx"] = pos1.get(0, tx0)
   st["edit_ty"] = pos1.get(1, ty0)
   st["edit_tz"] = pos1.get(2, tz0)
   def rot1 = _slider3("inspect_obj_r", "Rotation XYZ deg", rx0, ry0, rz0, -360.0, 360.0, cell_w, packed)
   st["edit_rx"] = _deg_to_rad(rot1.get(0, rx0))
   st["edit_ry"] = _deg_to_rad(rot1.get(1, ry0))
   st["edit_rz"] = _deg_to_rad(rot1.get(2, rz0))
   def scl1 = _slider3("inspect_obj_s", "Scale XYZ", sx0, sy0, sz0, 0.02, 10.0, cell_w, packed)
   st["edit_sx"] = scl1.get(0, sx0)
   st["edit_sy"] = scl1.get(1, sy0)
   st["edit_sz"] = scl1.get(2, sz0)
   mut action_idx = int(st.get("transform_action_idx", 0))
   if action_idx < 0 || action_idx >= TRANSFORM_ACTION_ITEMS.len { action_idx = 0 }
   if _wide_panel(st) {
      action_idx = gui.combo_box("inspect_obj_action", "Transform Action", TRANSFORM_ACTION_ITEMS, action_idx, 220.0, 6)
      gui.same_line()
      if gui.button("inspect_obj_action_apply", "Apply", 72.0) {
         case action_idx {
            1 -> { st["action"] = "reset_transform" }
            2 -> { st["action"] = "lookat" }
            3 -> { st["edit_sx"] = 1.0 st["edit_sy"] = 1.0 st["edit_sz"] = 1.0 }
            4 -> { st["edit_rx"] = 0.0 st["edit_ry"] = 0.0 st["edit_rz"] = 0.0 }
            5 -> {
               st["edit_tx"] = _snap_value(st.get("edit_tx", tx0), 0.10)
               st["edit_ty"] = _snap_value(st.get("edit_ty", ty0), 0.10)
               st["edit_tz"] = _snap_value(st.get("edit_tz", tz0), 0.10)
               st["edit_rx"] = _deg_to_rad(_snap_value(_rad_to_deg(st.get("edit_rx", 0.0)), 15.0))
               st["edit_ry"] = _deg_to_rad(_snap_value(_rad_to_deg(st.get("edit_ry", 0.0)), 15.0))
               st["edit_rz"] = _deg_to_rad(_snap_value(_rad_to_deg(st.get("edit_rz", 0.0)), 15.0))
               st["edit_sx"] = _snap_value(st.get("edit_sx", sx0), 0.10)
               st["edit_sy"] = _snap_value(st.get("edit_sy", sy0), 0.10)
               st["edit_sz"] = _snap_value(st.get("edit_sz", sz0), 0.10)
            }
            _ -> {}
         }
         action_idx = 0
      }
      st["transform_action_idx"] = action_idx
   } else {
      if gui.button("inspect_obj_reset", "Reset", 78.0) { st["action"] = "reset_transform" }
      gui.same_line()
      if gui.button("inspect_obj_fit", "Frame", 78.0) { st["action"] = "lookat" }
      gui.same_line()
      if gui.button("inspect_obj_snap", "Snap", 78.0) {
         st["edit_tx"] = _snap_value(st.get("edit_tx", tx0), 0.10)
         st["edit_ty"] = _snap_value(st.get("edit_ty", ty0), 0.10)
         st["edit_tz"] = _snap_value(st.get("edit_tz", tz0), 0.10)
         st["edit_rx"] = _deg_to_rad(_snap_value(_rad_to_deg(st.get("edit_rx", 0.0)), 15.0))
         st["edit_ry"] = _deg_to_rad(_snap_value(_rad_to_deg(st.get("edit_ry", 0.0)), 15.0))
         st["edit_rz"] = _deg_to_rad(_snap_value(_rad_to_deg(st.get("edit_rz", 0.0)), 15.0))
         st["edit_sx"] = _snap_value(st.get("edit_sx", sx0), 0.10)
         st["edit_sy"] = _snap_value(st.get("edit_sy", sy0), 0.10)
         st["edit_sz"] = _snap_value(st.get("edit_sz", sz0), 0.10)
      }
   }
   def changed =
   _changed_f64(tx0, _num(st.get("edit_tx", tx0))) || _changed_f64(ty0, _num(st.get("edit_ty", ty0))) || _changed_f64(tz0, _num(st.get("edit_tz", tz0))) ||
   _changed_f64(_deg_to_rad(rx0), _num(st.get("edit_rx", 0.0))) || _changed_f64(_deg_to_rad(ry0), _num(st.get("edit_ry", 0.0))) || _changed_f64(_deg_to_rad(rz0), _num(st.get("edit_rz", 0.0))) ||
   _changed_f64(sx0, _num(st.get("edit_sx", sx0))) || _changed_f64(sy0, _num(st.get("edit_sy", sy0))) || _changed_f64(sz0, _num(st.get("edit_sz", sz0)))
   if changed { st["transform_changed"] = true }
   st
}

fn _camera_tab(any state) dict {
   mut st = state
   if gui.collapsing_header("inspect_camera_transform", "Camera", true) {
      def px0, py0, pz0 = float(st.get("cam_x", 0.0)), float(st.get("cam_y", 0.0)), float(st.get("cam_z", 0.0))
      def yaw0, pitch0, fov0 = float(st.get("yaw", 0.0)), float(st.get("pitch", 0.0)), float(st.get("fov", 60.0))
      st["cam_x"] = gui.slider_float("inspect_cam_x", "Position X", px0, -1000.0, 1000.0)
      st["cam_y"] = gui.slider_float("inspect_cam_y", "Position Y", py0, -1000.0, 1000.0)
      st["cam_z"] = gui.slider_float("inspect_cam_z", "Position Z", pz0, -1000.0, 1000.0)
      st["yaw"] = gui.slider_float("inspect_cam_yaw", "Yaw", yaw0, -360.0, 360.0)
      st["pitch"] = gui.slider_float("inspect_cam_pitch", "Pitch", pitch0, -89.9, 89.9)
      st["fov"] = gui.slider_float("inspect_cam_fov", "Field of View", fov0, 15.0, 120.0)
      if px0 != st.get("cam_x", px0) || py0 != st.get("cam_y", py0) || pz0 != st.get("cam_z", pz0) || yaw0 != st.get("yaw", yaw0) || pitch0 != st.get("pitch", pitch0) || fov0 != st.get("fov", fov0) { st["camera_changed"] = true }
   }
   if gui.collapsing_header("inspect_camera_motion", "Camera Motion / Projection", false) {
      def speed0, sens0, rmb0 = float(st.get("speed", 0.0)), float(st.get("sens", 0.0)), float(st.get("rmb_sens", 0.0))
      def drag0, damp0 = float(st.get("drag", 0.0)), float(st.get("damp", 0.0))
      st["speed"] = gui.slider_float("inspect_cam_speed", "Move Speed", speed0, 20.0, 5000.0)
      st["sens"] = gui.slider_float("inspect_cam_sens", "Look Sensitivity", sens0, 0.01, 0.60)
      st["rmb_sens"] = gui.slider_float("inspect_cam_rmb_sens", "RMB Look Scale", rmb0, 0.05, 1.00)
      st["drag"] = gui.slider_float("inspect_cam_drag", "Velocity Drag", drag0, 0.0, 40.0)
      st["damp"] = gui.slider_float("inspect_cam_damp", "Input Damping", damp0, 1.0, 40.0)
      def next_ortho = gui.checkbox("inspect_projection_ortho", "Orthographic Projection", bool(st.get("is_ortho", false)))
      if next_ortho != bool(st.get("is_ortho", false)) { st["is_ortho"] = next_ortho st["projection_changed"] = true }
      if next_ortho { st["ortho_zoom"] = gui.slider_float("inspect_ortho_zoom", "Ortho Zoom", float(st.get("ortho_zoom", 40.0)), 5.0, 220.0) }
      if speed0 != st.get("speed", speed0) || sens0 != st.get("sens", sens0) || rmb0 != st.get("rmb_sens", rmb0) || drag0 != st.get("drag", drag0) || damp0 != st.get("damp", damp0) { st["camera_changed"] = true }
      if gui.button("inspect_cam_fit", "Fit Scene", 106.0) { return _action(st, "autofit") }
      gui.same_line()
      if gui.button("inspect_cam_stop", "Stop Motion", 118.0) { st["camera_changed"] = true }
   }
   st
}

fn _playback_tab(any state) dict {
   mut st = state
   def open = int(st.get("anim_count", 0)) > 0
   if gui.collapsing_header("inspect_view_playback", "Playback", open) {
      _prop("Clips", int(st.get("anim_count", 0)))
      _prop("Time", f"{float(st.get('anim_time', 0.0)):.3f}" + " / " + f"{float(st.get('anim_duration', 0.0)):.3f}")
      st["anim_enabled"] = gui.checkbox("inspect_view_anim_enabled", "Animation Enabled", bool(st.get("anim_enabled", false)))
      st["anim_speed"] = gui.slider_float("inspect_view_anim_speed", "Animation Speed", float(st.get("anim_speed", 1.0)), 0.0, 4.0)
   }
   st
}

fn _transform_tab(any state) dict {
   mut st = state
   if gui.collapsing_header("inspect_view_transform", "Transform Gizmo", true) {
      mut mode = int(st.get("gizmo_mode", 0))
      if mode < 0 || mode > 2 { mode = 0 }
      def before = mode
      if _mode_button("inspect_gizmo_move", "toolmove", "Move", mode == 0) { mode = 0 }
      gui.same_line()
      if _mode_button("inspect_gizmo_rotate", "toolrotate", "Rotate", mode == 1) { mode = 1 }
      gui.same_line()
      if _mode_button("inspect_gizmo_scale", "toolscale", "Scale", mode == 2) { mode = 2 }
      st["gizmo_mode"] = mode
      if mode != before { st["gizmo_mode_changed"] = true }
      mut axis = int(st.get("gizmo_axis", 0))
      if axis < 0 || axis > 3 { axis = 0 }
      def axis0 = axis
      if _wide_panel(st) {
         axis = gui.combo_box("inspect_axis_combo", "Axis Constraint", AXIS_ITEMS, axis, 142.0, 4)
         gui.same_line()
         st["gizmo_snap"] = gui.icon_button("inspect_snap", -1, "Snap", 72.0, 30.0, bool(st.get("gizmo_snap", false))) ? !bool(st.get("gizmo_snap", false)) : bool(st.get("gizmo_snap", false))
         gui.same_line()
         st["gizmo_precise"] = gui.icon_button("inspect_precise", -1, "Precise", 108.0, 30.0, bool(st.get("gizmo_precise", false))) ? !bool(st.get("gizmo_precise", false)) : bool(st.get("gizmo_precise", false))
      } else {
         gui.text_colored("Axis constraint", [0.70, 0.70, 0.70, 0.94])
         def free_icon = ui_profile.env_toggle_cached("NY_UI_SVG_INSPECTOR_ICONS", false) ? icons.icon_sprite("tooltransform") : -1
         if gui.icon_button("inspect_axis_free", free_icon, "Free", 70.0, 30.0, axis == 0) { axis = 0 }
         gui.same_line()
         if _axis_button("inspect_axis_x", "X", axis == 1) { axis = (axis == 1) ? 0 : 1 }
         gui.same_line()
         if _axis_button("inspect_axis_y", "Y", axis == 2) { axis = (axis == 2) ? 0 : 2 }
         gui.same_line()
         if _axis_button("inspect_axis_z", "Z", axis == 3) { axis = (axis == 3) ? 0 : 3 }
      }
      def ruler0 = bool(st.get("gizmo_ruler", true))
      st["gizmo_ruler"] = gui.icon_button("inspect_ruler", -1, "Ruler", 76.0, 30.0, ruler0) ? !ruler0 : ruler0
      if !_wide_panel(st) {
         gui.same_line()
         def snap0 = bool(st.get("gizmo_snap", false))
         st["gizmo_snap"] = gui.icon_button("inspect_snap", -1, "Snap", 72.0, 30.0, snap0) ? !snap0 : snap0
         gui.same_line()
         def precise0 = bool(st.get("gizmo_precise", false))
         st["gizmo_precise"] = gui.icon_button("inspect_precise", -1, "Fine", 72.0, 30.0, precise0) ? !precise0 : precise0
      }
      _prop("Current", _gizmo_label(mode) + " / " + _axis_label(axis) +
         " / snap " + _bool_text(st.get("gizmo_snap", false)) +
      " / ruler " + _bool_text(st.get("gizmo_ruler", true)))
      st["gizmo_axis"] = axis
      if axis != axis0 { st["gizmo_axis_changed"] = true }
      def sel0 = bool(st.get("scene_selected", false))
      st["scene_selected"] = gui.checkbox("inspect_scene_selected", "Scene Selected", sel0)
      if sel0 != bool(st.get("scene_selected", sel0)) { st["selection_changed"] = true }
      def rect0 = bool(st.get("selection_rect", true))
      st["selection_rect"] = gui.checkbox("inspect_selection_bounds", "Bounds Box", rect0)
      if rect0 != bool(st.get("selection_rect", rect0)) { st["selection_changed"] = true }
   }
   st
}

fn _viewport_tab(any state) dict {
   mut st = state
   if gui.collapsing_header("inspect_viewport_flags", "Viewport Flags", false) {
      st["stats"] = gui.checkbox("inspect_stats", "Stats Overlay", bool(st.get("stats", true)))
      def wire0 = bool(st.get("wire", false))
      st["wire"] = gui.checkbox("inspect_wire", "Wireframe", wire0)
      if wire0 != bool(st.get("wire", wire0)) { st["wire_changed"] = true }
      st["show_scene"] = gui.checkbox("inspect_show_scene", "Draw Loaded Scene", bool(st.get("show_scene", false)))
      def cursor0 = bool(st.get("cursor_lock", false))
      st["cursor_lock"] = gui.checkbox("inspect_cursor_lock", "Cursor Lock For Free Look", cursor0)
      if cursor0 != bool(st.get("cursor_lock", cursor0)) { st["cursor_changed"] = true }
   }
   if gui.collapsing_header("inspect_viewport_style", "Viewport Style", false) {
      def bg0 = colorpicker.rgba(st.get("bg", [0, 0, 0, 1]), [0, 0, 0, 1])
      st["bg"] = colorpicker.edit4("inspect_clear_color", "Clear Color", bg0, [0, 0, 0, 1])
      if colorpicker.changed(bg0, st.get("bg", bg0)) { st["bg_changed"] = true }
      def scale0 = float(st.get("gui_scale", 1.0))
      st["gui_scale"] = gui.slider_float("inspect_ui_scale", "UI Scale", scale0, 0.80, 1.90)
      if ui_app.app_absf(scale0 - float(st.get("gui_scale", scale0))) > 0.001 { st["scale_changed"] = true }
      gui.set_scale(float(st.get("gui_scale", scale0)))
      st["layout_gap"] = float(gui.slider_int("inspect_layout_gap", "Layout Gap", int(st.get("layout_gap", 0.0)), 0, 32))
      st["workspace_grid"] = gui.slider_float("inspect_grid_cell", "Grid Cell", float(st.get("workspace_grid", 24.0)), 8.0, 96.0)
      st["workspace_major"] = gui.slider_int("inspect_grid_major", "Grid Major Step", int(st.get("workspace_major", 4)), 2, 10)
   }
   st
}

fn _env_controls(any state, str header_id, str combo_id, str sky_id, str load_id, str off_id, str title, bool include_scene, int mat_mask=0) dict {
   mut st = state
   if gui.collapsing_header(header_id, title, false) {
      def env0 = int(st.get("env_mode", 0))
      st["env_mode"] = gui.combo_box(combo_id, "Environment Mode", st.get("env_items", []), env0, 0.0, 5)
      if int(st.get("env_mode", env0)) == 3 && (env0 != 3 || int(st.get("skybox_tex", -1)) < 0) { st["action"] = "ensure_skybox" }
      def sky0 = bool(st.get("skybox_enabled", false))
      st["skybox_enabled"] = gui.checkbox(sky_id, "Draw Skybox Background", sky0)
      if bool(st.get("skybox_enabled", sky0)) && (!sky0 || int(st.get("skybox_tex", -1)) < 0) { st["action"] = "ensure_skybox" }
      if gui.button(load_id, "Load / Build Env", 142.0) { st["action"] = "load_skybox" }
      gui.same_line()
      if gui.button(off_id, "Lighting Off", 112.0) { st["env_mode"] = 4 st["action"] = "lighting_off" }
      _prop("Sky tex", int(st.get("skybox_tex", -1)))
      _prop("Sky spec", int(st.get("skybox_spec_tex", -1)))
      _prop("Studio tex", int(st.get("compare_env_tex", -1)))
      _prop("Neutral tex", int(st.get("neutral_env_tex", -1)))
      if include_scene {
         _prop_bool("Scene env sensitive", bool(st.get("scene_env_sensitive", false)))
         _prop("Optical need", (band(mat_mask, 128 | 256 | 512 | 4096 | 8192 | 32768 | 65536) != 0) ? "yes" : "no")
      }
   }
   st
}

fn _env_tab(any state) dict {
   mut st = state
   st = _env_controls(st, "inspect_env_mode", "inspect_env_mode_combo", "inspect_skybox_draw", "inspect_env_load", "inspect_env_off", "Environment / Cubemap", true, int(st.get("mat_mask", 0)))
   if gui.collapsing_header("inspect_env_policy", "Scene Policy", false) {
      _prop_bool("Prefers studio", bool(st.get("pref_studio", false)))
      _prop_bool("Prefers neutral", bool(st.get("pref_neutral", false)))
      _prop_bool("Reflection compare", bool(st.get("pref_reflect", false)))
      _prop_bool("Visible compare", bool(st.get("pref_visible", false)))
      _prop_bool("Optical spec", bool(st.get("pref_optical", false)))
   }
   st
}

fn _view_env_tab(any state) dict {
   mut st = _object_transform_tab(state)
   st = _playback_tab(st)
   st = _transform_tab(st)
   st = _camera_tab(st)
   st = _viewport_tab(st)
   st = _env_tab(st)
   _settings_tab(st)
}

fn _settings_tab(any state) dict {
   mut st = state
   if gui.collapsing_header("inspect_settings_renderer", "Renderer Startup", false) {
      def msaa_idx0 = ui_app.app_msaa_index(int(st.get("msaa", 1)))
      def msaa_idx1 = gui.combo_box("inspect_settings_msaa", "MSAA", st.get("msaa_items", []), msaa_idx0, 0.0, 4)
      st["msaa"] = ui_app.app_msaa_samples(msaa_idx1)
      st["vsync"] = gui.checkbox("inspect_settings_vsync", "VSync on next window init", bool(st.get("vsync", false)))
      st["filter_linear"] = gui.checkbox("inspect_settings_filter", "Default linear texture filter", bool(st.get("filter_linear", false)))
      _prop("Backend", st.get("backend", ""))
      _prop("NYTRIX_FAST", ui_profile.env_trim_cached("NYTRIX_FAST"))
   }
   st = _env_controls(st, "inspect_settings_ibl", "inspect_settings_env_mode", "inspect_settings_skybox", "inspect_settings_env_load", "inspect_settings_lighting_off", "IBL / Cubemap", false, 0)
   if gui.collapsing_header("inspect_settings_ui", "UI / Fonts / DPI", false) {
      def scale0 = float(st.get("gui_scale", 1.0))
      def gap0 = float(st.get("layout_gap", 0.0))
      st["gui_scale"] = gui.slider_float("inspect_settings_ui_scale", "UI Scale", scale0, 0.70, 1.60)
      gui.set_scale(float(st.get("gui_scale", scale0)))
      st["layout_gap"] = float(gui.slider_int("inspect_settings_layout_gap", "Layout Gap", int(gap0), 0, 22))
      if ui_app.app_absf(gap0 - float(st.get("layout_gap", gap0))) > 0.001 || ui_app.app_absf(scale0 - float(st.get("gui_scale", scale0))) > 0.001 { st["layout_changed"] = true }
      _prop("DPI scale", f"{float(st.get('dpi_scale', 1.0)):.2f}")
      _prop("Body font", "maplemono.ttf")
      _prop("Title font", "maplemono.ttf")
      _prop("Font filtering", "linear / smooth")
   }
   if gui.collapsing_header("inspect_settings_texture", "Texture / Cache", false) {
      _prop("NY_TEX_MIPS", ui_profile.env_trim_cached("NY_TEX_MIPS"))
      _prop("NY_TEX_CACHE", ui_profile.env_trim_cached("NY_TEX_CACHE"))
   }
   st
}

fn _renderer_tab(any state) dict {
   mut st = state
   def rs = st.get("renderer", dict(0))
   if gui.collapsing_header("inspect_renderer_stats", "Renderer Counters", true) {
      _prop("Backend", rs.get("backend", st.get("backend", "")))
      _props_int_dict(rs, COUNTER_PROPS)
      _prop("Hotspot", _txt(st.get("renderer_hotspot", ""), "steady"), [0.78, 0.78, 0.78, 1.0])
   }
   if gui.collapsing_header("inspect_renderer_timing", "Timing", true) {
      _prop("Frame", f"{_num(st.get('last_frame_ms', 0.0)):.3f} ms")
      _prop("Update", f"{_num(st.get('last_update_ms', 0.0)):.3f} ms")
      _prop("World", f"{_num(st.get('last_world_ms', 0.0)):.3f} ms")
      _prop("Draw", f"{_num(st.get('last_draw_ms', 0.0)):.3f} ms")
      _prop("UI", f"{_num(st.get('last_ui_ms', 0.0)):.3f} ms")
      gui.progress_bar("inspect_frame_budget", clamp(_num(st.get("last_frame_ms", 0.0)) / 16.667, 0.0, 1.0), "Frame budget")
      def frame_stats = ui_app.app_hist_mean_max(st.get("frame_ms_samples", []))
      def avg = float(frame_stats.get(0, 0.0))
      def peak = float(frame_stats.get(1, 0.0))
      gui.progress_bar("inspect_frame_average", clamp(avg / 16.667, 0.0, 1.0), "Avg " + f"{avg:.2f} ms")
      gui.text_colored("Peak " + f"{peak:.2f} ms", [0.86, 0.78, 0.58, 1.0])
   }
   st
}

fn _diag_tab(any state) dict {
   mut st = state
   if gui.collapsing_header("inspect_diag_runtime", "Runtime Diagnostics", true) {
      _prop("Hovered", _txt(gui.hovered_id(), "idle"))
      _prop("Active", _txt(gui.active_id(), "idle"))
      _prop("Focused", _txt(gui.focused_id(), "idle"))
      _prop("Window", to_str(_intv(st.get("win_w", 0.0))) + "x" + to_str(_intv(st.get("win_h", 0.0))))
      _prop("Terminal", bool(st.get("term_open", false)) ? "open" : "closed")
      _prop("Last dump", _txt(st.get("last_dump", ""), ""))
      if gui.button("inspect_diag_probe", "Probe", 94.0) { return _action(st, "probe") }
      gui.same_line()
      if gui.button("inspect_diag_retile", "Retile", 94.0) { return _action(st, "retile") }
   }
   if gui.collapsing_header("inspect_diag_backends", "Backends", false) {
      _prop("Window backend", st.get("window_backend", ""))
      _prop("Render backend", st.get("backend", ""))
   }
   st
}

fn _render_diag_tab(any state) dict {
   mut st = _renderer_tab(state)
   _diag_tab(st)
}

fn draw_body(any state) dict {
   "Draws the inspector body and returns updated state plus an optional action."
   mut st = is_dict(state) ? state : dict(0)
   st["action"] = ""
   def scene_name = to_str(st.get("scene_name", ""))
   def rs = st.get("renderer", dict(0))
   gui.text_colored((scene_name.len > 0 ? scene_name : "No scene") +
      "  Parts " + to_str(_intv(st.get("part_count", 0))) +
   "  Draws " + to_str(_intv(rs.get("draws", 0))), [0.64, 0.69, 0.72, 1.0])
   st["tab"] = gui.tab_strip("inspector_tabs", st.get("tab_items", TAB_ITEMS), int(st.get("tab", 0)))
   gui.separator()
   case int(st.get("tab", 0)){
      0 -> { return _scene_tab(st) }
      1 -> { return _view_env_tab(st) }
      _ -> { return _render_diag_tab(st) }
   }
}

#main {
   def out = draw_body({"tab": 0, "tab_items": TAB_ITEMS, "renderer": {"draws": 2}})
   assert(is_dict(out) && int(out.get("tab", -1)) >= 0, "viewer inspector body")
   print("✓ viewer inspector self-test passed")
}
