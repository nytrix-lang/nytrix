;; Keywords: fs filesystem files os
;; Filesystem Management for Nytrix
;; References:
;; - std.os
module std.os.fs(is_file, is_dir, list_dir, walk, rename)
use std.core
use std.core as core
use std.os
use std.os.path as ospath
use std.core.str

fn is_file(any path) bool {
   "Returns true if the given `path` exists and points to a regular file."
   if !is_str(path) { return false }
   def p = ospath.normalize(path)
   if !(file_exists(p)) { return false }
   if is_dir(p) { return false }
   return true
}

fn is_dir(any path) bool {
   "Returns true if the given `path` exists and points to a directory."
   if !is_str(path) { return false }
   if path == "." || path == ".." { return true }
   def p = ospath.normalize(path)
   __is_dir(p) == 1
}

fn list_dir(any path) list {
   "Returns a list of filenames contained within the directory at `path`. Excludes '.' and '..' entries."
   if !is_str(path) { return list(0) }
   def p, h = ospath.normalize(path), __dir_open(p)
   if !h { return list(0) }
   mut files = list(8)
   while 1 {
      def name = __dir_read(h)
      if !name { break }
      if name == "." || name == ".." { continue }
      files = files.append(name)
   }
   __dir_close(h)
   return files
}

fn walk(any root, any cb, int max_depth=-1, any visited=nil) int {
   "Recursively traverses `root`, calling `cb(path)` for each entry. Optional `max_depth` limits recursion."
   if !is_str(root) { return 0 }
   mut r = ospath.normalize(root)
   if r.len == 0 { r = "." }
   if !file_exists(r) { return 0 }
   if visited == nil || !is_dict(visited) { visited = dict(16) }
   if visited.contains(r) { return 0 }
   visited = visited.set(r, true)
   cb(r)
   if max_depth == 0 { return 0 }
   if is_dir(r) {
      def items = list_dir(r)
      def n = items.len
      mut i = 0
      while i < n {
         def name = items.get(i)
         def full_path = ospath.join(r, name)
         def next_depth = max_depth > 0 ? max_depth - 1 : -1
         walk(full_path, cb, next_depth, visited)
         i += 1
      }
   }
   0
}

fn rename(str old_path, str new_path) Result<int, int> {
   "Renames or moves `old_path` to `new_path`."
   file_rename(old_path, new_path)
}

mut _fs_selftest_walk_hits = 0

fn _fs_selftest_walk_hit(any _path) int {
   _fs_selftest_walk_hits += 1
   0
}

#main {
   def tmp = ospath.temp_dir()
   assert(is_dir(tmp), "fs temp dir")
   def fp = ospath.join(tmp, "nytrix_fs_selftest_" + to_str(pid()) + "_" + to_str(ticks()) + ".tmp")
   unwrap(file_write(fp, "ok"))
   assert(is_file(fp), "fs temp file")
   def fp2 = fp + ".renamed"
   unwrap(rename(fp, fp2))
   assert(!is_file(fp) && is_file(fp2), "fs rename")
   unwrap(rename(fp2, fp))
   def entries = list_dir(".")
   assert(entries.len > 0, "fs list_dir cwd")
   mut i = 0
   mut n = entries.len
   if n > 32 { n = 32 }
   while i < n {
      def name = entries.get(i)
      assert(name != "." && name != "..", "fs list_dir excludes dots")
      i += 1
   }
   _fs_selftest_walk_hits = 0
   walk(fp, _fs_selftest_walk_hit)
   assert(_fs_selftest_walk_hits == 1, "fs walk file once")
   _fs_selftest_walk_hits = 0
   walk(".", _fs_selftest_walk_hit, 0)
   assert(_fs_selftest_walk_hits == 1, "fs walk max_depth zero")
   unwrap(file_remove(fp))
   print("✓ std.os.fs self-test passed")
}
