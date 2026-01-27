;; Keywords: os fs
;; Filesystem helpers.

use std.core *
use std.os *
use std.os.sys *
use std.os.process *
use std.str *
use std.str.io *

module std.os.fs (
   is_file, is_dir, walk
)

fn is_file(path){
   "Returns true if path exists and is a regular file."
   if(!is_str(path)){ return false }
   if(file_exists(path) == false){ return false }
   if(is_dir(path)){ return false }
   return true
}

fn is_dir(path){
   "Returns true if path is a directory."
   if(!is_str(path)){ return false }
   if(path == "." || path == ".."){ return true }
   def fd = sys_open(path, 65536, 0) ;; O_DIRECTORY
   if(fd >= 0){
      sys_close(fd)
      return true
   }
   return false
}

fn _read_fd_all(fd){
   ;; Deprecated/unused by walk now
   ""
}

fn list_dir(path){
   "Returns a list of filenames in the directory `path`."
   def fd = sys_open(path, 65536, 0) ; O_RDONLY | O_DIRECTORY (0x10000)
   if(fd < 0){ return list(0) }
   
   def buf_size = 4096
   def buf = malloc(buf_size)
   def files = list(8)
   
   while(1){
      def nread = sys_getdents64(fd, buf, buf_size)
      if(nread <= 0){ break }
      
      mut bpos = 0
      while(bpos < nread){
         if(bpos + 19 > nread){ break } ;; Header check
         def d_reclen = load16(buf, bpos + 16)
         if(d_reclen == 0){ break } ;; Safety
         if(bpos + d_reclen > nread){ break } ;; Entry check
         
         def d_type = load8(buf, bpos + 18)
         ;; def name_ptr = ptr_add(buf, bpos + 19) ;; Bad: creates odd ptr
         def name = cstr_to_str(buf, bpos + 19)
         
         if(name != "." && name != ".."){
             files = append(files, name)
         }
         
         bpos = bpos + d_reclen
      }
   }
   
   free(buf)
   sys_close(fd)
   files
}

fn walk(root, cb){
   "Walk files under root and call cb(path) recursively."
   cb(root)
   if(is_dir(root)){
      def items = list_dir(root)
      def n = list_len(items)
      mut i = 0
      while(i < n){
         def name = get(items, i)
         def full_path = (root == ".") ? name : (root + "/" + name)
         walk(full_path, cb)
         i = i + 1
      }
   }
   0
}
