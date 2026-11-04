;; Keywords: args argv cli os
;; Args for Nytrix
;; References:
;; - core.os
module core.os.args(argc, args, argv, program, positionals_from, positionals, value_from, flag_from, int_value_from, float_value_from, first_positive_int_from, value, flag, int_value, float_value, first_positive_int)
use core
use core.str as str

fn argc() int {
   "Returns the number of command-line arguments."
   __argc()
}

fn argv(int i) str {
   "Returns the argv string at index `i`, or an empty string when out of range."
   return __argv(i)
}

fn args() list {
   "Returns a list of argv strings."
   def n = __argc()
   mut out = list(8)
   mut i = 0
   while i < n {
      out = out.append(__argv(i))
      i += 1
   }
   out
}

fn program() str {
   "Returns argv[0] as a string, or an empty string."
   def p = __argv(0)
   p ? to_str(p) : ""
}

fn positionals_from(list ag) list {
   "Returns arguments after argv[0], or only those after the first `--` if present."
   mut out = list(8)
   mut i = 1
   while i < ag.len {
      def tok = to_str(ag.get(i))
      if tok == "--" {
         out = list(8)
         i += 1
         while i < ag.len {
            out = out.append(to_str(ag.get(i)))
            i += 1
         }
         return out
      }
      out = out.append(tok)
      i += 1
   }
   out
}

fn positionals() list {
   "Returns user arguments for this process."
   positionals_from(args())
}

fn _args_assignment_value(str tok, str name) any {
   def prefix = name + "="
   if str.startswith(tok, prefix) { return str.str_slice(tok, prefix.len, tok.len) }
   nil
}

fn _args_value_from(list ag, str name, str fallback="") str {
   "Returns the value from `--name value` or `--name=value` in argument list `ag`, or `fallback`."
   mut i = 1
   while i < ag.len {
      def tok = to_str(ag.get(i))
      if tok == "--" { return fallback }
      def prefix = name + "="
      if str.startswith(tok, prefix) {
         return str.str_slice(tok, prefix.len, tok.len)
      }
      if tok == name {
         if i + 1 >= ag.len { return fallback }
         def candidate = to_str(ag.get(i + 1))
         return candidate == "-" || !str.startswith(candidate, "-") ? candidate : fallback
      }
      i += 1
   }
   fallback
}

fn value_from(list ag, str name, str fallback="") str {
   "Returns the value from `--name value` or `--name=value` in argument list `ag`, or `fallback`."
   _args_value_from(ag, name, fallback)
}

fn _args_flag_from(list ag, str name) bool {
   "Returns the last positive or `--no-name` setting, including short clusters."
   mut i = 1
   mut stop = ag.len
   while i < ag.len {
      if to_str(ag.get(i)) == "--" {
         stop = i
         break
      }
      i += 1
   }
   i = stop - 1
   def negated = str.startswith(name, "--") ? "--no-" + str.str_slice(name, 2, name.len) : ""
   while i > 0 {
      def tok = to_str(ag.get(i))
      if tok == name || _args_assignment_value(tok, name) != nil {
         return true
      }
      if negated.len > 0 && tok == negated { return false }
      i -= 1
   }
   false
}

fn flag_from(list ag, str name) bool {
   "Returns whether `name` appears in argument list `ag`."
   _args_flag_from(ag, name)
}

fn int_value_from(list ag, str name, int fallback=0) int {
   "Parses integer option `name` from argument list `ag`, or returns `fallback`."
   def raw = _args_value_from(ag, name, "")
   raw.len == 0 ? fallback : str.atoi(raw)
}

fn float_value_from(list ag, str name, f64 fallback=0.0) f64 {
   "Parses floating option `name` from argument list `ag`, or returns `fallback`."
   def raw = _args_value_from(ag, name, "")
   raw.len == 0 ? fallback : str.atof(raw)
}

fn first_positive_int_from(list ag, int fallback=0) int {
   "Returns the first positive integer in `ag`, or `fallback`."
   mut i = 0
   while i < ag.len {
      def n = str.atoi(to_str(ag.get(i, "")))
      if n > 0 { return n }
      i += 1
   }
   fallback
}

fn value(str name, str fallback="") str { _args_value_from(args(), name, fallback) }

fn flag(str name) bool { _args_flag_from(args(), name) }

fn int_value(str name, int fallback=0) int { int_value_from(args(), name, fallback) }

fn float_value(str name, f64 fallback=0.0) f64 { float_value_from(args(), name, fallback) }

fn first_positive_int(int fallback=0) int {
   mut i = 0
   while i < __argc() {
      def n = str.atoi(to_str(__argv(i)))
      if n > 0 { return n }
      i += 1
   }
   fallback
}

#main {
   def ag = args()
   assert(is_list(ag), "args list")
   assert(ag.len > 0, "args nonempty")
   def prog = ag.get(0)
   assert(is_str(prog) && prog.len > 0, "args argv0")
   assert(argv(0) == prog, "args argv matches")
   assert(program() == prog, "args program")
   assert(is_str(argv(-1)) && argv(-1) == "", "args argv negative boundary")
   assert(is_str(argv(argc())) && argv(argc()) == "", "args argv upper boundary")
   assert(positionals_from(["ny", "a", "--", "--raw"]) == ["--raw"], "args positionals after separator")
   assert(positionals_from(["ny", "a", "b"]) == ["a", "b"], "args positionals without separator")
   assert(positionals_from(["ny", "--", "a", "--", "b"]) == ["a", "--", "b"], "args positionals consume first separator")
   assert(value_from(["ny", "--x", "42"], "--x", "0") == "42", "args value next")
   assert(value_from(["ny", "--x=42"], "--x", "0") == "42", "args value assigned")
   assert(value_from(["ny", "--x"], "--x", "fallback") == "fallback", "args value fallback")
   assert(flag_from(["ny", "--flag"], "--flag"), "args flag")
   assert(!flag_from(["ny", "--feature", "--no-feature"], "--feature"), "args negated flag")
   assert(value_from(["ny", "-o=out"], "-o", "") == "out", "args short equals value")
   assert(int_value_from(["ny", "--n", "7"], "--n", 0) == 7, "args int value")
   assert(float_value_from(["ny", "--f", "2.5"], "--f", 0.0) > 2.0, "args float value")
   assert(first_positive_int_from(["ny", "0", "-3", "12"], 5) == 12 && first_positive_int_from(["ny", "0"], 5) == 5, "args first positive int")
   print("✓ core.os.args self-test passed")
}
