;; Keywords: app
;; Application-level UI setup for layout, assets, renderer state, and run-loop integration.
module std.os.ui.app(
   app_absf, app_resolve_repo_asset, app_key_is_shift, app_key_is_ctrl, app_effective_mods,
   app_pick_font, app_shader_sources, app_list_find_text, app_push_hist_sample, app_hist_mean,
   app_hist_max, app_flag_names, app_scene_feature_names, app_msaa_index, app_msaa_samples,
   app_model_icon_name,
   app_window_extent_from_env, app_startup_render_config, app_dpi_scale_from_env_or_metrics,
   app_gui_scale_from_env, app_renderer_hotspot_label, app_renderer_stats_line, app_window_w,
   app_window_h, app_window_body_h, app_window_compact, app_rect_text, app_card_w, app_graph_node
)

use std.core
use std.core.str as str
use std.os (file_read)
use std.os.path as ospath
use std.os.ui.consts
use std.os.ui.gui as gui
use std.os.ui.window.input as uin
use std.os.ui.assets.catalog as asset_catalog
use std.core.common as common

def _APP_SCENE_FEATURE_FLAGS = [
   [16, "specular"], [32, "sheen"], [64, "clearcoat"], [128, "transmission"],
   [256, "volume"], [512, "ior"], [1024, "iridescence"], [2048, "anisotropy"],
   [4096, "dispersion"], [8192, "diffuse-transmission"], [16384, "alpha-coverage"],
   [32768, "refraction"], [65536, "subsurface"], [131072, "unlit"]
]

fn app_absf(any: x): f64 {
   def v = float(x)
   v < 0.0 ? (0.0 - v) : v
}

fn app_resolve_repo_asset(any: rel): str {
   def raw = str.strip(to_str(rel))
   if(raw.len == 0){ return "" }
   ospath.resolve_repo_asset(raw)
}

fn app_key_is_shift(any: k, any: sc=0): bool { k == uin.KEY_LEFT_SHIFT || k == uin.KEY_RIGHT_SHIFT || k == uin.KEY_SHIFT || sc == 50 || sc == 62 }

fn app_key_is_ctrl(any: k, any: sc=0): bool { k == uin.KEY_LEFT_CONTROL || k == uin.KEY_RIGHT_CONTROL || k == uin.KEY_CTRL || sc == 37 || sc == 105 }

fn app_effective_mods(any: mods, any: k=0, any: sc=0, any: shift_down=false, any: ctrl_down=false): int {
   mut out = int(mods)
   if(shift_down || app_key_is_shift(k, sc)){ out = out | MOD_SHIFT }
   if(ctrl_down || app_key_is_ctrl(k, sc)){ out = out | MOD_CONTROL }
   out
}

fn app_pick_font(any: primary, any: fallback): any { common.value_or(primary, fallback) }

fn _app_read_text(any: path): str {
   match file_read(path){
      ok(s) -> { return s }
      err(_) -> { return "" }
   }
}

fn app_shader_sources(any: vert_rel, any: frag_rel): list {
   [
      _app_read_text(app_resolve_repo_asset(vert_rel)),
      _app_read_text(app_resolve_repo_asset(frag_rel)),
   ]
}

fn app_list_find_text(any: items, any: value): int {
   if(!is_list(items)){ return -1 }
   def want = str.lower(str.strip(to_str(value)))
   mut i = 0
   def n = items.len
   while(i < n){
      if(str.lower(str.strip(to_str(items.get(i, "")))) == want){ return i }
      i += 1
   }
   -1
}

fn app_push_hist_sample(any: hist, any: value, any: limit=256): list {
   mut out = is_list(hist) ? hist : []
   out = out.append(float(value))
   if(out.len > int(limit)){ out = slice(out, out.len - int(limit), out.len) }
   out
}

fn app_hist_mean(any: hist): f64 {
   if(!is_list(hist) || hist.len <= 0){ return 0.0 }
   mut total = 0.0
   mut i = 0
   def n = hist.len
   while(i < n){
      total += float(hist[i])
      i += 1
   }
   total / float(n)
}

fn app_hist_max(any: hist): f64 {
   if(!is_list(hist) || hist.len <= 0){ return 0.0 }
   mut out = float(hist.get(0, 0.0))
   mut i = 1
   def n = hist.len
   while(i < n){
      def v = float(hist[i])
      if(v > out){ out = v }
      i += 1
   }
   out
}

fn app_flag_names(any: mask, any: flags, any: fallback=""): str {
   mut out = []
   def m = int(mask)
   mut i = 0
   def n = is_list(flags) ? flags.len : 0
   while(i < n){
      def row = flags.get(i, [])
      if(is_list(row) && band(m, int(row.get(0, 0))) != 0){ out = out.append(to_str(row.get(1, ""))) }
      i += 1
   }
   if(out.len == 0){ return to_str(fallback) }
   str.join(out, ", ")
}

fn app_scene_feature_names(any: mask): str { app_flag_names(mask, _APP_SCENE_FEATURE_FLAGS, "base PBR") }

fn app_model_icon_name(any: name): str { asset_catalog.asset_icon_name(name) }

fn app_msaa_index(any: samples): int {
   def s = int(samples)
   if(s >= 8){ return 3 }
   if(s >= 4){ return 2 }
   if(s >= 2){ return 1 }
   0
}

fn app_msaa_samples(any: idx): int {
   def i = int(idx)
   if(i >= 3){ return 8 }
   if(i == 2){ return 4 }
   if(i == 1){ return 2 }
   1
}

fn _app_clamp_f64(any: value, any: lo, any: hi): f64 {
   mut out = float(value)
   if(out < float(lo)){ out = float(lo) }
   if(out > float(hi)){ out = float(hi) }
   out
}

fn app_window_extent_from_env(any: headless=false): list {
   def fallback_w, fallback_h = bool(headless) ? 1920 : 1280, bool(headless) ? 1080 : 720
   def env_w = common.env_int_clamped("NY_UI_WIDTH", fallback_w, 320, 16384)
   def env_h = common.env_int_clamped("NY_UI_HEIGHT", fallback_h, 240, 16384)
   [
      common.env_int_clamped("NY_UI_WINDOW_W", env_w, 320, 16384),
      common.env_int_clamped("NY_UI_WINDOW_H", env_h, 240, 16384),
   ]
}

fn app_startup_render_config(any: msaa, any: vsync, any: filter_linear): dict {
   mut out = dict(3)
   mut samples = int(msaa)
   if(common.env_present("NY_UI_MSAA")){
      def v = common.env_int_clamped("NY_UI_MSAA", samples, 1, 8)
      samples = v <= 1 ? 1 : (v <= 2 ? 2 : (v <= 4 ? 4 : 8))
   }
   out["msaa"] = samples
   out["vsync"] = common.env_toggle("NY_UI_VSYNC", bool(vsync))
   out["filter_linear"] = common.env_toggle("NY_UI_FILTER_LINEAR", bool(filter_linear))
   out
}

fn app_dpi_scale_from_env_or_metrics(any: monitor_scale=1.0, any: window_h=720.0): f64 {
   def raw = common.env_trim("NY_UI_DPI_SCALE")
   if(raw.len > 0){ return _app_clamp_f64(str.atof(raw), 0.70, 2.25) }
   mut scale = float(monitor_scale)
   if(scale > 1.05){
      if(scale > 1.60){ scale = 1.60 }
      return scale
   }
   (max(1.0, float(window_h)) >= 2000.0) ? 1.25 : 1.0
}

fn app_gui_scale_from_env(): f64 {
   def raw = common.env_trim("NY_UI_SCALE")
   (raw.len > 0) ? _app_clamp_f64(str.atof(raw), 0.70, 1.60) : 1.0
}

fn app_renderer_hotspot_label(any: rs): str {
   def draws = int(rs.get("draws", 0))
   def dynamic_draws = int(rs.get("dynamic_draws", 0))
   def flushes = int(rs.get("flushes", 0))
   def pipeline_binds = int(rs.get("pipeline_binds", 0))
   def descriptor_binds = int(rs.get("descriptor_binds", 0))
   def submitted_vertices = int(rs.get("submitted_vertices", 0))
   if(dynamic_draws > max(36, int(float(draws) * 0.60))){ return "dynamic draw pressure" }
   if(pipeline_binds > max(28, int(float(draws) * 0.75)) || descriptor_binds > max(28, int(float(draws) * 0.80))){ return "state churn" }
   if(flushes > max(18, int(float(draws) * 0.33))){ return "flush pressure" }
   if(submitted_vertices > 1500000){ return "geometry throughput" }
   if(draws > 420){ return "draw call pressure" }
   "steady"
}

fn _app_renderer_stat_i(dict: rs, str: key, int: fallback=0): int { int(rs.get(key, fallback)) }

fn app_renderer_stats_line(dict: rs, bool: with_verts=true, bool: with_desc=true): str {
   mut out = "draws=" + to_str(_app_renderer_stat_i(rs, "draws", 0)) +
   " dyn=" + to_str(_app_renderer_stat_i(rs, "dynamic_draws", 0)) +
   " static=" + to_str(_app_renderer_stat_i(rs, "static_draws", 0)) +
   " indexed=" + to_str(_app_renderer_stat_i(rs, "indexed_draws", 0))
   if(with_verts){ out += " verts=" + to_str(_app_renderer_stat_i(rs, "submitted_vertices", 0)) }
   out += " flushes=" + to_str(_app_renderer_stat_i(rs, "flushes", 0)) +
   " pipes=" + to_str(_app_renderer_stat_i(rs, "pipeline_binds", 0))
   if(with_desc){ out += " desc=" + to_str(_app_renderer_stat_i(rs, "descriptor_binds", 0)) }
   out
}

fn app_window_w(any: window_id, any: fallback=0.0): f64 {
   def r, w = gui.window_rect(window_id), float(r.get(2, 0.0))
   w > 1.0 ? w : float(fallback)
}

fn app_window_h(any: window_id, any: fallback=0.0): f64 {
   def r, h = gui.window_rect(window_id), float(r.get(3, 0.0))
   h > 1.0 ? h : float(fallback)
}

fn app_window_body_h(any: window_id, any: fallback=260.0, any: overhead=126.0): f64 {
   def h = app_window_h(window_id, 0.0)
   if(h <= 1.0){ return float(fallback) }
   max(120.0, h - float(overhead))
}

fn app_window_compact(any: window_id, any: threshold=460.0): bool {
   def w = app_window_w(window_id, 0.0)
   w > 1.0 && w < float(threshold)
}

fn app_rect_text(any: window_id): str {
   def r = gui.window_rect(window_id)
   "[" + to_str(float(r.get(0, 0.0))) + "," + to_str(float(r.get(1, 0.0))) + "," +
   to_str(float(r.get(2, 0.0))) + "," + to_str(float(r.get(3, 0.0))) + "]"
}

fn app_card_w(any: window_id, any: cols=2, any: gap=12.0, any: min_w=140.0): f64 {
   def w = app_window_w(window_id, 0.0)
   if(w <= 1.0){ return max(float(min_w), 180.0) }
   max(float(min_w), (max(0.0, w - 24.0) - max(0.0, float(cols - 1)) * float(gap)) / float(max(cols, 1)))
}

fn app_graph_node(any: title, any: x, any: y, any: inputs, any: outputs, any: selected=false, any: w=180.0): dict {
   mut out = dict(8)
   out["title"] = to_str(title)
   out["x"] = float(x)
   out["y"] = float(y)
   out["w"] = float(w)
   out["inputs"] = is_list(inputs) ? inputs : []
   out["outputs"] = is_list(outputs) ? outputs : []
   out["selected"] = !!selected
   out
}
