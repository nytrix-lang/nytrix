;; Keywords: io fs
;; Io Fs module.

use std.core
use std.strings.str
use std.collections
use std.io.path
module std.io.fs (
   mkdir, mkdirs, listdir, listdir_full, is_file, is_dir, walk, rename, rmdir, chmod, chown,
   stat
)

fn mkdir(path){
   "Create a new directory at `path` with mode 0777."
   return __syscall(83, path, 511, 0,0,0,0)
}

fn mkdirs(path){
   "Create a directory and all parent directories as needed (like `mkdir -p`)."
   def parts = split(path, "/")
   def accum = ""
   def i =0
   while(i<list_len(parts)){
      def p = get(parts, i)
      if(len(p) > 0){
         if(len(accum)==0){
            accum = p
         } else {
            accum = f"{accum}/{p}"
         }
         mkdir(accum)
      }
      i=i+1
   }
   return 0
}

fn listdir(path){
   "Return a list of names of entries in the directory at `path` (excluding '.' and '..')."
   def fd = sys_open(path, 0, 0)
   if(!fd || fd < 0){ return list(8) }
   def res = list(8)
   def buf_sz = 4096
   def buf = __malloc(buf_sz)
   while(1){
      def nread = __syscall(217, fd, buf, buf_sz, 0,0,0)
      if(nread <= 0){ break }
      def bpos = 0
      while(bpos < nread){
         def reclen = load16(buf, bpos + 16)
         if(reclen == 0){ break } ; reclen 0 means EOF
         def name = cstr_to_str(buf, bpos + 19)
         if(!eq(name, ".") && !eq(name, "..")){
            res = append(res, name)
         }
         bpos = bpos + reclen
      }
   }
   __free(buf)
   sys_close(fd)
   return res
}

fn listdir_full(path){
   "Return a list of full absolute or relative paths for all items in a directory."
   def names = listdir(path)
   def res = list(8)
   def i = 0
   while(i < len(names)){
      res = append(res, path_join(path, get(names, i)))
      i = i + 1
   }
   return res
}

fn is_file(path){
   "Return 1 if `path` exists and is a regular file, 0 otherwise."
   def st = stat(path)
   if(list_len(st) == 0){ return 0 }
   def mode = get(st, 2)
   return (mode & 61440) == 32768
}

fn is_dir(path){
   "Return 1 if `path` exists and is a directory, 0 otherwise."
   def st = stat(path)
   if(list_len(st) == 0){ return 0 }
   def mode = get(st, 2)
   return (mode & 61440) == 16384
}

fn walk(path, cb){
   "Recursively walks the directory at `path`, calling function `cb(full_path)` for each item found."
   cb(path)
   if(is_dir(path)){
      def files = listdir(path)
      def i = 0
      while(i < list_len(files)){
         def f = get(files, i)
         def full = f"{path}/{f}"
         walk(full, cb)
         i = i + 1
      }
   }
}

fn rename(oldpath, newpath){
   "Renames a file or directory from `oldpath` to `newpath`."
   return __syscall(82, oldpath, newpath, 0,0,0,0)
}

fn rmdir(path){
   "Removes the empty directory at `path`."
   return __syscall(84, path, 0,0,0,0,0)
}

fn chmod(path, mode){
   "Changes the permissions of the file at `path` to `mode`."
   return __syscall(90, path, mode, 0,0,0,0)
}

fn chown(path, user, group){
   "Changes the ownership of the file at `path` to the specified `user` and `group` IDs."
   return __syscall(92, path, user, group, 0,0,0)
}

fn stat(path){
   "Retrieves status information for the file at `path`. Returns a list [dev, ino, mode, nlink, uid, gid, size, atime, mtime, ctime]."
   def buf = __malloc(144)
   def r = __syscall(4, path, buf, 0,0,0,0)
   if(r != 0){ __free(buf)  return list(8)  }
   def dev = (load64(buf, 0) << 1) | 1
   def ino = (load64(buf, 8) << 1) | 1
   def nlink = (load64(buf, 16) << 1) | 1
   def mode = load32(buf, 24)
   def uid = load32(buf, 28)
   def gid = load32(buf, 32)
   def size = (load64(buf, 48) << 1) | 1
   def atime = (load64(buf, 72) << 1) | 1
   def mtime = (load64(buf, 88) << 1) | 1
   def ctime = (load64(buf, 104) << 1) | 1
   def res = [dev, ino, mode, nlink, uid, gid, size, atime, mtime, ctime]
   __free(buf)
   return res
}