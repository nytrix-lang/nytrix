;; Keywords: args argv cli
;; Args for Nytrix
module std.os.args(argc, args, argv, program, positionals_from, positionals, value_from, flag_from, int_value_from, float_value_from, value, flag, int_value, float_value)
use std.core
use std.core.str as str

fn argc(): int {
   "Returns the number of command-line arguments."
   __argc()
}

fn argv(int: i): any {
   "Returns the argv string at index `i`, or 0."
   return __argv(i)
}

fn args(): list {
   "Returns a list of argv strings."
   def n = __argc()
   mut out = list(8)
   mut i = 0
   while(i < n){
      out = out.append(__argv(i))
      i += 1
   }
   out
}

fn program(): str {
   "Returns argv[0] as a string, or an empty string."
   def p = __argv(0)
   p ? to_str(p) : ""
}

fn positionals_from(list: ag): list {
   "Returns user arguments after argv[0]; if `--` is present, returns only arguments after it."
   mut out = list(8)
   mut i = 1
   mut passthrough = false
   while(i < ag.len){
      def item = to_str(ag.get(i))
      if(!passthrough && item == "--"){
         passthrough = true
      } else {
         out = out.append(item)
      }
      i += 1
   }
   out
}

fn positionals(): list {
   "Returns user arguments for this process."
   positionals_from(args())
}

fn _args_assignment_value(str: tok, str: name): any {
   def prefix = name + "="
   if(str.startswith(tok, prefix)){ return str.str_slice(tok, prefix.len, tok.len) }
   nil
}

fn _args_value_from(list: ag, str: name, str: fallback=""): str {
   "Returns the value from `--name value` or `--name=value` in argument list `ag`, or `fallback`."
   mut i = 1
   while(i < ag.len){
      def tok = to_str(ag.get(i))
      if(tok == "--"){ return fallback }
      def assigned = _args_assignment_value(tok, name)
      if(assigned != nil){ return to_str(assigned) }
      if(tok == name){ return(i + 1 < ag.len) ? to_str(ag.get(i + 1)) : fallback }
      i += 1
   }
   fallback
}

fn value_from(list: ag, str: name, str: fallback=""): str {
   "Returns the value from `--name value` or `--name=value` in argument list `ag`, or `fallback`."
   _args_value_from(ag, name, fallback)
}

fn _args_flag_from(list: ag, str: name): bool {
   "Returns whether `name` appears in argument list `ag`."
   mut i = 1
   while(i < ag.len){
      def tok = to_str(ag.get(i))
      if(tok == "--"){ return false }
      if(tok == name || _args_assignment_value(tok, name) != nil){ return true }
      i += 1
   }
   false
}

fn flag_from(list: ag, str: name): bool {
   "Returns whether `name` appears in argument list `ag`."
   _args_flag_from(ag, name)
}

fn int_value_from(list: ag, str: name, int: fallback=0): int {
   "Parses integer option `name` from argument list `ag`, or returns `fallback`."
   def raw = _args_value_from(ag, name, "")
   raw.len == 0 ? fallback : str.atoi(raw)
}

fn float_value_from(list: ag, str: name, f64: fallback=0.0): f64 {
   "Parses floating option `name` from argument list `ag`, or returns `fallback`."
   def raw = _args_value_from(ag, name, "")
   raw.len == 0 ? fallback : str.atof(raw)
}

fn value(str: name, str: fallback=""): str { _args_value_from(args(), name, fallback) }

fn flag(str: name): bool { _args_flag_from(args(), name) }

fn int_value(str: name, int: fallback=0): int { int_value_from(args(), name, fallback) }

fn float_value(str: name, f64: fallback=0.0): f64 { float_value_from(args(), name, fallback) }
