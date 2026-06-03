;; Keywords: dump snapshot artifacts
;; UI snapshot, dump, and artifact naming operations for visual diagnostics.
module std.os.ui.dump(safe_name, root_dir, path_named, snapshot_path, parse_skip_list, split_model_list, model_skipped, suite_field, suite_parse_line, suite_snapshot_path, gizmo_mode)
use std.core
use std.core.str as str
use std.core.common as common
use std.os.path as path

fn safe_name(any: name): str {
   mut out = str.str_replace(to_str(name), "/", "_")
   out = str.str_replace(out, "\\", "_")
   out = str.str_replace(out, ":", "_")
   out
}

fn root_dir(any: cli_dump_dir=""): str {
   def cli = to_str(cli_dump_dir)
   if(cli.len > 0){ return cli }
   def env_dir = common.env_trim("NY_UI_DUMP_DIR")
   env_dir.len > 0 ? env_dir : path.join(path.cache_dir(), "probe/fb")
}

fn path_named(any: name, any: cli_dump_dir=""): str { root_dir(cli_dump_dir) + "/" + safe_name(name) }

fn _snapshot_ext(): str {
   def raw = str.lower(str.strip(common.env_trim("NY_UI_BATCH_DUMP_EXT")))
   case raw {
      "tga", ".tga" -> ".tga"
      "png", ".png" -> ".png"
      _ -> ".tga"
   }
}

fn snapshot_path(any: model_name, any: batch_dir="", any: cli_dump_dir=""): str {
   def base_dir = to_str(batch_dir).len > 0 ? to_str(batch_dir) : root_dir(cli_dump_dir)
   base_dir + "/" + safe_name(model_name) + _snapshot_ext()
}

fn parse_skip_list(any: raw): list {
   mut out = []
   def normalized = str.str_replace(to_str(raw), "|", ",")
   def parts = str.split(normalized, ",")
   mut i = 0
   while(i < parts.len){
      def item = str.strip(to_str(parts.get(i, "")))
      if(item.len > 0){ out = out.append(item) }
      i += 1
   }
   out
}

fn split_model_list(any: raw): list { str.split(str.str_replace(to_str(raw), ",", "|"), "|") }

fn model_skipped(any: name, any: skip_models): bool {
   def target = str.lower(str.strip(to_str(name)))
   if(target.len == 0){ return false }
   mut i = 0
   def n = is_list(skip_models) ? skip_models.len : 0
   while(i < n){
      if(str.lower(str.strip(to_str(skip_models.get(i, "")))) == target){ return true }
      i += 1
   }
   false
}

fn suite_field(any: spec, any: idx, any: fallback=""): str {
   if(!is_list(spec) || int(idx) < 0 || int(idx) >= spec.len){ return to_str(fallback) }
   to_str(spec.get(int(idx), fallback))
}

fn suite_parse_line(any: line): list {
   def item = str.strip(to_str(line))
   if(item.len == 0 || str.startswith(item, "#")){ return [] }
   def cols = str.split(item, "\t")
   if(cols.len <= 0){ return [] }
   def filename = safe_name(str.strip(to_str(cols.get(0, ""))))
   if(filename.len == 0){ return [] }
   [
      filename,
      safe_name(str.lower(str.strip(to_str(cols.get(1, ""))))),
      str.lower(str.strip(to_str(cols.get(2, "")))),
      str.strip(to_str(cols.get(3, ""))),
      str.lower(str.strip(to_str(cols.get(4, "gui")))),
      str.lower(str.strip(to_str(cols.get(5, "")))),
      str.strip(to_str(cols.get(6, "")))
   ]
}

fn suite_snapshot_path(any: specs, any: index, any: suite_dir="", any: cli_dump_dir=""): str {
   if(!is_list(specs) || int(index) < 0 || int(index) >= specs.len){ return "" }
   def spec = specs.get(int(index), [])
   def base_dir = to_str(suite_dir).len > 0 ? to_str(suite_dir) : root_dir(cli_dump_dir)
   base_dir + "/" + safe_name(suite_field(spec, 0, "gui_dump.png"))
}

fn gizmo_mode(any: name, any: fallback): int {
   def s = str.lower(str.strip(to_str(name)))
   case s {
      "1", "rotate", "rot", "r" -> 1
      "2", "scale", "s" -> 2
      "0", "move", "translate", "g", "w" -> 0
      _ -> int(fallback)
   }
}
