;; Keywords: assets asset-browser catalog os ui
;; Asset catalog indexing, filtering, and grid data for UI asset browsers.
;; References:
;; - std.os.ui.assets
module std.os.ui.assets.catalog(catalog_filter_key, catalog_pick_cache, catalog_row_id, catalog_filter, scene_part_count, hierarchy_node_label, hierarchy_node_detail, indent_prefix, virtual_row_range, asset_grid_cols, asset_grid_usable_w, asset_tile_h, asset_grid_content_h, asset_grid_fit_h, asset_grid_view_h, format_name_list, asset_icon_name, asset_detail, asset_dirs_from_env, first_gltf_in_dir, gltf_catalog_make, gltf_catalog_roots, gltf_catalog_scan, gltf_catalog_ensure, gltf_catalog_resolve, gltf_catalog_names)
use std.core
use std.core.str as str
use std.math (clamp)
use std.os (env)
use std.os.fs as osfs
use std.os.path as ospath
use std.os.thread

def _ASSET_ICON_RULES = [
   ["asset_camera", ["camera"]],
   ["asset_light", ["light", "lamp", "lantern"]],
   ["asset_texture", ["texture", "uv", "normal"]],
   ["asset_material", ["material", "metal", "rough", "clearcoat", "specular", "iridescence", "transmission", "alpha", "sheen", "anisotropy", "dispersion"]],
   ["asset_animation", ["anim", "morph", "skin", "rigged", "interpolation"]]
]

fn catalog_filter_key(any filter) str { str.lower(str.strip(to_str(filter))) }

fn catalog_pick_cache(any cache, any items, any value, any filter) dict {
   "Runs the pick cache operation."
   mut out = is_dict(cache) ? cache : dict(8)
   def name = catalog_filter_key(value)
   def filter_key = catalog_filter_key(filter)
   def source_len = is_list(items) ? items.len : 0
   if to_str(out.get("name", "\x00")) == name &&
   to_str(out.get("filter", "\x00")) == filter_key &&
   int(out.get("len", -1)) == source_len{
      return out
   }
   mut idx = -1
   mut i = 0
   while is_list(items) && i < items.len {
      if catalog_filter_key(items.get(i, "")) == name {
         idx = i
         break
      }
      i += 1
   }
   out["name"] = name
   out["filter"] = filter_key
   out["len"] = source_len
   out["idx"] = idx
   out
}

fn catalog_row_id(any name) str {
   "Runs the row id operation."
   mut s = str.strip(to_str(name))
   if s.len == 0 { return "item" }
   s = str.str_replace(s, " ", "_")
   s = str.str_replace(s, "/", "_")
   s = str.str_replace(s, "\\", "_")
   s = str.str_replace(s, ":", "_")
   s = str.str_replace(s, ".", "_")
   s
}

fn catalog_filter(any names, any filter) list {
   "Runs the filter operation."
   if !is_list(names) { return [] }
   def want = catalog_filter_key(filter)
   if want.len == 0 { return names }
   mut out = []
   mut i = 0
   while i < names.len {
      def name = to_str(names.get(i, ""))
      if str.find(str.lower(name), want) >= 0 { out = out.append(name) }
      i += 1
   }
   out
}

fn scene_part_count(any scene_obj) int {
   "Runs the part count operation."
   if scene_obj == 0 || !is_dict(scene_obj) { return 0 }
   def gpu_n = int(scene_obj.get("gpu_parts_count", 0))
   if gpu_n > 0 { return gpu_n }
   def parts = scene_obj.get("parts", [])
   is_list(parts) ? parts.len : 0
}

fn indent_prefix(any depth) str {
   "Runs the indent prefix operation."
   mut s, i = "", 0
   while i < int(depth) {
      s = s + "  "
      i += 1
   }
   s
}

fn hierarchy_node_label(any node, int idx) str {
   "Runs the hierarchy node label operation."
   if !is_dict(node) { return "Node " + to_str(idx) }
   def name = str.strip(to_str(node.get("name", "")))
   (name.len > 0) ? name : ("Node " + to_str(idx))
}

fn hierarchy_node_detail(any node) str {
   "Runs the hierarchy node detail operation."
   if !is_dict(node) { return "" }
   mut parts = []
   if node.contains("mesh") { parts = parts.append("mesh " + to_str(int(node.get("mesh", -1)))) }
   if node.contains("camera") { parts = parts.append("camera " + to_str(int(node.get("camera", -1)))) }
   if node.contains("skin") { parts = parts.append("skin " + to_str(int(node.get("skin", -1)))) }
   def children = node.get("children", [])
   if is_list(children) && children.len > 0 { parts = parts.append(to_str(children.len) + " children") }
   if parts.len == 0 { return "transform node" }
   str.join(parts, "  ")
}

fn virtual_row_range(any total_rows, any row_step, any visible_h, any scroll_y, any overscan=3) list {
   "Runs the virtual row range operation."
   def total = max(0, int(total_rows))
   def step = max(1.0, float(row_step))
   mut first_row = int(float(scroll_y) / step)
   if first_row < 0 { first_row = 0 }
   if first_row > total { first_row = total }
   mut visible_rows = int(float(visible_h) / step) + int(overscan)
   if visible_rows < 1 { visible_rows = 1 }
   mut last_row = first_row + visible_rows
   if last_row > total { last_row = total }
   [first_row, last_row]
}

fn asset_grid_cols(any win_w, any compact=false) int {
   "Runs the grid cols operation."
   if bool(compact) { return 1 }
   def ww = int(float(win_w))
   case ww {
      1420..1000000 -> 6
      1120..1419 -> 5
      840..1119 -> 4
      560..839 -> 3
      420..559 -> 2
      _ -> 1
   }
}

fn asset_grid_usable_w(any win_w, any compact=false) f64 {
   "Runs the grid usable w operation."
   bool(compact) ? max(120.0, float(win_w) - 44.0) : max(1.0, float(win_w) - 64.0)
}

fn asset_tile_h(any show_paths=false) f64 {
   "Runs the tile h operation."
   bool(show_paths) ? 66.0 : 50.0
}

fn asset_grid_content_h(any model_count, any win_w, any compact=false, any show_paths=false) f64 {
   "Runs the grid content h operation."
   def total_items = int(model_count)
   if total_items <= 0 { return 58.0 }
   def cols = asset_grid_cols(asset_grid_usable_w(win_w, compact), compact)
   def rows = (total_items + cols - 1) / cols
   max(58.0, float(rows) * asset_tile_h(show_paths) + float(max(0, rows - 1)) * 8.0 + 4.0)
}

fn asset_grid_fit_h(any model_count, any win_w, any requested_h, any compact=false, any show_paths=false) f64 { clamp(asset_grid_content_h(model_count, win_w, compact, show_paths), 90.0, float(requested_h)) }

fn asset_grid_view_h(any requested_h, any compact=false, any standalone=false) f64 {
   "Runs the grid view h operation."
   def max_h = bool(standalone) ? 2400.0 : (bool(compact) ? 520.0 : 320.0)
   clamp(float(requested_h), bool(compact) ? 160.0 : 220.0, max_h)
}

fn format_name_list(any items) str {
   "Runs the format name list operation."
   if !is_list(items) { return "" }
   mut out = ""
   mut i = 0
   while i < items.len {
      def item = to_str(items.get(i, ""))
      if item.len > 0 { out = (out.len > 0) ? (out + ", " + item) : item }
      i += 1
   }
   out
}

fn _asset_dir_add(list dirs, any raw) list {
   def s = str.strip(to_str(raw))
   if s.len == 0 { return dirs }
   mut path = s
   if !osfs.is_dir(path) {
      def resolved = ospath.resolve_repo_asset(s)
      if osfs.is_dir(resolved) { path = resolved }
   }
   if !osfs.is_dir(path) { return dirs }
   mut i = 0
   while i < dirs.len {
      if to_str(dirs[i]) == path { return dirs }
      i += 1
   }
   dirs.append(path)
}

fn _asset_dir_tokens(any raw) list {
   def s0 = str.str_replace(str.strip(to_str(raw)), ";", "|")
   def s1 = str.str_replace(s0, ",", "|")
   str.split(s1, "|")
}

fn asset_dirs_from_env(any defaults=[], any env_names=[]) list {
   "Returns existing asset directories from env values plus defaults. Env lists use `|`, `;`, or comma separators."
   mut out = []
   mut i = 0
   def envs = is_list(env_names) ? env_names : [env_names]
   while i < envs.len {
      def raw = str.strip(to_str(env(to_str(envs[i]))))
      def toks = _asset_dir_tokens(raw)
      mut j = 0
      while j < toks.len {
         out = _asset_dir_add(out, toks[j])
         j += 1
      }
      i += 1
   }
   def defs = is_list(defaults) ? defaults : [defaults]
   i = 0
   while i < defs.len {
      out = _asset_dir_add(out, defs[i])
      i += 1
   }
   out
}

fn _first_existing_file(any paths) str {
   if !is_list(paths) { return "" }
   mut i = 0
   while i < paths.len {
      def cand = to_str(paths[i])
      if osfs.is_file(cand) { return cand }
      i += 1
   }
   ""
}

fn _first_gltf_in_dir(str dir, str prefer="") str {
   if !osfs.is_dir(dir) { return "" }
   def want = str.lower(str.strip(prefer))
   def dir_base = ospath.basename(dir)
   def exact_name = want.len > 0 ? prefer : dir_base
   if exact_name.len > 0 {
      def direct = _first_existing_file([ospath.join(dir, exact_name + ".glb"), ospath.join(dir, exact_name + ".gltf")])
      if direct.len > 0 { return direct }
   }
   if want.len > 0 {
      def named = _first_existing_file([
            ospath.join(ospath.join(dir, "glTF-Binary"), prefer + ".glb"),
            ospath.join(ospath.join(dir, "glTF"), prefer + ".gltf"),
            ospath.join(ospath.join(dir, "glTF"), prefer + ".glb")
      ])
      if named.len > 0 { return named }
   }
   mut fb = ""
   mut exact_gltf = ""
   mut exact_glb = ""
   def names = osfs.list_dir(dir)
   mut i = 0
   while i < names.len {
      def name = to_str(names[i])
      def full = ospath.join(dir, name)
      if osfs.is_file(full) {
         def ext = str.lower(ospath.extname(name))
         if ext == ".gltf" || ext == ".glb" {
            if want.len > 0 && str.lower(to_str(ospath.splitext(name)[0])) == want {
               if ext == ".glb" { exact_glb = full }
               else { exact_gltf = full }
            }
            if fb.len == 0 { fb = full }
         }
      } elif osfs.is_dir(full) {
         def nested = _first_gltf_in_dir(full, prefer)
         if nested.len > 0 {
            def ext = str.lower(ospath.extname(nested))
            def stem = str.lower(to_str(ospath.splitext(ospath.basename(nested))[0]))
            if want.len > 0 && stem == want {
               if ext == ".glb" { exact_glb = nested }
               else { exact_gltf = nested }
            }
            if fb.len == 0 { fb = nested }
         }
      }
      i += 1
   }
   if exact_glb.len > 0 { return exact_glb }
   if exact_gltf.len > 0 { return exact_gltf }
   fb
}

fn first_gltf_in_dir(str dir, str prefer="") str {
   "Returns the preferred .glb/.gltf file inside `dir`, searching common glTF sample layouts."
   _first_gltf_in_dir(dir, prefer)
}

fn _gltf_layout_dir_name(str name) bool {
   def n = str.lower(name)
   n == "gltf" || n == "gltf-binary" || n == "glb" || n == "source"
}

fn _gltf_catalog_name_for_file(str path) str {
   def stem = to_str(ospath.splitext(ospath.basename(path))[0])
   def parent_dir = ospath.dirname(path)
   def parent = ospath.basename(parent_dir)
   if _gltf_layout_dir_name(parent) {
      def grand = ospath.basename(ospath.dirname(parent_dir))
      if grand.len > 0 { return grand }
   }
   if stem.len > 0 { return stem }
   parent
}

fn _gltf_catalog_add(list names, dict paths, str name, str path) list {
   if name.len == 0 || path.len == 0 || paths.contains(name) { return [names, paths] }
   paths[name] = path
   [names.append(name), paths]
}

fn _gltf_catalog_scan_dir(str dir, list names, dict paths, int depth) list {
   if depth > 16 || !osfs.is_dir(dir) { return [names, paths] }
   def entries = osfs.list_dir(dir)
   mut i = 0
   while i < entries.len {
      def entry = to_str(entries[i])
      def full = ospath.join(dir, entry)
      if osfs.is_file(full) {
         def ext = str.lower(ospath.extname(entry))
         if ext == ".gltf" || ext == ".glb" {
            def added = _gltf_catalog_add(names, paths, _gltf_catalog_name_for_file(full), full)
            names = added[0]
            paths = added[1]
         }
      } elif osfs.is_dir(full) {
         def scanned = _gltf_catalog_scan_dir(full, names, paths, depth + 1)
         names = scanned[0]
         paths = scanned[1]
      }
      i += 1
   }
   [names, paths]
}

fn _root_list(any roots) list {
   if is_dict(roots) { return _root_list(roots.get("roots", [])) }
   if is_list(roots) { return roots }
   def s = str.strip(to_str(roots))
   s.len > 0 ? [s] : []
}

fn gltf_catalog_make(any roots=[]) dict {
   "Creates a reusable glTF asset catalog for one or more root directories."
   {
      "roots": _root_list(roots),
      "names": [],
      "paths": dict(512),
      "cache": dict(512),
      "mu": 0
   }
}

fn gltf_catalog_roots(any catalog_or_roots) list {
   "Runs the catalog roots operation."
   def roots = _root_list(catalog_or_roots)
   mut out = []
   mut i = 0
   while i < roots.len {
      def root = to_str(roots[i])
      if root.len > 0 && osfs.is_dir(root) { out = out.append(root) }
      i += 1
   }
   out
}

fn _catalog_mutex(dict catalog) any {
   def mu = catalog.get("mu", 0)
   if mu { return mu }
   def next = mutex_new()
   catalog["mu"] = next
   next
}

fn gltf_catalog_scan(any catalog_or_roots) list {
   "Scans catalog roots and returns [names, name_to_path]."
   mut names = []
   mut paths = dict(512)
   def roots = gltf_catalog_roots(catalog_or_roots)
   mut ri = 0
   while ri < roots.len {
      def root = to_str(roots[ri])
      def scanned = _gltf_catalog_scan_dir(root, names, paths, 0)
      names = scanned[0]
      paths = scanned[1]
      ri += 1
   }
   sort(names)
   [names, paths]
}

fn gltf_catalog_ensure(dict catalog) bool {
   "Ensures the catalog has been scanned."
   def mu = _catalog_mutex(catalog)
   mutex_lock(mu)
   if is_list(catalog.get("names", [])) && catalog.get("names", []).len > 0 {
      mutex_unlock(mu)
      return true
   }
   mutex_unlock(mu)
   def scanned = gltf_catalog_scan(catalog)
   mutex_lock(mu)
   catalog["names"] = scanned[0]
   catalog["paths"] = scanned[1]
   def ok = catalog.get("names", []).len > 0
   mutex_unlock(mu)
   ok
}

fn _catalog_store(dict catalog, str raw, str path, any mu=0) str {
   if mu { mutex_lock(mu) }
   def cache = catalog.get("cache", dict(512))
   cache[raw] = path
   catalog["cache"] = cache
   if mu { mutex_unlock(mu) }
   path
}

fn gltf_catalog_resolve(dict catalog, any spec) str {
   "Resolves a glTF asset spec: absolute file, relative file, directory, or catalog folder name."
   def raw = str.strip(to_str(spec))
   if raw.len == 0 { return "" }
   def mu = _catalog_mutex(catalog)
   mutex_lock(mu)
   def cache = catalog.get("cache", dict(512))
   if cache.contains(raw) {
      def cached = to_str(cache.get(raw, ""))
      mutex_unlock(mu)
      return cached
   }
   mutex_unlock(mu)
   if osfs.is_file(raw) { return _catalog_store(catalog, raw, raw, mu) }
   def rel = str.str_replace(raw, "\\", "/")
   def simple_name = !ospath.has_sep(rel) && len(ospath.extname(rel)) == 0
   def roots = gltf_catalog_roots(catalog)
   mut ri = 0
   while ri < roots.len {
      def root = to_str(roots[ri])
      def direct = ospath.join(root, rel)
      if osfs.is_file(direct) { return _catalog_store(catalog, raw, direct, mu) }
      if osfs.is_dir(direct) {
         def found = first_gltf_in_dir(direct, ospath.basename(direct))
         if found.len > 0 { return _catalog_store(catalog, raw, found, mu) }
      }
      ri += 1
   }
   if simple_name && gltf_catalog_ensure(catalog) {
      def paths = catalog.get("paths", dict(512))
      if paths.contains(raw) {
         def named = to_str(paths.get(raw, ""))
         if named.len > 0 { return _catalog_store(catalog, raw, named, mu) }
      }
   }
   ""
}

fn gltf_catalog_names(dict catalog, int limit=24) list {
   "Returns catalog folder names, optionally limited."
   if !gltf_catalog_ensure(catalog) { return [] }
   def names = catalog.get("names", [])
   if limit > 0 && names.len > limit { return slice(names, 0, limit, 1) }
   names
}

fn _text_has_any(str haystack, any needles) bool {
   mut i = 0
   def n = is_list(needles) ? needles.len : 0
   while i < n {
      if str.find(haystack, to_str(needles.get(i, ""))) >= 0 { return true }
      i += 1
   }
   false
}

fn asset_icon_name(any name, any rules=0) str {
   "Returns a stable UI icon name for a human-readable asset/model label."
   def s = str.lower(to_str(name))
   def rows = is_list(rules) ? rules : _ASSET_ICON_RULES
   mut i = 0
   while i < rows.len {
      def row = rows[i]
      if _text_has_any(s, row.get(1, [])) { return to_str(row.get(0, "asset_model")) }
      i += 1
   }
   "asset_model"
}

fn asset_detail(any name, any loaded=false, any rules=0) str {
   "Returns a compact asset detail label for browsers and inspectors."
   if bool(loaded) { return "Loaded scene" }
   def icon = asset_icon_name(name, rules)
   if icon == "asset_camera" { return "Camera" }
   if icon == "asset_light" { return "Light rig" }
   if icon == "asset_texture" { return "Texture set" }
   if icon == "asset_material" { return "Material test" }
   if icon == "asset_animation" { return "Animation rig" }
   "Model"
}
