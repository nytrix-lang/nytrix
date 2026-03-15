;; Keywords: fs filesystem files
;; Filesystem Management for Nytrix
module std.os.fs(is_file, is_dir, list_dir, walk)
use std.core
use std.core as core
use std.os
use std.os.path as ospath
use std.core.str

fn is_file(any: path): bool {
   "Returns true if the given `path` exists and points to a regular file."
   if(!is_str(path)){ return false }
   def p = ospath.normalize(path)
   if(!(file_exists(p))){ return false }
   if(is_dir(p)){ return false }
   return true
}

fn is_dir(any: path): bool {
   "Returns true if the given `path` exists and points to a directory."
   if(!is_str(path)){ return false }
   if(path == "." || path == ".."){ return true }
   def p = ospath.normalize(path)
   __is_dir(p) == 1
}

fn list_dir(any: path): list {
   "Returns a list of filenames contained within the directory at `path`. Excludes '.' and '..' entries."
   if(!is_str(path)){ return list(0) }
   def p, h = ospath.normalize(path), __dir_open(p)
   if(!h){ return list(0) }
   mut files = list(8)
   while(1){
      def name = __dir_read(h)
      if(!name){ break }
      if(name == "." || name == ".."){ continue }
      files = files.append(name)
   }
   __dir_close(h)
   return files
}

fn walk(any: root, any: cb): int {
   "Recursively traverses the filesystem starting at `root`, calling `cb(path)` for every file and directory encountered."
   if(!is_str(root)){ return 0 }
   mut r = ospath.normalize(root)
   if(r.len == 0){ r = "." }
   if(!file_exists(r)){ return 0 }
   cb(r)
   if(is_dir(r)){
      def items = list_dir(r)
      def n = items.len
      mut i = 0
      while(i < n){
         def name = items.get(i)
         def full_path = ospath.join(r, name)
         walk(full_path, cb)
         i += 1
      }
   }
   0
}
