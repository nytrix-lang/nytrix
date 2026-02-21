;; Keywords: os fs
;; Filesystem helpers.

module std.os.fs (
   is_file, is_dir, list_dir, walk
)
use std.core *
use std.core as core
use std.os *
use std.os.path as ospath
use std.str *

fn is_file(path){
   "Returns true if path exists and is a regular file."
   if(!is_str(path)){ return false }
   def p = ospath.normalize(path)
   if(!(file_exists(p))){ return false }
   if(is_dir(p)){ return false }
   return true
}

fn is_dir(path){
   "Returns true if path is a directory."
   if(!is_str(path)){ return false }
   if(path == "." || path == ".."){ return true }
   def p = ospath.normalize(path)
   __is_dir(p) == 1
}

fn list_dir(path){
   "Returns a list of filenames in the directory `path`."
   if(!is_str(path)){ return list(0) }
   def p = ospath.normalize(path)
   def h = __dir_open(p)
   if(!h){ return list(0) }
   mut files = list(8)
   while(1){
      def name = __dir_read(h)
      if(!name){ break }
      if(name == "." || name == ".."){ continue }
      files = append(files, name)
   }
   __dir_close(h)
   return files
}

fn walk(root, cb){
   "Walk files under root and call cb(path) recursively."
   if(!is_str(root)){ return 0 }
   mut r = ospath.normalize(root)
   if(str_len(r) == 0){ r = "." }
   if(!file_exists(r)){ return 0 }
   cb(r)
   if(is_dir(r)){
      def items = list_dir(r)
      def n = core.len(items)
      mut i = 0
      while(i < n){
         def name = get(items, i)
         def full_path = ospath.join(r, name)
         walk(full_path, cb)
         i += 1
      }
   }
   0
}

if(comptime{__main()}){
    use std.os.fs *
    use std.os.path as path
    use std.os.dirs *
    use std.os.time *
    use std.os *
    use std.core *
    use std.core.error *

    print("Testing std.os.fs...")

    def tmp = temp_dir()
    assert(is_dir(tmp), "is_dir temp_dir")

    def fp = path.join(tmp, "nytrix_fs_test_" + to_str(pid()) + "_" + to_str(ticks()) + ".tmp")
    match file_write(fp, "ok"){
       ok(_) -> {}
       err(e) -> { panic("file_write failed: " + to_str(e)) }
    }
    assert(is_file(fp), "is_file temp file")

    def entries = list_dir(".")
    assert(len(entries) > 0, "list_dir cwd non-empty")

    mut i = 0
    mut ncheck = len(entries)
    if(ncheck > 64){ ncheck = 64 }
    while(i < ncheck){
       def name = get(entries, i)
       assert(name != "." && name != "..", "list_dir excludes dot entries")
       i = i + 1
    }

    def walk_hits = malloc(8)
    store64(walk_hits, 0)
    fn _walk_hit(_p){
       "Test helper."
       store64(walk_hits, load64(walk_hits) + 1)
       0
    }
    walk(fp, _walk_hit)
    assert(load64(walk_hits) == 1, "walk on file visits once")
    free(walk_hits)

    unwrap(file_remove(fp))

    print("✓ std.os.fs tests passed")
}
