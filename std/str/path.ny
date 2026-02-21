;; Keywords: str path
;; Path helpers.

module std.str.path (
   sep, has_sep, is_abs, join, normalize, basename, dirname, extname, splitext
)
use std.core *
use std.os.path as osp

fn sep(){
   "Returns platform path separator."
   osp.sep()
}

fn is_abs(path){
   "Returns true if `path` is absolute."
   osp.is_abs(path)
}

fn has_sep(path){
   "Returns true if `path` contains a path separator."
   osp.has_sep(path)
}

fn join(a, b){
   "Joins two path segments."
   osp.join(a, b)
}

fn normalize(path){
   "Normalizes path separators and dot segments."
   osp.normalize(path)
}

fn basename(path){
   "Returns the last path component."
   osp.basename(path)
}

fn dirname(path){
   "Returns the parent directory component."
   osp.dirname(path)
}

fn extname(path){
   "Returns extension including dot, or empty string."
   osp.extname(path)
}

fn splitext(path){
   "Splits a path into `[root, ext]`."
   osp.splitext(path)
}

if(comptime{__main()}){
    use std.str.path as path
    use std.core *
    use std.core.error *
    use std.str *

    print("Testing std.str.path...")

    def s = path.sep()
    assert(str_len(s) == 1, "sep is one byte")

    if(s == "\\"){
       assert(path.is_abs("C:\\tmp"), "is_abs windows drive")
    } else {
       assert(path.is_abs("/tmp"), "is_abs unix absolute")
    }

    assert((path.join("a", "b") == "a" + s + "b"), "join")
    assert((path.normalize("a/./b/../c") == "a" + s + "c"), "normalize")
    assert((path.basename("a" + s + "b.txt") == "b.txt"), "basename")
    assert((path.dirname("a" + s + "b.txt") == "a"), "dirname")
    assert((path.extname("archive.tar.gz") == ".gz"), "extname")

    def parts = path.splitext("archive.tar.gz")
    assert((get(parts, 0) == "archive.tar"), "splitext root")
    assert((get(parts, 1) == ".gz"), "splitext ext")

    print("✓ std.str.path tests passed")
}
