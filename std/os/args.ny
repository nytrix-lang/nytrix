;; Keywords: os args
;; Args helpers.

module std.os.args (
   args, argv
)
use std.core *

fn argv(i){
   "Returns the argv string at index `i`, or 0."
   return __argv(i)
}

fn args(){
   "Returns a list of argv strings."
   def n = __argc()
   mut out = list(8)
   mut i = 0
   while(i < n){
      out = append(out, __argv(i))
      i += 1
   }
   out
}

if(comptime{__main()}){
    use std.core *
    use std.os.args *
    use std.str *
    use std.str.io *

    def ag = args()
    assert(is_list(ag), "args() returns list")
    assert(len(ag) > 0, "args() not empty")

    ;; Check first arg is program name
    def prog = get(ag, 0)
    assert(is_str(prog), "arg 0 is string")
    assert(str_len(prog) > 0, "arg 0 non-empty")

    def a0 = argv(0)
    assert((a0 == prog), "argv(0) matches args()[0]")

    print("✓ std.os.args tests passed")
}
