;; Keywords: editor project files tree workspace os ui render viewer text
;; Project file tree and workspace helpers for the editor sidebar.
;; References:
;; - std.os.fs
;; - std.os.path
module std.os.ui.render.viewer.editor.project(
   TREE_ROW_H, TREE_HEADER_H, MAX_TREE_ROWS,
   new, refresh, refresh_status, refresh_git, toggle, create_file, rename_entry, move_entry,
   trash_entry, undo_file_op, redo_file_op, can_undo_file_op, can_redo_file_op,
   set_file_history, file_undo_stack, file_redo_stack, tree, root, branch, counts,
   changes, diff_text, tree_height, visible_count, clamp_scroll, hit_entry,
   row_y, row_at, file_kind, file_icon, git_code, status_label
)

use std.core
use std.core.str as str
use std.math (max, min)
use std.os (file_exists, file_read, file_write, getcwd, ticks)
use std.os.fs as osfs
use std.os.path as ospath
use std.os.subprocess (run_capture)

def TREE_ROW_H = 22.0
def TREE_HEADER_H = 28.0
def MAX_TREE_ROWS = 5000
def FILE_OP_HISTORY_LIMIT = 128
def TRASH_DIR_NAME = ".nytrix-trash"

fn _norm(str path) str {
   str.str_replace(ospath.normalize(path), "\\", "/")
}

fn _rel(dict model, str path) str {
   def p = ospath.normalize(path)
   def r = ospath.normalize(to_str(model.get("root", ".")))
   if(p == r){ return "." }
   def prefix = r + ospath.sep()
   if(str.startswith(p, prefix)){ return _norm(str.str_slice(p, prefix.len, p.len)) }
   _norm(p)
}

fn _heavy_dir(str name) bool {
   name == ".git" || name == "build" || name == "node_modules" || name == "target" ||
   name == ".cache" || name == TRASH_DIR_NAME || name == "__pycache__" || name == ".pytest_cache" || name == ".mypy_cache"
}

fn _git_path(str line) str {
   if(line.len < 4){ return "" }
   mut path = str.strip(str.str_slice(line, 3, line.len))
   def arrow = str.find(path, " -> ")
   if(arrow >= 0){ path = str.str_slice(path, arrow + 4, path.len) }
   if(path.len >= 2 && load8(path, 0) == 34 && load8(path, path.len - 1) == 34){
      path = str.str_slice(path, 1, path.len - 1)
   }
   _norm(path)
}

fn _git_status(str line) str {
   line.len >= 2 ? str.strip(str.str_slice(line, 0, 2)) : ""
}

fn _git_branch_from_status(str line) str {
   if(!str.startswith(line, "## ")){ return "" }
   mut body = str.strip(str.str_slice(line, 3, line.len))
   def dots = str.find(body, "...")
   if(dots >= 0){ body = str.str_slice(body, 0, dots) }
   def sp = str.find(body, " ")
   if(sp >= 0){ body = str.str_slice(body, 0, sp) }
   body
}

fn _inc(dict counts, str key) dict {
   counts[key] = int(counts.get(key, 0)) + 1
   counts
}

fn _mark_git_dirs(dict dirs, str rel) dict {
   def parts = str.split(_norm(rel), "/")
   mut cur = ""
   mut i = 0
   while(i + 1 < parts.len){
      def part = to_str(parts.get(i, ""))
      if(part.len > 0){
         cur = cur.len > 0 ? (cur + "/" + part) : part
         dirs[cur] = true
      }
      i += 1
   }
   dirs
}

fn _git_change_rows(dict model, dict git) list {
   def keys = sort(dict_keys(git))
   mut out = []
   mut i = 0
   while(i < keys.len){
      def rel = to_str(keys.get(i, ""))
      def path = ospath.join(root(model), rel)
      out = out.append({
            "name": ospath.basename(rel), "path": path, "rel": rel, "dir": false,
            "depth": 0, "git": to_str(git.get(rel, "")), "open": false, "kind": file_kind(rel)
      })
      i += 1
   }
   out
}

fn _git_refresh(dict model) dict {
   mut status = dict(64)
   mut dirs = dict(64)
   mut tally = {"modified": 0, "added": 0, "deleted": 0, "untracked": 0, "renamed": 0}
   mut br = ""
   def res = run_capture(["git", "-C", to_str(model.get("root", ".")), "status", "--branch", "--porcelain=v1", "--untracked-files=all"], [], nil, false)
   if(res.get("ok", false)){
      def lines = str.split(to_str(res.get("stdout", "")), "\n")
      mut i = 0
      while(i < lines.len){
         def line = to_str(lines.get(i, ""))
         if(str.startswith(line, "## ")){
            br = _git_branch_from_status(line)
         } else {
            def path = _git_path(line)
            def code = _git_status(line)
            if(path.len > 0 && code.len > 0){
               status[path] = code
               dirs = _mark_git_dirs(dirs, path)
               if(code == "??"){ tally = _inc(tally, "untracked") }
               elif(str.str_contains(code, "A")){ tally = _inc(tally, "added") }
               elif(str.str_contains(code, "D")){ tally = _inc(tally, "deleted") }
               elif(str.str_contains(code, "R")){ tally = _inc(tally, "renamed") }
               elif(str.str_contains(code, "M")){ tally = _inc(tally, "modified") }
            }
         }
         i += 1
      }
   }
   model["git"] = status
   model["git_dirs"] = dirs
   model["branch"] = br
   model["counts"] = tally
   model["changes"] = _git_change_rows(model, status)
   model
}

fn _dir_status(dict model, str rel) str {
   if(rel == "."){ return "" }
   def dirs = model.get("git_dirs", nil)
   if(is_dict(dirs)){ return dirs.contains(rel) ? "..." : "" }
   def prefix = rel + "/"
   def keys = dict_keys(model.get("git", dict(8)))
   mut i = 0
   while(i < keys.len){
      if(str.startswith(to_str(keys.get(i, "")), prefix)){ return "..." }
      i += 1
   }
   ""
}

fn git_code(dict model, str rel, bool dir=false) str {
   def key = _norm(rel)
   def status = model.get("git", dict(8))
   if(status.contains(key)){ return to_str(status.get(key, "")) }
   dir ? _dir_status(model, key) : ""
}

fn _open(dict model, str rel, int depth) bool {
   if(rel == "."){ return true }
   def open = model.get("open", dict(8))
   is_dict(open) ? bool(open.get(rel, depth < 2)) : depth < 2
}

fn _open_dir(dict model, str rel, int depth, str name) bool {
   if(_heavy_dir(name) && !model.get("open", dict(8)).contains(rel)){ return false }
   _open(model, rel, depth)
}

fn _project_scan_tree(dict model, str dir, int depth, int max_depth, int limit) list {
   mut out = []
   if(depth > max_depth || limit <= 0){ return out }
   def names = sort(osfs.list_dir(dir))
   mut pass = 0
   while(pass < 2){
      mut i = 0
      while(i < names.len && out.len < limit){
         def name = to_str(names.get(i, ""))
         def full = ospath.join(dir, name)
         def dirp = osfs.is_dir(full)
         if((pass == 0 && dirp) || (pass == 1 && !dirp)){
            def rel = _rel(model, full)
            def opened = dirp && _open_dir(model, rel, depth, name)
            out = out.append({
                  "name": name, "path": full, "rel": rel, "dir": dirp,
                  "depth": depth, "git": git_code(model, rel, dirp),
                  "open": opened, "kind": dirp ? "dir" : file_kind(name)
            })
            if(opened && depth < max_depth){
               def sub = _project_scan_tree(model, full, depth + 1, max_depth, limit - out.len)
               mut j = 0
               while(j < sub.len && out.len < limit){
                  out = out.append(sub.get(j))
                  j += 1
               }
            }
         }
         i += 1
      }
      pass += 1
   }
   out
}

fn new(str project_root="") dict {
   def r = project_root.len > 0 ? ospath.normalize(project_root) : getcwd()
   {"root": r, "open": dict(32), "tree": [], "git": dict(64), "git_dirs": dict(64), "changes": [], "branch": "", "counts": dict(8), "ops_undo": [], "ops_redo": []}
}

fn refresh(dict model, int max_depth=4, int limit=0) dict {
   def r = to_str(model.get("root", "."))
   mut rows = [{
         "name": ospath.basename(r), "path": r, "rel": ".",
         "dir": true, "depth": 0, "git": "", "open": true, "kind": "dir"
   }]
   mut scan_limit = limit > 0 ? limit : MAX_TREE_ROWS
   mut sub_limit = scan_limit - 1
   if(sub_limit < 0){ sub_limit = 0 }
   def sub = _project_scan_tree(model, r, 1, max_depth, sub_limit)
   mut i = 0
   while(i < sub.len){
      rows = rows.append(sub.get(i))
      i += 1
   }
   model["tree"] = rows
   model
}

fn refresh_status(dict model) dict {
   _git_refresh(model)
}

fn refresh_git(dict model, int max_depth=4, int limit=0) dict {
   refresh(refresh_status(model), max_depth, limit)
}

fn toggle(dict model, str rel) dict {
   if(rel == "."){ return model }
   mut open = model.get("open", dict(32))
   open[rel] = !bool(open.get(rel, false))
   model["open"] = open
   refresh(model)
}

fn _entry_path(dict model, str rel) str {
   rel == "." || rel.len <= 0 ? root(model) : ospath.join(root(model), rel)
}

fn _parent_for_new(dict model, str rel) str {
   def base = _entry_path(model, rel)
   osfs.is_dir(base) ? base : ospath.dirname(base)
}

fn create_file(dict model, str base_rel, str name) dict {
   def clean = str.strip(name)
   if(clean.len <= 0 || ospath.has_sep(clean)){
      model["last_error"] = "bad file name"
      return model
   }
   def dst = ospath.join(_parent_for_new(model, base_rel), clean)
   if(file_exists(dst)){
      model["last_error"] = "file exists"
      return model
   }
   match file_write(dst, ""){
      ok(_) -> {
         model["last_error"] = ""
         model = refresh_git(model)
      }
      err(e) -> { model["last_error"] = "create failed " + to_str(e) }
   }
   model
}

fn rename_entry(dict model, str rel, str new_name) dict {
   def clean = str.strip(new_name)
   if(rel == "." || rel.len <= 0 || clean.len <= 0 || ospath.has_sep(clean)){
      model["last_error"] = "bad rename"
      return model
   }
   def src = ospath.join(to_str(model.get("root", ".")), rel)
   def dst = ospath.join(ospath.dirname(src), clean)
   match osfs.rename(src, dst){
      ok(_) -> {
         model["last_error"] = ""
         model = refresh(model)
      }
      err(e) -> { model["last_error"] = "rename failed " + to_str(e) }
   }
   model
}

fn move_entry(dict model, str src_rel, str dst_rel) dict {
   def src_clean = _norm(src_rel)
   def dst_clean = _norm(dst_rel)
   if(src_clean == "." || src_clean.len <= 0 || dst_clean.len <= 0 || src_clean == dst_clean){
      model["last_error"] = "bad move"
      return model
   }
   if(str.startswith(dst_clean + "/", src_clean + "/")){
      model["last_error"] = "cannot move into itself"
      return model
   }
   def src = _entry_path(model, src_clean)
   def dst_base = _entry_path(model, dst_clean)
   def dst_dir = osfs.is_dir(dst_base) ? dst_base : ospath.dirname(dst_base)
   def dst = ospath.join(dst_dir, ospath.basename(src))
   if(ospath.normalize(src) == ospath.normalize(dst)){
      model["last_error"] = "same location"
      return model
   }
   if(file_exists(dst)){
      model["last_error"] = "target exists"
      return model
   }
   match osfs.rename(src, dst){
      ok(_) -> {
         model["last_error"] = ""
         model = refresh_git(model)
      }
      err(e) -> { model["last_error"] = "move failed " + to_str(e) }
   }
   model
}

fn _mkdir_p(str path) bool {
   if(path.len <= 0){ return false }
   if(file_exists(path) && osfs.is_dir(path)){ return true }
   def res = run_capture(["mkdir", "-p", path], [], nil, false)
   bool(res.get("ok", false)) || (file_exists(path) && osfs.is_dir(path))
}

fn _stack_limit(list xs) list {
   if(xs.len <= FILE_OP_HISTORY_LIMIT){ return xs }
   mut out = []
   mut i = xs.len - FILE_OP_HISTORY_LIMIT
   while(i < xs.len){
      out = out.append(xs.get(i))
      i += 1
   }
   out
}

fn _drop_last(list xs) list {
   mut out = []
   mut i = 0
   while(i + 1 < xs.len){
      out = out.append(xs.get(i))
      i += 1
   }
   out
}

fn _push_undo(dict model, dict op) dict {
   model["ops_undo"] = _stack_limit(model.get("ops_undo", []).append(op))
   model["ops_redo"] = []
   model
}

fn _push_redo(dict model, dict op) dict {
   model["ops_redo"] = _stack_limit(model.get("ops_redo", []).append(op))
   model
}

fn _push_undo_keep_redo(dict model, dict op) dict {
   model["ops_undo"] = _stack_limit(model.get("ops_undo", []).append(op))
   model
}

fn _trash_root(dict model) str { ospath.join(root(model), TRASH_DIR_NAME) }

fn _trash_name(str rel, bool dirp) str {
   def base = ospath.basename(rel)
   def clean = base.len > 0 ? base : "entry"
   to_str(ticks()) + "_" + (dirp ? "dir_" : "file_") + clean
}

fn _trash_path(dict model, str rel, bool dirp) str {
   def dir = _trash_root(model)
   mut p = ospath.join(dir, _trash_name(rel, dirp))
   mut guard = 0
   while(file_exists(p) && guard < 32){
      p = ospath.join(dir, to_str(ticks()) + "_" + to_str(guard) + "_" + ospath.basename(rel))
      guard += 1
   }
   p
}

fn _entry_op(dict model, str rel, str trash_path, bool dirp) dict {
   def src = _entry_path(model, rel)
   {
      "action": "trash", "src_rel": _norm(rel), "src_path": src,
      "trash_path": trash_path, "name": ospath.basename(src),
      "dir": dirp, "time": ticks()
   }
}

fn trash_entry(dict model, str rel) dict {
   def clean = _norm(rel)
   if(clean == "." || clean.len <= 0){
      model["last_error"] = "cannot delete project root"
      return model
   }
   def src = _entry_path(model, clean)
   if(!file_exists(src)){
      model["last_error"] = "missing file"
      return model
   }
   def trash_dir = _trash_root(model)
   if(!_mkdir_p(trash_dir)){
      model["last_error"] = "trash unavailable"
      return model
   }
   def dirp = osfs.is_dir(src)
   def dst = _trash_path(model, clean, dirp)
   match osfs.rename(src, dst){
      ok(_) -> {
         model["last_error"] = ""
         model = _push_undo(model, _entry_op(model, clean, dst, dirp))
         model = refresh_git(model)
      }
      err(e) -> { model["last_error"] = "delete failed " + to_str(e) }
   }
   model
}

fn can_undo_file_op(dict model) bool { model.get("ops_undo", []).len > 0 }

fn can_redo_file_op(dict model) bool { model.get("ops_redo", []).len > 0 }

fn _move_back(dict model, dict op) dict {
   def src = to_str(op.get("src_path", ""))
   def trash = to_str(op.get("trash_path", ""))
   if(src.len <= 0 || trash.len <= 0 || !file_exists(trash)){
      model["last_error"] = "nothing to restore"
      return model
   }
   if(file_exists(src)){
      model["last_error"] = "restore target exists"
      return model
   }
   if(!_mkdir_p(ospath.dirname(src))){
      model["last_error"] = "restore parent unavailable"
      return model
   }
   match osfs.rename(trash, src){
      ok(_) -> {
         model["last_error"] = ""
         model = refresh_git(model)
      }
      err(e) -> { model["last_error"] = "restore failed " + to_str(e) }
   }
   model
}

fn _move_to_trash_again(dict model, dict op) dict {
   def src = to_str(op.get("src_path", ""))
   def trash = to_str(op.get("trash_path", ""))
   if(src.len <= 0 || trash.len <= 0 || !file_exists(src)){
      model["last_error"] = "nothing to redo"
      return model
   }
   if(file_exists(trash)){
      model["last_error"] = "trash target exists"
      return model
   }
   if(!_mkdir_p(ospath.dirname(trash))){
      model["last_error"] = "trash unavailable"
      return model
   }
   match osfs.rename(src, trash){
      ok(_) -> {
         model["last_error"] = ""
         model = refresh_git(model)
      }
      err(e) -> { model["last_error"] = "redo delete failed " + to_str(e) }
   }
   model
}

fn undo_file_op(dict model) dict {
   def undo = model.get("ops_undo", [])
   if(undo.len <= 0){
      model["last_error"] = "nothing to undo"
      return model
   }
   def op = undo.get(undo.len - 1, dict(8))
   model = _move_back(model, op)
   if(to_str(model.get("last_error", "")).len <= 0){
      model["ops_undo"] = _drop_last(undo)
      model = _push_redo(model, op)
   }
   model
}

fn redo_file_op(dict model) dict {
   def redo = model.get("ops_redo", [])
   if(redo.len <= 0){
      model["last_error"] = "nothing to redo"
      return model
   }
   def op = redo.get(redo.len - 1, dict(8))
   model = _move_to_trash_again(model, op)
   if(to_str(model.get("last_error", "")).len <= 0){
      model["ops_redo"] = _drop_last(redo)
      model = _push_undo_keep_redo(model, op)
   }
   model
}

fn set_file_history(dict model, list undo, list redo) dict {
   model["ops_undo"] = _stack_limit(undo)
   model["ops_redo"] = _stack_limit(redo)
   model
}

fn file_undo_stack(dict model) list { model.get("ops_undo", []) }

fn file_redo_stack(dict model) list { model.get("ops_redo", []) }

fn tree(dict model) list { model.get("tree", []) }

fn root(dict model) str { to_str(model.get("root", ".")) }

fn branch(dict model) str { to_str(model.get("branch", "")) }

fn counts(dict model) dict { model.get("counts", dict(8)) }

fn changes(dict model) list {
   "Returns changed files as sidebar rows."
   def cached = model.get("changes", nil)
   if(is_list(cached)){ return cached }
   def git = model.get("git", dict(8))
   _git_change_rows(model, git)
}

fn _capture_stdout(list argv) str {
   def res = run_capture(argv, [], nil, false)
   def out = to_str(res.get("stdout", ""))
   out.len > 0 ? out : to_str(res.get("stderr", ""))
}

fn _untracked_diff(str path, str rel) str {
   mut out = "diff --git a/" + rel + " b/" + rel + "\n"
   out += "new file mode 100644\n"
   out += "--- /dev/null\n"
   out += "+++ b/" + rel + "\n"
   if(!file_exists(path)){ return out + "@@ -0,0 +1 @@\n+missing file\n" }
   match file_read(path){
      ok(raw) -> {
         def lines = str.split(to_str(raw), "\n")
         out += "@@ -0,0 +" + to_str(lines.len) + " @@\n"
         mut i = 0
         while(i < lines.len){
            out += "+" + to_str(lines.get(i, "")) + "\n"
            i += 1
         }
      }
      err(e) -> { out += "@@ -0,0 +1 @@\n+read failed: " + to_str(e) + "\n" }
   }
   out
}

fn diff_text(dict model, dict entry) str {
   "Returns a unified Git diff for a changed sidebar entry."
   def rel = to_str(entry.get("rel", ""))
   if(rel.len <= 0){ return "no file selected\n" }
   def r = root(model)
   def code = to_str(entry.get("git", git_code(model, rel, false)))
   if(code == "??"){ return _untracked_diff(ospath.join(r, rel), rel) }
   mut out = _capture_stdout(["git", "-C", r, "diff", "--no-ext-diff", "--minimal", "HEAD", "--", rel])
   if(out.len <= 0){ out = _capture_stdout(["git", "-C", r, "diff", "--no-ext-diff", "--cached", "--", rel]) }
   if(out.len <= 0){ out = "no diff for " + rel + "\n" }
   out
}

fn tree_height(f64 rail_h, int row_count) f64 {
   min(max(150.0, rail_h * 0.64), TREE_HEADER_H + float(row_count) * TREE_ROW_H)
}

fn visible_count(f64 panel_h) int {
   max(0, int((panel_h - TREE_HEADER_H) / TREE_ROW_H))
}

fn clamp_scroll(int scroll, int total, int visible) int {
   min(max(0, scroll), max(0, total - visible))
}

fn row_y(f64 panel_y, int idx) f64 {
   panel_y + TREE_HEADER_H + float(idx) * TREE_ROW_H
}

fn row_at(f64 panel_x, f64 panel_y, f64 panel_w, f64 panel_h, f64 x, f64 y, int count) int {
   if(x < panel_x || x > panel_x + panel_w){ return -1 }
   if(y < panel_y + TREE_HEADER_H || y > panel_y + panel_h){ return -1 }
   def rel = y - panel_y - TREE_HEADER_H
   def idx = int(rel / TREE_ROW_H)
   if(idx < 0 || idx >= count){ return -1 }
   def in_row = rel - float(idx) * TREE_ROW_H
   in_row >= 0.0 && in_row < TREE_ROW_H ? idx : -1
}

fn hit_entry(dict model, f64 panel_x, f64 panel_y, f64 panel_w, f64 panel_h, int scroll, f64 x, f64 y) dict {
   def rows = tree(model)
   def visible = visible_count(panel_h)
   def start = clamp_scroll(scroll, rows.len, visible)
   def idx = row_at(panel_x, panel_y, panel_w, panel_h, x, y, min(visible, rows.len - start))
   idx < 0 ? dict(0) : rows.get(start + idx, {})
}

fn file_kind(str name) str {
   def ext = str.lower(ospath.extname(str.lower(name)))
   if(ext == ".ny" || ext == ".nyt" || ext == ".c" || ext == ".h" || ext == ".cpp" || ext == ".hpp" ||
      ext == ".rs" || ext == ".zig" || ext == ".py" || ext == ".js" || ext == ".ts" || ext == ".sh" ||
   ext == ".cmake" || ext == ".ninja" || ext == ".mk"){ return "code" }
   if(ext == ".md" || ext == ".txt" || ext == ".texi" || ext == ".rst" || ext == ".log"){ return "doc" }
   if(ext == ".json" || ext == ".toml" || ext == ".yaml" || ext == ".yml" || ext == ".xml" || ext == ".csv"){ return "data" }
   if(ext == ".svg" || ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".webp" ||
   ext == ".bmp" || ext == ".gif" || ext == ".hdr" || ext == ".exr" || ext == ".tga"){ return "image" }
   if(ext == ".gltf" || ext == ".glb" || ext == ".obj" || ext == ".fbx" || ext == ".dae" || ext == ".usd" || ext == ".usdz"){ return "model" }
   if(ext == ".glsl" || ext == ".vert" || ext == ".frag" || ext == ".comp" || ext == ".spv" || ext == ".wgsl"){ return "shader" }
   if(ext == ".ttf" || ext == ".otf" || ext == ".woff" || ext == ".woff2"){ return "font" }
   if(ext == ".wav" || ext == ".mp3" || ext == ".ogg" || ext == ".flac"){ return "audio" }
   if(ext == ".mp4" || ext == ".webm" || ext == ".mov" || ext == ".mkv"){ return "video" }
   if(ext == ".zip" || ext == ".tar" || ext == ".gz" || ext == ".xz" || ext == ".7z"){ return "archive" }
   if(ext == ".so" || ext == ".dll" || ext == ".dylib" || ext == ".a" || ext == ".o" || ext == ".obj" ||
   ext == ".exe" || ext == ".elf" || ext == ".bin"){ return "binary" }
   "file"
}

fn file_icon(dict e) str {
   if(e.get("dir", false)){ return "folder" }
   case to_str(e.get("kind", file_kind(to_str(e.get("name", ""))))){
      "code" -> "codeedit"
      "doc" -> "textfile"
      "data" -> "dictionary"
      "image" -> "asset_image"
      "model", "shader" -> "asset_shader"
      _ -> "file"
   }
}

fn status_label(str code) str {
   if(code == "??"){ return "?" }
   if(code == "..."){ return "*" }
   if(str.str_contains(code, "A")){ return "A" }
   if(str.str_contains(code, "D")){ return "D" }
   if(str.str_contains(code, "R")){ return "R" }
   if(str.str_contains(code, "M")){ return "M" }
   str.strip(code)
}

#main {
   mut p = refresh(new("."))
   assert(tree(p).len > 1 && root(p).len > 0, "project tree")
   assert(row_at(0.0, 0.0, 200.0, 200.0, 10.0, TREE_HEADER_H + 1.0, 1) == 0, "project row hit")
   assert(visible_count(150.0) > 0 && clamp_scroll(99, 10, 4) == 6, "project scroll")
   assert(!can_undo_file_op(p) && !can_redo_file_op(p), "project op stacks")
   print("✓ viewer editor project test passed")
}
