;; Keywords: assets batch prefetch gltf load os ui
;; Batch asset prefetch and GLTF loading helpers for viewer and editor workflows.
;; References:
;; - std.os.ui.assets.catalog
;; - std.os.ui.assets.viewer
module std.os.ui.assets.batch(parse_env, format_model, skip_enabled, elapsed_s, eta_s, fast_static_settle, fast_static_settle_frames, terminal_log_enabled, dump_anim_pose_fraction)
use std.core
use std.core.str as str
use std.math (clamp)
use std.math.float (is_nan, is_inf)
use std.os (file_read)
use std.os.fs as osfs
use std.os.path as ospath
use std.os.ui.assets.catalog as asset_catalog
use std.os.ui.render.dump as ui_dump
use std.os.ui.render.dump as ui_profile

fn skip_enabled(txt) bool {
   "Returns whether skip enabled."
   def raw = str.strip(to_str(txt))
   def low = str.lower(raw)
   raw.len > 0 && low != "0" && low != "false" && low != "off" && low != "no"
}

fn elapsed_s(int run_started_ns) f64 {
   "Runs the elapsed s operation."
   run_started_ns > 0 ? ui_profile.elapsed_s(run_started_ns) : 0.0
}

fn eta_s(int run_started_ns, int completed_count, int total_count) f64 {
   "Runs the eta s operation."
   if completed_count <= 0 || run_started_ns <= 0 { return 0.0 }
   def remaining = max(0, total_count - completed_count)
   def avg_s = elapsed_s(run_started_ns) / max(1.0, float(completed_count))
   avg_s * float(remaining)
}

fn fast_static_settle() bool {
   "Runs the fast static settle operation."
   !ui_profile.env_truthy_cached("NY_UI_BATCH_FULL_SETTLE") &&
   ui_profile.env_present_cached("NY_UI_BATCH_FAST_SETTLE_FRAMES")
}

fn fast_static_settle_frames(int settle_frames) int {
   "Runs the fast static settle frames operation."
   fast_static_settle() ? ui_profile.env_int_cached("NY_UI_BATCH_FAST_SETTLE_FRAMES", settle_frames, 0, settle_frames) : settle_frames
}

fn terminal_log_enabled(bool active, bool fast_env) bool {
   "Returns whether terminal log enabled."
   !active || !fast_env || ui_profile.env_truthy_cached("NY_UI_BATCH_TERMINAL_LOG")
}

fn dump_anim_pose_fraction(f64 fallback=0.5) f64 {
   "Runs the dump anim pose fraction operation."
   def raw = ui_profile.env_trim_cached("NY_UI_DUMP_ANIM_POSE_FRACTION")
   if raw.len <= 0 { return fallback }
   mut v = float(str.atof(raw))
   if is_nan(v) || is_inf(v) { v = fallback }
   clamp(v, 0.0, 1.0)
}

fn _read_file_parts(path) list {
   if path.len <= 0 { return [] }
   match file_read(path) {
      ok(file_txt) -> {
         def txt = str.strip(to_str(file_txt))
         return txt.len > 0 ? str.split(txt, "\n") : []
      }
      err(_) -> { return [] }
   }
}

fn _input_parts(cli_raw, cli_all, all_names) list {
   def batch_path = ui_profile.env_trim_cached("NY_UI_BATCH_DUMP_FILE")
   mut parts = _read_file_parts(batch_path)
   if is_list(parts) && parts.len > 0 { return parts }
   def txt = ui_profile.env_trim_cached("NY_UI_BATCH_DUMP_LIST")
   if txt.len > 0 { return ui_dump.split_model_list(txt) }
   if to_str(cli_raw).len > 0 { return ui_dump.split_model_list(cli_raw) }
   if bool(cli_all) || ui_profile.env_truthy_cached("NY_UI_BATCH_DUMP_ALL") {
      return is_list(all_names) ? all_names : []
   }
   []
}

fn _resolved_path(catalog, item) str {
   is_dict(catalog) ? asset_catalog.gltf_catalog_resolve(catalog, item) : ""
}

fn format_model(item, catalog=0) dict {
   "Runs the format model operation."
   def raw = str.strip(to_str(item))
   mut display = raw
   mut spec_path = raw
   def resolved = _resolved_path(catalog, raw)
   if resolved.len > 0 {
      spec_path = resolved
      if ospath.has_sep(raw) { display = ospath.basename(raw) }
   } elif ospath.has_sep(raw) {
      display = ospath.basename(raw)
   }
   def low = str.lower(display)
   if str.endswith(low, ".gltf") { display = str.str_slice(display, 0, display.len - 5) }
   elif str.endswith(low, ".glb") { display = str.str_slice(display, 0, display.len - 4) }
   if str.strip(to_str(display)).len == 0 { display = raw }
   {"display": display, "spec": spec_path}
}

fn _apply_missing_filter(models, specs, dump_dir, cli_dump_dir) dict {
   if dump_dir.len <= 0 || !is_list(models) || models.len == 0 {
      return {"models": models, "specs": specs, "no_missing": false, "before": is_list(models) ? models.len : 0}
   }
   def before = models.len
   mut keep_names = []
   mut keep_specs = []
   mut mi = 0
   while mi < models.len {
      def model_name = to_str(models.get(mi, ""))
      if !osfs.is_file(ui_dump.snapshot_path(model_name, dump_dir, cli_dump_dir)) {
         keep_names = keep_names.append(model_name)
         keep_specs = keep_specs.append(specs.get(mi, model_name))
      }
      mi += 1
   }
   {"models": keep_names, "specs": keep_specs, "no_missing": before > 0 && keep_names.len == 0, "before": before}
}

fn parse_env(cli_raw="", cli_all=false, all_names=[], cli_dump_dir="", cli_delay_frames=-1, cli_missing=false, catalog=0) dict {
   "Parses parse env."
   mut models = []
   mut specs = []
   def parts = _input_parts(cli_raw, cli_all, all_names)
   mut i = 0
   while is_list(parts) && i < parts.len {
      def item = str.strip(to_str(parts.get(i, "")))
      if item.len > 0 {
         def row = format_model(item, catalog)
         models = models.append(row.get("display", item))
         specs = specs.append(row.get("spec", item))
      }
      i += 1
   }
   mut dump_dir = ui_profile.env_trim_cached("NY_UI_BATCH_DUMP_DIR")
   if dump_dir.len == 0 && to_str(cli_dump_dir).len > 0 { dump_dir = to_str(cli_dump_dir) }
   mut settle_frames = 4
   if ui_profile.env_present_cached("NY_UI_BATCH_DUMP_SETTLE_FRAMES") {
      settle_frames = ui_profile.env_int_cached("NY_UI_BATCH_DUMP_SETTLE_FRAMES", settle_frames, 0, 1000000)
   }
   if int(cli_delay_frames) >= 0 { settle_frames = int(cli_delay_frames) }
   mut timeout_sec = 45.0
   def timeout_s = ui_profile.env_trim_cached("NY_UI_BATCH_MODEL_TIMEOUT_SEC")
   if timeout_s.len > 0 {
      def parsed_timeout = float(str.atof(timeout_s))
      if parsed_timeout > 0.0 { timeout_sec = parsed_timeout }
   }
   mut skip_models = []
   def skip_txt = ui_profile.env_trim_cached("NY_UI_BATCH_DUMP_SKIP_LIST")
   if skip_enabled(skip_txt) { skip_models = ui_dump.parse_skip_list(skip_txt) }
   def missing_only = bool(cli_missing) || ui_profile.env_truthy_cached("NY_UI_BATCH_DUMP_MISSING")
   mut no_missing = false
   if missing_only {
      def filtered = _apply_missing_filter(models, specs, dump_dir, to_str(cli_dump_dir))
      models = filtered.get("models", models)
      specs = filtered.get("specs", specs)
      no_missing = bool(filtered.get("no_missing", false))
   }
   {
      "models": models,
      "specs": specs,
      "dir": dump_dir,
      "settle_frames": settle_frames,
      "timeout_sec": timeout_sec,
      "skip_models": skip_models,
      "active": models.len > 0,
      "no_missing": no_missing
   }
}

#main {
   def row = format_model("foo/bar/Scene.glb", 0)
   assert(row.get("display", "") == "Scene" && row.get("spec", "") == "foo/bar/Scene.glb", "batch format path")
   assert(skip_enabled("a,b") && !skip_enabled("off"), "batch skip enabled")
   assert(elapsed_s(0) == 0.0 && eta_s(0, 0, 10) == 0.0, "batch timing empty")
   assert(terminal_log_enabled(false, true), "batch terminal log default")
   assert(dump_anim_pose_fraction() >= 0.0 && dump_anim_pose_fraction() <= 1.0, "batch pose fraction")
   print("✓ std.os.ui.assets.batch self-test passed")
}
