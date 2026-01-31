;; Keywords: str json
;; JSON helpers.

use std.core as core
use std.core.dict as _d
use std.str *

module std.str.json (
   json_decode
)

fn _json_trim(s){
   "Internal: normalizes whitespace around a JSON token."
   if(!core.is_str(s)){ return "" }
   strip(s)
}

fn json_decode(s){
   "Minimal JSON decoder (sufficient for std.util.ast)."
   if(!core.is_str(s)){ return 0 }
   def t = _json_trim(s)
   mut n = str_len(t)
   if(n == 0){ return core.list(0) }
   mut c = core.load8(t, 0)
   if(c == 91){ return core.list(0) } ; '['
   if(c == 123){ return _d.dict(8) } ; '{'
   if(c == 34){ return t } ; '"'
   if((c == 45) || (c >= 48 && c <= 57)){ return atoi(t) }
   if(startswith(t, "true")){ return 1 }
   if(startswith(t, "false")){ return 0 }
   if(startswith(t, "null")){ return 0 }
   core.list(0)
}
